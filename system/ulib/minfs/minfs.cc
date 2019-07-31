// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <inttypes.h>
#include <limits>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <bitmap/raw-bitmap.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_call.h>
#include <fbl/unique_ptr.h>
#include <fs/block-txn.h>
#include <fs/trace.h>
#include <safemath/checked_math.h>

#ifdef __Fuchsia__
#include <fbl/auto_lock.h>
#include <fuchsia/minfs/c/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/zx/event.h>
#endif

#include <minfs/fsck.h>
#include <minfs/minfs.h>

#include <utility>

#include "file.h"
#include "minfs-private.h"

namespace minfs {
namespace {

// Deletes all known slices from an MinFS Partition.
void minfs_free_slices(Bcache* bc, const Superblock* info) {
    if ((info->flags & kMinfsFlagFVM) == 0) {
        return;
    }
#ifdef __Fuchsia__
    extend_request_t request;
    const size_t kBlocksPerSlice = info->slice_size / kMinfsBlockSize;
    if (info->ibm_slices) {
        request.length = info->ibm_slices;
        request.offset = kFVMBlockInodeBmStart / kBlocksPerSlice;
        bc->FVMShrink(&request);
    }
    if (info->abm_slices) {
        request.length = info->abm_slices;
        request.offset = kFVMBlockDataBmStart / kBlocksPerSlice;
        bc->FVMShrink(&request);
    }
    if (info->ino_slices) {
        request.length = info->ino_slices;
        request.offset = kFVMBlockInodeStart / kBlocksPerSlice;
        bc->FVMShrink(&request);
    }
    if (info->dat_slices) {
        request.length = info->dat_slices;
        request.offset = kFVMBlockDataStart / kBlocksPerSlice;
        bc->FVMShrink(&request);
    }
#endif
}

} // namespace

zx_time_t GetTimeUTC() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    zx_time_t time = zx_time_add_duration(ZX_SEC(ts.tv_sec), ts.tv_nsec);
    return time;
}

void DumpInfo(const Superblock* info) {
    FS_TRACE_DEBUG("minfs: data blocks:  %10u (size %u)\n", info->block_count, info->block_size);
    FS_TRACE_DEBUG("minfs: inodes:  %10u (size %u)\n", info->inode_count, info->inode_size);
    FS_TRACE_DEBUG("minfs: allocated blocks  @ %10u\n", info->alloc_block_count);
    FS_TRACE_DEBUG("minfs: allocated inodes  @ %10u\n", info->alloc_inode_count);
    FS_TRACE_DEBUG("minfs: inode bitmap @ %10u\n", info->ibm_block);
    FS_TRACE_DEBUG("minfs: alloc bitmap @ %10u\n", info->abm_block);
    FS_TRACE_DEBUG("minfs: inode table  @ %10u\n", info->ino_block);
    FS_TRACE_DEBUG("minfs: data blocks  @ %10u\n", info->dat_block);
    FS_TRACE_DEBUG("minfs: FVM-aware: %s\n", (info->flags & kMinfsFlagFVM) ? "YES" : "NO");
}

void DumpInode(const Inode* inode, ino_t ino) {
    FS_TRACE_DEBUG("inode[%u]: magic:  %10u\n", ino, inode->magic);
    FS_TRACE_DEBUG("inode[%u]: size:   %10u\n", ino, inode->size);
    FS_TRACE_DEBUG("inode[%u]: blocks: %10u\n", ino, inode->block_count);
    FS_TRACE_DEBUG("inode[%u]: links:  %10u\n", ino, inode->link_count);
}

