// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <climits>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include <block-client/cpp/client.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/array.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <fbl/unique_fd.h>
#include <fbl/unique_ptr.h>
#include <fuchsia/hardware/block/c/fidl.h>
#include <fuchsia/hardware/block/partition/c/fidl.h>
#include <fuchsia/hardware/ramdisk/c/fidl.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <lib/fdio/watcher.h>
#include <lib/fzl/fdio.h>
#include <lib/fzl/fifo.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/sync/completion.h>
#include <lib/zx/channel.h>
#include <lib/zx/fifo.h>
#include <lib/zx/time.h>
#include <lib/zx/vmo.h>
#include <ramdevice-client/ramdisk.h>
#include <unittest/unittest.h>
#include <zircon/boot/image.h>
#include <zircon/device/block.h>
#include <zircon/syscalls.h>

#include <utility>

namespace tests {

using devmgr_integration_test::IsolatedDevmgr;
using devmgr_integration_test::RecursiveWaitForFile;

static ramdisk_client_t* GetRamdisk(uint64_t blk_size, uint64_t blk_count,
                                    const uint8_t* guid = nullptr, size_t guid_len = 0) {
    ramdisk_client_t* ramdisk = nullptr;
    zx_status_t rc = guid ? ramdisk_create_with_guid(blk_size, blk_count, guid, guid_len, &ramdisk)
                          : ramdisk_create(blk_size, blk_count, &ramdisk);
    if (rc != ZX_OK) {
        return nullptr;
    }

    return ramdisk;
}

// Small wrapper around the ramdisk which can be used to ensure the device
// is removed, even if the test fails.
class RamdiskTest {
public:
    static bool Create(uint64_t blk_size, uint64_t blk_count, fbl::unique_ptr<RamdiskTest>* out) {
        BEGIN_HELPER;
        ramdisk_client_t* ramdisk = GetRamdisk(blk_size, blk_count);
        ASSERT_NONNULL(ramdisk);
        *out = fbl::unique_ptr<RamdiskTest>(new RamdiskTest(ramdisk));
        END_HELPER;
    }

    static bool CreateWithGuid(uint64_t blk_size, uint64_t blk_count, const uint8_t* guid,
                               size_t guid_len, fbl::unique_ptr<RamdiskTest>* out) {
        BEGIN_HELPER;
        ramdisk_client_t* ramdisk = GetRamdisk(blk_size, blk_count, guid, guid_len);
        ASSERT_NONNULL(ramdisk);
        *out = fbl::unique_ptr<RamdiskTest>(new RamdiskTest(ramdisk));
        END_HELPER;
    }

    bool Terminate() {
        BEGIN_HELPER;
        if (ramdisk_) {
            ASSERT_EQ(ramdisk_destroy(ramdisk_), ZX_OK);
            ramdisk_ = nullptr;
        }
        END_HELPER;
    }

    ~RamdiskTest() { Terminate(); }

    int block_fd() const { return ramdisk_get_block_fd(ramdisk_); }

    const ramdisk_client_t* ramdisk_client() const { return ramdisk_; }

private:
    RamdiskTest(ramdisk_client_t* ramdisk) : ramdisk_(ramdisk) {}

    ramdisk_client_t* ramdisk_;
};

static bool RamdiskTestWaitForDevice(void) {
    BEGIN_TEST;

    EXPECT_EQ(wait_for_device("/", ZX_SEC(1)), ZX_ERR_BAD_PATH);

    char path[PATH_MAX];
    char mod[PATH_MAX];
    ramdisk_client_t* ramdisk = nullptr;
    ASSERT_EQ(ramdisk_create(512, 64, &ramdisk), ZX_OK);
    strlcpy(path, ramdisk_get_path(ramdisk), sizeof(path));

    // Null path/zero timeout
    EXPECT_EQ(wait_for_device(path, 0), ZX_ERR_INVALID_ARGS);
    EXPECT_EQ(wait_for_device(nullptr, ZX_SEC(1)), ZX_ERR_INVALID_ARGS);

    // Trailing slash:
    // .../ramdisk-xxx/block/
    snprintf(mod, sizeof(mod), "%s/", path);
    EXPECT_EQ(wait_for_device(mod, ZX_SEC(1)), ZX_OK);

    // Repeated slashes/empty path segment:
    // .../ramdisk-xxx//block
    char* sep = strrchr(path, '/');
    ASSERT_NONNULL(sep);
    size_t off = sep - path;
    snprintf(&mod[off], sizeof(mod) - off, "/%s", sep);
    EXPECT_EQ(wait_for_device(mod, ZX_SEC(1)), ZX_OK);

    // .../ramdisk-xxx/block
    EXPECT_EQ(wait_for_device(path, ZX_SEC(1)), ZX_OK);
    ASSERT_GE(ramdisk_destroy(ramdisk), 0, "Could not destroy ramdisk device");

    END_TEST;
}

static bool RamdiskTestSimple(void) {
    uint8_t buf[PAGE_SIZE];
    uint8_t out[PAGE_SIZE];

    BEGIN_TEST;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(PAGE_SIZE / 2, 512, &ramdisk));
    memset(buf, 'a', sizeof(buf));
    memset(out, 0, sizeof(out));

    // Write a page and a half
    ASSERT_EQ(write(ramdisk->block_fd(), buf, sizeof(buf)), (ssize_t)sizeof(buf));
    ASSERT_EQ(write(ramdisk->block_fd(), buf, sizeof(buf) / 2), (ssize_t)(sizeof(buf) / 2));

    // Seek to the start of the device and read the contents
    ASSERT_EQ(lseek(ramdisk->block_fd(), 0, SEEK_SET), 0);
    ASSERT_EQ(read(ramdisk->block_fd(), out, sizeof(out)), (ssize_t)sizeof(out));
    ASSERT_EQ(memcmp(out, buf, sizeof(out)), 0);

    END_TEST;
}

static bool RamdiskTestStats(void) {

    BEGIN_TEST;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    constexpr size_t kBlockSize = 512;
    constexpr size_t kBlockCount = 512;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, kBlockCount, &ramdisk));
    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());

    constexpr size_t kBlocksToWrite = 2;
    uint8_t buf[kBlockSize * kBlocksToWrite];
    memset(buf, 'a', sizeof(buf));

    // Query stats. Until we have isolated devmgr integration, only query write
    // stats to avoid a race condition with the block watcher.
    bool clear = true;
    zx_status_t status;
    fuchsia_hardware_block_BlockStats block_stats;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetStats(channel->get(), clear,
                                                   &status, &block_stats), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_stats.writes, 0);
    ASSERT_EQ(block_stats.blocks_written, 0);

    // Write a couple blocks to the device.
    ASSERT_EQ(write(ramdisk->block_fd(), buf, sizeof(buf)), (ssize_t)sizeof(buf));

    // Observe that those writes are measurable via stats.
    ASSERT_EQ(fuchsia_hardware_block_BlockGetStats(channel->get(), clear,
                                                   &status, &block_stats), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(block_stats.writes, 1);
    ASSERT_EQ(block_stats.blocks_written, kBlocksToWrite);

    END_TEST;
}

static bool RamdiskGrowTestDimensionsChange(void) {
    BEGIN_TEST;
    constexpr size_t kBlockCount = 512;
    constexpr size_t kBlockSize = PAGE_SIZE / 2;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, kBlockCount, &ramdisk));

    // Grow the ramdisk.
    ASSERT_EQ(ramdisk_grow(ramdisk->ramdisk_client(), 2 * kBlockSize * kBlockCount), ZX_OK,
              "Failed to grow ramdisk");

    // Check new block count.
    fuchsia_hardware_block_BlockInfo info;
    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx_status_t status;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetInfo(ramdisk_connection.borrow_channel(), &status,
                                                  &info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(info.block_count, 2 * kBlockCount);
    ASSERT_EQ(info.block_size, kBlockSize);
    END_TEST;
}

static bool RamdiskGrowTestReadFromOldBlocks(void) {
    uint8_t buf[PAGE_SIZE];
    uint8_t out[PAGE_SIZE];
    constexpr size_t kBlockCount = 512;
    constexpr size_t kBlockSize = PAGE_SIZE / 2;

    BEGIN_TEST;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, kBlockCount, &ramdisk));
    memset(buf, 'a', sizeof(buf));
    memset(out, 0, sizeof(out));

    // Write a page and a half
    ASSERT_EQ(write(ramdisk->block_fd(), buf, sizeof(buf)), static_cast<ssize_t>(sizeof(buf)));
    ASSERT_EQ(write(ramdisk->block_fd(), buf, sizeof(buf) / 2),
              static_cast<ssize_t>(sizeof(buf) / 2));

    // Grow the ramdisk.
    ASSERT_EQ(ramdisk_grow(ramdisk->ramdisk_client(), 2 * kBlockSize * kBlockCount), ZX_OK,
              "Failed to grow ramdisk");

    // Seek to the start of the device and read the contents
    ASSERT_EQ(lseek(ramdisk->block_fd(), 0, SEEK_SET), 0);
    ASSERT_EQ(read(ramdisk->block_fd(), out, sizeof(out)), static_cast<ssize_t>(sizeof(out)));
    ASSERT_EQ(memcmp(out, buf, sizeof(out)), 0);

    END_TEST;
}

