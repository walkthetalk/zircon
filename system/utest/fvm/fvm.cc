// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <climits>
#include <errno.h>
#include <fcntl.h>
#include <limits>
#include <new>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <utime.h>

#include <blobfs/format.h>
#include <block-client/client.h>
#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>
#include <fbl/function.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <fbl/unique_fd.h>
#include <fbl/unique_ptr.h>
#include <fbl/vector.h>
#include <fs-management/fvm.h>
#include <fs-management/mount.h>
#include <fuchsia/device/c/fidl.h>
#include <fuchsia/hardware/block/c/fidl.h>
#include <fuchsia/hardware/block/partition/c/fidl.h>
#include <fuchsia/hardware/block/volume/c/fidl.h>
#include <fuchsia/io/c/fidl.h>
#include <fvm/format.h>
#include <fvm/fvm-check.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/unsafe.h>
#include <lib/fzl/fdio.h>
#include <lib/memfs/memfs.h>
#include <lib/zx/channel.h>
#include <lib/zx/fifo.h>
#include <lib/zx/vmo.h>
#include <minfs/format.h>
#include <ramdevice-client/ramdisk.h>
#include <zircon/device/block.h>
#include <zircon/device/vfs.h>
#include <zircon/syscalls.h>
#include <zircon/thread_annotations.h>

#include <unittest/unittest.h>

#define FVM_DRIVER_LIB "/boot/driver/fvm.so"
#define STRLEN(s) (sizeof(s) / sizeof((s)[0]))

namespace {

/////////////////////// Helper functions for creating FVM:

using filesystem_info_t = fuchsia_io_FilesystemInfo;
using volume_info_t = fuchsia_hardware_block_volume_VolumeInfo;

const char kTmpfsPath[] = "/fvm-tmp";
const char kMountPath[] = "/fvm-tmp/minfs_test_mountpath";

static bool use_real_disk = false;
static ramdisk_client_t* test_ramdisk = nullptr;
static char test_disk_path[PATH_MAX];
static uint64_t test_block_size;
static uint64_t test_block_count;

int StartFVMTest(uint64_t blk_size, uint64_t initial_blk_count, uint64_t max_blk_count,
                 uint64_t slice_size, char* disk_path_out, char* fvm_driver_out) {
    zx::channel fvm_channel;
    zx_status_t status, call_status;
    auto cleanup = fbl::MakeAutoCall([disk_path_out]() {
        if (!use_real_disk && disk_path_out[0]) {
            ramdisk_destroy(test_ramdisk);
        }
    });
    fbl::unique_fd fd;
    disk_path_out[0] = 0;
    if (!use_real_disk) {
        if (ramdisk_create(blk_size, initial_blk_count, &test_ramdisk)) {
            fprintf(stderr, "fvm: Could not create ramdisk\n");
            return -1;
        }
        strlcpy(disk_path_out, ramdisk_get_path(test_ramdisk), PATH_MAX);
    } else {
        strlcpy(disk_path_out, test_disk_path, PATH_MAX);
    }

    fd.reset(open(disk_path_out, O_RDWR));
    if (!fd) {
        fprintf(stderr, "fvm: Could not open ramdisk\n");
        return -1;
    }

    if (fvm_init_preallocated(fd.get(), initial_blk_count * blk_size, max_blk_count * blk_size,
                              slice_size) != ZX_OK) {
        fprintf(stderr, "fvm: Could not initialize fvm\n");
        return -1;
    }

    if (fdio_get_service_handle(fd.get(), fvm_channel.reset_and_get_address()) != ZX_OK) {
        fprintf(stderr, "fvm: Could not convert fd to channel\n");
        return -1;
    }
    status = fuchsia_device_ControllerBind(fvm_channel.get(), FVM_DRIVER_LIB,
                                           STRLEN(FVM_DRIVER_LIB), &call_status);
    if (status == ZX_OK) {
        status = call_status;
    }
    if (status != ZX_OK) {
        fprintf(stderr, "fvm: Error binding to fvm driver\n");
        return -1;
    }
    fvm_channel.reset();

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/fvm", disk_path_out);
    if (wait_for_device(path, ZX_SEC(3)) != ZX_OK) {
        fprintf(stderr, "fvm: Error waiting for fvm driver to bind\n");
        return -1;
    }

    // TODO(security): SEC-70.  This may overflow |fvm_driver_out|.
    strcpy(fvm_driver_out, path);
    cleanup.cancel();
    return 0;
}

int StartFVMTest(uint64_t blk_size, uint64_t blk_count, uint64_t slice_size, char* disk_path_out,
                 char* fvm_driver_out) {
    return StartFVMTest(blk_size, blk_count, blk_count, slice_size, disk_path_out, fvm_driver_out);
}

typedef struct {
    const char* name;
    size_t number;
} partition_entry_t;

fbl::unique_fd FVMRebind(fbl::unique_fd fvm_fd, char* disk_path, const partition_entry_t* entries,
                         size_t entry_count) {
    if (use_real_disk) {
        {
            fbl::unique_fd disk_fd(open(disk_path, O_RDWR));
            if (!disk_fd) {
                fprintf(stderr, "fvm rebind: Could not open disk\n");
                return fbl::unique_fd();
            }
            fzl::FdioCaller disk_client(std::move(disk_fd));
            zx_status_t status;
            if ((fuchsia_hardware_block_BlockRebindDevice(disk_client.borrow_channel(), &status) !=
                 ZX_OK) ||
                status != ZX_OK) {
                fprintf(stderr, "fvm rebind: Rebind hack failed\n");
                return fbl::unique_fd();
            }
        }

        // Wait for the disk to rebind to a block driver
        if (wait_for_device(disk_path, ZX_SEC(3)) != ZX_OK) {
            fprintf(stderr, "fvm rebind: Block driver did not rebind to disk\n");
            return fbl::unique_fd();
        }

        zx::channel disk_dev, disk_dev_remote;
        if (zx::channel::create(0, &disk_dev, &disk_dev_remote) != ZX_OK) {
            fprintf(stderr, "fvm rebind: Could not create channel\n");
            return fbl::unique_fd();
        }
        if (fdio_service_connect(disk_path, disk_dev_remote.release()) != ZX_OK) {
            fprintf(stderr, "fvm rebind: Could not connect to disk\n");
            return fbl::unique_fd();
        }
        zx_status_t call_status;
        zx_status_t status = fuchsia_device_ControllerBind(disk_dev.get(), FVM_DRIVER_LIB,
                                                           STRLEN(FVM_DRIVER_LIB), &call_status);
        if (status == ZX_OK) {
            status = call_status;
        }
        if (status != ZX_OK) {
            fprintf(stderr, "fvm rebind: Could not bind fvm driver\n");
            return fbl::unique_fd();
        }
    } else {
        if (ramdisk_rebind(test_ramdisk) != ZX_OK) {
            fprintf(stderr, "fvm rebind: Could not rebind ramdisk\n");
            return fbl::unique_fd();
        }
        fzl::UnownedFdioCaller disk_caller(ramdisk_get_block_fd(test_ramdisk));
        zx_status_t call_status;
        zx_status_t status = fuchsia_device_ControllerBind(
            disk_caller.borrow_channel(), FVM_DRIVER_LIB, STRLEN(FVM_DRIVER_LIB), &call_status);
        if (status == ZX_OK) {
            status = call_status;
        }
        if (status != ZX_OK) {
            fprintf(stderr, "fvm rebind: Could not bind fvm driver\n");
            return fbl::unique_fd();
        }
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/fvm", disk_path);
    if (wait_for_device(path, ZX_SEC(3)) != ZX_OK) {
        fprintf(stderr, "fvm rebind: Error waiting for fvm driver to bind\n");
        return fbl::unique_fd();
    }

    for (size_t i = 0; i < entry_count; i++) {
        snprintf(path, sizeof(path), "%s/fvm/%s-p-%zu/block", disk_path, entries[i].name,
                 entries[i].number);
        if (wait_for_device(path, ZX_SEC(3)) != ZX_OK) {
            fprintf(stderr, "  Failed to wait for %s\n", path);
            return fbl::unique_fd();
        }
    }

    snprintf(path, sizeof(path), "%s/fvm", disk_path);
    fvm_fd.reset(open(path, O_RDWR));
    if (!fvm_fd) {
        fprintf(stderr, "fvm rebind: Failed to open fvm\n");
        return fbl::unique_fd();
    }
    return fvm_fd;
}

bool FVMCheckSliceSize(const char* fvm_path, size_t expected_slice_size) {
    BEGIN_HELPER;
    fbl::unique_fd fd(open(fvm_path, O_RDWR));
    ASSERT_TRUE(fd, "Failed to open fvm driver\n");
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK, "Failed to query fvm\n");
    ASSERT_EQ(expected_slice_size, volume_info.slice_size, "Unexpected slice size\n");
    END_HELPER;
}

bool FVMCheckAllocatedCount(int fd, size_t expected_allocated, size_t expected_total) {
    BEGIN_HELPER;
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd, &volume_info), ZX_OK);
    ASSERT_EQ(volume_info.pslice_total_count, expected_total);
    ASSERT_EQ(volume_info.pslice_allocated_count, expected_allocated);
    END_HELPER;
}

enum class ValidationResult {
    Valid,
    Corrupted,
};

