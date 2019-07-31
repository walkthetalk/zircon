// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <map>

#include <block-client/cpp/block-device.h>
#include <fbl/mutex.h>
#include <lib/zx/vmo.h>
#include <range/range.h>
#include <zircon/assert.h>

namespace block_client {

// A fake device implementing (most of) the BlockDevice interface on top of an
// in-memory VMO representing the device. This allows clients of the BlockDevice
// interface to test against this fake in-process, instead of relying on a real
// block device.
//
// This class is thread-safe.
// This class is not movable or copyable.
class FakeBlockDevice : public BlockDevice {
public:
    FakeBlockDevice(uint64_t block_count, uint32_t block_size);
    FakeBlockDevice(const FakeBlockDevice&) = delete;
    FakeBlockDevice& operator=(const FakeBlockDevice&) = delete;
    FakeBlockDevice(FakeBlockDevice&& other) = delete;
    FakeBlockDevice& operator=(FakeBlockDevice&& other) = delete;

    virtual ~FakeBlockDevice() = default;

    void SetInfoFlags(uint32_t flags);
    void SetBlockCount(uint64_t block_count);
    void SetBlockSize(uint32_t block_size);
    bool IsRegistered(vmoid_t vmoid) const;

protected:
    zx_status_t GetDevicePath(size_t buffer_len, char* out_name,
                              size_t* out_len) const override {
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_status_t VolumeQuery(fuchsia_hardware_block_volume_VolumeInfo* out_info) const override {
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_status_t VolumeQuerySlices(const uint64_t* slices, size_t slices_count,
                                  fuchsia_hardware_block_volume_VsliceRange* out_ranges,
                                  size_t* out_ranges_count) const override {
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_status_t VolumeExtend(uint64_t offset, uint64_t length) override {
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_status_t VolumeShrink(uint64_t offset, uint64_t length) override {
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_status_t FifoTransaction(block_fifo_request_t* requests, size_t count) override;
    zx_status_t ReadBlock(uint64_t block_num, uint64_t fs_block_size, void* block) const final;
    zx_status_t BlockGetInfo(fuchsia_hardware_block_BlockInfo* out_info) const final;
    zx_status_t BlockAttachVmo(zx::vmo vmo, fuchsia_hardware_block_VmoID* out_vmoid) final;

    // Resizes the block device to be at least |new_size| bytes.
    void ResizeDeviceToAtLeast(uint64_t new_size);

private:
    bool IsRegisteredLocked(vmoid_t) const __TA_REQUIRES(lock_);
    void AdjustBlockDeviceSizeLocked(uint64_t new_size) __TA_REQUIRES(lock_);

    mutable fbl::Mutex lock_ = {};
    uint64_t block_count_ __TA_GUARDED(lock_) = 0;
    uint32_t block_size_ __TA_GUARDED(lock_) = 0;
    uint32_t block_info_flags_ __TA_GUARDED(lock_) = 0;
    vmoid_t next_vmoid_ __TA_GUARDED(lock_) = 1;
    std::map<vmoid_t, zx::vmo> vmos_ __TA_GUARDED(lock_);
    zx::vmo block_device_ __TA_GUARDED(lock_);
};

// An extension of FakeBlockDevice that allows for testing on FVM devices.
//
// This class is thread-safe.
// This class is not movable or copyable.
class FakeFVMBlockDevice final : public FakeBlockDevice {
public:
    FakeFVMBlockDevice(uint64_t block_count, uint32_t block_size,
                       uint64_t slice_size, uint64_t slice_capacity);

    zx_status_t FifoTransaction(block_fifo_request_t* requests, size_t count) final;
    zx_status_t VolumeQuery(fuchsia_hardware_block_volume_VolumeInfo* out_info) const final;
    zx_status_t VolumeQuerySlices(const uint64_t* slices, size_t slices_count,
                                  fuchsia_hardware_block_volume_VsliceRange* out_ranges,
                                  size_t* out_ranges_count) const final;
    zx_status_t VolumeExtend(uint64_t offset, uint64_t length) final;
    zx_status_t VolumeShrink(uint64_t offset, uint64_t length) final;

private:
    mutable fbl::Mutex lock_ = {};
    const uint64_t slice_size_;
    const uint64_t vslice_count_;
    uint64_t pslice_total_count_ __TA_GUARDED(lock_) = 0 ;
    uint64_t pslice_allocated_count_ __TA_GUARDED(lock_) = 0;

    // Start Slice -> Range.
    std::map<uint64_t, range::Range<uint64_t>> extents_ __TA_GUARDED(lock_);
};

} // namespace block_client