static bool RamdiskGrowTestWriteToAddedBlocks(void) {
    uint8_t buf[PAGE_SIZE];
    uint8_t out[PAGE_SIZE];
    constexpr size_t kBlockCount = 512;
    constexpr size_t kBlockSize = PAGE_SIZE / 2;

    BEGIN_TEST;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, kBlockCount, &ramdisk));
    memset(buf, 'a', sizeof(buf));
    memset(out, 0, sizeof(out));

    // Grow the ramdisk.
    ASSERT_EQ(ramdisk_grow(ramdisk->ramdisk_client(), 2 * kBlockSize * kBlockCount), ZX_OK,
              "Failed to grow ramdisk");

    // Write a page and a half
    ASSERT_EQ(lseek(ramdisk->block_fd(), kBlockSize * kBlockCount, SEEK_SET),
              kBlockSize * kBlockCount, strerror(errno));
    ASSERT_EQ(write(ramdisk->block_fd(), buf, sizeof(buf)), static_cast<ssize_t>(sizeof(buf)));
    ASSERT_EQ(write(ramdisk->block_fd(), buf, sizeof(buf) / 2),
              static_cast<ssize_t>(sizeof(buf) / 2));

    // Verify written data is readable from the new blocks.
    ASSERT_EQ(lseek(ramdisk->block_fd(), kBlockCount * kBlockSize, SEEK_SET),
              kBlockSize * kBlockCount);
    ASSERT_EQ(read(ramdisk->block_fd(), out, sizeof(out)), static_cast<ssize_t>(sizeof(out)));
    ASSERT_EQ(memcmp(out, buf, sizeof(out)), 0);

    END_TEST;
}

static bool RamdiskTestGuid(void) {
    constexpr uint8_t kGuid[ZBI_PARTITION_GUID_LEN] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
                                                       0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF};

    BEGIN_TEST;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::CreateWithGuid(PAGE_SIZE / 2, 512, kGuid, sizeof(kGuid), &ramdisk));

    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    fuchsia_hardware_block_partition_GUID guid;
    ASSERT_EQ(fuchsia_hardware_block_partition_PartitionGetTypeGuid(channel->get(),
                                                                    &status, &guid), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    static_assert(sizeof(guid) == sizeof(kGuid), "Mismatched GUID size");
    ASSERT_TRUE(memcmp(guid.value, kGuid, sizeof(guid)) == 0);

    END_TEST;
}

static bool RamdiskTestVmo(void) {
    BEGIN_TEST;

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(256 * PAGE_SIZE, 0, &vmo), ZX_OK);

    ramdisk_client_t* ramdisk = nullptr;
    ASSERT_EQ(ramdisk_create_from_vmo(vmo.release(), &ramdisk), ZX_OK);
    int block_fd = ramdisk_get_block_fd(ramdisk);

    uint8_t buf[PAGE_SIZE * 2];
    uint8_t out[PAGE_SIZE * 2];
    memset(buf, 'a', sizeof(buf));
    memset(out, 0, sizeof(out));

    EXPECT_EQ(write(block_fd, buf, sizeof(buf)), (ssize_t)sizeof(buf));
    EXPECT_EQ(write(block_fd, buf, sizeof(buf) / 2), (ssize_t)(sizeof(buf) / 2));

    // Seek to the start of the device and read the contents
    EXPECT_EQ(lseek(block_fd, 0, SEEK_SET), 0);
    EXPECT_EQ(read(block_fd, out, sizeof(out)), (ssize_t)sizeof(out));
    EXPECT_EQ(memcmp(out, buf, sizeof(out)), 0);

    EXPECT_GE(ramdisk_destroy(ramdisk), 0, "Could not unlink ramdisk device");

    END_TEST;
}

// This test creates a ramdisk, verifies it is visible in the filesystem
// (where we expect it to be!) and verifies that it is removed when we
// "unplug" the device.
static bool RamdiskTestFilesystem(void) {
    BEGIN_TEST;

    // Make a ramdisk
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(PAGE_SIZE / 2, 512, &ramdisk));
    char ramdisk_path[PATH_MAX];
    strlcpy(ramdisk_path, ramdisk_get_path(ramdisk->ramdisk_client()), sizeof(ramdisk_path));

    // Ramdisk name is of the form: ".../NAME/block"
    // Extract "NAME".
    const char* name_end = strrchr(ramdisk_path, '/');
    const char* name_start = name_end - 1;
    while (*(name_start - 1) != '/')
        name_start--;
    char name[NAME_MAX];
    memcpy(name, name_start, name_end - name_start);
    name[name_end - name_start] = 0;

    // Verify the ramdisk name
    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    size_t actual;
    char out_name[sizeof(name)];
    ASSERT_EQ(fuchsia_hardware_block_partition_PartitionGetName(channel->get(), &status,
                                                                out_name, sizeof(out_name),
                                                                &actual), ZX_OK);
    out_name[actual] = '\0';
    ASSERT_EQ(strnlen(out_name, sizeof(out_name)), strnlen(name, sizeof(name)));
    ASSERT_EQ(strncmp(out_name, name, strnlen(name, sizeof(name))), 0);

    // Find the name of the ramdisk under "/dev/class/block", since it is a block device.
    // Be slightly more lenient with errors during this section, since we might be poking
    // block devices that don't belong to us.
    char blockpath[PATH_MAX];
    strcpy(blockpath, "/dev/class/block/");
    DIR* dir = opendir(blockpath);
    ASSERT_NONNULL(dir);

    typedef struct watcher_args {
        const char* expected_name;
        char* blockpath;
        bool found;
    } watcher_args_t;

    watcher_args_t args;
    args.expected_name = name;
    args.blockpath = blockpath;
    args.found = false;

    auto cb = [](int dirfd, int event, const char* fn, void* cookie) {
        watcher_args_t* args = static_cast<watcher_args_t*>(cookie);
        if (event == WATCH_EVENT_ADD_FILE) {
            fbl::unique_fd fd(openat(dirfd, fn, O_RDONLY));
            if (!fd) {
                return ZX_OK;
            }

            fzl::FdioCaller ramdisk_connection(std::move(fd));
            zx::unowned_channel channel(ramdisk_connection.borrow_channel());
            zx_status_t io_status, status;
            size_t actual;
            char out_name[sizeof(name)];
            io_status = fuchsia_hardware_block_partition_PartitionGetName(
                    channel->get(), &status, out_name, sizeof(out_name), &actual);
            if (io_status == ZX_OK && status == ZX_OK &&
                actual == strlen(args->expected_name) &&
                strncmp(out_name, args->expected_name, strlen(args->expected_name)) == 0) {
                // Found a device under /dev/class/block/XYZ with the name of the
                // ramdisk we originally created.
                strncat(args->blockpath, fn, sizeof(blockpath) - (strlen(args->blockpath) + 1));
                args->found = true;
                return ZX_ERR_STOP;
            }
        }
        return ZX_OK;
    };

    zx_time_t deadline = zx_deadline_after(ZX_SEC(3));
    ASSERT_EQ(fdio_watch_directory(dirfd(dir), cb, deadline, &args), ZX_ERR_STOP);
    ASSERT_TRUE(args.found);
    ASSERT_EQ(closedir(dir), 0, "Could not close /dev/class/block");

    // Check dev block is accessible before destruction
    int devfd = open(blockpath, O_RDONLY);
    ASSERT_GE(devfd, 0, "Ramdisk is not visible in /dev/class/block");
    ASSERT_EQ(close(devfd), 0);

    ASSERT_TRUE(ramdisk->Terminate());
    // Now that we've unlinked the ramdisk, we should notice that it doesn't appear
    // under /dev/class/block.
    ASSERT_EQ(open(blockpath, O_RDONLY), -1, "Ramdisk is visible in /dev after destruction");

    END_TEST;
}

static bool RamdiskTestRebind(void) {
    BEGIN_TEST;

    // Make a ramdisk
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(PAGE_SIZE / 2, 512, &ramdisk));

    // Rebind the ramdisk driver
    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;

    ASSERT_EQ(fuchsia_hardware_block_BlockRebindDevice(channel->get(), &status), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(wait_for_device(ramdisk_get_path(ramdisk->ramdisk_client()), ZX_SEC(3)), ZX_OK);

    END_TEST;
}

