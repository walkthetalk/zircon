// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include <new>

#include <fbl/algorithm.h>
#include <fbl/unique_fd.h>
#include <fuchsia/device/c/fidl.h>
#include <fuchsia/nand/c/fidl.h>
#include <lib/fdio/watcher.h>
#include <lib/fzl/fdio.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/vmo.h>
#include <zircon/syscalls.h>
#include <zxtest/zxtest.h>

#include "parent.h"

namespace {

constexpr uint32_t kMinOobSize = 4;
constexpr uint32_t kMinBlockSize = 4;
constexpr uint32_t kMinNumBlocks = 5;
constexpr uint32_t kInMemoryPages = 20;

fbl::unique_fd OpenBroker(const char* path) {
    fbl::unique_fd broker;

    auto callback = [](int dir_fd, int event, const char* filename, void* cookie) {
        if (event != WATCH_EVENT_ADD_FILE || strcmp(filename, "broker") != 0) {
            return ZX_OK;
        }
        fbl::unique_fd* broker = reinterpret_cast<fbl::unique_fd*>(cookie);
        broker->reset(openat(dir_fd, filename, O_RDWR));
        return ZX_ERR_STOP;
    };

    fbl::unique_fd dir(open(path, O_DIRECTORY));
    if (dir) {
        zx_time_t deadline = zx_deadline_after(ZX_SEC(5));
        fdio_watch_directory(dir.get(), callback, deadline, &broker);
    }
    return broker;
}

// The device under test.
class NandDevice {
public:
    NandDevice();
    ~NandDevice() {
        if (linked_) {
            zx_status_t call_status;
            fuchsia_device_ControllerUnbind(channel(), &call_status);
        }
    }

    bool IsValid() const { return is_valid_; }

    // Provides a channel to issue fidl calls.
    zx_handle_t channel() { return caller_.borrow_channel(); }

    // Wrappers for "queue" operations that take care of preserving the vmo's handle
    // and translating the request to the desired block range on the actual device.
    zx_status_t Read(const zx::vmo& vmo, const fuchsia_nand_BrokerRequest& request);
    zx_status_t Write(const zx::vmo& vmo, const fuchsia_nand_BrokerRequest& request);
    zx_status_t Erase(const fuchsia_nand_BrokerRequest& request);

    // Erases a given block number.
    zx_status_t EraseBlock(uint32_t block_num);

    // Verifies that the buffer pointed to by the operation's vmo contains the given
    // pattern for the desired number of pages, skipping the pages before start.
    bool CheckPattern(uint8_t expected, int start, int num_pages, const void* memory);

    const fuchsia_hardware_nand_Info& Info() const { return parent_->Info(); }

    uint32_t PageSize() const { return parent_->Info().page_size; }
    uint32_t OobSize() const { return parent_->Info().oob_size; }
    uint32_t BlockSize() const { return parent_->Info().pages_per_block; }
    uint32_t NumBlocks() const { return num_blocks_; }
    uint32_t NumPages() const { return num_blocks_ * BlockSize(); }
    uint32_t MaxBufferSize() const { return kInMemoryPages * (PageSize() + OobSize()); }

    // True when the whole device under test can be modified.
    bool IsFullDevice() const { return full_device_; }

private:
    bool ValidateNandDevice();

