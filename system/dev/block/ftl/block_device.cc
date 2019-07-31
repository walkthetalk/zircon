// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "block_device.h"

#include <ddk/debug.h>
#include <fbl/auto_lock.h>
#include <fuchsia/hardware/block/c/fidl.h>
#include <lib/fzl/vmo-mapper.h>
#include <zircon/assert.h>

#include "nand_driver.h"

namespace {

constexpr char kDeviceName[] = "ftl";

zx_status_t Format(void* ctx, fidl_txn_t* txn)  {
    ftl::BlockDevice* device = reinterpret_cast<ftl::BlockDevice*>(ctx);
    zx_status_t status = device->Format();
    return fuchsia_hardware_block_FtlFormat_reply(txn, status);
}

fuchsia_hardware_block_Ftl_ops_t fidl_ops = {
    .Format = Format
};

// Encapsulates a block operation that is created by this device (so that it
// goes through the worker thread).
class LocalOperation {
  public:
    explicit LocalOperation(uint32_t command) {
        operation_.op.command = command;
    }

    block_op_t* op() { return &operation_.op; }

    // Waits for the completion of the operation. Returns the operation status.
    zx_status_t Execute(ftl::BlockDevice* parent) {
        parent->BlockImplQueue(&operation_.op, OnCompletion, this);
        zx_status_t status = sync_completion_wait(&event_, ZX_SEC(60));
        sync_completion_reset(&event_);
        if (status != ZX_OK) {
            return status;
        }
        return status_;
    }

  private:
    static void OnCompletion(void* cookie, zx_status_t status, block_op_t* op) {
        LocalOperation* operation = reinterpret_cast<LocalOperation*>(cookie);
        ZX_DEBUG_ASSERT(operation);
        operation->status_ = status;
        sync_completion_signal(&operation->event_);
    }

    sync_completion_t event_;
    zx_status_t status_ = ZX_ERR_BAD_STATE;
    ftl::FtlOp operation_ = {};
};

}  // namespace

namespace ftl {

BlockDevice::~BlockDevice() {
    if (thread_created_) {
        Kill();
        sync_completion_signal(&wake_signal_);
        int result_code;
        thrd_join(worker_, &result_code);

        for (;;) {
            FtlOp* nand_op = list_remove_head_type(&txn_list_, FtlOp, node);
            if (!nand_op) {
                break;
            }
            nand_op->completion_cb(nand_op->cookie, ZX_ERR_BAD_STATE, &nand_op->op);
        }
    }

    bool volume_created = (DdkGetSize() != 0);
    if (volume_created) {
        if (volume_->Unmount() != ZX_OK) {
            zxlogf(ERROR, "FTL: FtlUmmount() failed");
        }
    }
}

zx_status_t BlockDevice::Bind() {
    zxlogf(INFO, "FTL: parent: '%s'\n", device_get_name(parent()));

    if (device_get_protocol(parent(), ZX_PROTOCOL_NAND, &parent_) != ZX_OK) {
        zxlogf(ERROR, "FTL: device '%s' does not support nand protocol\n",
               device_get_name(parent()));
        return ZX_ERR_NOT_SUPPORTED;
    }

    // Get the optional bad block protocol.
    if (device_get_protocol(parent(), ZX_PROTOCOL_BAD_BLOCK, &bad_block_) != ZX_OK) {
        zxlogf(WARN, "FTL: Parent device '%s': does not support bad_block protocol\n",
               device_get_name(parent()));
    }

    zx_status_t status = Init();
    if (status != ZX_OK) {
        return status;
    }
    return DdkAdd(kDeviceName);
}

void BlockDevice::DdkUnbind() {
    Kill();
    sync_completion_signal(&wake_signal_);
    DdkRemove();
}

zx_status_t BlockDevice::Init() {
    ZX_DEBUG_ASSERT(!thread_created_);
    list_initialize(&txn_list_);
    if (thrd_create(&worker_, WorkerThreadStub, this) != thrd_success) {
        return ZX_ERR_NO_RESOURCES;
    }
    thread_created_ = true;

    if (!InitFtl()) {
        return ZX_ERR_NO_RESOURCES;
    }

    return ZX_OK;
}

zx_status_t BlockDevice::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
    return fuchsia_hardware_block_Ftl_dispatch(this, txn, msg, &fidl_ops);
}

zx_status_t BlockDevice::DdkSuspend(uint32_t flags) {
    zxlogf(INFO, "FTL: Suspend\n");
    LocalOperation operation(BLOCK_OP_FLUSH);
    return operation.Execute(this);
}