bool RamdiskTestBadRequests(void) {
    uint8_t buf[PAGE_SIZE];

    BEGIN_TEST;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(PAGE_SIZE, 512, &ramdisk));
    memset(buf, 'a', sizeof(buf));

    // Read / write non-multiples of the block size
    ASSERT_EQ(write(ramdisk->block_fd(), buf, PAGE_SIZE - 1), -1);
    ASSERT_EQ(errno, EINVAL);
    ASSERT_EQ(write(ramdisk->block_fd(), buf, PAGE_SIZE / 2), -1);
    ASSERT_EQ(errno, EINVAL);
    ASSERT_EQ(read(ramdisk->block_fd(), buf, PAGE_SIZE - 1), -1);
    ASSERT_EQ(errno, EINVAL);
    ASSERT_EQ(read(ramdisk->block_fd(), buf, PAGE_SIZE / 2), -1);
    ASSERT_EQ(errno, EINVAL);

    // Read / write from unaligned offset
    ASSERT_EQ(lseek(ramdisk->block_fd(), 1, SEEK_SET), 1);
    ASSERT_EQ(write(ramdisk->block_fd(), buf, PAGE_SIZE), -1);
    ASSERT_EQ(errno, EINVAL);
    ASSERT_EQ(read(ramdisk->block_fd(), buf, PAGE_SIZE), -1);
    ASSERT_EQ(errno, EINVAL);

    // Read / write at end of device
    off_t offset = PAGE_SIZE * 512;
    ASSERT_EQ(lseek(ramdisk->block_fd(), offset, SEEK_SET), offset);
    ASSERT_EQ(write(ramdisk->block_fd(), buf, PAGE_SIZE), -1);
    ASSERT_EQ(read(ramdisk->block_fd(), buf, PAGE_SIZE), -1);

    END_TEST;
}

bool RamdiskTestReleaseDuringAccess(void) {
    BEGIN_TEST;
    ramdisk_client_t* ramdisk = GetRamdisk(PAGE_SIZE, 512);
    ASSERT_NONNULL(ramdisk);

    // Spin up a background thread to repeatedly access
    // the first few blocks.
    auto bg_thread = [](void* arg) {
        int fd = *reinterpret_cast<int*>(arg);
        while (true) {
            uint8_t in[8192];
            memset(in, 'a', sizeof(in));
            if (write(fd, in, sizeof(in)) != static_cast<ssize_t>(sizeof(in))) {
                return 0;
            }
            uint8_t out[8192];
            memset(out, 0, sizeof(out));
            lseek(fd, 0, SEEK_SET);
            if (read(fd, out, sizeof(out)) != static_cast<ssize_t>(sizeof(out))) {
                return 0;
            }
            // If we DID manage to read it, then the data should be valid...
            if (memcmp(in, out, sizeof(in)) != 0) {
                return -1;
            }
        }
    };

    thrd_t thread;
    int raw_fd = ramdisk_get_block_fd(ramdisk);
    ASSERT_EQ(thrd_create(&thread, bg_thread, &raw_fd), thrd_success);
    // Let the background thread warm up a little bit...
    usleep(10000);
    // ... and close the entire ramdisk from undearneath it!
    ASSERT_EQ(ramdisk_destroy(ramdisk), ZX_OK);

    int res;
    ASSERT_EQ(thrd_join(thread, &res), thrd_success);
    ASSERT_EQ(res, 0, "Background thread failed");
    END_TEST;
}

bool RamdiskTestMultiple(void) {
    uint8_t buf[PAGE_SIZE];
    uint8_t out[PAGE_SIZE];

    BEGIN_TEST;

    fbl::unique_ptr<RamdiskTest> ramdisk1;
    ASSERT_TRUE(RamdiskTest::Create(PAGE_SIZE, 512, &ramdisk1));
    fbl::unique_ptr<RamdiskTest> ramdisk2;
    ASSERT_TRUE(RamdiskTest::Create(PAGE_SIZE, 512, &ramdisk2));

    // Write 'a' to fd1, write 'b', to fd2
    memset(buf, 'a', sizeof(buf));
    ASSERT_EQ(write(ramdisk1->block_fd(), buf, sizeof(buf)), (ssize_t)sizeof(buf));
    memset(buf, 'b', sizeof(buf));
    ASSERT_EQ(write(ramdisk2->block_fd(), buf, sizeof(buf)), (ssize_t)sizeof(buf));

    ASSERT_EQ(lseek(ramdisk1->block_fd(), 0, SEEK_SET), 0);
    ASSERT_EQ(lseek(ramdisk2->block_fd(), 0, SEEK_SET), 0);

    // Read 'b' from fd2, read 'a' from fd1
    ASSERT_EQ(read(ramdisk2->block_fd(), out, sizeof(buf)), (ssize_t)sizeof(buf));
    ASSERT_EQ(memcmp(out, buf, sizeof(out)), 0);
    ASSERT_TRUE(ramdisk2->Terminate(), "Could not unlink ramdisk device");

    memset(buf, 'a', sizeof(buf));
    ASSERT_EQ(read(ramdisk1->block_fd(), out, sizeof(buf)), (ssize_t)sizeof(buf));
    ASSERT_EQ(memcmp(out, buf, sizeof(out)), 0);
    ASSERT_TRUE(ramdisk1->Terminate(), "Could not unlink ramdisk device");

    END_TEST;
}

bool RamdiskTestFifoNoOp(void) {
    // Get a FIFO connection to a ramdisk and immediately close it
    BEGIN_TEST;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(PAGE_SIZE / 2, 512, &ramdisk));

    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());

    auto open_and_close_fifo = [&channel]() {
        BEGIN_HELPER;
        zx_status_t status;
        zx::fifo fifo;
        ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                      fifo.reset_and_get_address()), ZX_OK);
        ASSERT_EQ(status, ZX_OK);
        ASSERT_EQ(fuchsia_hardware_block_BlockCloseFifo(channel->get(), &status), ZX_OK);
        ASSERT_EQ(status, ZX_OK);
        END_HELPER;
    };

    ASSERT_TRUE(open_and_close_fifo());
    ASSERT_TRUE(open_and_close_fifo());

    ASSERT_TRUE(ramdisk->Terminate(), "Could not unlink ramdisk device");
    END_TEST;
}

static void fill_random(uint8_t* buf, uint64_t size) {
    static unsigned int seed = static_cast<unsigned int>(zx_ticks_get());
    // TODO(US-286): Make this easier to reproduce with reliably generated prng.
    unittest_printf("fill_random of %zu bytes with seed: %u\n", size, seed);
    for (size_t i = 0; i < size; i++) {
        buf[i] = static_cast<uint8_t>(rand_r(&seed));
    }
}

bool RamdiskTestFifoBasic(void) {
    BEGIN_TEST;
    // Set up the initial handshake connection with the ramdisk
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(PAGE_SIZE, 512, &ramdisk));

    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    groupid_t group = 0;

    // Create an arbitrary VMO, fill it with some stuff
    uint64_t vmo_size = PAGE_SIZE * 3;
    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(vmo_size, 0, &vmo), ZX_OK, "Failed to create VMO");
    fbl::unique_ptr<uint8_t[]> buf(new uint8_t[vmo_size]);
    fill_random(buf.get(), vmo_size);

    ASSERT_EQ(vmo.write(buf.get(), 0, vmo_size), ZX_OK);

    // Send a handle to the vmo to the block device, get a vmoid which identifies it
    fuchsia_hardware_block_VmoID vmoid;
    zx::vmo xfer_vmo;
    ASSERT_EQ(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &xfer_vmo), ZX_OK);
    ASSERT_EQ(fuchsia_hardware_block_BlockAttachVmo(channel->get(), xfer_vmo.release(), &status,
                                                    &vmoid), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    block_client::Client client;
    ASSERT_EQ(block_client::Client::Create(std::move(fifo), &client), ZX_OK);

    // Batch write the VMO to the ramdisk
    // Split it into two requests, spread across the disk
    block_fifo_request_t requests[2];
    requests[0].group = group;
    requests[0].vmoid = vmoid.id;
    requests[0].opcode = BLOCKIO_WRITE;
    requests[0].length = 1;
    requests[0].vmo_offset = 0;
    requests[0].dev_offset = 0;

    requests[1].group = group;
    requests[1].vmoid = vmoid.id;
    requests[1].opcode = BLOCKIO_WRITE;
    requests[1].length = 2;
    requests[1].vmo_offset = 1;
    requests[1].dev_offset = 100;

    ASSERT_EQ(client.Transaction(&requests[0], fbl::count_of(requests)), ZX_OK);

    // Empty the vmo, then read the info we just wrote to the disk
    fbl::unique_ptr<uint8_t[]> out(new uint8_t[vmo_size]());

    ASSERT_EQ(vmo.write(out.get(), 0, vmo_size), ZX_OK);
    requests[0].opcode = BLOCKIO_READ;
    requests[1].opcode = BLOCKIO_READ;
    ASSERT_EQ(client.Transaction(&requests[0], fbl::count_of(requests)), ZX_OK);
    ASSERT_EQ(vmo.read(out.get(), 0, vmo_size), ZX_OK);
    ASSERT_EQ(memcmp(buf.get(), out.get(), vmo_size), 0, "Read data not equal to written data");

    // Close the current vmo
    requests[0].opcode = BLOCKIO_CLOSE_VMO;
    ASSERT_EQ(client.Transaction(&requests[0], 1), ZX_OK);

    END_TEST;
}

