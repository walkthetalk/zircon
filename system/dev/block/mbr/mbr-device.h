// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cstddef>
#include <inttypes.h>
#include <memory>
#include <string.h>

#include <ddktl/device.h>
#include <ddktl/protocol/block.h>
#include <ddktl/protocol/block/partition.h>
#include <fbl/macros.h>
#include <fbl/string.h>
#include <fbl/vector.h>
#include <gpt/c/gpt.h>
#include <lib/sync/completion.h>
#include <zircon/status.h>

#include "mbr.h"

namespace mbr {

class MbrDevice;
using DeviceType = ddk::Device<MbrDevice, ddk::GetProtocolable, ddk::GetSizable, ddk::Unbindable>;

class MbrDevice final : public DeviceType,
                        public ddk::BlockImplProtocol<MbrDevice, ddk::base_protocol>,
                        public ddk::BlockPartitionProtocol<MbrDevice> {
public:
    explicit MbrDevice(zx_device_t* parent, const char* name, MbrPartitionEntry entry,
                       block_info_t info, size_t block_op_size)
        : DeviceType(parent), name_(name), parent_protocol_(parent), partition_(entry), info_(info),
          block_op_size_(block_op_size), bind_completed_() {
        ZX_ASSERT(info.block_count == entry.num_sectors);
    }

    DISALLOW_COPY_ASSIGN_AND_MOVE(MbrDevice);

    const fbl::String& Name() const { return name_; }

    // Reads the header information out of |parent| (which is expected to be a
    // device implementing ZX_PROTOCOL_BLOCK) and creates instances of MbrDevice
    // to manage the partitions in the MBR, returning these instances in
    // |devices_out|.
    // Does not bind the partition drivers.
    static zx_status_t Create(zx_device_t* parent,
                              fbl::Vector<std::unique_ptr<MbrDevice>>* devices_out);
    // Binds |device|.
    // If the bind succeeds, ownership of |device| is transferred to the DDK;
    // |device| is deallocated otherwise.
    static zx_status_t Bind(std::unique_ptr<MbrDevice> device);

    sync_completion_t* bind_completion() { return &bind_completed_; }

    // DDK mixin implementation.
    zx_status_t DdkGetProtocol(uint32_t proto_id, void* out);
    zx_off_t DdkGetSize();
    void DdkUnbind();
    void DdkRelease();

    // Block protocol implementation.
    void BlockImplQuery(block_info_t* info_out, size_t* block_op_size_out);
    void BlockImplQueue(block_op_t* operation, block_impl_queue_callback completion_cb,
                        void* cookie);

    // Partition protocol implementation.
    zx_status_t BlockPartitionGetGuid(guidtype_t guid_type, guid_t* out_guid);
    zx_status_t BlockPartitionGetName(char* out_name, size_t capacity);

    static bool SupportsPartitionType(uint8_t type);

private:
    const fbl::String name_;

    // The block protocol of the device we are binding against.
    ddk::BlockProtocolClient parent_protocol_;

    MbrPartitionEntry partition_;

    block_info_t info_;
    size_t block_op_size_;

    sync_completion_t bind_completed_;
};

} // namespace mbr