bool ValidateFVM(const char* device_path, ValidationResult result = ValidationResult::Valid) {
    BEGIN_HELPER;
    fbl::unique_fd fd(open(device_path, O_RDONLY));
    ASSERT_TRUE(fd);
    fzl::UnownedFdioCaller disk_caller(fd.get());
    fuchsia_hardware_block_BlockInfo block_info;
    zx_status_t status;
    ASSERT_EQ(
        fuchsia_hardware_block_BlockGetInfo(disk_caller.borrow_channel(), &status, &block_info),
        ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    fvm::Checker checker(std::move(fd), block_info.block_size, true);
    switch (result) {
    case ValidationResult::Valid:
        ASSERT_TRUE(checker.Validate());
        break;
    default:
        ASSERT_FALSE(checker.Validate());
    }
    END_HELPER;
}

// Unbind FVM driver and removes the backing ramdisk device, if one exists.
int EndFVMTest(const char* device_path) {
    if (!use_real_disk) {
        return ramdisk_destroy(test_ramdisk);
    } else {
        return fvm_destroy(device_path);
    }
}

/////////////////////// Helper functions, definitions

constexpr uint8_t kTestUniqueGUID[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                       0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
constexpr uint8_t kTestUniqueGUID2[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};

// Intentionally avoid aligning these GUIDs with
// the actual system GUIDs; otherwise, limited versions
// of Fuchsia may attempt to actually mount these
// partitions automatically.

// clang-format off
#define GUID_TEST_DATA_VALUE                                                                       \
    {                                                                                              \
        0xAA, 0xFF, 0xBB, 0x00, 0x33, 0x44, 0x88, 0x99,                                            \
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,                                            \
    }

#define GUID_TEST_BLOB_VALUE                                                                       \
    {                                                                                              \
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,                                            \
        0xAA, 0xFF, 0xBB, 0x00, 0x33, 0x44, 0x88, 0x99,                                            \
    }

#define GUID_TEST_SYS_VALUE                                                                        \
    {                                                                                              \
        0xEE, 0xFF, 0xBB, 0x00, 0x33, 0x44, 0x88, 0x99,                                            \
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,                                            \
    }
// clang-format on

constexpr char kTestPartName1[] = "data";
constexpr uint8_t kTestPartGUIDData[] = {0xAA, 0xFF, 0xBB, 0x00, 0x33, 0x44, 0x88, 0x99,
                                         0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};

constexpr char kTestPartName2[] = "blob";
constexpr uint8_t kTestPartGUIDBlob[] = GUID_TEST_BLOB_VALUE;

constexpr char kTestPartName3[] = "system";
constexpr uint8_t kTestPartGUIDSystem[] = GUID_TEST_SYS_VALUE;

class VmoBuf;

class VmoClient : public fbl::RefCounted<VmoClient> {
public:
    static bool Create(int fd, fbl::RefPtr<VmoClient>* out);

    ~VmoClient();
    bool CheckWrite(VmoBuf* vbuf, size_t buf_off, size_t dev_off, size_t len);
    bool CheckRead(VmoBuf* vbuf, size_t buf_off, size_t dev_off, size_t len);
    bool Transaction(block_fifo_request_t* requests, size_t count) {
        BEGIN_HELPER;
        ASSERT_EQ(block_fifo_txn(client_, &requests[0], count), ZX_OK);
        END_HELPER;
    }

    int fd() const { return fd_; }
    groupid_t group() { return 0; }

private:
    int fd_;
    fuchsia_hardware_block_BlockInfo info_;
    fifo_client_t* client_;
};

class VmoBuf {
public:
    static bool Create(fbl::RefPtr<VmoClient> client, size_t size, fbl::unique_ptr<VmoBuf>* out) {
        BEGIN_HELPER;

        fbl::unique_ptr<uint8_t[]> buf(new uint8_t[size]);

        zx::vmo vmo;
        ASSERT_EQ(zx::vmo::create(size, 0, &vmo), ZX_OK);
        zx::vmo xfer_vmo;
        ASSERT_EQ(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &xfer_vmo), ZX_OK);

        fzl::UnownedFdioCaller disk_connection(client->fd());
        zx::unowned_channel channel(disk_connection.borrow_channel());
        fuchsia_hardware_block_VmoID vmoid;
        zx_status_t status;
        ASSERT_EQ(fuchsia_hardware_block_BlockAttachVmo(channel->get(), xfer_vmo.release(), &status,
                                                        &vmoid),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);

        fbl::unique_ptr<VmoBuf> vb(
            new VmoBuf(std::move(client), std::move(vmo), std::move(buf), vmoid));
        *out = std::move(vb);
        END_HELPER;
    }

    ~VmoBuf() {
        if (vmo_.is_valid()) {
            block_fifo_request_t request;
            request.group = client_->group();
            request.vmoid = vmoid_.id;
            request.opcode = BLOCKIO_CLOSE_VMO;
            client_->Transaction(&request, 1);
        }
    }

private:
    friend VmoClient;

    VmoBuf(fbl::RefPtr<VmoClient> client, zx::vmo vmo, fbl::unique_ptr<uint8_t[]> buf,
           fuchsia_hardware_block_VmoID vmoid)
        : client_(std::move(client)), vmo_(std::move(vmo)), buf_(std::move(buf)), vmoid_(vmoid) {}

    fbl::RefPtr<VmoClient> client_;
    zx::vmo vmo_;
    fbl::unique_ptr<uint8_t[]> buf_;
    fuchsia_hardware_block_VmoID vmoid_;
};

bool VmoClient::Create(int fd, fbl::RefPtr<VmoClient>* out) {
    BEGIN_HELPER;
    fbl::RefPtr<VmoClient> vc = fbl::AdoptRef(new VmoClient());

    fzl::UnownedFdioCaller disk_connection(fd);
    zx::unowned_channel channel(disk_connection.borrow_channel());
    zx_status_t status;

    zx::fifo fifo;
    ASSERT_EQ(
        fuchsia_hardware_block_BlockGetFifo(channel->get(), &status, fifo.reset_and_get_address()),
        ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(channel->get(), &status, &vc->info_), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_fifo_create_client(fifo.release(), &vc->client_), ZX_OK);
    vc->fd_ = fd;
    *out = std::move(vc);
    END_HELPER;
}

VmoClient::~VmoClient() {
    fzl::UnownedFdioCaller disk_connection(fd());
    zx_status_t status;
    fuchsia_hardware_block_BlockCloseFifo(disk_connection.borrow_channel(), &status);
    block_fifo_release_client(client_);
}

bool VmoClient::CheckWrite(VmoBuf* vbuf, size_t buf_off, size_t dev_off, size_t len) {
    BEGIN_HELPER;
    // Write to the client-side buffer
    for (size_t i = 0; i < len; i++)
        vbuf->buf_[i + buf_off] = static_cast<uint8_t>(rand());

    // Write to the registered VMO
    ASSERT_EQ(vbuf->vmo_.write(&vbuf->buf_[buf_off], buf_off, len), ZX_OK);

    // Write to the block device
    block_fifo_request_t request;
    request.group = group();
    request.vmoid = vbuf->vmoid_.id;
    request.opcode = BLOCKIO_WRITE;
    ASSERT_EQ(len % info_.block_size, 0);
    ASSERT_EQ(buf_off % info_.block_size, 0);
    ASSERT_EQ(dev_off % info_.block_size, 0);
    request.length = static_cast<uint32_t>(len / info_.block_size);
    request.vmo_offset = buf_off / info_.block_size;
    request.dev_offset = dev_off / info_.block_size;
    ASSERT_TRUE(Transaction(&request, 1));
    END_HELPER;
}

bool VmoClient::CheckRead(VmoBuf* vbuf, size_t buf_off, size_t dev_off, size_t len) {
    BEGIN_HELPER;

    // Create a comparison buffer
    fbl::AllocChecker ac;
    fbl::unique_ptr<uint8_t[]> out(new (&ac) uint8_t[len]);
    ASSERT_TRUE(ac.check());
    memset(out.get(), 0, len);

    // Read from the block device
    block_fifo_request_t request;
    request.group = group();
    request.vmoid = vbuf->vmoid_.id;
    request.opcode = BLOCKIO_READ;
    ASSERT_EQ(len % info_.block_size, 0);
    ASSERT_EQ(buf_off % info_.block_size, 0);
    ASSERT_EQ(dev_off % info_.block_size, 0);
    request.length = static_cast<uint32_t>(len / info_.block_size);
    request.vmo_offset = buf_off / info_.block_size;
    request.dev_offset = dev_off / info_.block_size;
    ASSERT_TRUE(Transaction(&request, 1));

    // Read from the registered VMO
    ASSERT_EQ(vbuf->vmo_.read(out.get(), buf_off, len), ZX_OK);

    ASSERT_EQ(memcmp(&vbuf->buf_[buf_off], out.get(), len), 0);
    END_HELPER;
}

bool CheckWrite(int fd, size_t off, size_t len, uint8_t* buf) {
    BEGIN_HELPER;
    for (size_t i = 0; i < len; i++) {
        buf[i] = static_cast<uint8_t>(rand());
    }
    ASSERT_EQ(lseek(fd, off, SEEK_SET), static_cast<ssize_t>(off));
    ASSERT_EQ(write(fd, buf, len), static_cast<ssize_t>(len));
    END_HELPER;
}

bool CheckRead(int fd, size_t off, size_t len, const uint8_t* in) {
    BEGIN_HELPER;
    fbl::AllocChecker ac;
    fbl::unique_ptr<uint8_t[]> out(new (&ac) uint8_t[len]);
    ASSERT_TRUE(ac.check());
    memset(out.get(), 0, len);
    ASSERT_EQ(lseek(fd, off, SEEK_SET), static_cast<ssize_t>(off));
    ASSERT_EQ(read(fd, out.get(), len), static_cast<ssize_t>(len));
    ASSERT_EQ(memcmp(in, out.get(), len), 0);
    END_HELPER;
}

bool CheckWriteColor(int fd, size_t off, size_t len, uint8_t color) {
    BEGIN_HELPER;
    fbl::AllocChecker ac;
    fbl::unique_ptr<uint8_t[]> buf(new (&ac) uint8_t[len]);
    ASSERT_TRUE(ac.check());
    memset(buf.get(), color, len);
    ASSERT_EQ(lseek(fd, off, SEEK_SET), static_cast<ssize_t>(off));
    ASSERT_EQ(write(fd, buf.get(), len), static_cast<ssize_t>(len));
    END_HELPER;
}

bool CheckReadColor(int fd, size_t off, size_t len, uint8_t color) {
    BEGIN_HELPER;
    fbl::AllocChecker ac;
    fbl::unique_ptr<uint8_t[]> buf(new (&ac) uint8_t[len]);
    ASSERT_TRUE(ac.check());
    ASSERT_EQ(lseek(fd, off, SEEK_SET), static_cast<ssize_t>(off));
    ASSERT_EQ(read(fd, buf.get(), len), static_cast<ssize_t>(len));
    for (size_t i = 0; i < len; i++) {
        ASSERT_EQ(buf[i], color);
    }
    END_HELPER;
}

bool CheckWriteReadBlock(int fd, size_t block, size_t count) {
    BEGIN_HELPER;
    fzl::UnownedFdioCaller disk_connection(fd);
    zx_status_t status;
    fuchsia_hardware_block_BlockInfo block_info;
    ASSERT_EQ(
        fuchsia_hardware_block_BlockGetInfo(disk_connection.borrow_channel(), &status, &block_info),
        ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    size_t len = block_info.block_size * count;
    size_t off = block_info.block_size * block;
    fbl::unique_ptr<uint8_t[]> in(new uint8_t[len]);
    ASSERT_TRUE(CheckWrite(fd, off, len, in.get()));
    ASSERT_TRUE(CheckRead(fd, off, len, in.get()));
    END_HELPER;
}

bool CheckNoAccessBlock(int fd, size_t block, size_t count) {
    BEGIN_HELPER;
    fzl::UnownedFdioCaller disk_connection(fd);
    zx_status_t status;
    fuchsia_hardware_block_BlockInfo block_info;
    ASSERT_EQ(
        fuchsia_hardware_block_BlockGetInfo(disk_connection.borrow_channel(), &status, &block_info),
        ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    fbl::unique_ptr<uint8_t[]> buf(new uint8_t[block_info.block_size * count]);
    size_t len = block_info.block_size * count;
    size_t off = block_info.block_size * block;
    for (size_t i = 0; i < len; i++)
        buf[i] = static_cast<uint8_t>(rand());
    ASSERT_EQ(lseek(fd, off, SEEK_SET), static_cast<ssize_t>(off));
    ASSERT_EQ(write(fd, buf.get(), len), -1);
    ASSERT_EQ(lseek(fd, off, SEEK_SET), static_cast<ssize_t>(off));
    ASSERT_EQ(read(fd, buf.get(), len), -1);
    END_HELPER;
}

bool CheckDeadBlock(int fd) {
    BEGIN_HELPER;
    fbl::AllocChecker ac;
    constexpr size_t kBlksize = 8192;
    fbl::unique_ptr<uint8_t[]> buf(new (&ac) uint8_t[kBlksize]);
    ASSERT_TRUE(ac.check());
    ASSERT_EQ(lseek(fd, 0, SEEK_SET), 0);
    ASSERT_EQ(write(fd, buf.get(), kBlksize), -1);
    ASSERT_EQ(lseek(fd, 0, SEEK_SET), 0);
    ASSERT_EQ(read(fd, buf.get(), kBlksize), -1);
    END_HELPER;
}

bool Upgrade(const fzl::FdioCaller& caller, const uint8_t* old_guid, const uint8_t* new_guid,
             zx_status_t result) {
    BEGIN_HELPER;

    fuchsia_hardware_block_partition_GUID old_guid_fidl;
    memcpy(&old_guid_fidl.value, old_guid, fuchsia_hardware_block_partition_GUID_LENGTH);
    fuchsia_hardware_block_partition_GUID new_guid_fidl;
    memcpy(&new_guid_fidl.value, new_guid, fuchsia_hardware_block_partition_GUID_LENGTH);

    zx_status_t status;
    zx_status_t io_status = fuchsia_hardware_block_volume_VolumeManagerActivate(
        caller.borrow_channel(), &old_guid_fidl, &new_guid_fidl, &status);
    ASSERT_EQ(ZX_OK, io_status);
    ASSERT_EQ(result, status);

    END_HELPER;
}

/////////////////////// Actual tests:

// Test initializing the FVM on a partition that is smaller than a slice
bool TestTooSmall() {
    BEGIN_TEST;

    if (use_real_disk) {
        fprintf(stderr, "Test is ramdisk-exclusive; ignoring\n");
        return true;
    }

    uint64_t blk_size = 512;
    uint64_t blk_count = (1 << 15);
    ASSERT_GE(ramdisk_create(blk_size, blk_count, &test_ramdisk), 0);
    const char* ramdisk_path = ramdisk_get_path(test_ramdisk);
    fbl::unique_fd fd(open(ramdisk_path, O_RDWR));
    ASSERT_TRUE(fd);
    size_t slice_size = blk_size * blk_count;
    ASSERT_EQ(fvm_init(fd.get(), slice_size), ZX_ERR_NO_SPACE);
    ASSERT_TRUE(ValidateFVM(ramdisk_path, ValidationResult::Corrupted));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test initializing the FVM on a large partition, with metadata size > the max transfer size
bool TestLarge() {
    BEGIN_TEST;

    if (use_real_disk) {
        fprintf(stderr, "Test is ramdisk-exclusive; ignoring\n");
        return true;
    }

    char fvm_path[PATH_MAX];
    uint64_t blk_size = 512;
    uint64_t blk_count = 8 * (1 << 20);
    ASSERT_GE(ramdisk_create(blk_size, blk_count, &test_ramdisk), 0);
    const char* ramdisk_path = ramdisk_get_path(test_ramdisk);

    size_t slice_size = 16 * (1 << 10);
    size_t metadata_size = fvm::MetadataSize(blk_size * blk_count, slice_size);

    fbl::unique_fd fd(open(ramdisk_path, O_RDWR));
    ASSERT_GT(fd.get(), 0);
    fzl::UnownedFdioCaller disk_connection(fd.get());
    zx::unowned_channel channel(disk_connection.borrow_channel());
    zx_status_t status;
    fuchsia_hardware_block_BlockInfo block_info;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(channel->get(), &status, &block_info), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_LT(block_info.max_transfer_size, metadata_size);

    ASSERT_EQ(fvm_init(fd.get(), slice_size), ZX_OK);

    ASSERT_EQ(fuchsia_device_ControllerBind(channel->get(), FVM_DRIVER_LIB, STRLEN(FVM_DRIVER_LIB),
                                            &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    snprintf(fvm_path, sizeof(fvm_path), "%s/fvm", ramdisk_path);
    ASSERT_EQ(wait_for_device(fvm_path, ZX_SEC(3)), ZX_OK);
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Load and unload an empty FVM
bool TestEmpty() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test allocating a single partition
bool TestAllocateOne() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);

    // Allocate one VPart
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);

    // Check that the name matches what we provided
    char name[fvm::kMaxVPartitionNameLength + 1];
    fzl::UnownedFdioCaller partition_connection(vp_fd.get());

    zx_status_t status;
    size_t actual;
    ASSERT_EQ(fuchsia_hardware_block_partition_PartitionGetName(
                  partition_connection.borrow_channel(), &status, name, sizeof(name), &actual),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    name[actual] = '\0';
    ASSERT_EQ(memcmp(name, kTestPartName1, strlen(kTestPartName1)), 0);

    // Check that we can read from / write to it.
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), 0, 1));

    // Try accessing the block again after closing / re-opening it.
    ASSERT_EQ(close(vp_fd.release()), 0);
    vp_fd.reset(open_partition(kTestUniqueGUID, kTestPartGUIDData, 0, nullptr));
    ASSERT_TRUE(vp_fd, "Couldn't re-open Data VPart");
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), 0, 1));

    ASSERT_EQ(close(vp_fd.release()), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test allocating a collection of partitions
bool TestAllocateMany() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);

    // Test allocation of multiple VPartitions
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd data_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(data_fd);

    strcpy(request.name, kTestPartName2);
    memcpy(request.type, kTestPartGUIDBlob, GUID_LEN);
    fbl::unique_fd blob_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(blob_fd);

    strcpy(request.name, kTestPartName3);
    memcpy(request.type, kTestPartGUIDSystem, GUID_LEN);
    fbl::unique_fd sys_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(sys_fd);

    ASSERT_TRUE(CheckWriteReadBlock(data_fd.get(), 0, 1));
    ASSERT_TRUE(CheckWriteReadBlock(blob_fd.get(), 0, 1));
    ASSERT_TRUE(CheckWriteReadBlock(sys_fd.get(), 0, 1));

    ASSERT_EQ(close(data_fd.release()), 0);
    ASSERT_EQ(close(blob_fd.release()), 0);
    ASSERT_EQ(close(sys_fd.release()), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test that the fvm driver can cope with a sudden close during read / write
// operations.
bool TestCloseDuringAccess() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);

    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);

    auto bg_thread = [](void* arg) {
        int vp_fd = *reinterpret_cast<int*>(arg);
        while (true) {
            uint8_t in[8192];
            memset(in, 'a', sizeof(in));
            if (write(vp_fd, in, sizeof(in)) != static_cast<ssize_t>(sizeof(in))) {
                return 0;
            }
            uint8_t out[8192];
            memset(out, 0, sizeof(out));
            lseek(vp_fd, 0, SEEK_SET);
            if (read(vp_fd, out, sizeof(out)) != static_cast<ssize_t>(sizeof(out))) {
                return 0;
            }
            // If we DID manage to read it, then the data should be valid...
            if (memcmp(in, out, sizeof(in)) != 0) {
                return -1;
            }
        }
    };

    // Launch a background thread to read from / write to the VPartition
    thrd_t thread;
    int raw_fd = vp_fd.get();
    ASSERT_EQ(thrd_create(&thread, bg_thread, &raw_fd), thrd_success);
    // Let the background thread warm up a little bit...
    usleep(10000);
    // ... and close the fd from underneath it!
    //
    // Yes, this is a little unsafe (we risk the bg thread accessing an
    // unallocated fd), but no one else in this test process should be adding
    // fds, so we won't risk anyone reusing "vp_fd" within this test case.
    ASSERT_EQ(close(vp_fd.release()), 0);

    int res;
    ASSERT_EQ(thrd_join(thread, &res), thrd_success);
    ASSERT_EQ(res, 0, "Background thread failed");

    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test that the fvm driver can cope with a sudden release during read / write