bool RamdiskTestFifoNoGroup(void) {
    BEGIN_TEST;
    // Set up the initial handshake connection with the ramdisk
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(PAGE_SIZE, 512, &ramdisk));

    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo raw_fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  raw_fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    fzl::fifo<block_fifo_request_t, block_fifo_response_t> fifo(std::move(raw_fifo));

    // Create an arbitrary VMO, fill it with some stuff
    uint64_t vmo_size = PAGE_SIZE * 3;
    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(vmo_size, 0, &vmo), ZX_OK, "Failed to create VMO");
    fbl::unique_ptr<uint8_t[]> buf(new uint8_t[vmo_size]);
    fill_random(buf.get(), vmo_size);

    ASSERT_EQ(vmo.write(buf.get(), 0, vmo_size), ZX_OK);

    // Send a handle to the vmo to the block device, get a vmoid which identifies it
    fuchsia_hardware_block_VmoID vmoid;
    zx::vmo xfer_vmo;
    ASSERT_EQ(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &xfer_vmo), ZX_OK);
    ASSERT_EQ(fuchsia_hardware_block_BlockAttachVmo(channel->get(), xfer_vmo.release(), &status,
                                                    &vmoid), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    // Batch write the VMO to the ramdisk
    // Split it into two requests, spread across the disk
    block_fifo_request_t requests[2];
    requests[0].reqid = 0;
    requests[0].vmoid = vmoid.id;
    requests[0].opcode = BLOCKIO_WRITE;
    requests[0].length = 1;
    requests[0].vmo_offset = 0;
    requests[0].dev_offset = 0;

    requests[1].reqid = 1;
    requests[1].vmoid = vmoid.id;
    requests[1].opcode = BLOCKIO_WRITE;
    requests[1].length = 2;
    requests[1].vmo_offset = 1;
    requests[1].dev_offset = 100;

    auto write_request = [&fifo](block_fifo_request_t* request) {
        BEGIN_HELPER;
        size_t actual;
        ASSERT_EQ(fifo.write(request, 1, &actual), ZX_OK);
        ASSERT_EQ(actual, 1);
        END_HELPER;
    };

    auto read_response = [&fifo](reqid_t reqid) {
        BEGIN_HELPER;
        zx::time deadline = zx::deadline_after(zx::sec(1));
        block_fifo_response_t response;
        ASSERT_EQ(fifo.wait_one(ZX_FIFO_READABLE, deadline, nullptr), ZX_OK);
        ASSERT_EQ(fifo.read(&response, 1, nullptr), ZX_OK);
        ASSERT_EQ(response.status, ZX_OK);
        ASSERT_EQ(response.reqid, reqid);
        END_HELPER;
    };

    ASSERT_TRUE(write_request(&requests[0]));
    ASSERT_TRUE(read_response(0));
    ASSERT_TRUE(write_request(&requests[1]));
    ASSERT_TRUE(read_response(1));

    // Empty the vmo, then read the info we just wrote to the disk
    fbl::unique_ptr<uint8_t[]> out(new uint8_t[vmo_size]());

    ASSERT_EQ(vmo.write(out.get(), 0, vmo_size), ZX_OK);

    requests[0].opcode = BLOCKIO_READ;
    requests[1].opcode = BLOCKIO_READ;

    ASSERT_TRUE(write_request(&requests[0]));
    ASSERT_TRUE(read_response(0));
    ASSERT_TRUE(write_request(&requests[1]));
    ASSERT_TRUE(read_response(1));

    ASSERT_EQ(vmo.read(out.get(), 0, vmo_size), ZX_OK);
    ASSERT_EQ(memcmp(buf.get(), out.get(), vmo_size), 0, "Read data not equal to written data");

    // Close the current vmo
    requests[0].opcode = BLOCKIO_CLOSE_VMO;
    ASSERT_EQ(fifo.write(requests, 1, nullptr), ZX_OK);

    END_TEST;
}

typedef struct {
    uint64_t vmo_size;
    zx::vmo vmo;
    fuchsia_hardware_block_VmoID vmoid;
    fbl::unique_ptr<uint8_t[]> buf;
} TestVmoObject;

// Creates a VMO, fills it with data, and gives it to the block device.
//
// TODO(smklein): Operate directly on ramdisk_connection, rather than fd.
bool create_vmo_helper(int fd, TestVmoObject* obj, size_t kBlockSize) {
    BEGIN_HELPER;

    obj->vmo_size = kBlockSize + (rand() % 5) * kBlockSize;
    ASSERT_EQ(zx::vmo::create(obj->vmo_size, 0, &obj->vmo), ZX_OK, "Failed to create vmo");
    obj->buf.reset(new uint8_t[obj->vmo_size]);
    fill_random(obj->buf.get(), obj->vmo_size);
    ASSERT_EQ(obj->vmo.write(obj->buf.get(), 0, obj->vmo_size), ZX_OK,
              "Failed to write to vmo");

    fzl::UnownedFdioCaller ramdisk_connection(fd);
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::vmo xfer_vmo;
    ASSERT_EQ(obj->vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &xfer_vmo), ZX_OK);
    ASSERT_EQ(fuchsia_hardware_block_BlockAttachVmo(channel->get(), xfer_vmo.release(), &status,
                                                    &obj->vmoid), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    END_HELPER;
}

// Write all vmos in a striped pattern on disk.
// For objs.size() == 10,
// i = 0 will write vmo block 0, 1, 2, 3... to dev block 0, 10, 20, 30...
// i = 1 will write vmo block 0, 1, 2, 3... to dev block 1, 11, 21, 31...
bool write_striped_vmo_helper(const block_client::Client* client, TestVmoObject* obj, size_t i,
                              size_t objs, groupid_t group, size_t kBlockSize) {
    // Make a separate request for each block
    size_t blocks = obj->vmo_size / kBlockSize;
    fbl::Array<block_fifo_request_t> requests(new block_fifo_request_t[blocks], blocks);
    for (size_t b = 0; b < blocks; b++) {
        requests[b].group = group;
        requests[b].vmoid = obj->vmoid.id;
        requests[b].opcode = BLOCKIO_WRITE;
        requests[b].length = 1;
        requests[b].vmo_offset = b;
        requests[b].dev_offset = i + b * objs;
    }
    // Write entire vmos at once
    ASSERT_EQ(client->Transaction(&requests[0], requests.size()), ZX_OK);
    return true;
}

// Verifies the result from "write_striped_vmo_helper"
bool read_striped_vmo_helper(const block_client::Client* client, TestVmoObject* obj, size_t i,
                             size_t objs, groupid_t group, size_t kBlockSize) {
    // First, empty out the VMO
    fbl::unique_ptr<uint8_t[]> out(new uint8_t[obj->vmo_size]());
    ASSERT_EQ(obj->vmo.write(out.get(), 0, obj->vmo_size), ZX_OK);

    // Next, read to the vmo from the disk
    size_t blocks = obj->vmo_size / kBlockSize;
    fbl::Array<block_fifo_request_t> requests(new block_fifo_request_t[blocks], blocks);
    for (size_t b = 0; b < blocks; b++) {
        requests[b].group = group;
        requests[b].vmoid = obj->vmoid.id;
        requests[b].opcode = BLOCKIO_READ;
        requests[b].length = 1;
        requests[b].vmo_offset = b;
        requests[b].dev_offset = i + b * objs;
    }
    // Read entire vmos at once
    ASSERT_EQ(client->Transaction(&requests[0], requests.size()), ZX_OK);

    // Finally, write from the vmo to an out buffer, where we can compare
    // the results with the input buffer.
    ASSERT_EQ(obj->vmo.read(out.get(), 0, obj->vmo_size), ZX_OK);
    ASSERT_EQ(memcmp(obj->buf.get(), out.get(), obj->vmo_size), 0,
              "Read data not equal to written data");
    return true;
}