zx_status_t BlockDevice::DdkGetProtocol(uint32_t proto_id, void* out_protocol) {
    auto* proto = static_cast<ddk::AnyProtocol*>(out_protocol);
    proto->ctx = this;
    switch (proto_id) {
    case ZX_PROTOCOL_BLOCK_IMPL:
        proto->ops = &block_impl_protocol_ops_;
        return ZX_OK;
    case ZX_PROTOCOL_BLOCK_PARTITION:
        proto->ops = &block_partition_protocol_ops_;
        return ZX_OK;
    default:
        return ZX_ERR_NOT_SUPPORTED;
    }
}

void BlockDevice::BlockImplQuery(block_info_t* info_out, size_t* block_op_size_out) {
    zxlogf(TRACE, "FTL: Query\n");
    memset(info_out, 0, sizeof(*info_out));
    info_out->block_count = params_.num_pages;
    info_out->block_size = params_.page_size;
    info_out->max_transfer_size = BLOCK_MAX_TRANSFER_UNBOUNDED;
    *block_op_size_out = sizeof(FtlOp);
}

void BlockDevice::BlockImplQueue(block_op_t* operation, block_impl_queue_callback completion_cb,
                                 void* cookie) {
  zxlogf(TRACE, "FTL: Queue\n");
  uint32_t max_pages = params_.num_pages;
  switch (operation->command) {
    case BLOCK_OP_WRITE:
    case BLOCK_OP_READ: {
        if (operation->rw.offset_dev >= max_pages || !operation->rw.length ||
            (max_pages - operation->rw.offset_dev) < operation->rw.length) {
            completion_cb(cookie, ZX_ERR_OUT_OF_RANGE, operation);
            return;
        }
        break;
    }
    case BLOCK_OP_TRIM:
        if (operation->trim.offset_dev >= max_pages || !operation->trim.length ||
            (max_pages - operation->trim.offset_dev) < operation->trim.length) {
            completion_cb(cookie, ZX_ERR_OUT_OF_RANGE, operation);
            return;
        }
        break;

    case BLOCK_OP_FLUSH:
        break;

    default:
        completion_cb(cookie, ZX_ERR_NOT_SUPPORTED, operation);
        return;
    }

    FtlOp* block_op = reinterpret_cast<FtlOp*>(operation);
    block_op->completion_cb = completion_cb;
    block_op->cookie = cookie;
    if (AddToList(block_op)) {
        sync_completion_signal(&wake_signal_);
    } else {
        completion_cb(cookie, ZX_ERR_BAD_STATE, operation);
    }
}

zx_status_t BlockDevice::BlockPartitionGetGuid(guidtype_t guid_type, guid_t* out_guid) {
    if (guid_type != GUIDTYPE_TYPE) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    memcpy(out_guid, guid_, ZBI_PARTITION_GUID_LEN);
    return ZX_OK;
}

zx_status_t BlockDevice::BlockPartitionGetName(char* out_name, size_t capacity) {
    if (capacity < sizeof(kDeviceName)) {
        return ZX_ERR_BUFFER_TOO_SMALL;
    }
    strncpy(out_name, kDeviceName, capacity);
    return ZX_OK;
}

bool BlockDevice::OnVolumeAdded(uint32_t page_size, uint32_t num_pages) {
    params_ = {page_size, num_pages};
    zxlogf(INFO, "FTL: %d pages of %d bytes\n", num_pages, page_size);
    return true;
}

zx_status_t BlockDevice::Format() {
    zx_status_t status = volume_->Format();
    if (status != ZX_OK) {
        zxlogf(ERROR, "FTL: format failed\n");
    }
    return status;
}

bool BlockDevice::InitFtl() {
    std::unique_ptr<NandDriver> driver = NandDriver::Create(&parent_, &bad_block_);
    const char* error = driver->Init();
    if (error) {
        zxlogf(ERROR, "FTL: %s\n", error);
        return false;
    }
    memcpy(guid_, driver->info().partition_guid, ZBI_PARTITION_GUID_LEN);

    if (!volume_) {
        volume_ = std::make_unique<ftl::VolumeImpl>(this);
    }

    error = volume_->Init(std::move(driver));
    if (error) {
        zxlogf(ERROR, "FTL: %s\n", error);
        return false;
    }

    Volume::Stats stats;
    if (volume_->GetStats(&stats) == ZX_OK) {
        zxlogf(INFO, "FTL: Wear count: %u, Garbage level: %d%%\n", stats.wear_count,
               stats.garbage_level);
    }

    zxlogf(INFO, "FTL: InitFtl ok\n");
    return true;
}

void BlockDevice::Kill() {
    fbl::AutoLock lock(&lock_);
    dead_ = true;
}