// operations.
bool TestReleaseDuringAccess() {
    BEGIN_TEST;

    if (use_real_disk) {
        fprintf(stderr, "Test is ramdisk-exclusive; ignoring\n");
        return true;
    }

    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);

    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);

    auto bg_thread = [](void* arg) {
        int vp_fd = *reinterpret_cast<int*>(arg);
        while (true) {
            uint8_t in[8192];
            memset(in, 'a', sizeof(in));
            if (write(vp_fd, in, sizeof(in)) != static_cast<ssize_t>(sizeof(in))) {
                return 0;
            }
            uint8_t out[8192];
            memset(out, 0, sizeof(out));
            lseek(vp_fd, 0, SEEK_SET);
            if (read(vp_fd, out, sizeof(out)) != static_cast<ssize_t>(sizeof(out))) {
                return 0;
            }
            // If we DID manage to read it, then the data should be valid...
            if (memcmp(in, out, sizeof(in)) != 0) {
                return -1;
            }
        }
    };

    // Launch a background thread to read from / write to the VPartition
    thrd_t thread;
    int raw_fd = vp_fd.get();
    ASSERT_EQ(thrd_create(&thread, bg_thread, &raw_fd), thrd_success);
    // Let the background thread warm up a little bit...
    usleep(10000);
    // ... and close the entire ramdisk from underneath it!
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");

    int res;
    ASSERT_EQ(thrd_join(thread, &res), thrd_success);
    ASSERT_EQ(res, 0, "Background thread failed");

    END_TEST;
}

bool TestDestroyDuringAccess() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);

    alloc_req_t request;
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);

    auto bg_thread = [](void* arg) {
        int vp_fd = *reinterpret_cast<int*>(arg);
        unsigned count = 0;
        while (true) {
            if (++count % 10000 == 0) {
                printf("Run %u\n", count);
            }
            uint8_t in[8192];
            memset(in, 'a', sizeof(in));
            if (write(vp_fd, in, sizeof(in)) != static_cast<ssize_t>(sizeof(in))) {
                return 0;
            }
            uint8_t out[8192];
            memset(out, 0, sizeof(out));
            lseek(vp_fd, 0, SEEK_SET);
            if (read(vp_fd, out, sizeof(out)) != static_cast<ssize_t>(sizeof(out))) {
                return 0;
            }
            // If we DID manage to read it, then the data should be valid...
            if (memcmp(in, out, sizeof(in)) != 0) {
                return -1;
            }
        }
    };

    // Launch a background thread to read from / write to the VPartition
    thrd_t thread;
    int raw_fd = vp_fd.get();
    ASSERT_EQ(thrd_create(&thread, bg_thread, &raw_fd), thrd_success);
    // Let the background thread warm up a little bit...
    usleep(10000);
    // ... and destroy the vpartition
    fzl::FdioCaller partition_caller(std::move(vp_fd));
    zx_status_t status;
    ASSERT_EQ(
        fuchsia_hardware_block_volume_VolumeDestroy(partition_caller.borrow_channel(), &status),
        ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    int res;
    ASSERT_EQ(thrd_join(thread, &res), thrd_success);
    ASSERT_EQ(res, 0, "Background thread failed");

    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test allocating additional slices to a vpartition.
bool TestVPartitionExtend() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);
    const size_t kDiskSize = use_real_disk ? test_block_size * test_block_count : 512 * (1 << 20);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd, "Couldn't open Volume Manager");
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;
    size_t slices_total = fvm::UsableSlicesCount(kDiskSize, slice_size);
    size_t slices_left = slices_total;

    ASSERT_TRUE(FVMCheckAllocatedCount(fd.get(), slices_total - slices_left, slices_total));

    // Allocate one VPart
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    size_t slice_count = 1;
    request.slice_count = slice_count;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd, "Couldn't open Volume");
    slices_left--;
    ASSERT_TRUE(FVMCheckAllocatedCount(fd.get(), slices_total - slices_left, slices_total));

    // Confirm that the disk reports the correct number of slices
    fzl::FdioCaller partition_caller(std::move(vp_fd));
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());
    fuchsia_hardware_block_BlockInfo block_info;
    zx_status_t status;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, slice_size * slice_count);

    // Try re-allocating an already allocated vslice
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), 0, 1, &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK, "Expected request failure");
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, slice_size * slice_count);

    // Try again with a portion of the request which is unallocated
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), 0, 2, &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK, "Expected request failure");

    // Allocate OBSCENELY too many slices
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), slice_count,
                                                         std::numeric_limits<size_t>::max(),
                                                         &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK, "Expected request failure");

    // Allocate slices at a too-large offset
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(
                  partition_channel->get(), std::numeric_limits<size_t>::max(), 1, &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK, "Expected request failure");

    // Attempt to allocate slightly too many slices
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), slice_count,
                                                         slices_left + 1, &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK, "Expected request failure");

    // The number of free slices should be unchanged.
    ASSERT_TRUE(FVMCheckAllocatedCount(fd.get(), slices_total - slices_left, slices_total));

    // Allocate exactly the remaining number of slices
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), slice_count,
                                                         slices_left, &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    slice_count += slices_left;
    slices_left = 0;
    ASSERT_TRUE(FVMCheckAllocatedCount(fd.get(), slices_total - slices_left, slices_total));
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, slice_size * slice_count);

    // We can't allocate any more to this VPartition
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), slice_count, 1,
                                                         &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK, "Expected request failure");

    // We can't allocate a new VPartition
    strcpy(request.name, kTestPartName2);
    memcpy(request.type, kTestPartGUIDBlob, GUID_LEN);
    ASSERT_LT(fvm_allocate_partition(fd.get(), &request), 0, "Expected VPart allocation failure");

    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test allocating very sparse VPartition
bool TestVPartitionExtendSparse() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    uint64_t blk_size = use_real_disk ? test_block_size : 512;
    uint64_t blk_count = use_real_disk ? test_block_size : 1 << 20;
    uint64_t slice_size = 16 * blk_size;
    ASSERT_EQ(StartFVMTest(blk_size, blk_count, slice_size, ramdisk_path, fvm_driver), 0);

    size_t slices_left = fvm::UsableSlicesCount(blk_size * blk_count, slice_size);
    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);

    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    slices_left--;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), 0, 1));

    // Double check that we can access a block at this vslice address
    // (this isn't always possible; for certain slice sizes, blocks may be
    // allocatable / freeable, but not addressable).
    size_t bno = (fvm::kMaxVSlices - 1) * (slice_size / blk_size);
    ASSERT_EQ(bno / (slice_size / blk_size), (fvm::kMaxVSlices - 1), "bno overflowed");
    ASSERT_EQ((bno * blk_size) / blk_size, bno, "block access will overflow");

    fzl::UnownedFdioCaller partition_caller(vp_fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());
    zx_status_t status;

    // Try allocating at a location that's slightly too large
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), fvm::kMaxVSlices,
                                                         1, &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK, "Expected request failure");

    // Try allocating at the largest offset
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(),
                                                         fvm::kMaxVSlices - 1, 1, &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), bno, 1));

    // Try freeing beyond largest offset
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), fvm::kMaxVSlices,
                                                         1, &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK, "Expected request failure");
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), bno, 1));

    // Try freeing at the largest offset
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(),
                                                         fvm::kMaxVSlices - 1, 1, &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), bno, 1));

    ASSERT_EQ(close(vp_fd.release()), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, slice_size));
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test removing slices from a VPartition.
bool TestVPartitionShrink() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);
    const size_t kDiskSize = use_real_disk ? test_block_size * test_block_count : 512 * (1 << 20);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd, "Couldn't open Volume Manager");
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;
    size_t slices_total = fvm::UsableSlicesCount(kDiskSize, slice_size);
    size_t slices_left = slices_total;

    ASSERT_TRUE(FVMCheckAllocatedCount(fd.get(), slices_total - slices_left, slices_total));

    // Allocate one VPart
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    size_t slice_count = 1;
    request.slice_count = slice_count;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd, "Couldn't open Volume");
    slices_left--;

    fzl::UnownedFdioCaller partition_caller(vp_fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());
    zx_status_t status;

    // Confirm that the disk reports the correct number of slices
    fuchsia_hardware_block_BlockInfo block_info;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, slice_size * slice_count);
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), (slice_size / block_info.block_size) - 1, 1));
    ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), (slice_size / block_info.block_size) - 1, 2));
    ASSERT_TRUE(FVMCheckAllocatedCount(fd.get(), slices_total - slices_left, slices_total));

    // Try shrinking the 0th vslice
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), 0, 1, &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK, "Expected request failure");

    // Try no-op requests (length = 0).
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), 1, 0, &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), 1, 0, &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, slice_size * slice_count);

    // Try again with a portion of the request which is unallocated
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), 1, 2, &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK);
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, slice_size * slice_count);
    ASSERT_TRUE(FVMCheckAllocatedCount(fd.get(), slices_total - slices_left, slices_total));

    // Allocate exactly the remaining number of slices
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), slice_count,
                                                         slices_left, &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    slice_count += slices_left;
    slices_left = 0;

    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, slice_size * slice_count);
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), (slice_size / block_info.block_size) - 1, 1));
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), (slice_size / block_info.block_size) - 1, 2));
    ASSERT_TRUE(FVMCheckAllocatedCount(fd.get(), slices_total - slices_left, slices_total));

    // We can't allocate any more to this VPartition
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), slice_count, 1,
                                                         &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK);

    // Try to shrink off the end (okay, since SOME of the slices are allocated)
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), 1,
                                                         slice_count + 3, &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_TRUE(FVMCheckAllocatedCount(fd.get(), 1, slices_total));

    // The same request to shrink should now fail (NONE of the slices are
    // allocated)
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), 1,
                                                         slice_count - 1, &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK);
    ASSERT_TRUE(FVMCheckAllocatedCount(fd.get(), 1, slices_total));

    // ... unless we re-allocate and try again.
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), 1,
                                                         slice_count - 1, &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), 1,
                                                         slice_count - 1, &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test splitting a contiguous slice extent into multiple parts
bool TestVPartitionSplit() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);
    size_t disk_size = 512 * (1 << 20);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;
    size_t slices_left = fvm::UsableSlicesCount(disk_size, slice_size);

    // Allocate one VPart
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    size_t slice_count = 5;
    request.slice_count = slice_count;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);
    slices_left--;

    fzl::UnownedFdioCaller partition_caller(vp_fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());

    // Confirm that the disk reports the correct number of slices
    fuchsia_hardware_block_BlockInfo block_info;
    zx_status_t status;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, slice_size * slice_count);

    extend_request_t reset_erequest;
    reset_erequest.offset = 1;
    reset_erequest.length = slice_count - 1;
    extend_request_t mid_erequest;
    mid_erequest.offset = 2;
    mid_erequest.length = 1;
    extend_request_t start_erequest;
    start_erequest.offset = 1;
    start_erequest.length = 1;
    extend_request_t end_erequest;
    end_erequest.offset = 3;
    end_erequest.length = slice_count - 3;

    auto verifyExtents = [&](bool start, bool mid, bool end) {
        size_t start_block = start_erequest.offset * (slice_size / block_info.block_size);
        size_t mid_block = mid_erequest.offset * (slice_size / block_info.block_size);
        size_t end_block = end_erequest.offset * (slice_size / block_info.block_size);

        if (start) {
            ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), start_block, 1));
        } else {
            ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), start_block, 1));
        }
        if (mid) {
            ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), mid_block, 1));
        } else {
            ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), mid_block, 1));
        }
        if (end) {
            ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), end_block, 1));
        } else {
            ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), end_block, 1));
        }
        return true;
    };

    auto doExtend = [](const zx::unowned_channel& partition_channel, extend_request_t request) {
        BEGIN_HELPER;
        zx_status_t status;
        ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(
                      partition_channel->get(), request.offset, request.length, &status),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);
        END_HELPER;
    };

    auto doShrink = [](const zx::unowned_channel& partition_channel, extend_request_t request) {
        BEGIN_HELPER;
        zx_status_t status;
        ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(
                      partition_channel->get(), request.offset, request.length, &status),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);
        END_HELPER;
    };

    // We should be able to split the extent.
    ASSERT_TRUE(verifyExtents(true, true, true));
    ASSERT_TRUE(doShrink(partition_channel, mid_erequest));
    ASSERT_TRUE(verifyExtents(true, false, true));
    ASSERT_TRUE(doShrink(partition_channel, start_erequest));
    ASSERT_TRUE(verifyExtents(false, false, true));
    ASSERT_TRUE(doShrink(partition_channel, end_erequest));
    ASSERT_TRUE(verifyExtents(false, false, false));

    ASSERT_TRUE(doExtend(partition_channel, reset_erequest));

    ASSERT_TRUE(doShrink(partition_channel, start_erequest));
    ASSERT_TRUE(verifyExtents(false, true, true));
    ASSERT_TRUE(doShrink(partition_channel, mid_erequest));
    ASSERT_TRUE(verifyExtents(false, false, true));
    ASSERT_TRUE(doShrink(partition_channel, end_erequest));
    ASSERT_TRUE(verifyExtents(false, false, false));

    ASSERT_TRUE(doExtend(partition_channel, reset_erequest));

    ASSERT_TRUE(doShrink(partition_channel, end_erequest));
    ASSERT_TRUE(verifyExtents(true, true, false));
    ASSERT_TRUE(doShrink(partition_channel, mid_erequest));
    ASSERT_TRUE(verifyExtents(true, false, false));
    ASSERT_TRUE(doShrink(partition_channel, start_erequest));
    ASSERT_TRUE(verifyExtents(false, false, false));

    ASSERT_TRUE(doExtend(partition_channel, reset_erequest));

    ASSERT_TRUE(doShrink(partition_channel, end_erequest));
    ASSERT_TRUE(verifyExtents(true, true, false));
    ASSERT_TRUE(doShrink(partition_channel, start_erequest));
    ASSERT_TRUE(verifyExtents(false, true, false));
    ASSERT_TRUE(doShrink(partition_channel, mid_erequest));
    ASSERT_TRUE(verifyExtents(false, false, false));

    // We should also be able to combine extents
    ASSERT_TRUE(doExtend(partition_channel, mid_erequest));
    ASSERT_TRUE(verifyExtents(false, true, false));
    ASSERT_TRUE(doExtend(partition_channel, start_erequest));
    ASSERT_TRUE(verifyExtents(true, true, false));
    ASSERT_TRUE(doExtend(partition_channel, end_erequest));
    ASSERT_TRUE(verifyExtents(true, true, true));

    ASSERT_TRUE(doShrink(partition_channel, reset_erequest));

    ASSERT_TRUE(doExtend(partition_channel, end_erequest));
    ASSERT_TRUE(verifyExtents(false, false, true));
    ASSERT_TRUE(doExtend(partition_channel, mid_erequest));
    ASSERT_TRUE(verifyExtents(false, true, true));
    ASSERT_TRUE(doExtend(partition_channel, start_erequest));
    ASSERT_TRUE(verifyExtents(true, true, true));

    ASSERT_TRUE(doShrink(partition_channel, reset_erequest));

    ASSERT_TRUE(doExtend(partition_channel, end_erequest));
    ASSERT_TRUE(verifyExtents(false, false, true));
    ASSERT_TRUE(doExtend(partition_channel, start_erequest));
    ASSERT_TRUE(verifyExtents(true, false, true));
    ASSERT_TRUE(doExtend(partition_channel, mid_erequest));
    ASSERT_TRUE(verifyExtents(true, true, true));

    ASSERT_EQ(close(vp_fd.release()), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test removing VPartitions within an FVM
bool TestVPartitionDestroy() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);

    // Test allocation of multiple VPartitions
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd data_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(data_fd);
    fzl::UnownedFdioCaller data_caller(data_fd.get());
    zx::unowned_channel data_channel(data_caller.borrow_channel());

    strcpy(request.name, kTestPartName2);
    memcpy(request.type, kTestPartGUIDBlob, GUID_LEN);
    fbl::unique_fd blob_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(blob_fd);
    fzl::UnownedFdioCaller blob_caller(blob_fd.get());
    zx::unowned_channel blob_channel(blob_caller.borrow_channel());

    strcpy(request.name, kTestPartName3);
    memcpy(request.type, kTestPartGUIDSystem, GUID_LEN);
    fbl::unique_fd sys_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(sys_fd);
    fzl::UnownedFdioCaller sys_caller(sys_fd.get());
    zx::unowned_channel sys_channel(sys_caller.borrow_channel());

    // We can access all three...
    ASSERT_TRUE(CheckWriteReadBlock(data_fd.get(), 0, 1));
    ASSERT_TRUE(CheckWriteReadBlock(blob_fd.get(), 0, 1));
    ASSERT_TRUE(CheckWriteReadBlock(sys_fd.get(), 0, 1));

    // But not after we destroy the blob partition.
    zx_status_t status;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeDestroy(blob_channel->get(), &status), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_TRUE(CheckWriteReadBlock(data_fd.get(), 0, 1));
    ASSERT_TRUE(CheckDeadBlock(blob_fd.get()));
    ASSERT_TRUE(CheckWriteReadBlock(sys_fd.get(), 0, 1));

    // We also can't re-destroy the blob partition.
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeDestroy(blob_channel->get(), &status), ZX_OK);
    ASSERT_NE(status, ZX_OK);

    // We also can't allocate slices to the destroyed blob partition.
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(blob_channel->get(), 1, 1, &status),
              ZX_OK);
    ASSERT_NE(status, ZX_OK);

    // Destroy the other two VPartitions.
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeDestroy(data_channel->get(), &status), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_TRUE(CheckDeadBlock(data_fd.get()));
    ASSERT_TRUE(CheckDeadBlock(blob_fd.get()));
    ASSERT_TRUE(CheckWriteReadBlock(sys_fd.get(), 0, 1));

    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeDestroy(sys_channel->get(), &status), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_TRUE(CheckDeadBlock(data_fd.get()));
    ASSERT_TRUE(CheckDeadBlock(blob_fd.get()));
    ASSERT_TRUE(CheckDeadBlock(sys_fd.get()));

    ASSERT_EQ(close(data_fd.release()), 0);
    ASSERT_EQ(close(blob_fd.release()), 0);
    ASSERT_EQ(close(sys_fd.release()), 0);
    ASSERT_EQ(close(fd.release()), 0);

    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