// Tears down an object created by "create_vmo_helper".
bool close_vmo_helper(const block_client::Client* client, TestVmoObject* obj, groupid_t group) {
    block_fifo_request_t request;
    request.group = group;
    request.vmoid = obj->vmoid.id;
    request.opcode = BLOCKIO_CLOSE_VMO;
    ASSERT_EQ(client->Transaction(&request, 1), ZX_OK);
    return true;
}

bool RamdiskTestFifoMultipleVmo(void) {
    BEGIN_TEST;
    // Set up the initial handshake connection with the ramdisk
    const size_t kBlockSize = PAGE_SIZE;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    groupid_t group = 0;
    block_client::Client client;
    ASSERT_EQ(block_client::Client::Create(std::move(fifo), &client), ZX_OK);

    // Create multiple VMOs
    fbl::Array<TestVmoObject> objs(new TestVmoObject[10](), 10);
    for (size_t i = 0; i < objs.size(); i++) {
        ASSERT_TRUE(create_vmo_helper(ramdisk->block_fd(), &objs[i], kBlockSize));
    }

    for (size_t i = 0; i < objs.size(); i++) {
        ASSERT_TRUE(write_striped_vmo_helper(&client, &objs[i], i, objs.size(), group, kBlockSize));
    }

    for (size_t i = 0; i < objs.size(); i++) {
        ASSERT_TRUE(read_striped_vmo_helper(&client, &objs[i], i, objs.size(), group, kBlockSize));
    }

    for (size_t i = 0; i < objs.size(); i++) {
        ASSERT_TRUE(close_vmo_helper(&client, &objs[i], group));
    }

    END_TEST;
}

typedef struct {
    TestVmoObject* obj;
    size_t i;
    size_t objs;
    int fd;
    const block_client::Client* client;
    groupid_t group;
    size_t kBlockSize;
} TestThreadArg;

int fifo_vmo_thread(void* arg) {
    TestThreadArg* fifoarg = (TestThreadArg*)arg;
    TestVmoObject* obj = fifoarg->obj;
    size_t i = fifoarg->i;
    size_t objs = fifoarg->objs;
    int fd = fifoarg->fd;
    const block_client::Client* client = fifoarg->client;
    groupid_t group = fifoarg->group;
    size_t kBlockSize = fifoarg->kBlockSize;

    ASSERT_TRUE(create_vmo_helper(fd, obj, kBlockSize));
    ASSERT_TRUE(write_striped_vmo_helper(client, obj, i, objs, group, kBlockSize));
    ASSERT_TRUE(read_striped_vmo_helper(client, obj, i, objs, group, kBlockSize));
    ASSERT_TRUE(close_vmo_helper(client, obj, group));
    return 0;
}

bool RamdiskTestFifoMultipleVmoMultithreaded(void) {
    BEGIN_TEST;
    // Set up the initial handshake connection with the ramdisk
    const size_t kBlockSize = PAGE_SIZE;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    block_client::Client client;
    ASSERT_EQ(block_client::Client::Create(std::move(fifo), &client), ZX_OK);

    // Create multiple VMOs
    size_t num_threads = MAX_TXN_GROUP_COUNT;
    fbl::Array<TestVmoObject> objs(new TestVmoObject[num_threads](), num_threads);
    fbl::Array<thrd_t> threads(new thrd_t[num_threads](), num_threads);
    fbl::Array<TestThreadArg> thread_args(new TestThreadArg[num_threads](), num_threads);

    for (size_t i = 0; i < num_threads; i++) {
        // Yes, this does create a bunch of duplicate fields, but it's an easy way to
        // transfer some data without creating global variables.
        thread_args[i].obj = &objs[i];
        thread_args[i].i = i;
        thread_args[i].objs = objs.size();
        thread_args[i].fd = ramdisk->block_fd();
        thread_args[i].client = &client;
        thread_args[i].group = static_cast<groupid_t>(i);
        thread_args[i].kBlockSize = kBlockSize;
        ASSERT_EQ(thrd_create(&threads[i], fifo_vmo_thread, &thread_args[i]), thrd_success);
    }

    for (size_t i = 0; i < num_threads; i++) {
        int res;
        ASSERT_EQ(thrd_join(threads[i], &res), thrd_success);
        ASSERT_EQ(res, 0);
    }

    END_TEST;
}

bool RamdiskTestFifoLargeOpsCount(void) {
    BEGIN_TEST;
    // Set up the ramdisk
    const size_t kBlockSize = PAGE_SIZE;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

    // Create a connection to the ramdisk
    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    block_client::Client client;
    ASSERT_EQ(block_client::Client::Create(std::move(fifo), &client), ZX_OK);

    // Create a vmo
    TestVmoObject obj;
    ASSERT_TRUE(create_vmo_helper(ramdisk->block_fd(), &obj, kBlockSize));

    for (size_t num_ops = 1; num_ops <= 32; num_ops++) {
        groupid_t group = 0;

        fbl::Array<block_fifo_request_t> requests(new block_fifo_request_t[num_ops](), num_ops);

        for (size_t b = 0; b < num_ops; b++) {
            requests[b].group = group;
            requests[b].vmoid = obj.vmoid.id;
            requests[b].opcode = BLOCKIO_WRITE;
            requests[b].length = 1;
            requests[b].vmo_offset = 0;
            requests[b].dev_offset = 0;
        }

        ASSERT_EQ(client.Transaction(&requests[0], requests.size()), ZX_OK);
    }

    END_TEST;
}

bool RamdiskTestFifoLargeOpsCountShutdown(void) {
    BEGIN_TEST;
    // Set up the ramdisk
    const size_t kBlockSize = PAGE_SIZE;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

    // Create a connection to the ramdisk
    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    // Create a vmo
    TestVmoObject obj;
    ASSERT_TRUE(create_vmo_helper(ramdisk->block_fd(), &obj, kBlockSize));

    const size_t kNumOps = BLOCK_FIFO_MAX_DEPTH;
    groupid_t group = 0;

    fbl::Array<block_fifo_request_t> requests(new block_fifo_request_t[kNumOps](), kNumOps);

    for (size_t b = 0; b < kNumOps; b++) {
        requests[b].group = group;
        requests[b].vmoid = obj.vmoid.id;
        requests[b].opcode = BLOCKIO_WRITE | BLOCKIO_BARRIER_BEFORE | BLOCKIO_GROUP_ITEM;
        requests[b].length = 1;
        requests[b].vmo_offset = 0;
        requests[b].dev_offset = b;
    }

    // Enqueue multiple barrier-based operations without waiting
    // for completion. The intention here is for the block device
    // server to be busy processing multiple pending operations
    // when the FIFO is suddenly closed, causing "server termination
    // with pending work".
    //
    // It's obviously hit-or-miss whether the server will actually
    // be processing work when we shut down the fifo, but run in a
    // loop, this test was able to trigger deadlocks in a buggy
    // version of the server; as a consequence, it is preserved
    // to help detect regressions.
    size_t actual;
    ZX_ASSERT(fifo.write(sizeof(block_fifo_request_t), &requests[0], requests.size(), &actual) ==
              ZX_OK);
    usleep(100);
    fifo.reset();

    END_TEST;
}

bool RamdiskTestFifoIntermediateOpFailure(void) {
    BEGIN_TEST;
    // Set up the ramdisk
    const size_t kBlockSize = PAGE_SIZE;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

    // Create a connection to the ramdisk
    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    block_client::Client client;
    ASSERT_EQ(block_client::Client::Create(std::move(fifo), &client), ZX_OK);
    groupid_t group = 0;

    constexpr size_t kRequestCount = 3;
    constexpr size_t kBufferSize = kRequestCount * kBlockSize;

    // Create a vmo
    TestVmoObject obj;
    ASSERT_TRUE(create_vmo_helper(ramdisk->block_fd(), &obj, kBufferSize));

    // Store the original value of the VMO
    fbl::unique_ptr<uint8_t[]> originalbuf;
    originalbuf.reset(new uint8_t[kBufferSize]);

    ASSERT_EQ(obj.vmo.read(originalbuf.get(), 0, kBufferSize), ZX_OK);

    // Test that we can use regular transactions (writing)
    block_fifo_request_t requests[kRequestCount];
    for (size_t i = 0; i < fbl::count_of(requests); i++) {
        requests[i].group = group;
        requests[i].vmoid = obj.vmoid.id;
        requests[i].opcode = BLOCKIO_WRITE;
        requests[i].length = 1;
        requests[i].vmo_offset = i;
        requests[i].dev_offset = i;
    }
    ASSERT_EQ(client.Transaction(&requests[0], fbl::count_of(requests)), ZX_OK);

    fbl::unique_ptr<uint8_t[]> tmpbuf;
    tmpbuf.reset(new uint8_t[kBufferSize]);

    for (size_t bad_arg = 0; bad_arg < fbl::count_of(requests); bad_arg++) {
        // Empty out the VMO so we can test reading it
        memset(tmpbuf.get(), 0, kBufferSize);
        ASSERT_EQ(obj.vmo.write(tmpbuf.get(), 0, kBufferSize), ZX_OK);

        // Test that invalid intermediate operations cause:
        // - Previous operations to continue anyway
        // - Later operations to fail
        for (size_t i = 0; i < fbl::count_of(requests); i++) {
            requests[i].group = group;
            requests[i].vmoid = obj.vmoid.id;
            requests[i].opcode = BLOCKIO_READ;
            requests[i].length = 1;
            requests[i].vmo_offset = i;
            requests[i].dev_offset = i;
        }
        // Inserting "bad argument".
        requests[bad_arg].length = 0;
        ASSERT_EQ(client.Transaction(&requests[0], fbl::count_of(requests)), ZX_ERR_INVALID_ARGS);

        // Test that all operations up the bad argument completed, but the later
        // ones did not.
        ASSERT_EQ(obj.vmo.read(tmpbuf.get(), 0, kBufferSize), ZX_OK);

        // First few (successful) operations
        ASSERT_EQ(memcmp(tmpbuf.get(), originalbuf.get(), kBlockSize * bad_arg), 0);
        // Later (failed) operations
        for (size_t i = kBlockSize * (bad_arg + 1); i < kBufferSize; i++) {
            ASSERT_EQ(tmpbuf[i], 0);
        }
    }

    END_TEST;
}