bool BlockDevice::AddToList(FtlOp* operation) {
    fbl::AutoLock lock(&lock_);
    if (!dead_) {
        list_add_tail(&txn_list_, &operation->node);
    }
    return !dead_;
}

bool BlockDevice::RemoveFromList(FtlOp** operation) {
    fbl::AutoLock lock(&lock_);
    if (!dead_) {
        *operation = list_remove_head_type(&txn_list_, FtlOp, node);
    }
    return !dead_;
}

int BlockDevice::WorkerThread() {
    for (;;) {
        FtlOp* operation;
        for (;;) {
            if (!RemoveFromList(&operation)) {
                return 0;
            }
            if (operation) {
                sync_completion_reset(&wake_signal_);
                break;
            } else {
                // Flush any pending data after 15 seconds of inactivity. This is
                // meant to reduce the chances of data loss if power is removed.
                // This value is only a guess.
                zx_duration_t timeout = pending_flush_ ? ZX_SEC(15) : ZX_TIME_INFINITE;
                zx_status_t status = sync_completion_wait(&wake_signal_, timeout);
                if (status == ZX_ERR_TIMED_OUT) {
                    Flush();
                    pending_flush_ = false;
                }
            }
        }

        zx_status_t status = ZX_OK;

        switch (operation->op.command) {
        case BLOCK_OP_WRITE:
        case BLOCK_OP_READ:
            pending_flush_ = true;
            status = ReadWriteData(&operation->op);
            break;

        case BLOCK_OP_TRIM:
            pending_flush_ = true;
            status = TrimData(&operation->op);
            break;

        case BLOCK_OP_FLUSH: {
            status = Flush();
            pending_flush_ = false;
            break;
        }
        default:
            ZX_DEBUG_ASSERT(false);  // Unexpected.
        }

        operation->completion_cb(operation->cookie, status, &operation->op);
    }
}

int BlockDevice::WorkerThreadStub(void* arg) {
    BlockDevice* device = reinterpret_cast<BlockDevice*>(arg);
    return device->WorkerThread();
}

zx_status_t BlockDevice::ReadWriteData(block_op_t* operation) {
    uint64_t addr = operation->rw.offset_vmo * params_.page_size;
    uint32_t length = operation->rw.length * params_.page_size;
    uint32_t offset = static_cast<uint32_t>(operation->rw.offset_dev);
    if (offset != operation->rw.offset_dev) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    // TODO(ZX-2541): We may go back to ask the kernel to copy the data for us
    // if that ends up being more efficient.
    fzl::VmoMapper mapper;
    zx_status_t status = mapper.Map(*zx::unowned_vmo(operation->rw.vmo), addr, length,
                                    ZX_VM_FLAG_PERM_READ | ZX_VM_FLAG_PERM_WRITE);
    if (status != ZX_OK) {
        return status;
    }

    if (operation->command == BLOCK_OP_WRITE) {
        zxlogf(SPEW, "FTL: BLK To write %d blocks at %d :\n", operation->rw.length, offset);
        status = volume_->Write(offset, operation->rw.length, mapper.start());
        if (status != ZX_OK) {
            zxlogf(ERROR, "FTL: Failed to write to ftl\n");
            return status;
        }
    }

    if (operation->command == BLOCK_OP_READ) {
        zxlogf(SPEW, "FTL: BLK To read %d blocks at %d :\n", operation->rw.length, offset);
        status = volume_->Read(offset, operation->rw.length, mapper.start());
        if (status != ZX_OK) {
            zxlogf(ERROR, "FTL: Failed to read from ftl\n");
            return status;
        }
    }

    return ZX_OK;
}

zx_status_t BlockDevice::TrimData(block_op_t* operation) {
    uint32_t offset = static_cast<uint32_t>(operation->trim.offset_dev);
    if (offset != operation->trim.offset_dev) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    ZX_DEBUG_ASSERT(operation->command == BLOCK_OP_TRIM);
    zxlogf(SPEW, "FTL: BLK To trim %d blocks at %d :\n", operation->trim.length, offset);
    zx_status_t status = volume_->Trim(offset, operation->trim.length);
    if (status != ZX_OK) {
        zxlogf(ERROR, "FTL: Failed to trim\n");
        return status;
    }

    return ZX_OK;
}

zx_status_t BlockDevice::Flush() {
    zx_status_t status = volume_->Flush();
    if (status != ZX_OK) {
        zxlogf(ERROR, "FTL: flush failed\n");
        return status;
    }

    zxlogf(SPEW, "FTL: Finished flush\n");
    return status;
}

}  // namespace ftl.