bool TestVPartitionQuery() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    size_t slice_count = 64;
    size_t block_count = 512;
    size_t block_size = 1 << 20;
    size_t slice_size = (block_count * block_size) / slice_count;
    ASSERT_EQ(StartFVMTest(block_count, block_size, slice_size, ramdisk_path, fvm_driver), 0);
    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);

    // Allocate partition
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 10;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd part_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(part_fd);
    fzl::FdioCaller partition_caller(std::move(part_fd));
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());

    // Create non-contiguous extent.
    zx_status_t status;
    uint64_t offset = 20;
    uint64_t length = 10;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset, length,
                                                         &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);

    // Query various vslice ranges
    uint64_t start_slices[6];
    start_slices[0] = 0;
    start_slices[1] = 10;
    start_slices[2] = 20;
    start_slices[3] = 50;
    start_slices[4] = 25;
    start_slices[5] = 15;

    // Check response from partition query
    fuchsia_hardware_block_volume_VsliceRange
        ranges[fuchsia_hardware_block_volume_MAX_SLICE_REQUESTS];
    size_t actual_ranges_count;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                  partition_channel->get(), start_slices, fbl::count_of(start_slices), &status,
                  ranges, &actual_ranges_count),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(actual_ranges_count, fbl::count_of(start_slices));
    ASSERT_TRUE(ranges[0].allocated);
    ASSERT_EQ(ranges[0].count, 10);
    ASSERT_FALSE(ranges[1].allocated);
    ASSERT_EQ(ranges[1].count, 10);
    ASSERT_TRUE(ranges[2].allocated);
    ASSERT_EQ(ranges[2].count, 10);
    ASSERT_FALSE(ranges[3].allocated);
    ASSERT_EQ(ranges[3].count, volume_info.vslice_count - 50);
    ASSERT_TRUE(ranges[4].allocated);
    ASSERT_EQ(ranges[4].count, 5);
    ASSERT_FALSE(ranges[5].allocated);
    ASSERT_EQ(ranges[5].count, 5);

    // Merge the extents!
    offset = 10;
    length = 10;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset, length,
                                                         &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    // Check partition query response again after extend
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                  partition_channel->get(), start_slices, fbl::count_of(start_slices), &status,
                  ranges, &actual_ranges_count),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(actual_ranges_count, fbl::count_of(start_slices));
    ASSERT_TRUE(ranges[0].allocated);
    ASSERT_EQ(ranges[0].count, 30);
    ASSERT_TRUE(ranges[1].allocated);
    ASSERT_EQ(ranges[1].count, 20);
    ASSERT_TRUE(ranges[2].allocated);
    ASSERT_EQ(ranges[2].count, 10);
    ASSERT_FALSE(ranges[3].allocated);
    ASSERT_EQ(ranges[3].count, volume_info.vslice_count - 50);
    ASSERT_TRUE(ranges[4].allocated);
    ASSERT_EQ(ranges[4].count, 5);
    ASSERT_TRUE(ranges[5].allocated);
    ASSERT_EQ(ranges[5].count, 15);

    start_slices[0] = volume_info.vslice_count + 1;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                  partition_channel->get(), start_slices, fbl::count_of(start_slices), &status,
                  ranges, &actual_ranges_count),
              ZX_OK);
    ASSERT_EQ(status, ZX_ERR_OUT_OF_RANGE);

    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, slice_size));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test allocating and accessing slices which are allocated contiguously.
bool TestSliceAccessContiguous() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;

    // Allocate one VPart
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);

    fzl::UnownedFdioCaller partition_caller(vp_fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());
    fuchsia_hardware_block_BlockInfo block_info;
    zx_status_t status;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    // This is the last 'accessible' block.
    size_t last_block = (slice_size / block_info.block_size) - 1;

    {
        fbl::RefPtr<VmoClient> vc;
        ASSERT_TRUE(VmoClient::Create(vp_fd.get(), &vc));
        fbl::unique_ptr<VmoBuf> vb;
        ASSERT_TRUE(VmoBuf::Create(vc, block_info.block_size * 2, &vb));
        ASSERT_TRUE(
            vc->CheckWrite(vb.get(), 0, block_info.block_size * last_block, block_info.block_size));
        ASSERT_TRUE(
            vc->CheckRead(vb.get(), 0, block_info.block_size * last_block, block_info.block_size));

        // Try writing out of bounds -- check that we don't have access.
        ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), (slice_size / block_info.block_size) - 1, 2));
        ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), slice_size / block_info.block_size, 1));

        // Attempt to access the next contiguous slice
        ASSERT_EQ(
            fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), 1, 1, &status),
            ZX_OK);
        ASSERT_EQ(status, ZX_OK);

        // Now we can access the next slice...
        ASSERT_TRUE(vc->CheckWrite(vb.get(), block_info.block_size,
                                   block_info.block_size * (last_block + 1),
                                   block_info.block_size));
        ASSERT_TRUE(vc->CheckRead(vb.get(), block_info.block_size,
                                  block_info.block_size * (last_block + 1), block_info.block_size));
        // ... We can still access the previous slice...
        ASSERT_TRUE(
            vc->CheckRead(vb.get(), 0, block_info.block_size * last_block, block_info.block_size));
        // ... And we can cross slices
        ASSERT_TRUE(vc->CheckRead(vb.get(), 0, block_info.block_size * last_block,
                                  block_info.block_size * 2));
    }

    ASSERT_EQ(close(vp_fd.release()), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test allocating and accessing multiple (3+) slices at once.
bool TestSliceAccessMany() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    // The size of a slice must be carefully constructed for this test
    // so that we can hold multiple slices in memory without worrying
    // about hitting resource limits.
    const size_t kBlockSize = use_real_disk ? test_block_size : 512;
    const size_t kBlocksPerSlice = 256;
    const size_t kSliceSize = kBlocksPerSlice * kBlockSize;
    ASSERT_EQ(StartFVMTest(kBlockSize, (1 << 20), kSliceSize, ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    ASSERT_EQ(volume_info.slice_size, kSliceSize);

    // Allocate one VPart
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);

    fzl::UnownedFdioCaller partition_caller(vp_fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());
    fuchsia_hardware_block_BlockInfo block_info;
    zx_status_t status;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_size, kBlockSize);

    {
        fbl::RefPtr<VmoClient> vc;
        ASSERT_TRUE(VmoClient::Create(vp_fd.get(), &vc));
        fbl::unique_ptr<VmoBuf> vb;
        ASSERT_TRUE(VmoBuf::Create(vc, kSliceSize * 3, &vb));

        // Access the first slice
        ASSERT_TRUE(vc->CheckWrite(vb.get(), 0, 0, kSliceSize));
        ASSERT_TRUE(vc->CheckRead(vb.get(), 0, 0, kSliceSize));

        // Try writing out of bounds -- check that we don't have access.
        ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), kBlocksPerSlice - 1, 2));
        ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), kBlocksPerSlice, 1));

        // Attempt to access the next contiguous slices
        uint64_t offset = 1;
        uint64_t length = 2;
        ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset,
                                                             length, &status),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);

        // Now we can access the next slices...
        ASSERT_TRUE(vc->CheckWrite(vb.get(), kSliceSize, kSliceSize, 2 * kSliceSize));
        ASSERT_TRUE(vc->CheckRead(vb.get(), kSliceSize, kSliceSize, 2 * kSliceSize));
        // ... We can still access the previous slice...
        ASSERT_TRUE(vc->CheckRead(vb.get(), 0, 0, kSliceSize));
        // ... And we can cross slices for reading.
        ASSERT_TRUE(vc->CheckRead(vb.get(), 0, 0, 3 * kSliceSize));

        // Also, we can cross slices for writing.
        ASSERT_TRUE(vc->CheckWrite(vb.get(), 0, 0, 3 * kSliceSize));
        ASSERT_TRUE(vc->CheckRead(vb.get(), 0, 0, 3 * kSliceSize));

        // Additionally, we can access "parts" of slices in a multi-slice
        // operation. Here, read one block into the first slice, and read
        // up to the last block in the final slice.
        ASSERT_TRUE(vc->CheckWrite(vb.get(), 0, kBlockSize, 3 * kSliceSize - 2 * kBlockSize));
        ASSERT_TRUE(vc->CheckRead(vb.get(), 0, kBlockSize, 3 * kSliceSize - 2 * kBlockSize));
    }

    ASSERT_EQ(close(vp_fd.release()), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, kSliceSize));
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test allocating and accessing slices which are allocated
// virtually contiguously (they appear sequential to the client) but are
// actually noncontiguous on the FVM partition.
bool TestSliceAccessNonContiguousPhysical() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];

    // This takes 130sec on a fast desktop, target x86 non-kvm qemu.
    // On the bots for arm it times out after 200sec.
    // For now just disable the timeout. An alternative is to make it
    // a large test, but then it won't get run for CQ/CI.
    unittest_cancel_timeout();

    ASSERT_EQ(StartFVMTest(512, 1 << 20, 8lu * (1 << 20), ramdisk_path, fvm_driver), 0);
    const size_t kDiskSize = use_real_disk ? test_block_size * test_block_count : 512 * (1 << 20);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;

    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);

    constexpr size_t kNumVParts = 3;
    typedef struct vdata {
        fbl::unique_fd fd;
        uint8_t guid[GUID_LEN];
        char name[32];
        size_t slices_used;
    } vdata_t;

    vdata_t vparts[kNumVParts] = {
        {fbl::unique_fd(), GUID_TEST_DATA_VALUE, "data", request.slice_count},
        {fbl::unique_fd(), GUID_TEST_BLOB_VALUE, "blob", request.slice_count},
        {fbl::unique_fd(), GUID_TEST_SYS_VALUE, "sys", request.slice_count},
    };

    for (size_t i = 0; i < fbl::count_of(vparts); i++) {
        strcpy(request.name, vparts[i].name);
        memcpy(request.type, vparts[i].guid, GUID_LEN);
        vparts[i].fd.reset(fvm_allocate_partition(fd.get(), &request));
        ASSERT_TRUE(vparts[i].fd);
    }

    fzl::UnownedFdioCaller partition_caller(vparts[0].fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());
    fuchsia_hardware_block_BlockInfo block_info;
    zx_status_t status;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    size_t usable_slices_per_vpart = fvm::UsableSlicesCount(kDiskSize, slice_size) / kNumVParts;
    size_t i = 0;
    while (vparts[i].slices_used < usable_slices_per_vpart) {
        int vfd = vparts[i].fd.get();
        // This is the last 'accessible' block.
        size_t last_block = (vparts[i].slices_used * (slice_size / block_info.block_size)) - 1;
        fbl::RefPtr<VmoClient> vc;
        ASSERT_TRUE(VmoClient::Create(vfd, &vc));
        fbl::unique_ptr<VmoBuf> vb;
        ASSERT_TRUE(VmoBuf::Create(vc, block_info.block_size * 2, &vb));

        ASSERT_TRUE(
            vc->CheckWrite(vb.get(), 0, block_info.block_size * last_block, block_info.block_size));
        ASSERT_TRUE(
            vc->CheckRead(vb.get(), 0, block_info.block_size * last_block, block_info.block_size));

        // Try writing out of bounds -- check that we don't have access.
        ASSERT_TRUE(CheckNoAccessBlock(vfd, last_block, 2));
        ASSERT_TRUE(CheckNoAccessBlock(vfd, last_block + 1, 1));

        // Attempt to access the next contiguous slice
        fzl::UnownedFdioCaller partition_caller(vfd);
        zx::unowned_channel partition_channel(partition_caller.borrow_channel());
        uint64_t offset = vparts[i].slices_used;
        uint64_t length = 1;
        ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset,
                                                             length, &status),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);

        // Now we can access the next slice...
        ASSERT_TRUE(vc->CheckWrite(vb.get(), block_info.block_size,
                                   block_info.block_size * (last_block + 1),
                                   block_info.block_size));
        ASSERT_TRUE(vc->CheckRead(vb.get(), block_info.block_size,
                                  block_info.block_size * (last_block + 1), block_info.block_size));
        // ... We can still access the previous slice...
        ASSERT_TRUE(
            vc->CheckRead(vb.get(), 0, block_info.block_size * last_block, block_info.block_size));
        // ... And we can cross slices
        ASSERT_TRUE(vc->CheckRead(vb.get(), 0, block_info.block_size * last_block,
                                  block_info.block_size * 2));

        vparts[i].slices_used++;
        i = (i + 1) % kNumVParts;
    }

    for (size_t i = 0; i < kNumVParts; i++) {
        printf("Testing multi-slice operations on vslice %lu\n", i);

        // We need at least five slices, so we can access three "middle"
        // slices and jitter to test off-by-one errors.
        ASSERT_GE(vparts[i].slices_used, 5);

        {
            fbl::RefPtr<VmoClient> vc;
            ASSERT_TRUE(VmoClient::Create(vparts[i].fd.get(), &vc));
            fbl::unique_ptr<VmoBuf> vb;
            ASSERT_TRUE(VmoBuf::Create(vc, slice_size * 4, &vb));

            // Try accessing 3 noncontiguous slices at once, with the
            // addition of "off by one block".
            size_t dev_off_start = slice_size - block_info.block_size;
            size_t dev_off_end = slice_size + block_info.block_size;
            size_t len_start = slice_size * 3 - block_info.block_size;
            size_t len_end = slice_size * 3 + block_info.block_size;

            // Test a variety of:
            // Starting device offsets,
            size_t bsz = block_info.block_size;
            for (size_t dev_off = dev_off_start; dev_off <= dev_off_end; dev_off += bsz) {
                printf("  Testing non-contiguous write/read starting at offset: %zu\n", dev_off);
                // Operation lengths,
                for (size_t len = len_start; len <= len_end; len += bsz) {
                    printf("    Testing operation of length: %zu\n", len);
                    // and starting VMO offsets
                    for (size_t vmo_off = 0; vmo_off < 3 * bsz; vmo_off += bsz) {
                        // Try writing & reading the entire section (multiple
                        // slices) at once.
                        ASSERT_TRUE(vc->CheckWrite(vb.get(), vmo_off, dev_off, len));
                        ASSERT_TRUE(vc->CheckRead(vb.get(), vmo_off, dev_off, len));

                        // Try reading the section one slice at a time.
                        // The results should be the same.
                        size_t sub_off = 0;
                        size_t sub_len = slice_size - (dev_off % slice_size);
                        while (sub_off < len) {
                            ASSERT_TRUE(vc->CheckRead(vb.get(), vmo_off + sub_off,
                                                      dev_off + sub_off, sub_len));
                            sub_off += sub_len;
                            sub_len = fbl::min(slice_size, len - sub_off);
                        }
                    }
                }
            }
        }
        ASSERT_EQ(close(vparts[i].fd.release()), 0);
    }

    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, slice_size));
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test allocating and accessing slices which are
// allocated noncontiguously from the client's perspective.
bool TestSliceAccessNonContiguousVirtual() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);
    const size_t kDiskSize = 512 * (1 << 20);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;

    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);

    constexpr size_t kNumVParts = 3;
    typedef struct vdata {
        fbl::unique_fd fd;
        uint8_t guid[GUID_LEN];
        char name[32];
        size_t slices_used;
        size_t last_slice;
    } vdata_t;

    vdata_t vparts[kNumVParts] = {
        {fbl::unique_fd(), GUID_TEST_DATA_VALUE, "data", request.slice_count, request.slice_count},
        {fbl::unique_fd(), GUID_TEST_BLOB_VALUE, "blob", request.slice_count, request.slice_count},
        {fbl::unique_fd(), GUID_TEST_SYS_VALUE, "sys", request.slice_count, request.slice_count},
    };

    for (size_t i = 0; i < fbl::count_of(vparts); i++) {
        strcpy(request.name, vparts[i].name);
        memcpy(request.type, vparts[i].guid, GUID_LEN);
        vparts[i].fd.reset(fvm_allocate_partition(fd.get(), &request));
        ASSERT_TRUE(vparts[i].fd);
    }

    fzl::UnownedFdioCaller partition_caller(vparts[0].fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());
    fuchsia_hardware_block_BlockInfo block_info;
    zx_status_t status;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    size_t usable_slices_per_vpart = fvm::UsableSlicesCount(kDiskSize, slice_size) / kNumVParts;
    size_t i = 0;
    while (vparts[i].slices_used < usable_slices_per_vpart) {
        int vfd = vparts[i].fd.get();
        // This is the last 'accessible' block.
        size_t last_block = (vparts[i].last_slice * (slice_size / block_info.block_size)) - 1;
        ASSERT_TRUE(CheckWriteReadBlock(vfd, last_block, 1));

        // Try writing out of bounds -- check that we don't have access.
        ASSERT_TRUE(CheckNoAccessBlock(vfd, last_block, 2));
        ASSERT_TRUE(CheckNoAccessBlock(vfd, last_block + 1, 1));

        // Attempt to access a non-contiguous slice
        fzl::UnownedFdioCaller partition_caller(vfd);
        zx::unowned_channel partition_channel(partition_caller.borrow_channel());
        uint64_t offset = vparts[i].last_slice + 2;
        uint64_t length = 1;
        ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset,
                                                             length, &status),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);

        // We still don't have access to the next slice...
        ASSERT_TRUE(CheckNoAccessBlock(vfd, last_block, 2));
        ASSERT_TRUE(CheckNoAccessBlock(vfd, last_block + 1, 1));

        // But we have access to the slice we asked for!
        size_t requested_block = (offset * slice_size) / block_info.block_size;
        ASSERT_TRUE(CheckWriteReadBlock(vfd, requested_block, 1));

        vparts[i].slices_used++;
        vparts[i].last_slice = offset;
        i = (i + 1) % kNumVParts;
    }

    for (size_t i = 0; i < kNumVParts; i++) {
        ASSERT_EQ(close(vparts[i].fd.release()), 0);
    }

    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, slice_size));
    ASSERT_TRUE(ValidateFVM(ramdisk_path));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test that the FVM driver actually persists updates.