    ParentDevice* parent_ = g_parent_device_;
    fzl::FdioCaller caller_;
    uint32_t num_blocks_ = 0;
    uint32_t first_block_ = 0;
    bool full_device_ = true;
    bool linked_ = false;
    bool is_valid_ = false;
};

NandDevice::NandDevice() {
    ZX_ASSERT(parent_->IsValid());
    if (parent_->IsBroker()) {
        caller_.reset(fbl::unique_fd(open(parent_->Path(), O_RDWR)));
    } else {
        fdio_t* io = fdio_unsafe_fd_to_io(parent_->get());
        if (io == nullptr) {
            return;
        }
        zx_status_t call_status;
        const char kBroker[] = "/boot/driver/nand-broker.so";
        zx_status_t status = fuchsia_device_ControllerBind(fdio_unsafe_borrow_channel(io), kBroker,
                                                           strlen(kBroker), &call_status);
        fdio_unsafe_release(io);
        if (status == ZX_OK) {
            status = call_status;
        }
        if (status != ZX_OK) {
            printf("Failed to bind broker\n");
            return;
        }
        linked_ = true;
        caller_.reset(OpenBroker(parent_->Path()));
    }
    is_valid_ = ValidateNandDevice();
}

zx_status_t NandDevice::Read(const zx::vmo& vmo, const fuchsia_nand_BrokerRequest& request) {
    fuchsia_nand_BrokerRequest request_copy = request;
    if (!full_device_) {
        request_copy.offset_nand = request.offset_nand + first_block_ * BlockSize();
        ZX_DEBUG_ASSERT(request.offset_nand < NumPages());
        ZX_DEBUG_ASSERT(request.offset_nand + request.length <= NumPages());
    }

    uint32_t bit_flips;
    zx::vmo dup;
    zx_status_t status = vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup);
    if (status != ZX_OK) {
        return status;
    }
    request_copy.vmo = dup.release();
    zx_status_t io_status = fuchsia_nand_BrokerRead(channel(), &request_copy, &status, &bit_flips);
    return (io_status != ZX_OK) ? io_status : status;
}

zx_status_t NandDevice::Write(const zx::vmo& vmo, const fuchsia_nand_BrokerRequest& request) {
    fuchsia_nand_BrokerRequest request_copy = request;
    if (!full_device_) {
        request_copy.offset_nand = request.offset_nand + first_block_ * BlockSize();
        ZX_DEBUG_ASSERT(request.offset_nand < NumPages());
        ZX_DEBUG_ASSERT(request.offset_nand + request.length <= NumPages());
    }

    zx::vmo dup;
    zx_status_t status = vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup);
    if (status != ZX_OK) {
        return status;
    }
    request_copy.vmo = dup.release();
    zx_status_t io_status = fuchsia_nand_BrokerWrite(channel(), &request_copy, &status);
    return (io_status != ZX_OK) ? io_status : status;
}

zx_status_t NandDevice::Erase(const fuchsia_nand_BrokerRequest& request) {
    fuchsia_nand_BrokerRequest request_copy = request;
    if (!full_device_) {
        request_copy.offset_nand = request.offset_nand + first_block_;
        ZX_DEBUG_ASSERT(request.offset_nand < NumBlocks());
        ZX_DEBUG_ASSERT(request.offset_nand + request.length <= NumBlocks());
    }

    zx_status_t status;
    zx_status_t io_status = fuchsia_nand_BrokerErase(channel(), &request_copy, &status);
    return (io_status != ZX_OK) ? io_status : status;
}

zx_status_t NandDevice::EraseBlock(uint32_t block_num) {
    fuchsia_nand_BrokerRequest request = {};
    request.length = 1;
    request.offset_nand = block_num;
    return Erase(request);
}

bool NandDevice::CheckPattern(uint8_t expected, int start, int num_pages, const void* memory) {
    const uint8_t* buffer = reinterpret_cast<const uint8_t*>(memory) + PageSize() * start;
    for (uint32_t i = 0; i < PageSize() * num_pages; i++) {
        if (buffer[i] != expected) {
            return false;
        }
    }
    return true;
}

bool NandDevice::ValidateNandDevice() {
    if (parent_->IsExternal()) {
        // This looks like using code under test to setup the test, but this
        // path is for external devices, not really the broker. The issue is that
        // ParentDevice cannot query a nand device for the actual parameters.
        fuchsia_hardware_nand_Info info;
        zx_status_t status;
        if (fuchsia_nand_BrokerGetInfo(channel(), &status, &info) != ZX_OK || status != ZX_OK) {
            printf("Failed to query nand device\n");
            return false;
        }
        parent_->SetInfo(info);
    }

    num_blocks_ = parent_->NumBlocks();
    first_block_ = parent_->FirstBlock();
    if (OobSize() < kMinOobSize || BlockSize() < kMinBlockSize || num_blocks_ < kMinNumBlocks ||
        num_blocks_ + first_block_ > parent_->Info().num_blocks) {
        printf("Invalid nand device parameters\n");
        return false;
    }
    if (num_blocks_ != parent_->Info().num_blocks) {
        // Not using the whole device, don't need to test all limits.
        num_blocks_ = fbl::min(num_blocks_, kMinNumBlocks);
        full_device_ = false;
    }
    return true;
}