zx_status_t CheckSuperblock(const Superblock* info, Bcache* bc) {
    uint32_t max = bc->Maxblk();
    DumpInfo(info);

    if ((info->magic0 != kMinfsMagic0) || (info->magic1 != kMinfsMagic1)) {
        FS_TRACE_ERROR("minfs: bad magic\n");
        return ZX_ERR_INVALID_ARGS;
    }
    if (info->version != kMinfsVersion) {
        FS_TRACE_ERROR("minfs: FS Version: %08x. Driver version: %08x\n", info->version,
                       kMinfsVersion);
        return ZX_ERR_INVALID_ARGS;
    }
    if ((info->block_size != kMinfsBlockSize) || (info->inode_size != kMinfsInodeSize)) {
        FS_TRACE_ERROR("minfs: bsz/isz %u/%u unsupported\n", info->block_size, info->inode_size);
        return ZX_ERR_INVALID_ARGS;
    }

#ifdef __Fuchsia__
    if ((info->flags & kMinfsFlagClean) == 0) {
        FS_TRACE_ERROR("minfs: filesystem in dirty state. Was not unmounted cleanly.\n");
    } else {
        FS_TRACE_INFO("minfs: filesystem in clean state.\n");
    }
#endif

    TransactionLimits limits(*info);
    if ((info->flags & kMinfsFlagFVM) == 0) {
        if (info->dat_block + info->block_count != max) {
            FS_TRACE_ERROR("minfs: too large for device\n");
            return ZX_ERR_INVALID_ARGS;
        }

        if (info->dat_block - info->journal_start_block < limits.GetMinimumJournalBlocks()) {
            FS_TRACE_ERROR("minfs: journal too small\n");
            return ZX_ERR_BAD_STATE;
        }
    } else {
        const size_t kBlocksPerSlice = info->slice_size / kMinfsBlockSize;
#ifdef __Fuchsia__
        fuchsia_hardware_block_volume_VolumeInfo fvm_info;
        zx_status_t status = bc->FVMQuery(&fvm_info);
        if (status != ZX_OK) {
            FS_TRACE_ERROR("minfs: unable to query FVM :%d\n", status);
            return ZX_ERR_UNAVAILABLE;
        }

        if (info->slice_size != fvm_info.slice_size) {
            FS_TRACE_ERROR("minfs: slice size %lu did not match expected size %lu\n",
                           info->slice_size, fvm_info.slice_size );
            return ZX_ERR_BAD_STATE;
        }

        size_t expected_count[4];
        expected_count[0] = info->ibm_slices;
        expected_count[1] = info->abm_slices;
        expected_count[2] = info->ino_slices;
        expected_count[3] = info->dat_slices;

        query_request_t request;
        request.count = 4;
        request.vslice_start[0] = kFVMBlockInodeBmStart / kBlocksPerSlice;
        request.vslice_start[1] = kFVMBlockDataBmStart / kBlocksPerSlice;
        request.vslice_start[2] = kFVMBlockInodeStart / kBlocksPerSlice;
        request.vslice_start[3] = kFVMBlockDataStart / kBlocksPerSlice;

        fuchsia_hardware_block_volume_VsliceRange
                ranges[fuchsia_hardware_block_volume_MAX_SLICE_REQUESTS];
        size_t ranges_count;

        if ((status = bc->FVMVsliceQuery(&request, ranges, &ranges_count)) != ZX_OK) {
            FS_TRACE_ERROR("minfs: unable to query FVM: %d\n", status);
            return ZX_ERR_UNAVAILABLE;
        }

        if (ranges_count != request.count) {
            FS_TRACE_ERROR("minfs: requested FVM range :%lu does not match received: %lu\n",
                           request.count, ranges_count);
            return ZX_ERR_BAD_STATE;
        }

        for (unsigned i = 0; i < request.count; i++) {
            size_t minfs_count = expected_count[i];
            size_t fvm_count = ranges[i].count;

            if (!ranges[i].allocated || fvm_count < minfs_count) {
                // Currently, since Minfs can only grow new slices, it should not be possible for
                // the FVM to report a slice size smaller than what is reported by Minfs. In this
                // case, automatically fail without trying to resolve the situation, as it is
                // possible that Minfs structures are allocated in the slices that have been lost.
                FS_TRACE_ERROR("minfs: mismatched slice count\n");
                return ZX_ERR_IO_DATA_INTEGRITY;
            }

            if (fvm_count > minfs_count) {
                // If FVM reports more slices than we expect, try to free remainder.
                extend_request_t shrink;
                shrink.length = fvm_count - minfs_count;
                shrink.offset = request.vslice_start[i] + minfs_count;
                if ((status = bc->FVMShrink(&shrink)) != ZX_OK) {
                    FS_TRACE_ERROR("minfs: Unable to shrink to expected size, status: %d\n",
                                   status);
                    return ZX_ERR_IO_DATA_INTEGRITY;
                }
            }
        }
#endif
        // Verify that the allocated slices are sufficient to hold
        // the allocated data structures of the filesystem.
        size_t ibm_blocks_needed = (info->inode_count + kMinfsBlockBits - 1) / kMinfsBlockBits;
        size_t ibm_blocks_allocated = info->ibm_slices * kBlocksPerSlice;
        if (ibm_blocks_needed > ibm_blocks_allocated) {
            FS_TRACE_ERROR("minfs: Not enough slices for inode bitmap\n");
            return ZX_ERR_INVALID_ARGS;
        } else if (ibm_blocks_allocated + info->ibm_block >= info->abm_block) {
            FS_TRACE_ERROR("minfs: Inode bitmap collides into block bitmap\n");
            return ZX_ERR_INVALID_ARGS;
        }

        size_t abm_blocks_needed = (info->block_count + kMinfsBlockBits - 1) / kMinfsBlockBits;
        size_t abm_blocks_allocated = info->abm_slices * kBlocksPerSlice;
        if (abm_blocks_needed > abm_blocks_allocated) {
            FS_TRACE_ERROR("minfs: Not enough slices for block bitmap\n");
            return ZX_ERR_INVALID_ARGS;
        } else if (abm_blocks_allocated + info->abm_block >= info->ino_block) {
            FS_TRACE_ERROR("minfs: Block bitmap collides with inode table\n");
            return ZX_ERR_INVALID_ARGS;
        }

        size_t ino_blocks_needed =
            (info->inode_count + kMinfsInodesPerBlock - 1) / kMinfsInodesPerBlock;
        size_t ino_blocks_allocated = info->ino_slices * kBlocksPerSlice;
        if (ino_blocks_needed > ino_blocks_allocated) {
            FS_TRACE_ERROR("minfs: Not enough slices for inode table\n");
            return ZX_ERR_INVALID_ARGS;
        } else if (ino_blocks_allocated + info->ino_block >= info->journal_start_block) {
            FS_TRACE_ERROR("minfs: Inode table collides with data blocks\n");
            return ZX_ERR_INVALID_ARGS;
        }

        size_t journal_blocks_needed = limits.GetMinimumJournalBlocks();
        size_t journal_blocks_allocated = info->journal_slices * kBlocksPerSlice;
        if (journal_blocks_needed > journal_blocks_allocated) {
            FS_TRACE_ERROR("minfs: Not enough slices for journal\n");
            return ZX_ERR_INVALID_ARGS;
        }
        if (journal_blocks_allocated + info->journal_start_block > info->dat_block) {
            FS_TRACE_ERROR("minfs: Journal collides with data blocks\n");
            return ZX_ERR_INVALID_ARGS;
        }

        size_t dat_blocks_needed = info->block_count;
        size_t dat_blocks_allocated = info->dat_slices * kBlocksPerSlice;
        if (dat_blocks_needed > dat_blocks_allocated) {
            FS_TRACE_ERROR("minfs: Not enough slices for data blocks\n");
            return ZX_ERR_INVALID_ARGS;
        } else if (dat_blocks_allocated + info->dat_block > std::numeric_limits<blk_t>::max()) {
            FS_TRACE_ERROR("minfs: Data blocks overflow blk_t\n");
            return ZX_ERR_INVALID_ARGS;
        } else if (dat_blocks_needed <= 1) {
            FS_TRACE_ERROR("minfs: Not enough data blocks\n");
            return ZX_ERR_INVALID_ARGS;
        }
    }
    // TODO: validate layout
    return 0;
}

#ifndef __Fuchsia__
BlockOffsets::BlockOffsets(const Bcache& bc, const SuperblockManager& sb) {
    if (bc.extent_lengths_.size() > 0) {
        ZX_ASSERT(bc.extent_lengths_.size() == kExtentCount);
        ibm_block_count_ = static_cast<blk_t>(bc.extent_lengths_[1] / kMinfsBlockSize);
        abm_block_count_ = static_cast<blk_t>(bc.extent_lengths_[2] / kMinfsBlockSize);
        ino_block_count_ = static_cast<blk_t>(bc.extent_lengths_[3] / kMinfsBlockSize);
        journal_block_count_ = static_cast<blk_t>(bc.extent_lengths_[4] / kMinfsBlockSize);
        dat_block_count_ = static_cast<blk_t>(bc.extent_lengths_[5] / kMinfsBlockSize);

        ibm_start_block_ = static_cast<blk_t>(bc.extent_lengths_[0] / kMinfsBlockSize);
        abm_start_block_ = ibm_start_block_ + ibm_block_count_;
        ino_start_block_ = abm_start_block_ + abm_block_count_;
        journal_start_block_ = ino_start_block_ + ino_block_count_;
        dat_start_block_ = journal_start_block_ + journal_block_count_;
    } else {
        ibm_start_block_ = sb.Info().ibm_block;
        abm_start_block_ = sb.Info().abm_block;
        ino_start_block_ = sb.Info().ino_block;
        journal_start_block_ = sb.Info().journal_start_block;
        dat_start_block_ = sb.Info().dat_block;

        ibm_block_count_ = abm_start_block_ - ibm_start_block_;
        abm_block_count_ = ino_start_block_ - abm_start_block_;
        ino_block_count_ = dat_start_block_ - ino_start_block_;
        journal_block_count_ = dat_start_block_ - journal_start_block_;
        dat_block_count_ = sb.Info().block_count;
    }
}
#endif

zx_status_t Minfs::BeginTransaction(size_t reserve_inodes, size_t reserve_blocks,
                                    fbl::unique_ptr<Transaction>* out) {
    ZX_DEBUG_ASSERT(reserve_inodes <= TransactionLimits::kMaxInodeBitmapBlocks);
#ifdef __Fuchsia__
    if (writeback_ == nullptr) {
        return ZX_ERR_BAD_STATE;
    }

    // TODO(planders): Once we are splitting up write transactions, assert this on host as well.
    ZX_DEBUG_ASSERT(reserve_blocks <= limits_.GetMaximumDataBlocks());
#endif
    // Reserve blocks from allocators before returning WritebackWork to client.
    return Transaction::Create(this, reserve_inodes, reserve_blocks, inodes_.get(),
                               block_allocator_.get(), out);
}

zx_status_t Minfs::EnqueueWork(fbl::unique_ptr<WritebackWork> work) {
#ifdef __Fuchsia__
    return writeback_->Enqueue(std::move(work));
#else
    return work->Complete();
#endif
}