bool TestPersistenceSimple() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    constexpr uint64_t kBlkSize = 512;
    constexpr uint64_t kBlkCount = 1 << 20;
    constexpr uint64_t kSliceSize = 64 * (1 << 20);
    ASSERT_EQ(StartFVMTest(kBlkSize, kBlkCount, kSliceSize, ramdisk_path, fvm_driver), 0);

    constexpr uint64_t kDiskSize = kBlkSize * kBlkCount;
    size_t slices_left = fvm::UsableSlicesCount(kDiskSize, kSliceSize);
    const uint64_t kSliceCount = slices_left;

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;

    // Allocate one VPart
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);
    slices_left--;

    fzl::UnownedFdioCaller partition_caller(vp_fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());

    // Check that the name matches what we provided
    char name[fvm::kMaxVPartitionNameLength + 1];
    zx_status_t status;
    size_t actual;
    ASSERT_EQ(fuchsia_hardware_block_partition_PartitionGetName(partition_channel->get(), &status,
                                                                name, sizeof(name), &actual),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    name[actual] = '\0';
    ASSERT_EQ(memcmp(name, kTestPartName1, strlen(kTestPartName1)), 0);
    fuchsia_hardware_block_BlockInfo block_info;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    fbl::unique_ptr<uint8_t[]> buf(new uint8_t[block_info.block_size * 2]);

    // Check that we can read from / write to it
    ASSERT_TRUE(CheckWrite(vp_fd.get(), 0, block_info.block_size, buf.get()));
    ASSERT_TRUE(CheckRead(vp_fd.get(), 0, block_info.block_size, buf.get()));
    ASSERT_EQ(close(vp_fd.release()), 0);

    // Check that it still exists after rebinding the driver
    const partition_entry_t entries[] = {
        {kTestPartName1, 1},
    };
    fd = FVMRebind(std::move(fd), ramdisk_path, entries, 1);
    ASSERT_TRUE(fd, "Failed to rebind FVM driver");

    vp_fd.reset(open_partition(kTestUniqueGUID, kTestPartGUIDData, 0, nullptr));
    ASSERT_TRUE(vp_fd, "Couldn't re-open Data VPart");
    ASSERT_TRUE(CheckRead(vp_fd.get(), 0, block_info.block_size, buf.get()));

    // Try extending the vpartition, and checking that the extension persists.
    // This is the last 'accessible' block.
    size_t last_block = (slice_size / block_info.block_size) - 1;
    ASSERT_TRUE(CheckWrite(vp_fd.get(), block_info.block_size * last_block, block_info.block_size,
                           &buf[0]));
    ASSERT_TRUE(
        CheckRead(vp_fd.get(), block_info.block_size * last_block, block_info.block_size, &buf[0]));

    // Try writing out of bounds -- check that we don't have access.
    ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), (slice_size / block_info.block_size) - 1, 2));
    ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), slice_size / block_info.block_size, 1));

    partition_caller.reset(vp_fd.get());
    partition_channel = zx::unowned_channel(partition_caller.borrow_channel());
    uint64_t offset = 1;
    uint64_t length = 1;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset, length,
                                                         &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    slices_left--;

    // Rebind the FVM driver, check the extension has succeeded.
    fd = FVMRebind(std::move(fd), ramdisk_path, entries, 1);
    ASSERT_TRUE(fd, "Failed to rebind FVM driver");
    partition_caller.reset(vp_fd.get());
    partition_channel = zx::unowned_channel(partition_caller.borrow_channel());

    // Now we can access the next slice...
    ASSERT_TRUE(CheckWrite(vp_fd.get(), block_info.block_size * (last_block + 1),
                           block_info.block_size, &buf[block_info.block_size]));
    ASSERT_TRUE(CheckRead(vp_fd.get(), block_info.block_size * (last_block + 1),
                          block_info.block_size, &buf[block_info.block_size]));
    // ... We can still access the previous slice...
    ASSERT_TRUE(
        CheckRead(vp_fd.get(), block_info.block_size * last_block, block_info.block_size, &buf[0]));
    // ... And we can cross slices
    ASSERT_TRUE(CheckRead(vp_fd.get(), block_info.block_size * last_block,
                          block_info.block_size * 2, &buf[0]));

    // Try allocating the rest of the slices, rebinding, and ensuring
    // that the size stays updated.
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, kSliceSize * 2);

    offset = 2;
    length = slices_left;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset, length,
                                                         &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, kSliceSize * kSliceCount);

    ASSERT_EQ(close(vp_fd.release()), 0);
    fd = FVMRebind(std::move(fd), ramdisk_path, entries, 1);
    ASSERT_TRUE(fd, "Failed to rebind FVM driver");

    vp_fd.reset(open_partition(kTestUniqueGUID, kTestPartGUIDData, 0, nullptr));
    ASSERT_TRUE(vp_fd, "Couldn't re-open Data VPart");
    partition_caller.reset(vp_fd.get());
    partition_channel = zx::unowned_channel(partition_caller.borrow_channel());

    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, kSliceSize * kSliceCount);

    ASSERT_EQ(close(vp_fd.release()), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

bool CorruptMountHelper(const char* partition_path, disk_format_t disk_format,
                        const query_request_t& query_request) {
    BEGIN_HELPER;

    // Format the VPart as |disk_format|.
    ASSERT_EQ(mkfs(partition_path, disk_format, launch_stdio_sync, &default_mkfs_options), ZX_OK);

    fbl::unique_fd vp_fd(open_partition(kTestUniqueGUID, kTestPartGUIDData, 0, nullptr));
    ASSERT_TRUE(vp_fd);
    fuchsia_hardware_block_volume_VsliceRange
        ranges[fuchsia_hardware_block_volume_MAX_SLICE_REQUESTS];
    zx_status_t status;
    size_t actual_ranges_count;

    // Check initial slice allocation.
    //
    // Avoid keping the "FdioCaller" in-scope across mount, as the caller prevents
    // the file descriptor from being transferred.
    {
        fzl::UnownedFdioCaller partition_caller(vp_fd.get());
        zx::unowned_channel partition_channel(partition_caller.borrow_channel());
        ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                      partition_channel->get(), query_request.vslice_start, query_request.count,
                      &status, ranges, &actual_ranges_count),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);
        ASSERT_EQ(query_request.count, actual_ranges_count);

        for (unsigned i = 0; i < actual_ranges_count; i++) {
            ASSERT_TRUE(ranges[i].allocated);
            ASSERT_EQ(ranges[i].count, 1);
        }

        // Manually shrink slices so FVM will differ from the partition.
        uint64_t offset = query_request.vslice_start[0];
        uint64_t length = 1;
        ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), offset,
                                                             length, &status),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);

        // Check slice allocation after manual grow/shrink
        ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                      partition_channel->get(), query_request.vslice_start, query_request.count,
                      &status, ranges, &actual_ranges_count),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);
        ASSERT_EQ(query_request.count, actual_ranges_count);
        ASSERT_FALSE(ranges[0].allocated);
        ASSERT_EQ(ranges[0].count, query_request.vslice_start[1] - query_request.vslice_start[0]);
    }

    // Try to mount the VPart.
    ASSERT_NE(
        mount(vp_fd.release(), kMountPath, disk_format, &default_mount_options, launch_stdio_async),
        ZX_OK);

    {
        vp_fd.reset(open_partition(kTestUniqueGUID, kTestPartGUIDData, 0, nullptr));
        ASSERT_TRUE(vp_fd);

        fzl::UnownedFdioCaller partition_caller(vp_fd.get());
        zx::unowned_channel partition_channel(partition_caller.borrow_channel());

        // Grow back the slice we shrunk earlier.
        uint64_t offset = query_request.vslice_start[0];
        uint64_t length = 1;
        ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset,
                                                             length, &status),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);

        // Verify grow was successful.
        ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                      partition_channel->get(), query_request.vslice_start, query_request.count,
                      &status, ranges, &actual_ranges_count),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);
        ASSERT_EQ(query_request.count, actual_ranges_count);
        ASSERT_TRUE(ranges[0].allocated);
        ASSERT_EQ(ranges[0].count, 1);

        // Now extend all extents by some number of additional slices.
        for (unsigned i = 0; i < query_request.count; i++) {
            uint64_t offset = query_request.vslice_start[i] + 1;
            uint64_t length = query_request.count - i;
            ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset,
                                                                 length, &status),
                      ZX_OK);
            ASSERT_EQ(status, ZX_OK);
        }

        // Verify that the extensions were successful.
        ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                      partition_channel->get(), query_request.vslice_start, query_request.count,
                      &status, ranges, &actual_ranges_count),
                  ZX_OK);
        ASSERT_EQ(status, ZX_OK);
        ASSERT_EQ(query_request.count, actual_ranges_count);
        for (unsigned i = 0; i < query_request.count; i++) {
            ASSERT_TRUE(ranges[i].allocated);
            ASSERT_EQ(ranges[i].count, 1 + query_request.count - i);
        }
    }

    // Try mount again.
    ASSERT_EQ(
        mount(vp_fd.release(), kMountPath, disk_format, &default_mount_options, launch_stdio_async),
        ZX_OK);
    ASSERT_EQ(umount(kMountPath), ZX_OK);

    vp_fd.reset(open_partition(kTestUniqueGUID, kTestPartGUIDData, 0, nullptr));
    ASSERT_TRUE(vp_fd);
    fzl::UnownedFdioCaller partition_caller(vp_fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());

    // Verify that slices were fixed on mount.
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                  partition_channel->get(), query_request.vslice_start, query_request.count,
                  &status, ranges, &actual_ranges_count),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(query_request.count, actual_ranges_count);

    for (unsigned i = 0; i < query_request.count; i++) {
        ASSERT_TRUE(ranges[i].allocated);
        ASSERT_EQ(ranges[i].count, 1);
    }

    END_HELPER;
}