TEST(NandBrokerTest, TrivialLifetime) {
    NandDevice device;
    ASSERT_TRUE(device.IsValid());
}

TEST(NandBrokerTest, Query) {
    NandDevice device;
    ASSERT_TRUE(device.IsValid());

    fuchsia_hardware_nand_Info info;
    zx_status_t status;
    ASSERT_OK(fuchsia_nand_BrokerGetInfo(device.channel(), &status, &info));
    ASSERT_OK(status);

    EXPECT_EQ(device.Info().page_size, info.page_size);
    EXPECT_EQ(device.Info().oob_size, info.oob_size);
    EXPECT_EQ(device.Info().pages_per_block, info.pages_per_block);
    EXPECT_EQ(device.Info().num_blocks, info.num_blocks);
    EXPECT_EQ(device.Info().ecc_bits, info.ecc_bits);
    EXPECT_EQ(device.Info().nand_class, info.nand_class);
}

TEST(NandBrokerTest, ReadWriteLimits) {
    NandDevice device;
    ASSERT_TRUE(device.IsValid());

    fzl::VmoMapper mapper;
    zx::vmo vmo;
    ASSERT_OK(mapper.CreateAndMap(device.MaxBufferSize(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                  nullptr, &vmo));

    fuchsia_nand_BrokerRequest request = {};
    EXPECT_EQ(ZX_ERR_OUT_OF_RANGE, device.Read(vmo, request));
    EXPECT_EQ(ZX_ERR_OUT_OF_RANGE, device.Write(vmo, request));

    if (device.IsFullDevice()) {
        request.length = 1;
        request.offset_nand = device.NumPages();

        EXPECT_EQ(ZX_ERR_OUT_OF_RANGE, device.Read(vmo, request));
        EXPECT_EQ(ZX_ERR_OUT_OF_RANGE, device.Write(vmo, request));

        request.length = 2;
        request.offset_nand = device.NumPages() - 1;

        EXPECT_EQ(ZX_ERR_OUT_OF_RANGE, device.Read(vmo, request));
        EXPECT_EQ(ZX_ERR_OUT_OF_RANGE, device.Write(vmo, request));
    }

    request.length = 1;
    request.offset_nand = device.NumPages() - 1;

    EXPECT_EQ(ZX_ERR_BAD_HANDLE, device.Read(vmo, request));
    EXPECT_EQ(ZX_ERR_BAD_HANDLE, device.Write(vmo, request));

    request.data_vmo = true;

    EXPECT_OK(device.Read(vmo, request));
    EXPECT_OK(device.Write(vmo, request));
}

TEST(NandBrokerTest, EraseLimits) {
    NandDevice device;
    ASSERT_TRUE(device.IsValid());

    fuchsia_nand_BrokerRequest request = {};
    EXPECT_EQ(ZX_ERR_OUT_OF_RANGE, device.Erase(request));

    request.offset_nand = device.NumBlocks();

    if (device.IsFullDevice()) {
        request.length = 1;
        EXPECT_EQ(ZX_ERR_OUT_OF_RANGE, device.Erase(request));

        request.length = 2;
        request.offset_nand = device.NumBlocks() - 1;
        EXPECT_EQ(ZX_ERR_OUT_OF_RANGE, device.Erase(request));
    }

    request.length = 1;
    request.offset_nand = device.NumBlocks() - 1;
    EXPECT_OK(device.Erase(request));
}

TEST(NandBrokerTest, ReadWrite) {
    NandDevice device;
    ASSERT_TRUE(device.IsValid());
    ASSERT_OK(device.EraseBlock(0));

    fzl::VmoMapper mapper;
    zx::vmo vmo;
    ASSERT_OK(mapper.CreateAndMap(device.MaxBufferSize(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                  nullptr, &vmo));
    memset(mapper.start(), 0x55, mapper.size());

    fuchsia_nand_BrokerRequest request = {};
    request.length = 4;
    request.offset_nand = 4;
    request.data_vmo = true;

    ASSERT_OK(device.Write(vmo, request));

    memset(mapper.start(), 0, mapper.size());

    ASSERT_OK(device.Read(vmo, request));
    ASSERT_TRUE(device.CheckPattern(0x55, 0, 4, mapper.start()));
}

TEST(NandBrokerTest, ReadWriteOob) {
    NandDevice device;
    ASSERT_TRUE(device.IsValid());
    ASSERT_OK(device.EraseBlock(0));

    fzl::VmoMapper mapper;
    zx::vmo vmo;
    ASSERT_OK(mapper.CreateAndMap(device.MaxBufferSize(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                  nullptr, &vmo));
    const char desired[] = {'a', 'b', 'c', 'd'};
    memcpy(mapper.start(), desired, sizeof(desired));

    fuchsia_nand_BrokerRequest request = {};
    request.length = 1;
    request.offset_nand = 2;
    request.oob_vmo = true;

    ASSERT_OK(device.Write(vmo, request));

    request.length = 2;
    request.offset_nand = 1;
    memset(mapper.start(), 0, device.OobSize() * 2);

    ASSERT_OK(device.Read(vmo, request));

    // The "second page" has the data of interest.
    ASSERT_EQ(0,
              memcmp(reinterpret_cast<char*>(mapper.start()) + device.OobSize(), desired,
                     sizeof(desired)));
}

TEST(NandBrokerTest, ReadWriteDataAndOob) {
    NandDevice device;
    ASSERT_TRUE(device.IsValid());
    ASSERT_OK(device.EraseBlock(0));

    fzl::VmoMapper mapper;
    zx::vmo vmo;
    ASSERT_OK(mapper.CreateAndMap(device.MaxBufferSize(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                  nullptr, &vmo));

    char* buffer = reinterpret_cast<char*>(mapper.start());
    memset(buffer, 0x55, device.PageSize() * 2);
    memset(buffer + device.PageSize() * 2, 0xaa, device.OobSize() * 2);

    fuchsia_nand_BrokerRequest request = {};
    request.length = 2;
    request.offset_nand = 2;
    request.offset_oob_vmo = 2; // OOB is right after data.
    request.data_vmo = true;
    request.oob_vmo = true;

    ASSERT_OK(device.Write(vmo, request));

    memset(buffer, 0, device.PageSize() * 4);
    ASSERT_OK(device.Read(vmo, request));

    // Verify data.
    ASSERT_TRUE(device.CheckPattern(0x55, 0, 2, buffer));

    // Verify OOB.
    memset(buffer, 0xaa, device.PageSize());
    ASSERT_EQ(0, memcmp(buffer + device.PageSize() * 2, buffer, device.OobSize() * 2));
}

TEST(NandBrokerTest, Erase) {
    NandDevice device;
    ASSERT_TRUE(device.IsValid());

    fzl::VmoMapper mapper;
    zx::vmo vmo;
    ASSERT_OK(mapper.CreateAndMap(device.MaxBufferSize(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                  nullptr, &vmo));

    memset(mapper.start(), 0x55, mapper.size());

    fuchsia_nand_BrokerRequest request = {};
    request.length = kMinBlockSize;
    request.data_vmo = true;
    request.offset_nand = device.BlockSize();
    ASSERT_OK(device.Write(vmo, request));

    request.offset_nand = device.BlockSize() * 2;
    ASSERT_OK(device.Write(vmo, request));

    ASSERT_OK(device.EraseBlock(1));
    ASSERT_OK(device.EraseBlock(2));

    ASSERT_OK(device.Read(vmo, request));
    ASSERT_TRUE(device.CheckPattern(0xff, 0, kMinBlockSize, mapper.start()));

    request.offset_nand = device.BlockSize();
    ASSERT_OK(device.Read(vmo, request));
    ASSERT_TRUE(device.CheckPattern(0xff, 0, kMinBlockSize, mapper.start()));
}

} // namespace