zx_status_t Minfs::CommitTransaction(fbl::unique_ptr<Transaction> transaction) {
#ifdef __Fuchsia__
    ZX_DEBUG_ASSERT(writeback_ != nullptr);
    // TODO(planders): Move this check to Journal enqueue.
    ZX_DEBUG_ASSERT(transaction->GetWork()->BlockCount() <= limits_.GetMaximumEntryDataBlocks());
#endif
    // Take the transaction's metadata updates, and pass them back to the writeback buffer.
    // This begins the pipeline of "actually writing these updates out to persistent storage".
    return EnqueueWork(transaction->RemoveWork());
}

#ifdef __Fuchsia__
void Minfs::Sync(SyncCallback closure) {
    if (assigner_ != nullptr) {
        // This callback will be processed after all "delayed data" operations have completed:
        // this is why we "enqueue a callback" that will later "enqueue a callback" somewhere
        // else.
        auto cb = [closure = std::move(closure)](TransactionalFs* minfs) mutable {
            minfs->EnqueueCallback(std::move(closure));
        };
        assigner_->EnqueueCallback(std::move(cb));
    } else {
        // If Minfs is read-only (data block assigner has not been initialized), immediately
        // resolve the callback.
        closure(ZX_OK);
    }
}
#endif

#ifdef __Fuchsia__
Minfs::Minfs(fbl::unique_ptr<Bcache> bc, fbl::unique_ptr<SuperblockManager> sb,
             fbl::unique_ptr<Allocator> block_allocator, fbl::unique_ptr<InodeManager> inodes,
             uint64_t fs_id)
    : bc_(std::move(bc)), sb_(std::move(sb)), block_allocator_(std::move(block_allocator)),
      inodes_(std::move(inodes)), fs_id_(fs_id), limits_(sb_->Info()) {}
#else
Minfs::Minfs(fbl::unique_ptr<Bcache> bc, fbl::unique_ptr<SuperblockManager> sb,
             fbl::unique_ptr<Allocator> block_allocator, fbl::unique_ptr<InodeManager> inodes,
             BlockOffsets offsets)
    : bc_(std::move(bc)), sb_(std::move(sb)), block_allocator_(std::move(block_allocator)),
      inodes_(std::move(inodes)), offsets_(std::move(offsets)), limits_(sb_->Info()) {}
#endif

Minfs::~Minfs() {
    vnode_hash_.clear();
}

#ifdef __Fuchsia__
zx_status_t Minfs::FVMQuery(fuchsia_hardware_block_volume_VolumeInfo* info) const {
    if (!(Info().flags & kMinfsFlagFVM)) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    return bc_->FVMQuery(info);
}
#endif

zx_status_t Minfs::InoFree(Transaction* transaction, VnodeMinfs* vn) {
    TRACE_DURATION("minfs", "Minfs::InoFree", "ino", vn->GetIno());

#ifdef __Fuchsia__
    vn->CancelPendingWriteback();
#endif

    inodes_->Free(transaction->GetWork(), vn->GetIno());
    uint32_t block_count = vn->GetInode()->block_count;

    // release all direct blocks
    for (unsigned n = 0; n < kMinfsDirect; n++) {
        if (vn->GetInode()->dnum[n] == 0) {
            continue;
        }
        ValidateBno(vn->GetInode()->dnum[n]);
        block_count--;
        block_allocator_->Free(transaction->GetWork(), vn->GetInode()->dnum[n]);
    }

    // release all indirect blocks
    for (unsigned n = 0; n < kMinfsIndirect; n++) {
        if (vn->GetInode()->inum[n] == 0) {
            continue;
        }

#ifdef __Fuchsia__
        zx_status_t status;
        if ((status = vn->InitIndirectVmo()) != ZX_OK) {
            return status;
        }

        uint32_t* entry;
        vn->ReadIndirectVmoBlock(n, &entry);
#else
        uint32_t entry[kMinfsBlockSize];
        vn->ReadIndirectBlock(vn->GetInode()->inum[n], entry);
#endif

        // release the direct blocks pointed at by the entries in the indirect block
        for (unsigned m = 0; m < kMinfsDirectPerIndirect; m++) {
            if (entry[m] == 0) {
                continue;
            }
            block_count--;
            block_allocator_->Free(transaction->GetWork(), entry[m]);
        }
        // release the direct block itself
        block_count--;
        block_allocator_->Free(transaction->GetWork(), vn->GetInode()->inum[n]);
    }

    // release doubly indirect blocks
    for (unsigned n = 0; n < kMinfsDoublyIndirect; n++) {
        if (vn->GetInode()->dinum[n] == 0) {
            continue;
        }
#ifdef __Fuchsia__
        zx_status_t status;
        if ((status = vn->InitIndirectVmo()) != ZX_OK) {
            return status;
        }

        uint32_t* dentry;
        vn->ReadIndirectVmoBlock(GetVmoOffsetForDoublyIndirect(n), &dentry);
#else
        uint32_t dentry[kMinfsBlockSize];
        vn->ReadIndirectBlock(vn->GetInode()->dinum[n], dentry);
#endif
        // release indirect blocks
        for (unsigned m = 0; m < kMinfsDirectPerIndirect; m++) {
            if (dentry[m] == 0) {
                continue;
            }

#ifdef __Fuchsia__
            if ((status = vn->LoadIndirectWithinDoublyIndirect(n)) != ZX_OK) {
                return status;
            }

            uint32_t* entry;
            vn->ReadIndirectVmoBlock(GetVmoOffsetForIndirect(n) + m, &entry);

#else
            uint32_t entry[kMinfsBlockSize];
            vn->ReadIndirectBlock(dentry[m], entry);
#endif

            // release direct blocks
            for (unsigned k = 0; k < kMinfsDirectPerIndirect; k++) {
                if (entry[k] == 0) {
                    continue;
                }

                block_count--;
                block_allocator_->Free(transaction->GetWork(), entry[k]);
            }

            block_count--;
            block_allocator_->Free(transaction->GetWork(), dentry[m]);
        }

        // release the doubly indirect block itself
        block_count--;
        block_allocator_->Free(transaction->GetWork(), vn->GetInode()->dinum[n]);
    }

    ZX_DEBUG_ASSERT(block_count == 0);
    ZX_DEBUG_ASSERT(vn->IsUnlinked());
    return ZX_OK;
}

void Minfs::AddUnlinked(Transaction* transaction, VnodeMinfs* vn) {
    ZX_DEBUG_ASSERT(vn->GetInode()->link_count == 0);

    Superblock* info = sb_->MutableInfo();

    if (info->unlinked_tail == 0) {
        // If no other vnodes are unlinked, |vn| is now both the head and the tail.
        ZX_DEBUG_ASSERT(info->unlinked_head == 0);
        info->unlinked_head = vn->GetIno();
        info->unlinked_tail = vn->GetIno();
    } else {
        // Since all vnodes in the unlinked list are necessarily open, the last vnode
        // must currently exist in the vnode lookup.
        fbl::RefPtr<VnodeMinfs> last_vn = VnodeLookupInternal(info->unlinked_tail);
        ZX_DEBUG_ASSERT(last_vn != nullptr);

        // Add |vn| to the end of the unlinked list.
        last_vn->SetNextInode(vn->GetIno());
        vn->SetLastInode(last_vn->GetIno());
        info->unlinked_tail = vn->GetIno();

        last_vn->InodeSync(transaction->GetWork(), kMxFsSyncDefault);
        vn->InodeSync(transaction->GetWork(), kMxFsSyncDefault);
    }

    sb_->Write(transaction->GetWork());
}