bool TestCorruptMount() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    size_t slice_size = 1 << 20;
    ASSERT_EQ(StartFVMTest(512, 1 << 20, slice_size, ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    ASSERT_EQ(slice_size, volume_info.slice_size);

    // Allocate one VPart
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);
    ASSERT_EQ(close(vp_fd.release()), 0);

    ASSERT_EQ(mkdir(kMountPath, 0666), 0);

    char partition_path[PATH_MAX];
    snprintf(partition_path, sizeof(partition_path), "%s/%s-p-1/block", fvm_driver, kTestPartName1);

    size_t kMinfsBlocksPerSlice = slice_size / minfs::kMinfsBlockSize;
    query_request_t query_request;
    query_request.count = 4;
    query_request.vslice_start[0] = minfs::kFVMBlockInodeBmStart / kMinfsBlocksPerSlice;
    query_request.vslice_start[1] = minfs::kFVMBlockDataBmStart / kMinfsBlocksPerSlice;
    query_request.vslice_start[2] = minfs::kFVMBlockInodeStart / kMinfsBlocksPerSlice;
    query_request.vslice_start[3] = minfs::kFVMBlockDataStart / kMinfsBlocksPerSlice;

    // Run the test for Minfs.
    ASSERT_TRUE(CorruptMountHelper(partition_path, DISK_FORMAT_MINFS, query_request));

    size_t kBlobfsBlocksPerSlice = slice_size / blobfs::kBlobfsBlockSize;
    query_request.count = 3;
    query_request.vslice_start[0] = blobfs::kFVMBlockMapStart / kBlobfsBlocksPerSlice;
    query_request.vslice_start[1] = blobfs::kFVMNodeMapStart / kBlobfsBlocksPerSlice;
    query_request.vslice_start[2] = blobfs::kFVMDataStart / kBlobfsBlocksPerSlice;

    // Run the test for Blobfs.
    ASSERT_TRUE(CorruptMountHelper(partition_path, DISK_FORMAT_BLOBFS, query_request));

    // Clean up
    ASSERT_EQ(rmdir(kMountPath), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

bool TestVPartitionUpgrade() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    constexpr uint64_t kBlkSize = 512;
    constexpr uint64_t kBlkCount = 1 << 20;
    constexpr uint64_t kSliceSize = 64 * (1 << 20);
    ASSERT_EQ(StartFVMTest(kBlkSize, kBlkCount, kSliceSize, ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd, "Couldn't open Volume Manager");

    fzl::FdioCaller volume_manager(std::move(fd));

    // Short-hand for asking if we can open a partition.
    auto openable = [](const uint8_t* instanceGUID, const uint8_t* typeGUID) {
        fbl::unique_fd fd(open_partition(instanceGUID, typeGUID, 0, nullptr));
        return fd.is_valid();
    };

    // Allocate two VParts, one active, and one inactive.
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.flags = fuchsia_hardware_block_volume_AllocatePartitionFlagInactive;
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(volume_manager.fd().get(), &request));
    ASSERT_TRUE(vp_fd, "Couldn't open Volume");
    ASSERT_EQ(close(vp_fd.release()), 0);

    request.flags = 0;
    memcpy(request.guid, kTestUniqueGUID2, GUID_LEN);
    strcpy(request.name, kTestPartName2);
    vp_fd.reset(fvm_allocate_partition(volume_manager.fd().get(), &request));
    ASSERT_TRUE(vp_fd, "Couldn't open volume");
    ASSERT_EQ(close(vp_fd.release()), 0);

    const partition_entry_t entries[] = {
        {kTestPartName2, 2},
    };
    fd = FVMRebind(volume_manager.release(), ramdisk_path, entries, 1);
    ASSERT_TRUE(fd, "Failed to rebind FVM driver");
    volume_manager.reset(std::move(fd));

    // We shouldn't be able to re-open the inactive partition...
    ASSERT_FALSE(openable(kTestUniqueGUID, kTestPartGUIDData));
    // ... but we SHOULD be able to re-open the active partition.
    ASSERT_TRUE(openable(kTestUniqueGUID2, kTestPartGUIDData));

    // Try to upgrade the partition (from GUID2 --> GUID)
    request.flags = fuchsia_hardware_block_volume_AllocatePartitionFlagInactive;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    fbl::unique_fd new_fd(fvm_allocate_partition(volume_manager.fd().get(), &request));
    ASSERT_TRUE(new_fd, "Couldn't open new volume");
    ASSERT_EQ(close(new_fd.release()), 0);

    ASSERT_TRUE(Upgrade(volume_manager, kTestUniqueGUID2, kTestUniqueGUID, ZX_OK));

    // After upgrading, we should be able to open both partitions
    ASSERT_TRUE(openable(kTestUniqueGUID, kTestPartGUIDData));
    ASSERT_TRUE(openable(kTestUniqueGUID2, kTestPartGUIDData));

    // Rebind the FVM driver, check the upgrade has succeeded.
    // The original (GUID2) should be deleted, and the new partition (GUID)
    // should exist.
    const partition_entry_t upgraded_entries[] = {
        {kTestPartName1, 1},
    };
    fd = FVMRebind(volume_manager.release(), ramdisk_path, upgraded_entries, 1);
    ASSERT_TRUE(fd, "Failed to rebind FVM driver");
    volume_manager.reset(std::move(fd));

    ASSERT_TRUE(openable(kTestUniqueGUID, kTestPartGUIDData));
    ASSERT_FALSE(openable(kTestUniqueGUID2, kTestPartGUIDData));

    // Try upgrading when the "new" version doesn't exist.
    // (It should return an error and have no noticable effect).
    ASSERT_TRUE(Upgrade(volume_manager, kTestUniqueGUID, kTestUniqueGUID2, ZX_ERR_NOT_FOUND));

    fd = FVMRebind(volume_manager.release(), ramdisk_path, upgraded_entries, 1);
    ASSERT_TRUE(fd, "Failed to rebind FVM driver");
    volume_manager.reset(std::move(fd));

    ASSERT_TRUE(openable(kTestUniqueGUID, kTestPartGUIDData));
    ASSERT_FALSE(openable(kTestUniqueGUID2, kTestPartGUIDData));

    // Try upgrading when the "old" version doesn't exist.
    request.flags = fuchsia_hardware_block_volume_AllocatePartitionFlagInactive;
    memcpy(request.guid, kTestUniqueGUID2, GUID_LEN);
    strcpy(request.name, kTestPartName2);
    new_fd.reset(fvm_allocate_partition(volume_manager.fd().get(), &request));
    ASSERT_TRUE(new_fd, "Couldn't open volume");
    ASSERT_EQ(close(new_fd.release()), 0);

    uint8_t fake_guid[GUID_LEN];
    memset(fake_guid, 0, GUID_LEN);
    ASSERT_TRUE(Upgrade(volume_manager, fake_guid, kTestUniqueGUID2, ZX_OK));

    const partition_entry_t upgraded_entries_both[] = {
        {kTestPartName1, 1},
        {kTestPartName2, 2},
    };
    fd = FVMRebind(volume_manager.release(), ramdisk_path, upgraded_entries_both, 2);
    ASSERT_TRUE(fd, "Failed to rebind FVM driver");
    volume_manager.reset(std::move(fd));

    // We should be able to open both partitions again.
    ASSERT_TRUE(openable(kTestUniqueGUID, kTestPartGUIDData));
    ASSERT_TRUE(openable(kTestUniqueGUID2, kTestPartGUIDData));

    // Destroy and reallocate the first partition as inactive.
    vp_fd.reset(open_partition(kTestUniqueGUID, kTestPartGUIDData, 0, nullptr));
    ASSERT_TRUE(vp_fd, "Couldn't open volume");
    fzl::FdioCaller partition_caller(std::move(vp_fd));
    zx_status_t status;
    ASSERT_EQ(
        fuchsia_hardware_block_volume_VolumeDestroy(partition_caller.borrow_channel(), &status),
        ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    partition_caller.reset();
    request.flags = fuchsia_hardware_block_volume_AllocatePartitionFlagInactive;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    new_fd.reset(fvm_allocate_partition(volume_manager.fd().get(), &request));
    ASSERT_TRUE(new_fd, "Couldn't open volume");
    ASSERT_EQ(close(new_fd.release()), 0);

    // Upgrade the partition with old_guid == new_guid.
    // This should activate the partition.
    ASSERT_TRUE(Upgrade(volume_manager, kTestUniqueGUID, kTestUniqueGUID, ZX_OK));

    fd = FVMRebind(volume_manager.release(), ramdisk_path, upgraded_entries_both, 2);
    ASSERT_TRUE(fd, "Failed to rebind FVM driver");
    volume_manager.reset(std::move(fd));

    // We should be able to open both partitions again.
    ASSERT_TRUE(openable(kTestUniqueGUID, kTestPartGUIDData));
    ASSERT_TRUE(openable(kTestUniqueGUID2, kTestPartGUIDData));

    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test that the FVM driver can mount filesystems.
bool TestMounting() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;

    // Allocate one VPart
    alloc_req_t request;
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);

    // Format the VPart as minfs
    char partition_path[PATH_MAX];
    snprintf(partition_path, sizeof(partition_path), "%s/%s-p-1/block", fvm_driver, kTestPartName1);
    ASSERT_EQ(mkfs(partition_path, DISK_FORMAT_MINFS, launch_stdio_sync, &default_mkfs_options),
              ZX_OK);

    // Mount the VPart
    ASSERT_EQ(mkdir(kMountPath, 0666), 0);
    ASSERT_EQ(mount(vp_fd.release(), kMountPath, DISK_FORMAT_MINFS, &default_mount_options,
                    launch_stdio_async),
              ZX_OK);

    // Verify that the mount was successful.
    fbl::unique_fd rootfd(open(kMountPath, O_RDONLY | O_DIRECTORY));
    ASSERT_TRUE(rootfd);
    zx_status_t status;
    filesystem_info_t filesystem_info;
    fzl::FdioCaller caller(std::move(rootfd));
    ASSERT_EQ(fuchsia_io_DirectoryAdminQueryFilesystem(caller.borrow_channel(), &status,
                                                       &filesystem_info),
              ZX_OK);
    const char* kFsName = "minfs";
    const char* name = reinterpret_cast<const char*>(filesystem_info.name);
    ASSERT_EQ(strncmp(name, kFsName, strlen(kFsName)), 0, "Unexpected filesystem mounted");

    // Verify that MinFS does not try to use more of the VPartition than
    // was originally allocated.
    ASSERT_LE(filesystem_info.total_bytes, slice_size * request.slice_count);

    // Clean up.
    ASSERT_EQ(umount(kMountPath), ZX_OK);
    ASSERT_EQ(rmdir(kMountPath), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test that FVM-aware filesystem can be reformatted.
bool TestMkfs() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;

    // Allocate one VPart.
    alloc_req_t request;
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);

    // Format the VPart as minfs.
    char partition_path[PATH_MAX];
    snprintf(partition_path, sizeof(partition_path), "%s/%s-p-1/block", fvm_driver, kTestPartName1);
    ASSERT_EQ(mkfs(partition_path, DISK_FORMAT_MINFS, launch_stdio_sync, &default_mkfs_options),
              ZX_OK);

    // Format it as MinFS again, even though it is already formatted.
    ASSERT_EQ(mkfs(partition_path, DISK_FORMAT_MINFS, launch_stdio_sync, &default_mkfs_options),
              ZX_OK);

    // Now try reformatting as blobfs.
    ASSERT_EQ(mkfs(partition_path, DISK_FORMAT_BLOBFS, launch_stdio_sync, &default_mkfs_options),
              ZX_OK);

    // Demonstrate that mounting as minfs will fail, but mounting as blobfs
    // is successful.
    ASSERT_EQ(mkdir(kMountPath, 0666), 0);
    ASSERT_NE(mount(vp_fd.release(), kMountPath, DISK_FORMAT_MINFS, &default_mount_options,
                    launch_stdio_sync),
              ZX_OK);
    vp_fd.reset(open(partition_path, O_RDWR));
    ASSERT_TRUE(vp_fd);
    ASSERT_EQ(mount(vp_fd.release(), kMountPath, DISK_FORMAT_BLOBFS, &default_mount_options,
                    launch_stdio_async),
              ZX_OK);
    ASSERT_EQ(umount(kMountPath), ZX_OK);

    // ... and reformat back to MinFS again.
    ASSERT_EQ(mkfs(partition_path, DISK_FORMAT_MINFS, launch_stdio_sync, &default_mkfs_options),
              ZX_OK);

    // Mount the VPart.
    vp_fd.reset(open(partition_path, O_RDWR));
    ASSERT_TRUE(vp_fd);
    ASSERT_EQ(mount(vp_fd.release(), kMountPath, DISK_FORMAT_MINFS, &default_mount_options,
                    launch_stdio_async),
              ZX_OK);

    // Verify that the mount was successful.
    fbl::unique_fd rootfd(open(kMountPath, O_RDONLY | O_DIRECTORY));
    ASSERT_TRUE(rootfd);
    zx_status_t status;
    filesystem_info_t filesystem_info;
    fzl::FdioCaller caller(std::move(rootfd));
    ASSERT_EQ(fuchsia_io_DirectoryAdminQueryFilesystem(caller.borrow_channel(), &status,
                                                       &filesystem_info),
              ZX_OK);
    const char* kFsName = "minfs";
    const char* name = reinterpret_cast<const char*>(filesystem_info.name);
    ASSERT_EQ(strncmp(name, kFsName, strlen(kFsName)), 0, "Unexpected filesystem mounted");

    // Verify that MinFS does not try to use more of the VPartition than
    // was originally allocated.
    ASSERT_LE(filesystem_info.total_bytes, slice_size * request.slice_count);

    // Clean up.
    ASSERT_EQ(umount(kMountPath), ZX_OK);
    ASSERT_EQ(rmdir(kMountPath), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Test that the FVM can recover when one copy of
// metadata becomes corrupt.
bool TestCorruptionOk() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];

    size_t kDiskSize = use_real_disk ? test_block_size * test_block_count : 512 * (1 << 20);
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    fbl::unique_fd ramdisk_fd(open(ramdisk_path, O_RDWR));
    ASSERT_TRUE(ramdisk_fd);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;

    // Allocate one VPart (writes to backup)
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);

    // Extend the vpart (writes to primary)
    fzl::UnownedFdioCaller partition_caller(vp_fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());
    zx_status_t status;
    uint64_t offset = 1;
    uint64_t length = 1;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset, length,
                                                         &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    fuchsia_hardware_block_BlockInfo block_info;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, slice_size * 2);

    // Initial slice access
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), 0, 1));
    // Extended slice access
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), slice_size / block_info.block_size, 1));

    ASSERT_EQ(close(vp_fd.release()), 0);

    // Corrupt the (backup) metadata and rebind.
    // The 'primary' was the last one written, so it'll be used.
    off_t off = fvm::BackupStart(kDiskSize, slice_size);
    uint8_t buf[fvm::kBlockSize];
    ASSERT_EQ(lseek(ramdisk_fd.get(), off, SEEK_SET), off);
    ASSERT_EQ(read(ramdisk_fd.get(), buf, sizeof(buf)), sizeof(buf));
    // Modify an arbitrary byte (not the magic bits; we still want it to mount!)
    buf[128]++;
    ASSERT_EQ(lseek(ramdisk_fd.get(), off, SEEK_SET), off);
    ASSERT_EQ(write(ramdisk_fd.get(), buf, sizeof(buf)), sizeof(buf));

    const partition_entry_t entries[] = {
        {kTestPartName1, 1},
    };

    fd = FVMRebind(std::move(fd), ramdisk_path, entries, 1);
    ASSERT_TRUE(fd, "Failed to rebind FVM driver");

    vp_fd.reset(open_partition(kTestUniqueGUID, kTestPartGUIDData, 0, nullptr));
    ASSERT_TRUE(vp_fd, "Couldn't re-open Data VPart");

    // The slice extension is still accessible.
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), 0, 1));
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), slice_size / block_info.block_size, 1));

    // Clean up
    ASSERT_EQ(close(vp_fd.release()), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_EQ(close(ramdisk_fd.release()), 0);

    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

bool TestCorruptionRegression() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);
    fbl::unique_fd ramdisk_fd(open(ramdisk_path, O_RDWR));
    ASSERT_TRUE(ramdisk_fd);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;

    // Allocate one VPart (writes to backup)
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);

    fzl::UnownedFdioCaller partition_caller(vp_fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());
    zx_status_t status;

    // Extend the vpart (writes to primary)
    uint64_t offset = 1;
    uint64_t length = 1;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset, length,
                                                         &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    fuchsia_hardware_block_BlockInfo block_info;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, slice_size * 2);

    // Initial slice access
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), 0, 1));
    // Extended slice access
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), slice_size / block_info.block_size, 1));

    ASSERT_EQ(close(vp_fd.release()), 0);

    // Corrupt the (primary) metadata and rebind.
    // The 'primary' was the last one written, so the backup will be used.
    off_t off = 0;
    uint8_t buf[fvm::kBlockSize];
    ASSERT_EQ(lseek(ramdisk_fd.get(), off, SEEK_SET), off);
    ASSERT_EQ(read(ramdisk_fd.get(), buf, sizeof(buf)), sizeof(buf));
    buf[128]++;
    ASSERT_EQ(lseek(ramdisk_fd.get(), off, SEEK_SET), off);
    ASSERT_EQ(write(ramdisk_fd.get(), buf, sizeof(buf)), sizeof(buf));

    const partition_entry_t entries[] = {
        {kTestPartName1, 1},
    };
    fd = FVMRebind(std::move(fd), ramdisk_path, entries, 1);
    ASSERT_TRUE(fd, "Failed to rebind FVM driver");
    vp_fd.reset(open_partition(kTestUniqueGUID, kTestPartGUIDData, 0, nullptr));
    ASSERT_TRUE(vp_fd);

    // The slice extension is no longer accessible
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), 0, 1));
    ASSERT_TRUE(CheckNoAccessBlock(vp_fd.get(), slice_size / block_info.block_size, 1));

    // Clean up
    ASSERT_EQ(close(vp_fd.release()), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_EQ(close(ramdisk_fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, 64lu * (1 << 20)));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

bool TestCorruptionUnrecoverable() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    const size_t kDiskSize = use_real_disk ? test_block_size * test_block_count : 512 * (1 << 20);
    fbl::unique_fd ramdisk_fd(open(ramdisk_path, O_RDWR));
    ASSERT_TRUE(ramdisk_fd);

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);
    volume_info_t volume_info;
    ASSERT_EQ(fvm_query(fd.get(), &volume_info), ZX_OK);
    size_t slice_size = volume_info.slice_size;

    // Allocate one VPart (writes to backup)
    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    request.slice_count = 1;
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);
    strcpy(request.name, kTestPartName1);
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    fbl::unique_fd vp_fd(fvm_allocate_partition(fd.get(), &request));
    ASSERT_TRUE(vp_fd);

    fzl::UnownedFdioCaller partition_caller(vp_fd.get());
    zx::unowned_channel partition_channel(partition_caller.borrow_channel());
    zx_status_t status;

    // Extend the vpart (writes to primary)
    uint64_t offset = 1;
    uint64_t length = 1;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset, length,
                                                         &status),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    fuchsia_hardware_block_BlockInfo block_info;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(partition_channel->get(), &status, &block_info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_info.block_count * block_info.block_size, slice_size * 2);

    // Initial slice access
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), 0, 1));
    // Extended slice access
    ASSERT_TRUE(CheckWriteReadBlock(vp_fd.get(), slice_size / block_info.block_size, 1));

    ASSERT_EQ(close(vp_fd.release()), 0);

    // Corrupt both copies of the metadata.
    // The 'primary' was the last one written, so the backup will be used.
    off_t off = 0;
    uint8_t buf[fvm::kBlockSize];
    ASSERT_EQ(lseek(ramdisk_fd.get(), off, SEEK_SET), off);
    ASSERT_EQ(read(ramdisk_fd.get(), buf, sizeof(buf)), sizeof(buf));
    buf[128]++;
    ASSERT_EQ(lseek(ramdisk_fd.get(), off, SEEK_SET), off);
    ASSERT_EQ(write(ramdisk_fd.get(), buf, sizeof(buf)), sizeof(buf));
    off = fvm::BackupStart(kDiskSize, slice_size);
    ASSERT_EQ(lseek(ramdisk_fd.get(), off, SEEK_SET), off);
    ASSERT_EQ(read(ramdisk_fd.get(), buf, sizeof(buf)), sizeof(buf));
    buf[128]++;
    ASSERT_EQ(lseek(ramdisk_fd.get(), off, SEEK_SET), off);
    ASSERT_EQ(write(ramdisk_fd.get(), buf, sizeof(buf)), sizeof(buf));

    const partition_entry_t entries[] = {
        {kTestPartName1, 1},
    };
    ASSERT_FALSE(FVMRebind(std::move(fd), ramdisk_path, entries, 1),
                 "FVM Should have failed to rebind");
    ASSERT_TRUE(ValidateFVM(ramdisk_path, ValidationResult::Corrupted));

    // Clean up
    ASSERT_EQ(close(ramdisk_fd.release()), 0);

    // FVM is no longer valid - only need to remove if using ramdisk
    if (!use_real_disk) {
        ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    } else {
        fvm_overwrite(ramdisk_path, slice_size);
    }
    END_TEST;
}

