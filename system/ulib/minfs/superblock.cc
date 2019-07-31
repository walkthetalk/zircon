// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdlib.h>
#include <string.h>

#include <bitmap/raw-bitmap.h>

#ifdef __Fuchsia__
#include <lib/fzl/owned-vmo-mapper.h>
#endif

#include <minfs/block-txn.h>
#include <minfs/superblock.h>

#include <utility>

namespace minfs {

#ifdef __Fuchsia__

SuperblockManager::SuperblockManager(const Superblock* info, fzl::OwnedVmoMapper mapper) :
    mapping_(std::move(mapper)) {}

#else

SuperblockManager::SuperblockManager(const Superblock* info) {
    memcpy(&info_blk_[0], info, sizeof(Superblock));
}

#endif

SuperblockManager::~SuperblockManager() = default;

zx_status_t SuperblockManager::Create(Bcache* bc, const Superblock* info,
                                      fbl::unique_ptr<SuperblockManager>* out,
                                      IntegrityCheck checks) {
    zx_status_t status = ZX_OK;
    if (checks == IntegrityCheck::kAll) {
        status = CheckSuperblock(info, bc);
        if (status != ZX_OK) {
            FS_TRACE_ERROR("SuperblockManager::Create failed to check info: %d\n", status);
            return status;
        }
    }

#ifdef __Fuchsia__
    fzl::OwnedVmoMapper mapper;
    // Create the info vmo
    if ((status = mapper.CreateAndMap(kMinfsBlockSize, "minfs-superblock")) != ZX_OK) {
        return status;
    }

    fuchsia_hardware_block_VmoID info_vmoid;
    if ((status = bc->AttachVmo(mapper.vmo(), &info_vmoid)) != ZX_OK) {
        return status;
    }
    memcpy(mapper.start(), info, sizeof(Superblock));

    auto sb = fbl::unique_ptr<SuperblockManager>(new SuperblockManager(info, std::move(mapper)));
#else
    auto sb = fbl::unique_ptr<SuperblockManager>(new SuperblockManager(info));
#endif
    *out = std::move(sb);
    return ZX_OK;
}

void SuperblockManager::Write(WriteTxn* txn) {
#ifdef __Fuchsia__
    auto data = mapping_.vmo().get();
#else
    auto data = &info_blk_[0];
#endif
    txn->Enqueue(data, 0, 0, 1);
}

} // namespace minfs