void Minfs::RemoveUnlinked(Transaction* transaction, VnodeMinfs* vn) {
    if (vn->GetInode()->last_inode == 0) {
        // If |vn| is the first unlinked inode, we just need to update the list head
        // to the next inode (which may not exist).
        ZX_DEBUG_ASSERT_MSG(Info().unlinked_head == vn->GetIno(),
            "Vnode %u has no previous link, but is not listed as unlinked list head", vn->GetIno());
        sb_->MutableInfo()->unlinked_head = vn->GetInode()->next_inode;
    } else {
        // Set the previous vnode's next to |vn|'s next.
        fbl::RefPtr<VnodeMinfs> last_vn = VnodeLookupInternal(vn->GetInode()->last_inode);
        ZX_DEBUG_ASSERT(last_vn != nullptr);
        last_vn->SetNextInode(vn->GetInode()->next_inode);
        last_vn->InodeSync(transaction->GetWork(), kMxFsSyncDefault);
    }

    if (vn->GetInode()->next_inode == 0) {
        // If |vn| is the last unlinked inode, we just need to update the list tail
        // to the previous inode (which may not exist).
        ZX_DEBUG_ASSERT_MSG(Info().unlinked_tail == vn->GetIno(),
            "Vnode %u has no next link, but is not listed as unlinked list tail", vn->GetIno());
        sb_->MutableInfo()->unlinked_tail = vn->GetInode()->last_inode;
    } else {
        // Set the next vnode's previous to |vn|'s previous.
        fbl::RefPtr<VnodeMinfs> next_vn = VnodeLookupInternal(vn->GetInode()->next_inode);
        ZX_DEBUG_ASSERT(next_vn != nullptr);
        next_vn->SetLastInode(vn->GetInode()->last_inode);
        next_vn->InodeSync(transaction->GetWork(), kMxFsSyncDefault);
    }
}

zx_status_t Minfs::PurgeUnlinked() {
    ino_t last_ino = 0;
    ino_t next_ino = Info().unlinked_head;
    ino_t unlinked_count = 0;

    // Loop through the unlinked list and free all allocated resources.
    while (next_ino != 0) {
        zx_status_t status;
        fbl::RefPtr<VnodeMinfs> vn;
        fbl::unique_ptr<Transaction> transaction;
        if ((status = BeginTransaction(0, 0, &transaction)) != ZX_OK) {
            return status;
        }
        if ((status = VnodeMinfs::Recreate(this, next_ino, &vn)) != ZX_OK) {
            return ZX_ERR_NO_MEMORY;
        }

        ZX_DEBUG_ASSERT(vn->GetInode()->last_inode == last_ino);
        ZX_DEBUG_ASSERT(vn->GetInode()->link_count == 0);

        if ((status = InoFree(transaction.get(), vn.get())) != ZX_OK) {
            return status;
        }

        last_ino = next_ino;
        next_ino = vn->GetInode()->next_inode;

        sb_->MutableInfo()->unlinked_head = next_ino;

        if (next_ino == 0) {
            ZX_DEBUG_ASSERT(Info().unlinked_tail == last_ino);
            sb_->MutableInfo()->unlinked_tail = 0;
        }

        sb_->Write(transaction->GetWork());
        status = CommitTransaction(std::move(transaction));
        if (status != ZX_OK) {
            return status;
        }

        unlinked_count++;
    }

    ZX_DEBUG_ASSERT(Info().unlinked_head == 0);
    ZX_DEBUG_ASSERT(Info().unlinked_tail == 0);

    if (unlinked_count > 0) {
        FS_TRACE_WARN("minfs: Found and purged %u unlinked vnode(s) on mount\n", unlinked_count);
    }

    return ZX_OK;
}

#ifdef __Fuchsia__
zx_status_t Minfs::CreateFsId(uint64_t* out) {
    zx::event event;
    zx_status_t status = zx::event::create(0, &event);
    if (status != ZX_OK) {
        return status;
    }
    zx_info_handle_basic_t info;
    status = event.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
    if (status != ZX_OK) {
        return status;
    }

    *out = info.koid;
    return ZX_OK;
}
#endif

fbl::RefPtr<VnodeMinfs> Minfs::VnodeLookupInternal(uint32_t ino) {
#ifdef __Fuchsia__
    fbl::RefPtr<VnodeMinfs> vn;
    {
        // Avoid releasing a reference to |vn| while holding |hash_lock_|.
        fbl::AutoLock lock(&hash_lock_);
        auto rawVn = vnode_hash_.find(ino);
        if (!rawVn.IsValid()) {
            // Nothing exists in the lookup table
            return nullptr;
        }
        vn = fbl::MakeRefPtrUpgradeFromRaw(rawVn.CopyPointer(), hash_lock_);
        if (vn == nullptr) {
            // The vn 'exists' in the map, but it is being deleted.
            // Remove it (by key) so the next person doesn't trip on it,
            // and so we can insert another node with the same key into the hash
            // map.
            // Notably, VnodeRelease erases the vnode by object, not key,
            // so it will not attempt to replace any distinct Vnodes that happen
            // to be re-using the same inode.
            vnode_hash_.erase(ino);
        }
    }
    return vn;
#else
    return fbl::WrapRefPtr(vnode_hash_.find(ino).CopyPointer());
#endif
}

void Minfs::InoNew(Transaction* transaction, const Inode* inode, ino_t* out_ino) {
    size_t allocated_ino = transaction->AllocateInode();
    *out_ino = static_cast<ino_t>(allocated_ino);
    // Write the inode back to storage.
    InodeUpdate(transaction->GetWork(), *out_ino, inode);
}