typedef struct {
    // Both in units of "slice"
    size_t start;
    size_t len;
} fvm_extent_t;

typedef struct {
    fbl::unique_fd vp_fd;
    fbl::Vector<fvm_extent_t> extents;
    thrd_t thr;
} fvm_thread_state_t;

template <size_t ThreadCount>
struct fvm_test_state_t {
    size_t block_size;
    size_t slice_size;
    size_t slices_total;
    fvm_thread_state_t thread_states[ThreadCount];

    fbl::Mutex lock;
    size_t slices_left TA_GUARDED(lock);
};

template <size_t ThreadCount>
struct thrd_args_t {
    size_t tid;
    fvm_test_state_t<ThreadCount>* st;
};

template <size_t ThreadCount>
int random_access_thread(void* arg) {
    auto ta = static_cast<thrd_args_t<ThreadCount>*>(arg);
    uint8_t color = static_cast<uint8_t>(ta->tid);
    auto st = ta->st;
    auto self = &st->thread_states[color];

    unsigned int seed = static_cast<unsigned int>(zx_ticks_get());
    unittest_printf("random_access_thread using seed: %u\n", seed);

    // Before we begin, color our first slice.
    // We'll identify our own slices by the "color", which
    // is distinct between threads.
    ASSERT_TRUE(CheckWriteColor(self->vp_fd.get(), 0, st->slice_size, color));
    ASSERT_TRUE(CheckReadColor(self->vp_fd.get(), 0, st->slice_size, color));

    zx_status_t status;
    size_t num_ops = 100;
    for (size_t i = 0; i < num_ops; ++i) {
        switch (rand_r(&seed) % 5) {
        case 0: {
            // Extend and color slice, if possible
            size_t extent_index = rand_r(&seed) % self->extents.size();
            size_t extension_length = 0;
            {
                fbl::AutoLock al(&st->lock);
                if (!st->slices_left) {
                    continue;
                }
                extension_length = fbl::min((rand_r(&seed) % st->slices_left) + 1, 5lu);
                st->slices_left -= extension_length;
            }
            uint64_t offset = self->extents[extent_index].start + self->extents[extent_index].len;
            uint64_t length = extension_length;
            size_t byte_off = offset * st->slice_size;
            size_t byte_len = extension_length * st->slice_size;
            ASSERT_TRUE(CheckNoAccessBlock(self->vp_fd.get(), byte_off / st->block_size,
                                           byte_len / st->block_size));

            fzl::UnownedFdioCaller partition_caller(self->vp_fd.get());
            zx::unowned_channel partition_channel(partition_caller.borrow_channel());
            ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset,
                                                                 length, &status),
                      ZX_OK);
            ASSERT_EQ(status, ZX_OK);
            self->extents[extent_index].len += extension_length;

            ASSERT_TRUE(CheckWriteColor(self->vp_fd.get(), byte_off, byte_len, color));
            ASSERT_TRUE(CheckReadColor(self->vp_fd.get(), byte_off, byte_len, color));
            break;
        }
        case 1: {
            // Allocate a new slice, if possible
            fvm_extent_t extent;
            // Space out the starting offsets far enough that there
            // is no risk of collision between fvm extents
            extent.start = (self->extents.end() - 1)->start + st->slices_total;
            {
                fbl::AutoLock al(&st->lock);
                if (!st->slices_left) {
                    continue;
                }
                extent.len = fbl::min((rand_r(&seed) % st->slices_left) + 1, 5lu);
                st->slices_left -= extent.len;
            }
            uint64_t offset = extent.start;
            uint64_t length = extent.len;
            size_t byte_off = offset * st->slice_size;
            size_t byte_len = extent.len * st->slice_size;
            ASSERT_TRUE(CheckNoAccessBlock(self->vp_fd.get(), byte_off / st->block_size,
                                           byte_len / st->block_size));
            fzl::UnownedFdioCaller partition_caller(self->vp_fd.get());
            zx::unowned_channel partition_channel(partition_caller.borrow_channel());
            ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(partition_channel->get(), offset,
                                                                 length, &status),
                      ZX_OK);
            ASSERT_EQ(status, ZX_OK);
            ASSERT_TRUE(CheckWriteColor(self->vp_fd.get(), byte_off, byte_len, color));
            ASSERT_TRUE(CheckReadColor(self->vp_fd.get(), byte_off, byte_len, color));
            self->extents.push_back(std::move(extent));
            break;
        }
        case 2: {
            // Shrink slice, if possible
            size_t extent_index = rand_r(&seed) % self->extents.size();
            if (self->extents[extent_index].len == 1) {
                continue;
            }
            size_t shrink_length = (rand_r(&seed) % (self->extents[extent_index].len - 1)) + 1;

            uint64_t offset =
                self->extents[extent_index].start + self->extents[extent_index].len - shrink_length;
            uint64_t length = shrink_length;
            size_t byte_off = self->extents[extent_index].start * st->slice_size;
            size_t byte_len = self->extents[extent_index].len * st->slice_size;
            ASSERT_TRUE(CheckReadColor(self->vp_fd.get(), byte_off, byte_len, color));
            fzl::UnownedFdioCaller partition_caller(self->vp_fd.get());
            zx::unowned_channel partition_channel(partition_caller.borrow_channel());
            ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), offset,
                                                                 length, &status),
                      ZX_OK);
            ASSERT_EQ(status, ZX_OK);
            self->extents[extent_index].len -= shrink_length;
            byte_len = self->extents[extent_index].len * st->slice_size;
            ASSERT_TRUE(CheckReadColor(self->vp_fd.get(), byte_off, byte_len, color));
            {
                fbl::AutoLock al(&st->lock);
                st->slices_left += shrink_length;
            }
            break;
        }
        case 3: {
            // Split slice, if possible
            size_t extent_index = rand_r(&seed) % self->extents.size();
            if (self->extents[extent_index].len < 3) {
                continue;
            }
            size_t shrink_length = (rand_r(&seed) % (self->extents[extent_index].len - 2)) + 1;
            uint64_t offset = self->extents[extent_index].start + 1;
            uint64_t length = shrink_length;
            size_t byte_off = self->extents[extent_index].start * st->slice_size;
            size_t byte_len = self->extents[extent_index].len * st->slice_size;
            ASSERT_TRUE(CheckReadColor(self->vp_fd.get(), byte_off, byte_len, color));

            fzl::UnownedFdioCaller partition_caller(self->vp_fd.get());
            zx::unowned_channel partition_channel(partition_caller.borrow_channel());
            ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), offset,
                                                                 length, &status),
                      ZX_OK);
            ASSERT_EQ(status, ZX_OK);

            // We can read the slice before...
            byte_off = self->extents[extent_index].start * st->slice_size;
            byte_len = st->slice_size;
            ASSERT_TRUE(CheckReadColor(self->vp_fd.get(), byte_off, byte_len, color));
            // ... and the slices after...
            byte_off = (self->extents[extent_index].start + 1 + shrink_length) * st->slice_size;
            byte_len = (self->extents[extent_index].len - shrink_length - 1) * st->slice_size;
            ASSERT_TRUE(CheckReadColor(self->vp_fd.get(), byte_off, byte_len, color));
            // ... but not in the middle.
            byte_off = (self->extents[extent_index].start + 1) * st->slice_size;
            byte_len = (shrink_length)*st->slice_size;
            ASSERT_TRUE(CheckNoAccessBlock(self->vp_fd.get(), byte_off / st->block_size,
                                           byte_len / st->block_size));

            // To avoid collisions between test extents, let's remove the
            // trailing extent.
            offset = self->extents[extent_index].start + 1 + shrink_length;
            length = self->extents[extent_index].len - shrink_length - 1;
            ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), offset,
                                                                 length, &status),
                      ZX_OK);
            ASSERT_EQ(status, ZX_OK);

            self->extents[extent_index].len = 1;
            byte_off = self->extents[extent_index].start * st->slice_size;
            byte_len = self->extents[extent_index].len * st->slice_size;
            ASSERT_TRUE(CheckReadColor(self->vp_fd.get(), byte_off, byte_len, color));
            {
                fbl::AutoLock al(&st->lock);
                st->slices_left += shrink_length;
            }
            break;
        }
        case 4: {
            // Deallocate a slice
            size_t extent_index = rand_r(&seed) % self->extents.size();
            if (extent_index == 0) {
                // We must keep the 0th slice
                continue;
            }
            uint64_t offset = self->extents[extent_index].start;
            uint64_t length = self->extents[extent_index].len;
            size_t byte_off = self->extents[extent_index].start * st->slice_size;
            size_t byte_len = self->extents[extent_index].len * st->slice_size;
            ASSERT_TRUE(CheckReadColor(self->vp_fd.get(), byte_off, byte_len, color));

            fzl::UnownedFdioCaller partition_caller(self->vp_fd.get());
            zx::unowned_channel partition_channel(partition_caller.borrow_channel());
            ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(partition_channel->get(), offset,
                                                                 length, &status),
                      ZX_OK);
            ASSERT_EQ(status, ZX_OK);
            ASSERT_TRUE(CheckNoAccessBlock(self->vp_fd.get(), byte_off / st->block_size,
                                           byte_len / st->block_size));
            {
                fbl::AutoLock al(&st->lock);
                st->slices_left += self->extents[extent_index].len;
            }
            for (size_t i = extent_index; i < self->extents.size() - 1; i++) {
                self->extents[i] = std::move(self->extents[i + 1]);
            }
            self->extents.pop_back();
            break;
        }
        }
    }
    return 0;
}