bool RamdiskTestFifoBadClientVmoid(void) {
    // Try to flex the server's error handling by sending 'malicious' client requests.
    BEGIN_TEST;
    // Set up the ramdisk
    const size_t kBlockSize = PAGE_SIZE;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

    // Create a connection to the ramdisk
    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    block_client::Client client;
    ASSERT_EQ(block_client::Client::Create(std::move(fifo), &client), ZX_OK);
    groupid_t group = 0;

    // Create a vmo
    TestVmoObject obj;
    ASSERT_TRUE(create_vmo_helper(ramdisk->block_fd(), &obj, kBlockSize));

    // Bad request: Writing to the wrong vmoid
    block_fifo_request_t request;
    request.group = group;
    request.vmoid = static_cast<vmoid_t>(obj.vmoid.id + 5);
    request.opcode = BLOCKIO_WRITE;
    request.length = 1;
    request.vmo_offset = 0;
    request.dev_offset = 0;
    ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_IO, "Expected IO error with bad vmoid");

    END_TEST;
}

bool RamdiskTestFifoBadClientUnalignedRequest(void) {
    // Try to flex the server's error handling by sending 'malicious' client requests.
    BEGIN_TEST;
    // Set up the ramdisk
    const size_t kBlockSize = PAGE_SIZE;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

    // Create a connection to the ramdisk
    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    block_client::Client client;
    ASSERT_EQ(block_client::Client::Create(std::move(fifo), &client), ZX_OK);
    groupid_t group = 0;

    // Create a vmo of at least size "kBlockSize * 2", since we'll
    // be reading "kBlockSize" bytes from an offset below, and we want it
    // to fit within the bounds of the VMO.
    TestVmoObject obj;
    ASSERT_TRUE(create_vmo_helper(ramdisk->block_fd(), &obj, kBlockSize * 2));

    block_fifo_request_t request;
    request.group = group;
    request.vmoid = static_cast<vmoid_t>(obj.vmoid.id);
    request.opcode = BLOCKIO_WRITE;

    // Send a request that has zero length
    request.length = 0;
    request.vmo_offset = 0;
    request.dev_offset = 0;
    ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_INVALID_ARGS);

    END_TEST;
}

bool RamdiskTestFifoBadClientOverflow(void) {
    // Try to flex the server's error handling by sending 'malicious' client requests.
    BEGIN_TEST;
    // Set up the ramdisk
    const uint64_t kBlockSize = PAGE_SIZE;
    const uint64_t kBlockCount = 1 << 18;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, kBlockCount, &ramdisk));

    // Create a connection to the ramdisk
    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    block_client::Client client;
    ASSERT_EQ(block_client::Client::Create(std::move(fifo), &client), ZX_OK);
    groupid_t group = 0;

    // Create a vmo of at least size "kBlockSize * 2", since we'll
    // be reading "kBlockSize" bytes from an offset below, and we want it
    // to fit within the bounds of the VMO.
    TestVmoObject obj;
    ASSERT_TRUE(create_vmo_helper(ramdisk->block_fd(), &obj, kBlockSize * 2));

    block_fifo_request_t request;
    request.group = group;
    request.vmoid = static_cast<vmoid_t>(obj.vmoid.id);
    request.opcode = BLOCKIO_WRITE;

    // Send a request that is barely out-of-bounds for the device
    request.length = 1;
    request.vmo_offset = 0;
    request.dev_offset = kBlockCount;
    ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

    // Send a request that is half out-of-bounds for the device
    request.length = 2;
    request.vmo_offset = 0;
    request.dev_offset = kBlockCount - 1;
    ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

    // Send a request that is very out-of-bounds for the device
    request.length = 1;
    request.vmo_offset = 0;
    request.dev_offset = kBlockCount + 1;
    ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

    // Send a request that tries to overflow the VMO
    request.length = 2;
    request.vmo_offset = std::numeric_limits<uint64_t>::max();
    request.dev_offset = 0;
    ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

    // Send a request that tries to overflow the device
    request.length = 2;
    request.vmo_offset = 0;
    request.dev_offset = std::numeric_limits<uint64_t>::max();
    ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

    END_TEST;
}

bool RamdiskTestFifoBadClientBadVmo(void) {
    // Try to flex the server's error handling by sending 'malicious' client requests.
    BEGIN_TEST;
    // Set up the ramdisk
    const size_t kBlockSize = PAGE_SIZE;
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(kBlockSize, 1 << 18, &ramdisk));

    // Create a connection to the ramdisk
    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    block_client::Client client;
    ASSERT_EQ(block_client::Client::Create(std::move(fifo), &client), ZX_OK);
    groupid_t group = 0;

    // create a VMO of 1 block, which will round up to PAGE_SIZE
    TestVmoObject obj;
    obj.vmo_size = kBlockSize;
    ASSERT_EQ(zx::vmo::create(obj.vmo_size, 0, &obj.vmo), ZX_OK, "Failed to create vmo");
    obj.buf.reset(new uint8_t[obj.vmo_size]);
    fill_random(obj.buf.get(), obj.vmo_size);
    ASSERT_EQ(obj.vmo.write(obj.buf.get(), 0, obj.vmo_size), ZX_OK,
              "Failed to write to vmo");

    zx::vmo xfer_vmo;
    ASSERT_EQ(obj.vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &xfer_vmo), ZX_OK);
    ASSERT_EQ(fuchsia_hardware_block_BlockAttachVmo(channel->get(), xfer_vmo.release(), &status,
                                                    &obj.vmoid), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    // Send a request to write to write 2 blocks -- even though that's larger than the VMO
    block_fifo_request_t request;
    request.group = group;
    request.vmoid = static_cast<vmoid_t>(obj.vmoid.id);
    request.opcode = BLOCKIO_WRITE;
    request.length = 2;
    request.vmo_offset = 0;
    request.dev_offset = 0;
    ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);
    // Do the same thing, but for reading
    request.opcode = BLOCKIO_READ;
    ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);
    request.length = 2;
    ASSERT_EQ(client.Transaction(&request, 1), ZX_ERR_OUT_OF_RANGE);

    END_TEST;
}