zx_status_t Minfs::VnodeNew(Transaction* transaction, fbl::RefPtr<VnodeMinfs>* out, uint32_t type) {
    TRACE_DURATION("minfs", "Minfs::VnodeNew");
    if ((type != kMinfsTypeFile) && (type != kMinfsTypeDir)) {
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::RefPtr<VnodeMinfs> vn;

    // Allocate the in-memory vnode
    VnodeMinfs::Allocate(this, type, &vn);

    // Allocate the on-disk inode
    ino_t ino;
    InoNew(transaction, vn->GetInode(), &ino);
    vn->SetIno(ino);
    VnodeInsert(vn.get());
    *out = std::move(vn);
    return ZX_OK;
}

void Minfs::VnodeInsert(VnodeMinfs* vn) {
#ifdef __Fuchsia__
    fbl::AutoLock lock(&hash_lock_);
#endif

    ZX_DEBUG_ASSERT_MSG(!vnode_hash_.find(vn->GetKey()).IsValid(), "ino %u already in map\n",
                        vn->GetKey());
    vnode_hash_.insert(vn);
}

fbl::RefPtr<VnodeMinfs> Minfs::VnodeLookup(uint32_t ino) {
    fbl::RefPtr<VnodeMinfs> vn = VnodeLookupInternal(ino);
#ifdef __Fuchsia__
    if (vn != nullptr && vn->IsUnlinked()) {
        vn = nullptr;
    }
#endif
    return vn;
}

void Minfs::VnodeRelease(VnodeMinfs* vn) {
#ifdef __Fuchsia__
    fbl::AutoLock lock(&hash_lock_);
#endif
    vnode_hash_.erase(*vn);
}

zx_status_t Minfs::VnodeGet(fbl::RefPtr<VnodeMinfs>* out, ino_t ino) {
    TRACE_DURATION("minfs", "Minfs::VnodeGet", "ino", ino);
    if ((ino < 1) || (ino >= Info().inode_count)) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    fs::Ticker ticker(StartTicker());

    fbl::RefPtr<VnodeMinfs> vn = VnodeLookup(ino);
    if (vn != nullptr) {
        *out = std::move(vn);
        UpdateOpenMetrics(/* cache_hit= */ true, ticker.End());
        return ZX_OK;
    }

    zx_status_t status;
    if ((status = VnodeMinfs::Recreate(this, ino, &vn)) != ZX_OK) {
        return ZX_ERR_NO_MEMORY;
    }

    if (vn->IsUnlinked()) {
        // If a vnode we have recreated from disk is unlinked, something has gone wrong during the
        // unlink process and our filesystem is now in an inconsistent state. In order to avoid
        // further inconsistencies, prohibit access to this vnode.
        FS_TRACE_WARN("minfs: Attempted to load unlinked vnode %u\n", ino);
        return ZX_ERR_BAD_STATE;
    }

    VnodeInsert(vn.get());
    *out = std::move(vn);
    UpdateOpenMetrics(/* cache_hit= */ false, ticker.End());
    return ZX_OK;
}

// Allocate a new data block from the block bitmap.
void Minfs::BlockNew(Transaction* transaction, blk_t* out_bno) {
    size_t allocated_bno = transaction->AllocateBlock();
    *out_bno = static_cast<blk_t>(allocated_bno);
    ValidateBno(*out_bno);
}

bool Minfs::IsReadonly() {
 #ifdef __Fuchsia__
    fbl::AutoLock lock(&vfs_lock_);
 #endif
    return ReadonlyLocked();
}

void Minfs::UpdateFlags(Transaction* transaction, uint32_t flags, bool set) {
     if (set) {
        sb_->MutableInfo()->flags |= flags;
     } else {
        sb_->MutableInfo()->flags &= (~flags);
     }
     sb_->Write(transaction->GetWork());
}

#ifdef __Fuchsia__
void Minfs::BlockSwap(Transaction* transaction, blk_t in_bno, blk_t* out_bno) {
    if (in_bno > 0) {
        ValidateBno(in_bno);
    }

    size_t allocated_bno = transaction->SwapBlock(in_bno);
    *out_bno = static_cast<blk_t>(allocated_bno);
    ValidateBno(*out_bno);
}
#endif

void Minfs::BlockFree(Transaction* transaction, blk_t bno) {
    ValidateBno(bno);
    block_allocator_->Free(transaction->GetWork(), bno);
}

void InitializeDirectory(void* bdata, ino_t ino_self, ino_t ino_parent) {
#define DE0_SIZE DirentSize(1)

    // directory entry for self
    Dirent* de = (Dirent*)bdata;
    de->ino = ino_self;
    de->reclen = DE0_SIZE;
    de->namelen = 1;
    de->type = kMinfsTypeDir;
    de->name[0] = '.';

    // directory entry for parent
    de = (Dirent*)((uintptr_t)bdata + DE0_SIZE);
    de->ino = ino_parent;
    de->reclen = DirentSize(2) | kMinfsReclenLast;
    de->namelen = 2;
    de->type = kMinfsTypeDir;
    de->name[0] = '.';
    de->name[1] = '.';
}

zx_status_t Minfs::Create(fbl::unique_ptr<Bcache> bc, const Superblock* info,
                          fbl::unique_ptr<Minfs>* out, IntegrityCheck checks) {
#ifndef __Fuchsia__
    if (bc->extent_lengths_.size() != 0 && bc->extent_lengths_.size() != kExtentCount) {
        FS_TRACE_ERROR("minfs: invalid number of extents\n");
        return ZX_ERR_INVALID_ARGS;
    }
#endif

    fbl::unique_ptr<SuperblockManager> sb;
    zx_status_t status;

    if ((status = SuperblockManager::Create(bc.get(), info, &sb, checks)) != ZX_OK) {
        FS_TRACE_ERROR("Minfs::Create failed to initialize superblock: %d\n", status);
        return status;
    }

#ifdef __Fuchsia__
    const blk_t abm_start_block = sb->Info().abm_block;
    const blk_t ibm_start_block = sb->Info().ibm_block;
    const blk_t ino_start_block = sb->Info().ino_block;
#else
    BlockOffsets offsets(*bc, *sb);
    const blk_t abm_start_block = offsets.AbmStartBlock();
    const blk_t ibm_start_block = offsets.IbmStartBlock();
    const blk_t ino_start_block = offsets.InoStartBlock();
#endif

    fs::ReadTxn transaction(bc.get());

    // Block Bitmap allocator initialization.
    AllocatorFvmMetadata block_allocator_fvm = AllocatorFvmMetadata(
        &sb->MutableInfo()->dat_slices, &sb->MutableInfo()->abm_slices, info->slice_size);
    AllocatorMetadata block_allocator_meta =
        AllocatorMetadata(info->dat_block, abm_start_block, (info->flags & kMinfsFlagFVM) != 0,
                          std::move(block_allocator_fvm), &sb->MutableInfo()->alloc_block_count,
                          &sb->MutableInfo()->block_count);

    fbl::unique_ptr<PersistentStorage> storage(
        new PersistentStorage(bc.get(), sb.get(), kMinfsBlockSize, nullptr,
                              std::move(block_allocator_meta)));

    fbl::unique_ptr<Allocator> block_allocator;
    if ((status = Allocator::Create(&transaction, std::move(storage), &block_allocator)) != ZX_OK) {
        FS_TRACE_ERROR("Minfs::Create failed to initialize block allocator: %d\n", status);
        return status;
    }

    // Inode Bitmap allocator initialization.
    AllocatorFvmMetadata inode_allocator_fvm = AllocatorFvmMetadata(
        &sb->MutableInfo()->ino_slices, &sb->MutableInfo()->ibm_slices, info->slice_size);
    AllocatorMetadata inode_allocator_meta =
        AllocatorMetadata(ino_start_block, ibm_start_block, (info->flags & kMinfsFlagFVM) != 0,
                          std::move(inode_allocator_fvm), &sb->MutableInfo()->alloc_inode_count,
                          &sb->MutableInfo()->inode_count);

    fbl::unique_ptr<InodeManager> inodes;
    if ((status = InodeManager::Create(bc.get(), sb.get(), &transaction,
                                       std::move(inode_allocator_meta),
                                       ino_start_block, info->inode_count, &inodes)) != ZX_OK) {
        FS_TRACE_ERROR("Minfs::Create failed to initialize inodes: %d\n", status);
        return status;
    }

    if ((status = transaction.Transact()) != ZX_OK) {
        FS_TRACE_ERROR("Minfs::Create failed to read initial blocks: %d\n", status);
        return status;
    }

#ifdef __Fuchsia__
    uint64_t id;
    status = Minfs::CreateFsId(&id);
    if (status != ZX_OK) {
        FS_TRACE_ERROR("minfs: failed to create fs_id: %d\n", status);
        return status;
    }

    *out =
        fbl::unique_ptr<Minfs>(new Minfs(std::move(bc), std::move(sb), std::move(block_allocator),
                                         std::move(inodes), id));
#else
    *out =
        fbl::unique_ptr<Minfs>(new Minfs(std::move(bc), std::move(sb), std::move(block_allocator),
                                         std::move(inodes), std::move(offsets)));
#endif

    return ZX_OK;
}

#ifdef __Fuchsia__
zx_status_t Minfs::InitializeWriteback() {
    // Use a heuristics-based approach based on physical RAM size to
    // determine the size of the writeback buffer.
    //
    // Currently, we set the writeback buffer size to 2% of physical
    // memory.
    static const size_t kWriteBufferSize =
        fbl::round_up((zx_system_get_physmem() * 2) / 100, kMinfsBlockSize);
    static const blk_t kWriteBufferBlocks = static_cast<blk_t>(kWriteBufferSize / kMinfsBlockSize);

    zx_status_t status;
    if ((status = WritebackQueue::Create(bc_.get(), kWriteBufferBlocks, &writeback_)) != ZX_OK) {
        return status;
    }

    if ((status = PurgeUnlinked()) != ZX_OK) {
        return status;
    }

    if ((status = WorkQueue::Create(this, &assigner_)) != ZX_OK) {
        return status;
    }

    return ZX_OK;
}
#endif

zx_status_t Mount(fbl::unique_ptr<minfs::Bcache> bc, const MountOptions& options,
                  fbl::RefPtr<VnodeMinfs>* root_out) {
    TRACE_DURATION("minfs", "minfs_mount");
    zx_status_t status;

    char blk[kMinfsBlockSize];
    if ((status = bc->Readblk(0, &blk)) != ZX_OK) {
        FS_TRACE_ERROR("minfs: could not read info block: %d\n", status);
        return status;
    }
    const Superblock* info = reinterpret_cast<Superblock*>(blk);
#ifdef __Fuchsia__
    if ((info->flags & kMinfsFlagClean) == 0) {
        FS_TRACE_WARN("minfs: filesystem not unmounted cleanly. Integrity check required\n");
    }
#endif
    fbl::unique_ptr<Minfs> fs;
    if ((status = Minfs::Create(std::move(bc), info, &fs, IntegrityCheck::kAll)) != ZX_OK) {
        FS_TRACE_ERROR("minfs: failed to create filesystem object %d\n", status);
        return status;
    }

#ifdef __Fuchsia__
    if (!options.readonly && (status = fs->InitializeWriteback()) != ZX_OK) {
        return status;
    }
#endif

    fbl::RefPtr<VnodeMinfs> vn;
    if ((status = fs->VnodeGet(&vn, kMinfsRootIno)) != ZX_OK) {
        FS_TRACE_ERROR("minfs: cannot find root inode: %d\n", status);
        return status;
    }

    ZX_DEBUG_ASSERT(vn->IsDirectory());

#ifdef __Fuchsia__
    // Filesystem is safely mounted at this point. On a read-write filesystem, since we can now
    // serve writes on the filesystem, we need to unset the kMinfsFlagClean flag to indicate
    // that the filesystem may not be in a "clean" state anymore. This helps to make sure we are
    // unmounted cleanly i.e the kMinfsFlagClean flag is set back on clean unmount.
    if (options.readonly == false) {
        fbl::unique_ptr<Transaction> transaction;
        if ((status = fs->BeginTransaction(0, 0, &transaction)) == ZX_OK) {
            fs->UpdateFlags(transaction.get(), kMinfsFlagClean, false);
            status = fs->CommitTransaction(std::move(transaction));
        }
        if (status != ZX_OK) {
            FS_TRACE_WARN("minfs: failed to unset clean flag: %d\n", status);
        }
     }
#endif
    __UNUSED auto r = fs.release();
    *root_out = std::move(vn);
    return ZX_OK;
}

#ifdef __Fuchsia__
zx_status_t MountAndServe(const MountOptions& options, async_dispatcher_t* dispatcher,
                          fbl::unique_ptr<Bcache> bc, zx::channel mount_channel,
                          fbl::Closure on_unmount) {
    TRACE_DURATION("minfs", "MountAndServe");

    fbl::RefPtr<VnodeMinfs> vn;
    zx_status_t status = Mount(std::move(bc), options, &vn);
    if (status != ZX_OK) {
        return status;
    }

    Minfs* vfs = vn->Vfs();
    vfs->SetReadonly(options.readonly);
    vfs->SetMetrics(options.metrics);
    vfs->SetUnmountCallback(std::move(on_unmount));
    vfs->SetDispatcher(dispatcher);
    return vfs->ServeDirectory(std::move(vn), std::move(mount_channel));
}

void Minfs::Shutdown(fs::Vfs::ShutdownCallback cb) {
    // On a read-write filesystem, set the kMinfsFlagClean on a clean unmount.
    if (IsReadonly() == false) {
        fbl::unique_ptr<Transaction> transaction;
        zx_status_t status;
        if ((status = BeginTransaction(0, 0, &transaction)) == ZX_OK) {
            UpdateFlags(transaction.get(), kMinfsFlagClean, true);
            status = CommitTransaction(std::move(transaction));
        }
        if (status != ZX_OK) {
            FS_TRACE_WARN("minfs: Failed to set clean flag on unmount: %d\n", status);;
        }
    }
    ManagedVfs::Shutdown([this, cb = std::move(cb)](zx_status_t status) mutable {
        Sync([this, cb = std::move(cb)](zx_status_t) mutable {
            async::PostTask(dispatcher(), [this, cb = std::move(cb)]() mutable {
                // Ensure writeback buffer completes before auxilliary structures
                // are deleted.
                // The data block assigner must be resolved first, so it can enqueue any pending
                // transactions to the writeback buffer.
                assigner_ = nullptr;
                writeback_ = nullptr;
                bc_->Sync();

                auto on_unmount = std::move(on_unmount_);

                // Explicitly delete this (rather than just letting the memory release when
                // the process exits) to ensure that the block device's fifo has been
                // closed.
                delete this;

                // Identify to the unmounting channel that teardown is complete.
                cb(ZX_OK);

                // Identify to the unmounting thread that teardown is complete.
                if (on_unmount) {
                    on_unmount();
                }
            });
        });
    });
}
#endif

uint32_t BlocksRequiredForInode(uint64_t inode_count) {
    return safemath::checked_cast<uint32_t>((inode_count + kMinfsInodesPerBlock - 1) /
                                            kMinfsInodesPerBlock);
}

uint32_t BlocksRequiredForBits(uint64_t bit_count) {
    return safemath::checked_cast<uint32_t>((bit_count + kMinfsBlockBits - 1) / kMinfsBlockBits);
}

zx_status_t Mkfs(const MountOptions& options, fbl::unique_ptr<Bcache> bc) {
    Superblock info;
    memset(&info, 0x00, sizeof(info));
    info.magic0 = kMinfsMagic0;
    info.magic1 = kMinfsMagic1;
    info.version = kMinfsVersion;
    info.flags = kMinfsFlagClean;
    info.block_size = kMinfsBlockSize;
    info.inode_size = kMinfsInodeSize;

    uint32_t blocks = 0;
    uint32_t inodes = 0;

    zx_status_t status;
    auto fvm_cleanup =
        fbl::MakeAutoCall([bc = bc.get(), &info]() { minfs_free_slices(bc, &info); });
#ifdef __Fuchsia__
    fuchsia_hardware_block_volume_VolumeInfo fvm_info;
    if (bc->FVMQuery(&fvm_info) == ZX_OK) {
        info.slice_size = fvm_info.slice_size;
        SetMinfsFlagFvm(info);

        if (info.slice_size % kMinfsBlockSize) {
            FS_TRACE_ERROR("minfs mkfs: Slice size not multiple of minfs block: %lu\n",
                           info.slice_size);
            return -1;
        }

        const size_t kBlocksPerSlice = info.slice_size / kMinfsBlockSize;
        extend_request_t request;
        request.length = 1;
        request.offset = kFVMBlockInodeBmStart / kBlocksPerSlice;
        if ((status = bc->FVMReset()) != ZX_OK) {
            FS_TRACE_ERROR("minfs mkfs: Failed to reset FVM slices: %d\n", status);
            return status;
        }
        if ((status = bc->FVMExtend(&request)) != ZX_OK) {
            FS_TRACE_ERROR("minfs mkfs: Failed to allocate inode bitmap: %d\n", status);
            return status;
        }
        info.ibm_slices = 1;
        request.offset = kFVMBlockDataBmStart / kBlocksPerSlice;
        if ((status = bc->FVMExtend(&request)) != ZX_OK) {
            FS_TRACE_ERROR("minfs mkfs: Failed to allocate data bitmap: %d\n", status);
            return status;
        }
        info.abm_slices = 1;
        request.offset = kFVMBlockInodeStart / kBlocksPerSlice;
        if ((status = bc->FVMExtend(&request)) != ZX_OK) {
            FS_TRACE_ERROR("minfs mkfs: Failed to allocate inode table: %d\n", status);
            return status;
        }
        info.ino_slices = 1;

        TransactionLimits limits(info);
        blk_t journal_blocks = limits.GetRecommendedJournalBlocks();
        request.length = fbl::round_up(journal_blocks, kBlocksPerSlice) / kBlocksPerSlice;
        request.offset = kFVMBlockJournalStart / kBlocksPerSlice;
        if ((status = bc->FVMExtend(&request)) != ZX_OK) {
            FS_TRACE_ERROR("minfs mkfs: Failed to allocate journal blocks: %d\n", status);
            return status;
        }
        info.journal_slices = static_cast<blk_t>(request.length);

        ZX_ASSERT(options.fvm_data_slices > 0);
        request.length = options.fvm_data_slices;
        request.offset = kFVMBlockDataStart / kBlocksPerSlice;
        if ((status = bc->FVMExtend(&request)) != ZX_OK) {
            FS_TRACE_ERROR("minfs mkfs: Failed to allocate data blocks: %d\n", status);
            return status;
        }
        info.dat_slices = options.fvm_data_slices;

        inodes = static_cast<uint32_t>(info.ino_slices * info.slice_size / kMinfsInodeSize);
        blocks = static_cast<uint32_t>(info.dat_slices * info.slice_size / kMinfsBlockSize);
    }
#endif
    if ((info.flags & kMinfsFlagFVM) == 0) {
        inodes = kMinfsDefaultInodeCount;
        blocks = bc->Maxblk();
    }

    // Determine how many blocks of inodes, allocation bitmaps,
    // and inode bitmaps there are
    uint32_t inoblks = (inodes + kMinfsInodesPerBlock - 1) / kMinfsInodesPerBlock;
    uint32_t ibmblks = (inodes + kMinfsBlockBits - 1) / kMinfsBlockBits;
    uint32_t abmblks = 0;

    info.inode_count = inodes;
    info.alloc_block_count = 0;
    info.alloc_inode_count = 0;

    if ((info.flags & kMinfsFlagFVM) == 0) {
        blk_t non_dat_blocks;
        blk_t journal_blocks = 0;

        info.ibm_block = 8;
        info.abm_block = info.ibm_block + fbl::round_up(ibmblks, 8u);

        for (uint32_t alloc_bitmap_rounded = 8; alloc_bitmap_rounded < blocks;
             alloc_bitmap_rounded += 8) {
            // Increment bitmap blocks by 8, since we will always round this value up to 8.
            ZX_ASSERT(alloc_bitmap_rounded % 8 == 0);

            info.ino_block = info.abm_block + alloc_bitmap_rounded;

            // Calculate the journal size based on other metadata structures.
            TransactionLimits limits(info);
            journal_blocks = limits.GetRecommendedJournalBlocks();

            non_dat_blocks = 8 + fbl::round_up(ibmblks, 8u) + alloc_bitmap_rounded + inoblks;

            // If the recommended journal count is too high, try using the minimum instead.
            if (non_dat_blocks + journal_blocks >= blocks) {
                journal_blocks = limits.GetMinimumJournalBlocks();
            }

            non_dat_blocks += journal_blocks;
            if (non_dat_blocks >= blocks) {
                FS_TRACE_ERROR("mkfs: Partition size (%" PRIu64 " bytes) is too small\n",
                        static_cast<uint64_t>(blocks) * kMinfsBlockSize);
                return ZX_ERR_INVALID_ARGS;
            }

            info.block_count = blocks - non_dat_blocks;
            // Calculate the exact number of bitmap blocks needed to track this many data blocks.
            abmblks = (info.block_count + kMinfsBlockBits - 1) / kMinfsBlockBits;

            if (alloc_bitmap_rounded >= abmblks) {
                // It is possible that the abmblks value will actually bring us back to the next
                // lowest tier of 8-rounded values. This means we may have 8 blocks allocated for
                // the block bitmap which will never actually be used. This is not ideal, but is
                // expected, and should only happen for very particular block counts.
                break;
            }
        }

        info.journal_start_block = info.ino_block + inoblks;
        info.dat_block = info.journal_start_block + journal_blocks;
    } else {
        info.block_count = blocks;
        abmblks = (info.block_count + kMinfsBlockBits - 1) / kMinfsBlockBits;
        info.ibm_block = kFVMBlockInodeBmStart;
        info.abm_block = kFVMBlockDataBmStart;
        info.ino_block = kFVMBlockInodeStart;
        info.journal_start_block = kFVMBlockJournalStart;
        info.dat_block = kFVMBlockDataStart;
    }

    DumpInfo(&info);

    RawBitmap abm;
    RawBitmap ibm;

    // By allocating the bitmap and then shrinking it, we keep the underlying
    // storage a block multiple but ensure we can't allocate beyond the last
    // real block or inode.
    if ((status = abm.Reset(fbl::round_up(info.block_count, kMinfsBlockBits))) != ZX_OK) {
        FS_TRACE_ERROR("mkfs: Failed to allocate block bitmap: %d\n", status);
        return status;
    }
    if ((status = ibm.Reset(fbl::round_up(info.inode_count, kMinfsBlockBits))) != ZX_OK) {
        FS_TRACE_ERROR("mkfs: Failed to allocate inode bitmap: %d\n", status);
        return status;
    }
    if ((status = abm.Shrink(info.block_count)) != ZX_OK) {
        FS_TRACE_ERROR("mkfs: Failed to shrink block bitmap: %d\n", status);
        return status;
    }
    if ((status = ibm.Shrink(info.inode_count)) != ZX_OK) {
        FS_TRACE_ERROR("mkfs: Failed to shrink inode bitmap: %d\n", status);
        return status;
    }

    // Write rootdir
    uint8_t blk[kMinfsBlockSize];
    memset(blk, 0, sizeof(blk));
    InitializeDirectory(blk, kMinfsRootIno, kMinfsRootIno);
    if ((status = bc->Writeblk(info.dat_block + 1, blk)) != ZX_OK) {
        FS_TRACE_ERROR("mkfs: Failed to write root directory: %d\n", status);
        return status;
    }

    // Update inode bitmap
    ibm.Set(0, 1);
    ibm.Set(kMinfsRootIno, kMinfsRootIno + 1);
    info.alloc_inode_count += 2;

    // update block bitmap:
    // Reserve the 0th data block (as a 'null' value)
    // Reserve the 1st data block (for root directory)
    abm.Set(0, 2);
    info.alloc_block_count += 2;

    // Write allocation bitmap
    for (uint32_t n = 0; n < abmblks; n++) {
        void* bmdata = fs::GetBlock(kMinfsBlockSize, abm.StorageUnsafe()->GetData(), n);
        memcpy(blk, bmdata, kMinfsBlockSize);
        if ((status = bc->Writeblk(info.abm_block + n, blk)) != ZX_OK) {
            return status;
        }
    }

    // Write inode bitmap
    for (uint32_t n = 0; n < ibmblks; n++) {
        void* bmdata = fs::GetBlock(kMinfsBlockSize, ibm.StorageUnsafe()->GetData(), n);
        memcpy(blk, bmdata, kMinfsBlockSize);
        if ((status = bc->Writeblk(info.ibm_block + n, blk)) != ZX_OK) {
            return status;
        }
    }

    // Write inodes
    memset(blk, 0, sizeof(blk));
    for (uint32_t n = 0; n < inoblks; n++) {
        if ((status = bc->Writeblk(info.ino_block + n, blk)) != ZX_OK) {
            return status;
        }
    }

    // Setup root inode
    Inode* ino = reinterpret_cast<Inode*>(&blk[0]);
    ino[kMinfsRootIno].magic = kMinfsMagicDir;
    ino[kMinfsRootIno].size = kMinfsBlockSize;
    ino[kMinfsRootIno].block_count = 1;
    ino[kMinfsRootIno].link_count = 2;
    ino[kMinfsRootIno].dirent_count = 2;
    ino[kMinfsRootIno].dnum[0] = 1;
    ino[kMinfsRootIno].create_time = GetTimeUTC();
    bc->Writeblk(info.ino_block, blk);

    // Write the superblock at block number 0.
    memset(blk, 0, sizeof(blk));
    memcpy(blk, &info, sizeof(info));
    bc->Writeblk(0, blk);

    // Write the journal info block to disk.
    memset(blk, 0, sizeof(blk));
    JournalInfo* journal_info = reinterpret_cast<JournalInfo*>(blk);
    journal_info->magic = kJournalMagic;
    bc->Writeblk(info.journal_start_block, blk);

    fvm_cleanup.cancel();
    return ZX_OK;
}

zx_status_t Minfs::ReadDat(blk_t bno, void* data) {
#ifdef __Fuchsia__
    return bc_->Readblk(Info().dat_block + bno, data);
#else
    return ReadBlk(bno, offsets_.DatStartBlock(), offsets_.DatBlockCount(), Info().block_count,
                   data);
#endif
}


zx_status_t Minfs::ReadBlock(blk_t start_block_num,  void* out_data) const {
    return bc_->Readblk(start_block_num, out_data);
}

#ifndef __Fuchsia__
zx_status_t Minfs::ReadBlk(blk_t bno, blk_t start, blk_t soft_max, blk_t hard_max, void* data) {
    if (bno >= hard_max) {
        return ZX_ERR_OUT_OF_RANGE;
    }
    if (bno >= soft_max) {
        memset(data, 0, kMinfsBlockSize);
        return ZX_OK;
    }

    return bc_->Readblk(start + bno, data);
}

zx_status_t CreateBcacheFromFd(fbl::unique_fd fd, off_t start, off_t end,
                               const fbl::Vector<size_t>& extent_lengths,
                               fbl::unique_ptr<minfs::Bcache>* out) {
    if (start >= end) {
        fprintf(stderr, "error: Insufficient space allocated\n");
        return ZX_ERR_INVALID_ARGS;
    }

    if (extent_lengths.size() != kExtentCount) {
        FS_TRACE_ERROR("error: invalid number of extents : %lu\n", extent_lengths.size());
        return ZX_ERR_INVALID_ARGS;
    }

    struct stat s;
    if (fstat(fd.get(), &s) < 0) {
        FS_TRACE_ERROR("error: minfs could not find end of file/device\n");
        return ZX_ERR_IO;
    }

    if (s.st_size < end) {
        FS_TRACE_ERROR("error: invalid file size\n");
        return ZX_ERR_INVALID_ARGS;
    }

    size_t size = (end - start) / minfs::kMinfsBlockSize;

    zx_status_t status;
    fbl::unique_ptr<minfs::Bcache> bc;
    if ((status = minfs::Bcache::Create(&bc, std::move(fd), static_cast<uint32_t>(size))) !=
        ZX_OK) {
        FS_TRACE_ERROR("error: cannot create block cache: %d\n", status);
        return status;
    }

    if ((status = bc->SetSparse(start, extent_lengths)) != ZX_OK) {
        FS_TRACE_ERROR("Bcache is already sparse: %d\n", status);
        return status;
    }

    *out = std::move(bc);
    return ZX_OK;
}

zx_status_t SparseFsck(fbl::unique_fd fd, off_t start, off_t end,
                       const fbl::Vector<size_t>& extent_lengths) {

    fbl::unique_ptr<minfs::Bcache> bc;
    zx_status_t status;
    if ((status = CreateBcacheFromFd(std::move(fd), start, end, extent_lengths, &bc)) != ZX_OK) {
        return status;
    }

    return Fsck(std::move(bc));
}

zx_status_t SparseUsedDataSize(fbl::unique_fd fd, off_t start, off_t end,
                               const fbl::Vector<size_t>& extent_lengths, uint64_t* out_size) {
    fbl::unique_ptr<minfs::Bcache> bc;
    zx_status_t status;

    if ((status = CreateBcacheFromFd(std::move(fd), start, end, extent_lengths, &bc)) != ZX_OK) {
        return status;
    }
    return UsedDataSize(bc, out_size);
}

zx_status_t SparseUsedInodes(fbl::unique_fd fd, off_t start, off_t end,
                             const fbl::Vector<size_t>& extent_lengths, uint64_t* out_inodes) {
    fbl::unique_ptr<minfs::Bcache> bc;
    zx_status_t status;
    if ((status = CreateBcacheFromFd(std::move(fd), start, end, extent_lengths, &bc)) != ZX_OK) {
        return status;
    }
    return UsedInodes(bc, out_inodes);
}

zx_status_t SparseUsedSize(fbl::unique_fd fd, off_t start, off_t end,
                           const fbl::Vector<size_t>& extent_lengths, uint64_t* out_size) {
    fbl::unique_ptr<minfs::Bcache> bc;
    zx_status_t status;

    if ((status = CreateBcacheFromFd(std::move(fd), start, end, extent_lengths, &bc)) != ZX_OK) {
        return status;
    }
    return UsedSize(bc, out_size);
}

#endif


#ifdef __Fuchsia__
fbl::Vector<BlockRegion> Minfs::GetAllocatedRegions() const {
    return block_allocator_->GetAllocatedRegions();
}
#endif

} // namespace minfs