template <size_t ThreadCount, bool Persistence>
bool TestRandomOpMultithreaded() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    const size_t kBlockSize = use_real_disk ? test_block_size : 512;
    const size_t kBlockCount = use_real_disk ? test_block_count : 1 << 20;
    const size_t kBlocksPerSlice = 32;
    const size_t kSliceSize = kBlocksPerSlice * kBlockSize;
    ASSERT_EQ(StartFVMTest(kBlockSize, kBlockCount, kSliceSize, ramdisk_path, fvm_driver), 0);

    const size_t kDiskSize = kBlockSize * kBlockCount;
    const size_t kSlicesCount = fvm::UsableSlicesCount(kDiskSize, kSliceSize);

    if (use_real_disk && kSlicesCount <= ThreadCount * 2) {
        printf("Not enough slices to distribute between threads: ignoring test\n");
        return true;
    }

    ASSERT_GT(kSlicesCount, ThreadCount * 2, "Not enough slices to distribute between threads");

    fvm_test_state_t<ThreadCount> s{};
    s.block_size = kBlockSize;
    s.slice_size = kSliceSize;
    {
        fbl::AutoLock al(&s.lock);
        s.slices_left = kSlicesCount - ThreadCount;
        s.slices_total = kSlicesCount;
    }

    fbl::unique_fd fd(open(fvm_driver, O_RDWR));
    ASSERT_TRUE(fd);

    alloc_req_t request;
    memset(&request, 0, sizeof(request));
    size_t slice_count = 1;
    request.slice_count = slice_count;
    strcpy(request.name, "TestPartition");
    memcpy(request.type, kTestPartGUIDData, GUID_LEN);
    memcpy(request.guid, kTestUniqueGUID, GUID_LEN);

    for (size_t i = 0; i < ThreadCount; i++) {
        // Change the GUID enough to be distinct for each thread
        request.guid[0] = static_cast<uint8_t>(i);
        s.thread_states[i].vp_fd.reset(fvm_allocate_partition(fd.get(), &request));
        ASSERT_TRUE(s.thread_states[i].vp_fd);
    }

    thrd_args_t<ThreadCount> ta[ThreadCount];

    // Initialize and launch all threads
    for (size_t i = 0; i < ThreadCount; i++) {
        ta[i].tid = i;
        ta[i].st = &s;

        EXPECT_EQ(s.thread_states[i].extents.size(), 0);
        fvm_extent_t extent;
        extent.start = 0;
        extent.len = 1;
        s.thread_states[i].extents.push_back(std::move(extent));
        EXPECT_TRUE(CheckWriteReadBlock(s.thread_states[i].vp_fd.get(), 0, kBlocksPerSlice));
        EXPECT_EQ(thrd_create(&s.thread_states[i].thr, random_access_thread<ThreadCount>, &ta[i]),
                  thrd_success);
    }

    if (Persistence) {
        partition_entry_t entries[ThreadCount];

        // Join all threads
        for (size_t i = 0; i < ThreadCount; i++) {
            int r;
            EXPECT_EQ(thrd_join(s.thread_states[i].thr, &r), thrd_success);
            EXPECT_EQ(r, 0);
            EXPECT_EQ(close(s.thread_states[i].vp_fd.release()), 0);
            entries[i].name = request.name;
            entries[i].number = i + 1;
        }

        // Rebind the FVM (simulating rebooting)
        fd = FVMRebind(std::move(fd), ramdisk_path, entries, fbl::count_of(entries));
        ASSERT_TRUE(fd);

        // Re-open all partitions, re-launch the worker threads
        for (size_t i = 0; i < ThreadCount; i++) {
            request.guid[0] = static_cast<uint8_t>(i);
            fbl::unique_fd vp_fd(open_partition(request.guid, request.type, 0, nullptr));
            ASSERT_TRUE(vp_fd);
            s.thread_states[i].vp_fd = std::move(vp_fd);
            EXPECT_EQ(
                thrd_create(&s.thread_states[i].thr, random_access_thread<ThreadCount>, &ta[i]),
                thrd_success);
        }
    }

    // Join all the threads, verify their initial block is still valid, and
    // destroy them.
    for (size_t i = 0; i < ThreadCount; i++) {
        int r;
        EXPECT_EQ(thrd_join(s.thread_states[i].thr, &r), thrd_success);
        EXPECT_EQ(r, 0);
        EXPECT_TRUE(CheckWriteReadBlock(s.thread_states[i].vp_fd.get(), 0, kBlocksPerSlice));

        fzl::FdioCaller partition_caller(std::move(s.thread_states[i].vp_fd));
        ;
        zx_status_t status;
        ASSERT_EQ(
            fuchsia_hardware_block_volume_VolumeDestroy(partition_caller.borrow_channel(), &status),
            ZX_OK);
        ASSERT_EQ(status, ZX_OK);
    }

    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_TRUE(FVMCheckSliceSize(fvm_driver, kSliceSize));
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0, "unmounting FVM");
    END_TEST;
}

// Tests the FVM checker using invalid arguments.
bool TestCheckBadArguments() {
    BEGIN_TEST;
    fvm::Checker checker;
    ASSERT_FALSE(checker.Validate(), "Checker should be missing device, block size");

    checker.SetBlockSize(512);
    ASSERT_FALSE(checker.Validate(), "Checker should be missing device");

    checker.SetBlockSize(0);
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);
    fbl::unique_fd fd(open(ramdisk_path, O_RDWR));
    ASSERT_TRUE(fd);
    checker.SetDevice(std::move(fd));
    ASSERT_FALSE(checker.Validate(), "Checker should be missing block size");

    ASSERT_EQ(EndFVMTest(ramdisk_path), 0);
    END_TEST;
}

// Tests the FVM checker against a just-initialized FVM.
bool TestCheckNewFVM() {
    BEGIN_TEST;
    char ramdisk_path[PATH_MAX];
    char fvm_driver[PATH_MAX];
    ASSERT_EQ(StartFVMTest(512, 1 << 20, 64lu * (1 << 20), ramdisk_path, fvm_driver), 0);

    fbl::unique_fd fd(open(ramdisk_path, O_RDWR));
    ASSERT_TRUE(fd);

    fvm::Checker checker(std::move(fd), 512, true);
    ASSERT_TRUE(checker.Validate());
    ASSERT_EQ(EndFVMTest(ramdisk_path), 0);
    END_TEST;
}

bool TestAbortDriverLoadSmallDevice() {
    BEGIN_TEST;
    constexpr uint64_t kBlkSize = 512;
    constexpr uint64_t kRamdiskBlkCount = 50 * (1 << 20) / kBlkSize;
    constexpr uint64_t kSliceSize = (1 << 20);
    constexpr uint64_t kFvmPartitionBlkCount = 4 * (1llu << 30) / kBlkSize;

    // Write metadata to ramdisk.
    ASSERT_EQ(ramdisk_create(kBlkSize, kRamdiskBlkCount, &test_ramdisk), ZX_OK);
    char disk_path[PATH_MAX];
    strlcpy(disk_path, ramdisk_get_path(test_ramdisk), PATH_MAX);
    fbl::unique_fd ramdisk_fd(open(disk_path, O_RDWR));

    // Init fvm with a partition bigger than the underlying disk.
    fvm_init_with_size(ramdisk_fd.get(), kBlkSize * kFvmPartitionBlkCount, kSliceSize);

    zx_status_t call_status;
    zx::channel fvm_channel;
    // Try to bind an fvm to the disk.
    ASSERT_EQ(fdio_get_service_handle(ramdisk_fd.get(), fvm_channel.reset_and_get_address()),
              ZX_OK);
    ASSERT_EQ(fuchsia_device_ControllerBind(fvm_channel.get(), FVM_DRIVER_LIB,
                                            STRLEN(FVM_DRIVER_LIB), &call_status),
              ZX_OK);
    ASSERT_EQ(call_status, ZX_OK);

    // Ugly way of validating that the driver failed to Load.
    char fvm_path[PATH_MAX];
    snprintf(fvm_path, sizeof(fvm_path), "%s/fvm", disk_path);
    ASSERT_EQ(wait_for_device(fvm_path, ZX_SEC(3)), ZX_ERR_TIMED_OUT);

    // Grow the ramdisk to the appropiate size and bind should suceed.
    ASSERT_EQ(ramdisk_grow(test_ramdisk, kFvmPartitionBlkCount * kBlkSize), ZX_OK);

    ASSERT_EQ(fuchsia_device_ControllerBind(fvm_channel.get(), FVM_DRIVER_LIB,
                                            STRLEN(FVM_DRIVER_LIB), &call_status),
              ZX_OK);
    ASSERT_EQ(call_status, ZX_OK);
    ASSERT_EQ(wait_for_device(fvm_path, ZX_SEC(3)), ZX_OK);
    ASSERT_EQ(EndFVMTest(fvm_path), 0, "unmounting FVM");

    END_TEST;
}
} // namespace

BEGIN_TEST_CASE(fvm_tests)
RUN_TEST_MEDIUM(TestTooSmall)
RUN_TEST_MEDIUM(TestLarge)
RUN_TEST_MEDIUM(TestEmpty)
RUN_TEST_MEDIUM(TestAllocateOne)
RUN_TEST_MEDIUM(TestAllocateMany)
RUN_TEST_MEDIUM(TestCloseDuringAccess)
RUN_TEST_MEDIUM(TestReleaseDuringAccess)
RUN_TEST_MEDIUM(TestDestroyDuringAccess)
RUN_TEST_MEDIUM(TestVPartitionExtend)
RUN_TEST_MEDIUM(TestVPartitionExtendSparse)
RUN_TEST_MEDIUM(TestVPartitionShrink)
RUN_TEST_MEDIUM(TestVPartitionSplit)
RUN_TEST_MEDIUM(TestVPartitionDestroy)
RUN_TEST_MEDIUM(TestVPartitionQuery)
RUN_TEST_MEDIUM(TestSliceAccessContiguous)
RUN_TEST_MEDIUM(TestSliceAccessMany)
RUN_TEST_MEDIUM(TestSliceAccessNonContiguousPhysical)
RUN_TEST_MEDIUM(TestSliceAccessNonContiguousVirtual)
RUN_TEST_MEDIUM(TestPersistenceSimple)
RUN_TEST_LARGE(TestVPartitionUpgrade)
RUN_TEST_LARGE(TestMounting)
RUN_TEST_LARGE(TestMkfs)
RUN_TEST_MEDIUM(TestCorruptionOk)
RUN_TEST_MEDIUM(TestCorruptionRegression)
RUN_TEST_MEDIUM(TestCorruptionUnrecoverable)
RUN_TEST_LARGE((TestRandomOpMultithreaded<1, /* persistent= */ false>))
RUN_TEST_LARGE((TestRandomOpMultithreaded<3, /* persistent= */ false>))
RUN_TEST_LARGE((TestRandomOpMultithreaded<5, /* persistent= */ false>))
RUN_TEST_LARGE((TestRandomOpMultithreaded<10, /* persistent= */ false>))
RUN_TEST_LARGE((TestRandomOpMultithreaded<25, /* persistent= */ false>))
RUN_TEST_LARGE((TestRandomOpMultithreaded<1, /* persistent= */ true>))
RUN_TEST_LARGE((TestRandomOpMultithreaded<3, /* persistent= */ true>))
RUN_TEST_LARGE((TestRandomOpMultithreaded<5, /* persistent= */ true>))
RUN_TEST_LARGE((TestRandomOpMultithreaded<10, /* persistent= */ true>))
RUN_TEST_LARGE((TestRandomOpMultithreaded<25, /* persistent= */ true>))
RUN_TEST_MEDIUM(TestCorruptMount)
RUN_TEST_MEDIUM(TestAbortDriverLoadSmallDevice)
END_TEST_CASE(fvm_tests)

BEGIN_TEST_CASE(fvm_check_tests)
RUN_TEST_SMALL(TestCheckBadArguments);
RUN_TEST_SMALL(TestCheckNewFVM);
END_TEST_CASE(fvm_check_tests)

int main(int argc, char** argv) {
    int i = 1;
    while (i < argc - 1) {
        if (!strcmp(argv[i], "-d")) {
            if (strnlen(argv[i + 1], PATH_MAX) > 0) {
                fbl::unique_fd fd(open(argv[i + 1], O_RDWR));

                if (!fd) {
                    fprintf(stderr, "[fs] Could not open block device\n");
                    return -1;
                }
                fdio_t* io = fdio_unsafe_fd_to_io(fd.get());
                if (io == nullptr) {
                    fprintf(stderr, "[fs] could not convert fd to io\n");
                    return -1;
                }
                zx_status_t call_status;
                size_t path_len;
                zx_status_t status = fuchsia_device_ControllerGetTopologicalPath(
                    fdio_unsafe_borrow_channel(io), &call_status, test_disk_path, PATH_MAX - 1,
                    &path_len);
                fdio_unsafe_release(io);
                if (status == ZX_OK) {
                    status = call_status;
                }
                if (status != ZX_OK) {
                    fprintf(stderr, "[fs] Could not acquire topological path of block device\n");
                    return -1;
                }
                test_disk_path[path_len] = 0;

                fzl::UnownedFdioCaller disk_caller(fd.get());
                fuchsia_hardware_block_BlockInfo block_info;
                zx_status_t io_status = fuchsia_hardware_block_BlockGetInfo(
                    disk_caller.borrow_channel(), &status, &block_info);
                if (io_status != ZX_OK || status != ZX_OK) {
                    fprintf(stderr, "[fs] Could not query block device info\n");
                    return -1;
                }

                // If there is already an FVM on this partition, remove it
                fvm_destroy(test_disk_path);

                use_real_disk = true;
                test_block_size = block_info.block_size;
                test_block_count = block_info.block_count;
                break;
            }
        }
        i += 1;
    }

    // Initialize tmpfs.
    async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
    if (loop.StartThread() != ZX_OK) {
        fprintf(stderr, "Error: Cannot initialize local tmpfs loop\n");
        return -1;
    }
    if (memfs_install_at(loop.dispatcher(), kTmpfsPath) != ZX_OK) {
        fprintf(stderr, "Error: Cannot install local tmpfs\n");
        return -1;
    }

    return unittest_run_all_tests(argc, argv) ? 0 : -1;
}