bool RamdiskTestFifoSleepUnavailable(void) {
    BEGIN_TEST;
    // Set up the initial handshake connection with the ramdisk
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(PAGE_SIZE, 512, &ramdisk));

    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    groupid_t group = 0;

    // Create an arbitrary VMO, fill it with some stuff
    uint64_t vmo_size = PAGE_SIZE * 3;
    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(vmo_size, 0, &vmo), ZX_OK, "Failed to create VMO");
    fbl::unique_ptr<uint8_t[]> buf(new uint8_t[vmo_size]);
    fill_random(buf.get(), vmo_size);

    ASSERT_EQ(vmo.write(buf.get(), 0, vmo_size), ZX_OK);

    // Send a handle to the vmo to the block device, get a vmoid which identifies it
    fuchsia_hardware_block_VmoID vmoid;
    zx::vmo xfer_vmo;
    ASSERT_EQ(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &xfer_vmo), ZX_OK);
    ASSERT_EQ(fuchsia_hardware_block_BlockAttachVmo(channel->get(), xfer_vmo.release(), &status,
                                                    &vmoid), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    block_client::Client client;
    ASSERT_EQ(block_client::Client::Create(std::move(fifo), &client), ZX_OK);

    // Put the ramdisk to sleep after 1 block (complete transaction).
    uint64_t one = 1;
    ASSERT_EQ(ramdisk_sleep_after(ramdisk->ramdisk_client(), one), ZX_OK);

    // Batch write the VMO to the ramdisk
    // Split it into two requests, spread across the disk
    block_fifo_request_t requests[2];
    requests[0].group = group;
    requests[0].vmoid = vmoid.id;
    requests[0].opcode = BLOCKIO_WRITE;
    requests[0].length = 1;
    requests[0].vmo_offset = 0;
    requests[0].dev_offset = 0;

    requests[1].group = group;
    requests[1].vmoid = vmoid.id;
    requests[1].opcode = BLOCKIO_WRITE;
    requests[1].length = 2;
    requests[1].vmo_offset = 1;
    requests[1].dev_offset = 100;

    // Send enough requests for the ramdisk to fall asleep before completing.
    // Other callers (e.g. block_watcher) may also send requests without affecting this test.
    ASSERT_EQ(client.Transaction(&requests[0], fbl::count_of(requests)), ZX_ERR_UNAVAILABLE);

    ramdisk_block_write_counts_t counts;
    ASSERT_EQ(ramdisk_get_block_counts(ramdisk->ramdisk_client(), &counts), ZX_OK);
    ASSERT_EQ(counts.received, 3);
    ASSERT_EQ(counts.successful, 1);
    ASSERT_EQ(counts.failed, 2);

    // Wake the ramdisk back up
    ASSERT_EQ(ramdisk_wake(ramdisk->ramdisk_client()), ZX_OK);
    requests[0].opcode = BLOCKIO_READ;
    requests[1].opcode = BLOCKIO_READ;
    ASSERT_EQ(client.Transaction(&requests[0], fbl::count_of(requests)), ZX_OK);

    // Put the ramdisk to sleep after 1 block (partial transaction).
    ASSERT_EQ(ramdisk_sleep_after(ramdisk->ramdisk_client(), one), ZX_OK);

    // Batch write the VMO to the ramdisk.
    // Split it into two requests, spread across the disk.
    requests[0].opcode = BLOCKIO_WRITE;
    requests[0].length = 2;

    requests[1].opcode = BLOCKIO_WRITE;
    requests[1].length = 1;
    requests[1].vmo_offset = 2;

    // Send enough requests for the ramdisk to fall asleep before completing.
    // Other callers (e.g. block_watcher) may also send requests without affecting this test.
    ASSERT_EQ(client.Transaction(&requests[0], fbl::count_of(requests)), ZX_ERR_UNAVAILABLE);

    ASSERT_EQ(ramdisk_get_block_counts(ramdisk->ramdisk_client(), &counts), ZX_OK);
    ASSERT_EQ(counts.received, 3);
    ASSERT_EQ(counts.successful, 1);
    ASSERT_EQ(counts.failed, 2);

    // Wake the ramdisk back up
    ASSERT_EQ(ramdisk_wake(ramdisk->ramdisk_client()), ZX_OK);
    requests[0].opcode = BLOCKIO_READ;
    requests[1].opcode = BLOCKIO_READ;
    ASSERT_EQ(client.Transaction(&requests[0], fbl::count_of(requests)), ZX_OK);

    // Close the current vmo
    requests[0].opcode = BLOCKIO_CLOSE_VMO;
    ASSERT_EQ(client.Transaction(&requests[0], 1), ZX_OK);

    END_TEST;
}

// This thread and its arguments can be used to wake a ramdisk that sleeps with deferred writes.
// The correct calling sequence in the calling thread is:
//   thrd_create(&thread, fifo_wake_thread, &wake);
//   ramdisk_sleep_after(wake->fd, &one);
//   sync_completion_signal(&wake.start);
//   block_fifo_txn(client, requests, fbl::count_of(requests));
//   thrd_join(thread, &res);
//
// This order matters!
// * |sleep_after| must be called from the same thread as |fifo_txn| (or they may be reordered, and
//   the txn counts zeroed).
// * The do-while loop below must not be started before |sleep_after| has been called (hence the
//   'start' signal).
// * This thread must not be waiting when the calling thread blocks in |fifo_txn| (i.e. 'start' must
//   have been signaled.)

typedef struct wake_args {
    const ramdisk_client_t* ramdisk_client;
    uint64_t after;
    sync_completion_t start;
    zx_time_t deadline;
} wake_args_t;

static int fifo_wake_thread(void* arg) {
    ssize_t res;

    // Always send a wake-up call; even if we failed to go to sleep.
    wake_args_t* wake = static_cast<wake_args_t*>(arg);
    auto cleanup = fbl::MakeAutoCall([&] { ramdisk_wake(wake->ramdisk_client); });

    // Wait for the start-up signal
    zx_status_t rc = sync_completion_wait_deadline(&wake->start, wake->deadline);
    sync_completion_reset(&wake->start);
    if (rc != ZX_OK) {
        return rc;
    }

    // Loop until timeout, |wake_after| txns received, or error getting counts
    ramdisk_block_write_counts_t counts;
    do {
        zx::nanosleep(zx::deadline_after(zx::msec(100)));
        if (wake->deadline < zx_clock_get_monotonic()) {
            return ZX_ERR_TIMED_OUT;
        }
        if ((res = ramdisk_get_block_counts(wake->ramdisk_client, &counts)) != ZX_OK) {
            return static_cast<zx_status_t>(res);
        }
    } while (counts.received < wake->after);
    return ZX_OK;
}

bool RamdiskTestFifoSleepDeferred(void) {
    BEGIN_TEST;
    // Set up the initial handshake connection with the ramdisk
    fbl::unique_ptr<RamdiskTest> ramdisk;
    ASSERT_TRUE(RamdiskTest::Create(PAGE_SIZE, 512, &ramdisk));

    fzl::UnownedFdioCaller ramdisk_connection(ramdisk->block_fd());
    zx::unowned_channel channel(ramdisk_connection.borrow_channel());
    zx_status_t status;
    zx::fifo fifo;
    ASSERT_EQ(fuchsia_hardware_block_BlockGetFifo(channel->get(), &status,
                                                  fifo.reset_and_get_address()), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    groupid_t group = 0;

    // Create an arbitrary VMO, fill it with some stuff
    const uint64_t kVmoSize = PAGE_SIZE * 16;
    fzl::VmoMapper mapping;
    zx::vmo vmo;
    ASSERT_EQ(ZX_OK,
              mapping.CreateAndMap(kVmoSize, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, nullptr, &vmo));

    fbl::unique_ptr<uint8_t[]> buf(new uint8_t[kVmoSize]);
    fill_random(buf.get(), kVmoSize);

    ASSERT_EQ(vmo.write(buf.get(), 0, kVmoSize), ZX_OK);

    // Send a handle to the vmo to the block device, get a vmoid which identifies it
    fuchsia_hardware_block_VmoID vmoid;
    zx::vmo xfer_vmo;
    ASSERT_EQ(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &xfer_vmo), ZX_OK);
    ASSERT_EQ(fuchsia_hardware_block_BlockAttachVmo(channel->get(), xfer_vmo.release(), &status,
                                                    &vmoid), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    block_client::Client client;
    ASSERT_EQ(block_client::Client::Create(std::move(fifo), &client), ZX_OK);

    // Create a bunch of requests, some of which are guaranteed to block.
    block_fifo_request_t requests[16];
    for (size_t i = 0; i < fbl::count_of(requests); ++i) {
        requests[i].group = group;
        requests[i].vmoid = vmoid.id;
        requests[i].opcode = BLOCKIO_WRITE;
        requests[i].length = 1;
        requests[i].vmo_offset = i;
        requests[i].dev_offset = i;
    }

    // Sleep and wake parameters
    uint32_t flags = fuchsia_hardware_ramdisk_RAMDISK_FLAG_RESUME_ON_WAKE;
    thrd_t thread;
    wake_args_t wake;
    wake.ramdisk_client = ramdisk->ramdisk_client();
    wake.after = fbl::count_of(requests);
    sync_completion_reset(&wake.start);
    wake.deadline = zx_deadline_after(ZX_SEC(3));
    uint64_t blks_before_sleep = 1;
    int res;

    // Send enough requests to put the ramdisk to sleep and then be awoken wake thread. The ordering
    // below matters!  See the comment on |ramdisk_wake_thread| for details.
    ASSERT_EQ(thrd_create(&thread, fifo_wake_thread, &wake), thrd_success);
    ASSERT_EQ(ramdisk_set_flags(ramdisk->ramdisk_client(), flags), ZX_OK);
    ASSERT_EQ(ramdisk_sleep_after(ramdisk->ramdisk_client(), blks_before_sleep), ZX_OK);
    sync_completion_signal(&wake.start);
    ASSERT_EQ(client.Transaction(&requests[0], fbl::count_of(requests)), ZX_OK);
    ASSERT_EQ(thrd_join(thread, &res), thrd_success);

    // Check that the wake thread succeeded.
    ASSERT_EQ(res, 0, "Background thread failed");

    for (size_t i = 0; i < fbl::count_of(requests); ++i) {
        requests[i].opcode = BLOCKIO_READ;
    }

    // Read data we wrote to disk back into the VMO.
    ASSERT_EQ(client.Transaction(&requests[0], fbl::count_of(requests)), ZX_OK);

    // Verify that the contents of the vmo match the buffer.
    ASSERT_EQ(memcmp(mapping.start(), buf.get(), kVmoSize), 0);

    // Now send 1 transaction with the full length of the VMO.
    requests[0].opcode = BLOCKIO_WRITE;
    requests[0].length = 16;
    requests[0].vmo_offset = 0;
    requests[0].dev_offset = 0;

    // Restart the wake thread and put ramdisk to sleep again.
    wake.after = 1;
    sync_completion_reset(&wake.start);
    ASSERT_EQ(thrd_create(&thread, fifo_wake_thread, &wake), thrd_success);
    ASSERT_EQ(ramdisk_sleep_after(ramdisk->ramdisk_client(), blks_before_sleep), ZX_OK);
    sync_completion_signal(&wake.start);
    ASSERT_EQ(client.Transaction(&requests[0], 1), ZX_OK);
    ASSERT_EQ(thrd_join(thread, &res), thrd_success);

    // Check the wake thread succeeded, and that the contents of the ramdisk match the buffer.
    ASSERT_EQ(res, 0, "Background thread failed");
    requests[0].opcode = BLOCKIO_READ;
    ASSERT_EQ(client.Transaction(&requests[0], 1), ZX_OK);
    ASSERT_EQ(memcmp(mapping.start(), buf.get(), kVmoSize), 0);

    // Check that we can do I/O normally again.
    requests[0].opcode = BLOCKIO_WRITE;
    ASSERT_EQ(client.Transaction(&requests[0], 1), ZX_OK);

    // Close the current vmo
    requests[0].opcode = BLOCKIO_CLOSE_VMO;
    ASSERT_EQ(client.Transaction(&requests[0], 1), ZX_OK);

    END_TEST;
}

static bool RamdiskCreateAt(void) {
    BEGIN_TEST;
    IsolatedDevmgr devmgr;
    auto args = IsolatedDevmgr::DefaultArgs();
    args.disable_block_watcher = false;
    args.sys_device_driver = devmgr_integration_test::IsolatedDevmgr::kSysdevDriver;
    args.load_drivers.push_back(devmgr_integration_test::IsolatedDevmgr::kSysdevDriver);
    args.driver_search_paths.push_back("/boot/driver");
    ASSERT_EQ(IsolatedDevmgr::Create(std::move(args), &devmgr), ZX_OK);

    fbl::unique_fd fd;
    ASSERT_EQ(RecursiveWaitForFile(devmgr.devfs_root(), "misc/ramctl", &fd),
              ZX_OK);
    ramdisk_client_t* ramdisk = nullptr;
    ASSERT_EQ(ramdisk_create_at(devmgr.devfs_root().get(), PAGE_SIZE / 2, 512, &ramdisk),
              ZX_OK);

    ASSERT_EQ(RecursiveWaitForFile(devmgr.devfs_root(), ramdisk_get_path(ramdisk), &fd),
              ZX_OK);
    END_TEST;
}

static bool RamdiskCreateAtGuid(void) {
    constexpr uint8_t kGuid[ZBI_PARTITION_GUID_LEN] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
                                                       0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF};
    BEGIN_TEST;
    IsolatedDevmgr devmgr;
    auto args = IsolatedDevmgr::DefaultArgs();
    args.disable_block_watcher = false;
    args.sys_device_driver = devmgr_integration_test::IsolatedDevmgr::kSysdevDriver;
    args.load_drivers.push_back(devmgr_integration_test::IsolatedDevmgr::kSysdevDriver);
    args.driver_search_paths.push_back("/boot/driver");
    ASSERT_EQ(IsolatedDevmgr::Create(std::move(args), &devmgr), ZX_OK);

    fbl::unique_fd fd;
    ASSERT_EQ(RecursiveWaitForFile(devmgr.devfs_root(), "misc/ramctl", &fd),
              ZX_OK);
    ramdisk_client_t* ramdisk = nullptr;
    ASSERT_EQ(ramdisk_create_at_with_guid(devmgr.devfs_root().get(), PAGE_SIZE / 2, 512, kGuid,
                                          sizeof(kGuid), &ramdisk),
              ZX_OK);

    ASSERT_EQ(RecursiveWaitForFile(devmgr.devfs_root(), ramdisk_get_path(ramdisk), &fd),
              ZX_OK);
    END_TEST;
}

static bool RamdiskCreateAtVmo(void) {
    BEGIN_TEST;
    IsolatedDevmgr devmgr;
    auto args = IsolatedDevmgr::DefaultArgs();
    args.disable_block_watcher = false;
    args.sys_device_driver = devmgr_integration_test::IsolatedDevmgr::kSysdevDriver;
    args.load_drivers.push_back(devmgr_integration_test::IsolatedDevmgr::kSysdevDriver);
    args.driver_search_paths.push_back("/boot/driver");
    ASSERT_EQ(IsolatedDevmgr::Create(std::move(args), &devmgr), ZX_OK);
    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(256 * PAGE_SIZE, 0, &vmo), ZX_OK);

    fbl::unique_fd fd;
    ASSERT_EQ(RecursiveWaitForFile(devmgr.devfs_root(), "misc/ramctl", &fd),
              ZX_OK);
    ramdisk_client_t* ramdisk = nullptr;
    ASSERT_EQ(ramdisk_create_at_from_vmo(devmgr.devfs_root().get(), vmo.release(), &ramdisk),
              ZX_OK);

    ASSERT_EQ(RecursiveWaitForFile(devmgr.devfs_root(), ramdisk_get_path(ramdisk), &fd),
              ZX_OK);
    END_TEST;
}

BEGIN_TEST_CASE(ramdisk_tests)
RUN_TEST_SMALL(RamdiskTestWaitForDevice)
RUN_TEST_SMALL(RamdiskGrowTestDimensionsChange)
RUN_TEST_SMALL(RamdiskGrowTestReadFromOldBlocks)
RUN_TEST_SMALL(RamdiskGrowTestWriteToAddedBlocks)
RUN_TEST_SMALL(RamdiskTestSimple)
RUN_TEST_SMALL(RamdiskTestStats)
RUN_TEST_SMALL(RamdiskTestGuid)
RUN_TEST_SMALL(RamdiskTestVmo)
RUN_TEST_SMALL(RamdiskTestFilesystem)
RUN_TEST_SMALL(RamdiskTestRebind)
RUN_TEST_SMALL(RamdiskTestBadRequests)
RUN_TEST_SMALL(RamdiskTestReleaseDuringAccess)
RUN_TEST_SMALL(RamdiskTestMultiple)
RUN_TEST_SMALL(RamdiskTestFifoNoOp)
RUN_TEST_SMALL(RamdiskTestFifoBasic)
RUN_TEST_SMALL(RamdiskTestFifoNoGroup)
RUN_TEST_SMALL(RamdiskTestFifoMultipleVmo)
RUN_TEST_SMALL(RamdiskTestFifoMultipleVmoMultithreaded)
// TODO(smklein): Test ops across different vmos
RUN_TEST_SMALL(RamdiskTestFifoLargeOpsCount)
RUN_TEST_SMALL(RamdiskTestFifoLargeOpsCountShutdown)
RUN_TEST_SMALL(RamdiskTestFifoIntermediateOpFailure)
RUN_TEST_SMALL(RamdiskTestFifoBadClientVmoid)
RUN_TEST_SMALL(RamdiskTestFifoBadClientUnalignedRequest)
RUN_TEST_SMALL(RamdiskTestFifoBadClientOverflow)
RUN_TEST_SMALL(RamdiskTestFifoBadClientBadVmo)
RUN_TEST_SMALL(RamdiskTestFifoSleepUnavailable)
RUN_TEST_SMALL(RamdiskTestFifoSleepDeferred)
RUN_TEST_MEDIUM(RamdiskCreateAt)
RUN_TEST_MEDIUM(RamdiskCreateAtVmo)
RUN_TEST_MEDIUM(RamdiskCreateAtGuid)
END_TEST_CASE(ramdisk_tests)

} // namespace tests
