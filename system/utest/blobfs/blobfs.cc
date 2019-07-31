// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <atomic>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <threads.h>
#include <type_traits>
#include <utility>
#include <utime.h>

#include <blobfs/format.h>
#include <blobfs/journal.h>
#include <digest/digest.h>
#include <digest/merkle-tree.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/string.h>
#include <fbl/unique_fd.h>
#include <fbl/unique_ptr.h>
#include <fbl/vector.h>
#include <fs-management/fvm.h>
#include <fs-management/mount.h>
#include <fs-test-utils/blobfs/blobfs.h>
#include <fs-test-utils/blobfs/bloblist.h>
#include <fuchsia/device/c/fidl.h>
#include <fuchsia/hardware/ramdisk/c/fidl.h>
#include <fuchsia/io/c/fidl.h>
#include <fvm/format.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/io.h>
#include <lib/fdio/unsafe.h>
#include <lib/fzl/fdio.h>
#include <lib/memfs/memfs.h>
#include <ramdevice-client/ramdisk.h>
#include <unittest/unittest.h>
#include <zircon/device/vfs.h>
#include <zircon/syscalls.h>

#include "blobfs-test.h"

#define TMPFS_PATH "/blobfs-tmp"
#define MOUNT_PATH "/blobfs-tmp/zircon-blobfs-test"

namespace {
using digest::Digest;
using digest::MerkleTree;

// Indicates whether we should enable ramdisk failure tests for the current test run.
bool gEnableRamdiskFailure = false;

// The maximum number of failure loops that should be tested. If set to 0, all of them will be run.
uint64_t gRamdiskFailureLoops = 0;

// Indicates whether we should enable the journal for the current test run.
bool gEnableJournal = true;

// Information about the real disk which must be constructed at runtime, but which persists
// between tests.
bool gUseRealDisk = false;
struct real_disk_info {
    uint64_t blk_size;
    uint64_t blk_count;
    char disk_path[PATH_MAX];
} gRealDiskInfo;

static bool gEnableOutput = true;

static_assert(std::is_pod<real_disk_info>::value, "Global variables should contain exclusively POD"
                                                  "data");

#define RUN_TEST_WRAPPER(test_size, test_name, test_type) \
    RUN_TEST_##test_size((TestWrapper<test_name, test_type>))

#define RUN_TEST_NORMAL(test_size, test_name) \
    RUN_TEST_WRAPPER(test_size, test_name, FsTestType::kNormal)

#define RUN_TEST_FVM(test_size, test_name) \
    RUN_TEST_WRAPPER(test_size, test_name, FsTestType::kFvm)

#define RUN_TESTS(test_size, test_name) \
    RUN_TEST_NORMAL(test_size, test_name) \
    RUN_TEST_FVM(test_size, test_name)

#define RUN_TESTS_SILENT(test_size, test_name) \
    gEnableOutput = false; \
    RUN_TESTS(test_size, test_name) \
    gEnableOutput = true;

// Defines a Blobfs test function which can be passed to the TestWrapper.
typedef bool (*TestFunction)(BlobfsTest* blobfsTest);

// Function which takes in print parameters and does nothing.
static void silent_printf(const char* line, int len, void* arg) {}

// A test wrapper which runs a Blobfs test. If the -f command line argument is used, the test will
// then be run the specified number of additional times, purposely causing the underlying ramdisk
// to fail at certain points.
template <TestFunction TestFunc, FsTestType TestType>
bool TestWrapper(void) {
    BEGIN_TEST;

    // Make sure that we are not testing ramdisk failures with a real disk.
    ASSERT_FALSE(gUseRealDisk && gEnableRamdiskFailure);

    BlobfsTest blobfsTest(TestType);
    blobfsTest.SetStdio(gEnableOutput);
    ASSERT_TRUE(blobfsTest.Init(), "Mounting Blobfs");

    if (gEnableRamdiskFailure) {
        // Sleep and re-wake the ramdisk to ensure that transaction counts have been reset.
        ASSERT_TRUE(blobfsTest.ToggleSleep());
        ASSERT_TRUE(blobfsTest.ToggleSleep());
    }

    // Run the test. This should pass.
    ASSERT_TRUE(TestFunc(&blobfsTest));

    // Get the total number of transactions required for this test.
    uint64_t block_count = 0;
    uint64_t interval = 1;
    uint64_t total = 0;

    if (gEnableRamdiskFailure) {
        // Based on the number of total blocks written and the user provided count of tests,
        // calculate the block interval to test with ramdisk failures.
        ASSERT_TRUE(blobfsTest.GetRamdiskCount(&block_count));

        if (gRamdiskFailureLoops && gRamdiskFailureLoops < block_count) {
            interval = block_count / gRamdiskFailureLoops;
        }
    }

    ASSERT_TRUE(blobfsTest.Teardown(), "Unmounting Blobfs");
    blobfsTest.SetStdio(false);

    // Run the test again, setting the ramdisk to fail after each |interval| blocks.
    for (uint64_t i = 1; i <= block_count; i += interval) {
        if (gRamdiskFailureLoops && total >= gRamdiskFailureLoops) {
            break;
        }

        if (total % 100 == 0) {
            printf("Running ramdisk failure test %" PRIu64 " / %" PRIu64
                   " (block %" PRIu64 " / %" PRIu64 ")\n", total+1,
                   gRamdiskFailureLoops ? gRamdiskFailureLoops : block_count, i, block_count);
        }

        ASSERT_TRUE(blobfsTest.Reset());
        ASSERT_TRUE(blobfsTest.Init(), "Mounting Blobfs");
        ASSERT_TRUE(blobfsTest.ToggleSleep(i));

        // The following test run may fail, but since we don't care, silence any test output.
        unittest_set_output_function(silent_printf, nullptr);

        // We do not care whether the test itself fails or not - regardless, fsck should pass
        // (even if the most recent file system state has not been preserved).
        TestFunc(&blobfsTest);
        current_test_info->all_ok = true;

        ASSERT_TRUE(blobfsTest.ToggleSleep());

        // Restore the default output function.
        unittest_restore_output_function();

        // Forcibly unmount and remount the blobfs partition. With journaling enabled, any
        // inconsistencies should be resolved.
        zx_status_t fsck_result;
        ASSERT_TRUE(blobfsTest.ForceRemount(&fsck_result));

        if (fsck_result != ZX_OK) {
            printf("Detected disk corruption on test %" PRIu64 " / %" PRIu64
                   " (block %" PRIu64 " / %" PRIu64 ")\n", total,
                   gRamdiskFailureLoops ? gRamdiskFailureLoops : block_count, i, block_count);
        }
        // TODO: When we convert to zxtest, print the above error message within this
        // assertion.
        ASSERT_EQ(fsck_result, ZX_OK);

        // The fsck check during Teardown should verify that journal replay was successful.
        ASSERT_TRUE(blobfsTest.Teardown(), "Unmounting Blobfs");
        total += 1;
    }

    END_TEST;
}

#define FVM_DRIVER_LIB "/boot/driver/fvm.so"
#define STRLEN(s) (sizeof(s) / sizeof((s)[0]))

// FVM slice size used for tests
constexpr size_t kTestFvmSliceSize = blobfs::kBlobfsBlockSize; // 8kb
// Minimum blobfs size required by CreateUmountRemountLargeMultithreaded test
constexpr size_t kBytesNormalMinimum = 5 * (1 << 20); // 5mb
// Minimum blobfs size required by ResizePartition test
constexpr size_t kSliceBytesFvmMinimum = 507 * kTestFvmSliceSize;
constexpr size_t kTotalBytesFvmMinimum = fvm::MetadataSize(kSliceBytesFvmMinimum,
                                         kTestFvmSliceSize) * 2 + kSliceBytesFvmMinimum; // ~8.5mb

constexpr uint8_t kTestUniqueGUID[] = {
    0xFF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};
constexpr uint8_t kTestPartGUID[] = {
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0xFF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
};

constexpr size_t kBlocksPerSlice = kTestFvmSliceSize / blobfs::kBlobfsBlockSize;

const fsck_options_t test_fsck_options = {
    .verbose = false,
    .never_modify = true,
    .always_modify = false,
    .force = true,
    .apply_journal = true,
};

BlobfsTest::~BlobfsTest() {
    switch (state_) {
    case FsTestState::kMinimal:
    case FsTestState::kRunning:
    case FsTestState::kError:
        EXPECT_EQ(Teardown(), 0);
        break;
    default:
        break;
    }
}

bool BlobfsTest::Init(FsTestState state) {
    BEGIN_HELPER;
    ASSERT_EQ(state_, FsTestState::kInit);
    auto error = fbl::MakeAutoCall([this](){ state_ = FsTestState::kError; });

    ASSERT_TRUE(mkdir(MOUNT_PATH, 0755) == 0 || errno == EEXIST,
                "Could not create mount point for test filesystems");

    if (gUseRealDisk) {
        strncpy(device_path_, gRealDiskInfo.disk_path, PATH_MAX);
        blk_size_ = gRealDiskInfo.blk_size;
        blk_count_ = gRealDiskInfo.blk_count;
    } else {
        ASSERT_EQ(ramdisk_create(blk_size_, blk_count_, &ramdisk_), ZX_OK,
                  "Blobfs: Could not create ramdisk");
        strlcpy(device_path_, ramdisk_get_path(ramdisk_), sizeof(device_path_));
    }

    if (type_ == FsTestType::kFvm) {
        ASSERT_EQ(kTestFvmSliceSize % blobfs::kBlobfsBlockSize, 0);

        fbl::unique_fd fd(open(device_path_, O_RDWR));
        ASSERT_TRUE(fd, "[FAILED]: Could not open test disk");
        ASSERT_EQ(fvm_init(fd.get(), kTestFvmSliceSize), ZX_OK,
                  "[FAILED]: Could not format disk with FVM");
        fzl::FdioCaller caller(std::move(fd));
        zx_status_t status;
        zx_status_t io_status = fuchsia_device_ControllerBind(caller.borrow_channel(),
                                                              FVM_DRIVER_LIB,
                                                              STRLEN(FVM_DRIVER_LIB), &status);
        ASSERT_EQ(io_status, ZX_OK, "[FAILED]: Could not send bind to FVM driver");
        ASSERT_EQ(status, ZX_OK, "[FAILED]: Could not bind disk to FVM driver");
        caller.reset();

        snprintf(fvm_path_, sizeof(fvm_path_), "%s/fvm", device_path_);
        ASSERT_EQ(wait_for_device(fvm_path_, zx::sec(10).get()), ZX_OK,
                  "[FAILED]: FVM driver never appeared");

        // Open "fvm" driver.
        fbl::unique_fd fvm_fd(open(fvm_path_, O_RDWR));
        ASSERT_GE(fvm_fd.get(), 0, "[FAILED]: Could not open FVM driver");

        // Restore the "fvm_disk_path" to the ramdisk, so it can
        // be destroyed when the test completes.
        fvm_path_[strlen(fvm_path_) - strlen("/fvm")] = 0;

        alloc_req_t request;
        memset(&request, 0, sizeof(request));
        request.slice_count = 1;
        strcpy(request.name, "fs-test-partition");
        memcpy(request.type, kTestPartGUID, sizeof(request.type));
        memcpy(request.guid, kTestUniqueGUID, sizeof(request.guid));

        fd.reset(fvm_allocate_partition(fvm_fd.get(), &request));
        ASSERT_TRUE(fd, "[FAILED]: Could not allocate FVM partition");
        fvm_fd.reset();
        fd.reset(open_partition(kTestUniqueGUID, kTestPartGUID, 0, device_path_));
        ASSERT_TRUE(fd, "[FAILED]: Could not locate FVM partition");
        fd.reset();
    }

    if (state != FsTestState::kMinimal) {
        ASSERT_EQ(state, FsTestState::kRunning);
        ASSERT_EQ(mkfs(device_path_, DISK_FORMAT_BLOBFS, launch_stdio_sync, &default_mkfs_options),
                  ZX_OK);
        ASSERT_TRUE(Mount());
    }

    error.cancel();
    state_ = state;
    END_HELPER;
}

bool BlobfsTest::Remount() {
    BEGIN_HELPER;
    ASSERT_EQ(state_, FsTestState::kRunning);
    auto error = fbl::MakeAutoCall([this](){ state_ = FsTestState::kError; });
    ASSERT_EQ(umount(MOUNT_PATH), ZX_OK, "Failed to unmount blobfs");
    LaunchCallback launch = stdio_ ? launch_stdio_sync : launch_silent_sync;
    ASSERT_EQ(fsck(device_path_, DISK_FORMAT_BLOBFS, &test_fsck_options, launch), ZX_OK,
              "Filesystem fsck failed");
    ASSERT_TRUE(Mount(), "Failed to mount blobfs");
    error.cancel();
    END_HELPER;
}

bool BlobfsTest::ForceRemount(zx_status_t* fsck_result) {
    BEGIN_HELPER;
    // Attempt to unmount, but do not check the result.
    // It is possible that the partition has already been unmounted.
    umount(MOUNT_PATH);

    if (fsck_result != nullptr) {
        *fsck_result = fsck(device_path_, DISK_FORMAT_BLOBFS, &test_fsck_options,
                            launch_silent_sync);
    }

    ASSERT_TRUE(Mount());

    // In the event of success, set state to kRunning, regardless of whether the state was kMinimal
    // before. Since the partition is now mounted, we will need to umount/fsck on Teardown.
    state_ = FsTestState::kRunning;
    END_HELPER;
}

bool BlobfsTest::Teardown() {
    BEGIN_HELPER;
    ASSERT_NE(state_, FsTestState::kComplete);
    auto error = fbl::MakeAutoCall([this](){ state_ = FsTestState::kError; });

    if (state_ == FsTestState::kRunning) {
        ASSERT_EQ(state_, FsTestState::kRunning);
        ASSERT_TRUE(CheckInfo());
        ASSERT_EQ(umount(MOUNT_PATH), ZX_OK, "Failed to unmount filesystem");
        ASSERT_EQ(fsck(device_path_, DISK_FORMAT_BLOBFS, &test_fsck_options, launch_stdio_sync),
                  ZX_OK, "Filesystem fsck failed");
    }

    if (gUseRealDisk) {
        if (type_ == FsTestType::kFvm) {
            ASSERT_EQ(fvm_destroy(fvm_path_), ZX_OK);
        }
    } else {
        ASSERT_EQ(ramdisk_destroy(ramdisk_), ZX_OK);
    }

    error.cancel();
    state_ = FsTestState::kComplete;
    END_HELPER;
}

bool BlobfsTest::GetDevicePath(char* path, size_t len) const {
    BEGIN_HELPER;
    if (type_ == FsTestType::kFvm) {
        strlcpy(path, fvm_path_, len);
        strlcat(path, "/fvm", len);
        while (true) {
            DIR* dir = opendir(path);
            ASSERT_NONNULL(dir, "Unable to open FVM dir");

            struct dirent* dir_entry;
            bool updated = false;
            while ((dir_entry = readdir(dir)) != NULL) {
                if (strcmp(dir_entry->d_name, ".") == 0) {
                    continue;
                }

                updated = true;
                strlcat(path, "/", len);
                strlcat(path, dir_entry->d_name, len);
            }

            closedir(dir);

            if (!updated) {
                break;
            }
        }
    } else {
        strlcpy(path, device_path_, len);
    }
    END_HELPER;
}

bool BlobfsTest::ForceReset() {
    BEGIN_HELPER;
    if (state_ == FsTestState::kComplete) {
        return Reset();
    }

    ASSERT_NE(state_, FsTestState::kInit);
    ASSERT_EQ(umount(MOUNT_PATH), ZX_OK, "Failed to unmount filesystem");

    if (gUseRealDisk) {
        if (type_ == FsTestType::kFvm) {
            ASSERT_EQ(fvm_destroy(fvm_path_), ZX_OK);
        }
    } else {
        ASSERT_EQ(ramdisk_destroy(ramdisk_), ZX_OK);
    }

    FsTestState old_state = state_;
    state_ = FsTestState::kInit;

    ASSERT_TRUE(Init(old_state));
    END_HELPER;
}

bool BlobfsTest::ToggleSleep(uint64_t blk_count) {
    BEGIN_HELPER;

    if (asleep_) {
        // If the ramdisk is asleep, wake it up.
        ASSERT_EQ(ramdisk_wake(ramdisk_), ZX_OK);
    } else {
        // If the ramdisk is active, put it to sleep after the specified block count.
        ASSERT_EQ(ramdisk_sleep_after(ramdisk_, blk_count), ZX_OK);
    }

    asleep_ = !asleep_;
    END_HELPER;
}

bool BlobfsTest::GetRamdiskCount(uint64_t* blk_count) const {
    BEGIN_HELPER;
    ramdisk_block_write_counts_t counts;

    ASSERT_EQ(ramdisk_get_block_counts(ramdisk_, &counts), ZX_OK);

    *blk_count = counts.received;
    END_HELPER;
}

bool BlobfsTest::CheckInfo(BlobfsUsage* usage) {
    fbl::unique_fd fd(open(MOUNT_PATH, O_RDONLY | O_DIRECTORY));
    ASSERT_TRUE(fd);

    zx_status_t status;
    fuchsia_io_FilesystemInfo info;
    fzl::FdioCaller caller(std::move(fd));
    ASSERT_EQ(fuchsia_io_DirectoryAdminQueryFilesystem(caller.borrow_channel(), &status, &info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    const char* kFsName = "blobfs";
    const char* name = reinterpret_cast<const char*>(info.name);
    ASSERT_EQ(strncmp(name, kFsName, strlen(kFsName)), 0, "Unexpected filesystem mounted");
    ASSERT_LE(info.used_nodes, info.total_nodes, "Used nodes greater than free nodes");
    ASSERT_LE(info.used_bytes, info.total_bytes, "Used bytes greater than free bytes");
    ASSERT_EQ(close(caller.release().release()), 0);

    if (usage != nullptr) {
        usage->used_bytes = info.used_bytes;
        usage->total_bytes = info.total_bytes;
        usage->used_nodes = info.used_nodes;
        usage->total_nodes = info.total_nodes;
    }

    return true;
}

bool BlobfsTest::Mount() {
    BEGIN_HELPER;
    int flags = read_only_ ? O_RDONLY : O_RDWR;

    fbl::unique_fd fd(open(device_path_, flags));
    ASSERT_TRUE(fd.get(), "Could not open ramdisk");

    mount_options_t options = default_mount_options;
    options.enable_journal = gEnableJournal;

    if (read_only_) {
        options.readonly = true;
    }

    LaunchCallback launch = stdio_ ? launch_stdio_async : launch_silent_async;

    // fd consumed by mount. By default, mount waits until the filesystem is
    // ready to accept commands.
    ASSERT_EQ(mount(fd.get(), MOUNT_PATH, DISK_FORMAT_BLOBFS, &options, launch), ZX_OK,
              "Could not mount blobfs");
    END_HELPER;
}

// Helper functions for testing:

// Creates an open blob with the provided Merkle tree + Data, and
// reads to verify the data.
static bool MakeBlob(fs_test_utils::BlobInfo* info, fbl::unique_fd* out_fd) {
    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);
    ASSERT_EQ(fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data), 0,
              "Failed to write Data");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    out_fd->reset(fd.release());
    return true;
}

static bool MakeBlobUnverified(fs_test_utils::BlobInfo* info, fbl::unique_fd* out_fd) {
    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);
    ASSERT_EQ(fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data), 0,
              "Failed to write Data");
    out_fd->reset(fd.release());
    return true;
}

static bool VerifyCompromised(int fd, const char* data, size_t size_data) {
    // Verify the contents of the Blob
    fbl::AllocChecker ac;
    fbl::unique_ptr<char[]> buf(new (&ac) char[size_data]);
    EXPECT_EQ(ac.check(), true);

    ASSERT_EQ(lseek(fd, 0, SEEK_SET), 0);
    ASSERT_EQ(fs_test_utils::StreamAll(read, fd, &buf[0], size_data), -1,
                                       "Expected reading to fail");
    return true;
}

// Creates a blob with the provided Merkle tree + Data, and
// reads to verify the data.
static bool MakeBlobCompromised(fs_test_utils::BlobInfo* info) {
    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);

    // If we're writing a blob with invalid sizes, it's possible that writing will fail.
    fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data);

    ASSERT_TRUE(VerifyCompromised(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);
    return true;
}

static bool uint8_to_hex_str(const uint8_t* data, char* hex_str) {
    for (size_t i = 0; i < 32; i++) {
        ASSERT_EQ(sprintf(hex_str + (i * 2), "%02x", data[i]), 2,
                  "Error converting name to string");
    }
    hex_str[64] = 0;
    return true;
}

bool QueryInfo(size_t expected_nodes, size_t expected_bytes) {
    fbl::unique_fd fd(open(MOUNT_PATH, O_RDONLY | O_DIRECTORY));
    ASSERT_TRUE(fd);

    zx_status_t status;
    fuchsia_io_FilesystemInfo info;
    fzl::FdioCaller caller(std::move(fd));
    ASSERT_EQ(fuchsia_io_DirectoryAdminQueryFilesystem(caller.borrow_channel(), &status, &info),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    const char* kFsName = "blobfs";
    const char* name = reinterpret_cast<const char*>(info.name);
    ASSERT_EQ(close(caller.release().release()), 0);
    ASSERT_EQ(strncmp(name, kFsName, strlen(kFsName)), 0, "Unexpected filesystem mounted");
    ASSERT_EQ(info.block_size, blobfs::kBlobfsBlockSize);
    ASSERT_EQ(info.max_filename_size, Digest::kLength * 2);
    ASSERT_EQ(info.fs_type, VFS_TYPE_BLOBFS);
    ASSERT_NE(info.fs_id, 0);

    // Check that used_bytes are within a reasonable range
    ASSERT_GE(info.used_bytes, expected_bytes);
    ASSERT_LE(info.used_bytes, info.total_bytes);

    // Check that total_bytes are a multiple of slice_size
    ASSERT_GE(info.total_bytes, kTestFvmSliceSize);
    ASSERT_EQ(info.total_bytes % kTestFvmSliceSize, 0);
    ASSERT_EQ(info.total_nodes, kTestFvmSliceSize / blobfs::kBlobfsInodeSize);
    ASSERT_EQ(info.used_nodes, expected_nodes);
    return true;
}

}  // namespace

// Actual tests:
static bool TestBasic(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    for (unsigned int i = 10; i < 16; i++) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));

        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info.get(), &fd));
        ASSERT_EQ(close(fd.release()), 0);

        // We can re-open and verify the Blob as read-only
        fd.reset(open(info->path, O_RDONLY));
        ASSERT_TRUE(fd, "Failed to-reopen blob");
        ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
        ASSERT_EQ(close(fd.release()), 0);

        // We cannot re-open the blob as writable
        fd.reset(open(info->path, O_RDWR | O_CREAT));
        ASSERT_FALSE(fd, "Shouldn't be able to re-create blob that exists");
        fd.reset(open(info->path, O_RDWR));
        ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");
        fd.reset(open(info->path, O_WRONLY));
        ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");

        ASSERT_EQ(unlink(info->path), 0);
    }
    END_HELPER;
}

static bool TestUnallocatedBlob(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 10, &info));

    // We can create a blob with a name.
    ASSERT_TRUE(fbl::unique_fd(open(info->path, O_CREAT | O_EXCL | O_RDWR)));
    // It won't exist if we close it before allocating space.
    ASSERT_FALSE(fbl::unique_fd(open(info->path, O_RDWR)));
    ASSERT_FALSE(fbl::unique_fd(open(info->path, O_RDONLY)));
    // We can "re-use" the name.
    {
        fbl::unique_fd fd(open(info->path, O_CREAT | O_EXCL | O_RDWR));
        ASSERT_TRUE(fd);
        ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);
    }

    END_HELPER;
}

static bool TestNullBlob(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 0, &info));

    fbl::unique_fd fd(open(info->path, O_CREAT | O_EXCL | O_RDWR));
    ASSERT_TRUE(fd);
    ASSERT_EQ(ftruncate(fd.get(), 0), 0);
    char buf[1];
    ASSERT_EQ(read(fd.get(), &buf[0], 1), 0, "Null Blob should reach EOF immediately");
    ASSERT_EQ(close(fd.release()), 0);

    fd.reset(open(info->path, O_CREAT | O_EXCL | O_RDWR));
    ASSERT_FALSE(fd, "Null Blob should already exist");
    fd.reset(open(info->path, O_CREAT | O_RDWR));
    ASSERT_FALSE(fd, "Null Blob should not be openable as writable");

    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_EQ(unlink(info->path), 0, "Null Blob should be unlinkable");

    END_HELPER;
}

static bool TestCompressibleBlob(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    for (size_t i = 10; i < 22; i++) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;

        // Create blobs which are trivially compressible.
        ASSERT_TRUE(fs_test_utils::GenerateBlob([](char* data, size_t length) {
            size_t i = 0;
            while (i < length) {
                size_t j = (rand() % (length - i)) + 1;
                memset(data, (char) j, j);
                data += j;
                i += j;
            }
        }, MOUNT_PATH, 1 << i, &info));

        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info.get(), &fd));
        ASSERT_EQ(close(fd.release()), 0);

        // We can re-open and verify the Blob as read-only
        fd.reset(open(info->path, O_RDONLY));
        ASSERT_TRUE(fd, "Failed to-reopen blob");
        ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
        ASSERT_EQ(close(fd.release()), 0);

        // We cannot re-open the blob as writable
        fd.reset(open(info->path, O_RDWR | O_CREAT));
        ASSERT_FALSE(fd, "Shouldn't be able to re-create blob that exists");
        fd.reset(open(info->path, O_RDWR));
        ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");
        fd.reset(open(info->path, O_WRONLY));
        ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");

        // Force decompression by remounting, re-accessing blob.
        ASSERT_TRUE(blobfsTest->Remount());
        fd.reset(open(info->path, O_RDONLY));
        ASSERT_TRUE(fd, "Failed to-reopen blob");
        ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
        ASSERT_EQ(close(fd.release()), 0);

        ASSERT_EQ(unlink(info->path), 0);
    }

    END_HELPER;
}

static bool TestMmap(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    for (size_t i = 10; i < 16; i++) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));

        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info.get(), &fd));
        ASSERT_EQ(close(fd.release()), 0);
        fd.reset(open(info->path, O_RDONLY));
        ASSERT_TRUE(fd, "Failed to-reopen blob");

        void* addr = mmap(NULL, info->size_data, PROT_READ, MAP_PRIVATE,
                          fd.get(), 0);
        ASSERT_NE(addr, MAP_FAILED, "Could not mmap blob");
        ASSERT_EQ(memcmp(addr, info->data.get(), info->size_data), 0, "Mmap data invalid");
        ASSERT_EQ(munmap(addr, info->size_data), 0, "Could not unmap blob");
        ASSERT_EQ(close(fd.release()), 0);
        ASSERT_EQ(unlink(info->path), 0);
    }
    END_HELPER;
}

static bool TestMmapUseAfterClose(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    for (size_t i = 10; i < 16; i++) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));

        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info.get(), &fd));
        ASSERT_EQ(close(fd.release()), 0);
        fd.reset(open(info->path, O_RDONLY));
        ASSERT_TRUE(fd, "Failed to-reopen blob");

        void* addr = mmap(NULL, info->size_data, PROT_READ, MAP_PRIVATE, fd.get(), 0);
        ASSERT_NE(addr, MAP_FAILED, "Could not mmap blob");
        ASSERT_EQ(close(fd.release()), 0);

        // We should be able to access the mapped data while the file is closed.
        ASSERT_EQ(memcmp(addr, info->data.get(), info->size_data), 0, "Mmap data invalid");

        // We should be able to re-open and remap the file.
        //
        // Although this isn't being tested explicitly (we lack a mechanism to
        // check that the second mapping uses the same underlying pages as the
        // first) the memory usage should avoid duplication in the second
        // mapping.
        fd.reset(open(info->path, O_RDONLY));
        void* addr2 = mmap(NULL, info->size_data, PROT_READ, MAP_PRIVATE, fd.get(), 0);
        ASSERT_NE(addr2, MAP_FAILED, "Could not mmap blob");
        ASSERT_EQ(close(fd.release()), 0);
        ASSERT_EQ(memcmp(addr2, info->data.get(), info->size_data), 0, "Mmap data invalid");

        ASSERT_EQ(munmap(addr, info->size_data), 0, "Could not unmap blob");
        ASSERT_EQ(munmap(addr2, info->size_data), 0, "Could not unmap blob");

        ASSERT_EQ(unlink(info->path), 0);
    }
    END_HELPER;
}

static bool TestReaddir(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    constexpr size_t kMaxEntries = 50;
    constexpr size_t kBlobSize = 1 << 10;

    fbl::AllocChecker ac;
    fbl::Array<fbl::unique_ptr<fs_test_utils::BlobInfo>>
        info(new (&ac) fbl::unique_ptr<fs_test_utils::BlobInfo>[kMaxEntries](), kMaxEntries);
    ASSERT_TRUE(ac.check());

    // Try to readdir on an empty directory
    DIR* dir = opendir(MOUNT_PATH);
    ASSERT_NONNULL(dir);
    auto cleanup = fbl::MakeAutoCall([dir](){ closedir(dir); });
    ASSERT_NULL(readdir(dir), "Expected blobfs to start empty");

    // Fill a directory with entries
    for (size_t i = 0; i < kMaxEntries; i++) {
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, kBlobSize, &info[i]));
        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info[i].get(), &fd));
        ASSERT_EQ(close(fd.release()), 0);
        fd.reset(open(info[i]->path, O_RDONLY));
        ASSERT_TRUE(fd, "Failed to-reopen blob");
        ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info[i]->data.get(),
                    info[i]->size_data));
        ASSERT_EQ(close(fd.release()), 0);
    }

    // Check that we see the expected number of entries
    size_t entries_seen = 0;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        entries_seen++;
    }
    ASSERT_EQ(entries_seen, kMaxEntries);
    entries_seen = 0;
    rewinddir(dir);

    // Readdir on a directory which contains entries, removing them as we go
    // along.
    while ((de = readdir(dir)) != nullptr) {
        for (size_t i = 0; i < kMaxEntries; i++) {
            if ((info[i]->size_data != 0) &&
                strcmp(strrchr(info[i]->path, '/') + 1, de->d_name) == 0) {
                ASSERT_EQ(unlink(info[i]->path), 0);
                // It's a bit hacky, but we set 'size_data' to zero
                // to identify the entry has been unlinked.
                info[i]->size_data = 0;
                goto found;
            }
        }
        ASSERT_TRUE(false, "Blobfs Readdir found an unexpected entry");
    found:
        entries_seen++;
    }
    ASSERT_EQ(entries_seen, kMaxEntries);

    ASSERT_NULL(readdir(dir), "Expected blobfs to end empty");
    cleanup.cancel();
    ASSERT_EQ(closedir(dir), 0);
    END_HELPER;
}

static bool TestDiskTooSmall(BlobfsTest* blobfsTest) {
    BEGIN_TEST;

    if (gUseRealDisk) {
        fprintf(stderr, "Ramdisk required; skipping test\n");
        return true;
    }

    uint64_t minimum_size = 0;
    if (blobfsTest->GetType() == FsTestType::kFvm) {
        size_t blocks_per_slice = kTestFvmSliceSize / blobfs::kBlobfsBlockSize;

        // Calculate slices required for data blocks based on minimum requirement and slice size.
        uint64_t required_data_slices = fbl::round_up(blobfs::kMinimumDataBlocks, blocks_per_slice)
                                        / blocks_per_slice;
        uint64_t required_journal_slices = fbl::round_up(blobfs::kDefaultJournalBlocks,
                                                         blocks_per_slice) / blocks_per_slice;

        // Require an additional 1 slice each for super, inode, and block bitmaps.
        uint64_t blobfs_size = (required_journal_slices + required_data_slices + 3)
                               * kTestFvmSliceSize;
        minimum_size = blobfs_size;
        uint64_t metadata_size = fvm::MetadataSize(blobfs_size, kTestFvmSliceSize);

        // Re-calculate minimum size until the metadata size stops growing.
        while (minimum_size - blobfs_size != metadata_size * 2) {
            minimum_size = blobfs_size + metadata_size * 2;
            metadata_size = fvm::MetadataSize(minimum_size, kTestFvmSliceSize);
        }

        ASSERT_EQ(minimum_size - blobfs_size,
                  fvm::MetadataSize(minimum_size, kTestFvmSliceSize) * 2);
    } else {
        blobfs::Superblock info;
        info.inode_count = blobfs::kBlobfsDefaultInodeCount;
        info.data_block_count = blobfs::kMinimumDataBlocks;
        info.journal_block_count = blobfs::kMinimumJournalBlocks;
        info.flags = 0;

        minimum_size = blobfs::TotalBlocks(info) * blobfs::kBlobfsBlockSize;
    }

    // Teardown the initial test configuration and reset the test state.
    ASSERT_TRUE(blobfsTest->Teardown());
    ASSERT_TRUE(blobfsTest->Reset());

    // Create disk with minimum possible size, make sure init passes.
    ASSERT_GE(minimum_size, blobfsTest->GetBlockSize());
    uint64_t disk_blocks = minimum_size / blobfsTest->GetBlockSize();
    ASSERT_TRUE(blobfsTest->SetBlockCount(disk_blocks));
    ASSERT_TRUE(blobfsTest->Init());
    ASSERT_TRUE(blobfsTest->Teardown());

    // Reset the disk size and test state.
    ASSERT_TRUE(blobfsTest->Reset());
    ASSERT_TRUE(blobfsTest->SetBlockCount(disk_blocks - 1));

    // Create disk with smaller than minimum size, make sure mkfs fails.
    ASSERT_TRUE(blobfsTest->Init(FsTestState::kMinimal));

    char device_path[PATH_MAX];
    ASSERT_TRUE(blobfsTest->GetDevicePath(device_path, PATH_MAX));
    ASSERT_NE(mkfs(device_path, DISK_FORMAT_BLOBFS, launch_stdio_sync, &default_mkfs_options),
              ZX_OK);

    // Reset the ramdisk counts so we don't attempt to run ramdisk failure tests. There isn't
    // really a point with this test since we are testing blobfs creation, which doesn't generate
    // any journal entries.
    ASSERT_TRUE(blobfsTest->ToggleSleep());
    ASSERT_TRUE(blobfsTest->ToggleSleep());
    END_TEST;
}

static bool TestQueryInfo(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    ASSERT_EQ(blobfsTest->GetType(), FsTestType::kFvm);

    size_t total_bytes = 0;
    ASSERT_TRUE(QueryInfo(0, 0));
    for (size_t i = 10; i < 16; i++) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));

        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info.get(), &fd));
        ASSERT_EQ(close(fd.release()), 0);
        total_bytes += fbl::round_up(info->size_merkle + info->size_data,
                       blobfs::kBlobfsBlockSize);
    }

    ASSERT_TRUE(QueryInfo(6, total_bytes));
    END_HELPER;
}

bool GetAllocations(zx::vmo* out_vmo, uint64_t* out_count) {
    BEGIN_HELPER;
    fbl::unique_fd fd(open(MOUNT_PATH, O_RDONLY | O_DIRECTORY));
    ASSERT_TRUE(fd);
    zx_status_t status;
    zx_handle_t vmo_handle;
    fzl::FdioCaller caller(std::move(fd));
    ASSERT_EQ(fuchsia_blobfs_BlobfsGetAllocatedRegions(caller.borrow_channel(), &status,
              &vmo_handle, out_count), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    out_vmo->reset(vmo_handle);
    END_HELPER;
}

bool TestGetAllocatedRegions(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    zx::vmo vmo;
    uint64_t count;
    size_t total_bytes = 0;
    size_t fidl_bytes = 0;

    // Although we expect this partition to be empty, we check the results of GetAllocations
    // in case blobfs chooses to store any metadata of pre-initialized data with the
    // allocated regions.
    ASSERT_TRUE(GetAllocations(&vmo, &count));
    fbl::Array<fuchsia_blobfs_BlockRegion> buffer(new fuchsia_blobfs_BlockRegion[count], count);
    ASSERT_EQ(vmo.read(buffer.get(), 0, sizeof(fuchsia_blobfs_BlockRegion) * count), ZX_OK);
    for (size_t i = 0; i < count; i++) {
        total_bytes += buffer[i].length * blobfs::kBlobfsBlockSize;
    }

    for (size_t i = 10; i < 16; i++) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));

        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info.get(), &fd));
        ASSERT_EQ(close(fd.release()), 0);
        total_bytes += fbl::round_up(info->size_merkle + info->size_data,
                       blobfs::kBlobfsBlockSize);
    }
    ASSERT_TRUE(GetAllocations(&vmo, &count));
    buffer.reset(new fuchsia_blobfs_BlockRegion[count], count);
    ASSERT_EQ(vmo.read(buffer.get(), 0, sizeof(fuchsia_blobfs_BlockRegion) * count), ZX_OK);
    for (size_t i = 0; i < count; i++) {
        fidl_bytes += buffer[i].length * blobfs::kBlobfsBlockSize;
    }
    ASSERT_EQ(fidl_bytes, total_bytes);
    END_HELPER;
}

static bool UseAfterUnlink(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    for (size_t i = 0; i < 16; i++) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));

        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info.get(), &fd));

        // We should be able to unlink the blob
        ASSERT_EQ(unlink(info->path), 0, "Failed to unlink");

        // We should still be able to read the blob after unlinking
        ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));

        // After closing the fd, however, we should not be able to re-open the blob
        ASSERT_EQ(close(fd.release()), 0);
        ASSERT_LT(open(info->path, O_RDONLY), 0, "Expected blob to be deleted");
    }
    END_HELPER;
}

static bool WriteAfterRead(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    for (size_t i = 0; i < 16; i++) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));

        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info.get(), &fd));

        // After blob generation, writes should be rejected
        ASSERT_LT(write(fd.get(), info->data.get(), 1), 0,
                  "After being written, the blob should refuse writes");

        off_t seek_pos = (rand() % info->size_data);
        ASSERT_EQ(lseek(fd.get(), seek_pos, SEEK_SET), seek_pos);
        ASSERT_LT(write(fd.get(), info->data.get(), 1), 0,
                  "After being written, the blob should refuse writes");
        ASSERT_LT(ftruncate(fd.get(), rand() % info->size_data), 0,
                  "The blob should always refuse to be truncated");

        // We should be able to unlink the blob
        ASSERT_EQ(close(fd.release()), 0);
        ASSERT_EQ(unlink(info->path), 0, "Failed to unlink");
    }
    END_HELPER;
}

bool WriteAfterUnlink(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    size_t size = 1 << 20;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, size, &info));

    // Partially write out first blob.
    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");
    ASSERT_EQ(ftruncate(fd.get(), size), 0);
    ASSERT_EQ(fs_test_utils::StreamAll(write, fd.get(), info->data.get(), size / 2), 0,
                                       "Failed to write Data");
    ASSERT_EQ(unlink(info->path), 0);
    ASSERT_EQ(fs_test_utils::StreamAll(write, fd.get(), info->data.get() + size / 2,
                                       size - (size / 2)), 0, "Failed to write Data");
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_LT(open(info->path, O_RDONLY), 0);
    END_HELPER;
}

static bool ReadTooLarge(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    for (size_t i = 0; i < 16; i++) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));

        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info.get(), &fd));

        // Verify the contents of the Blob
        fbl::AllocChecker ac;
        fbl::unique_ptr<char[]> buf(new (&ac) char[info->size_data]);
        EXPECT_EQ(ac.check(), true);

        // Try read beyond end of blob
        off_t end_off = info->size_data;
        ASSERT_EQ(lseek(fd.get(), end_off, SEEK_SET), end_off);
        ASSERT_EQ(read(fd.get(), &buf[0], 1), 0, "Expected empty read beyond end of file");

        // Try some reads which straddle the end of the blob
        for (ssize_t j = 1; j < static_cast<ssize_t>(info->size_data); j *= 2) {
            end_off = info->size_data - j;
            ASSERT_EQ(lseek(fd.get(), end_off, SEEK_SET), end_off);
            ASSERT_EQ(read(fd.get(), &buf[0], j * 2), j,
                      "Expected to only read one byte at end of file");
            ASSERT_EQ(memcmp(buf.get(), &info->data[info->size_data - j], j),
                      0, "Read data, but it was bad");
        }

        // We should be able to unlink the blob
        ASSERT_EQ(close(fd.release()), 0);
        ASSERT_EQ(unlink(info->path), 0, "Failed to unlink");
    }
    END_HELPER;
}

static bool BadAllocation(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    ASSERT_LT(open(MOUNT_PATH "/00112233445566778899AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTTUUVV",
                   O_CREAT | O_RDWR),
              0, "Only acceptable pathnames are hex");
    ASSERT_LT(open(MOUNT_PATH "/00112233445566778899AABBCCDDEEFF", O_CREAT | O_RDWR), 0,
              "Only acceptable pathnames are 32 hex-encoded bytes");

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 15, &info));

    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");
    ASSERT_EQ(ftruncate(fd.get(), 0), -1, "Blob without data doesn't match null blob");
    // This is the size of the entire disk; we won't have room.
    ASSERT_EQ(ftruncate(fd.get(), blobfsTest->GetDiskSize()), -1, "Huge blob");

    // Okay, finally, a valid blob!
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0, "Failed to allocate blob");

    // Write nothing, but close the blob. Since the write was incomplete,
    // it will be inaccessible.
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_LT(open(info->path, O_RDWR), 0, "Cannot access partial blob");
    ASSERT_LT(open(info->path, O_RDONLY), 0, "Cannot access partial blob");

    // And once more -- let's write everything but the last byte of a blob's data.
    fd.reset(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0, "Failed to allocate blob");
    ASSERT_EQ(fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data - 1), 0,
              "Failed to write data");
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_LT(open(info->path, O_RDWR), 0, "Cannot access partial blob");
    END_HELPER;
}

static bool CorruptedBlob(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    for (size_t i = 1; i < 18; i++) {
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));
        info->size_data -= (rand() % info->size_data) + 1;
        if (info->size_data == 0) {
            info->size_data = 1;
        }
        ASSERT_TRUE(MakeBlobCompromised(info.get()));
    }

    for (size_t i = 0; i < 18; i++) {
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));
        // Flip a random bit of the data
        size_t rand_index = rand() % info->size_data;
        char old_val = info->data.get()[rand_index];
        while ((info->data.get()[rand_index] = static_cast<char>(rand())) == old_val) {
        }
        ASSERT_TRUE(MakeBlobCompromised(info.get()));
    }

    END_HELPER;
}

static bool CorruptedDigest(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    for (size_t i = 1; i < 18; i++) {
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));

        char hexdigits[17] = "0123456789abcdef";
        size_t idx = strlen(info->path) - 1 - (rand() % (2 * Digest::kLength));
        char newchar = hexdigits[rand() % 16];
        while (info->path[idx] == newchar) {
            newchar = hexdigits[rand() % 16];
        }
        info->path[idx] = newchar;
        ASSERT_TRUE(MakeBlobCompromised(info.get()));
    }

    for (size_t i = 0; i < 18; i++) {
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));
        // Flip a random bit of the data
        size_t rand_index = rand() % info->size_data;
        char old_val = info->data.get()[rand_index];
        while ((info->data.get()[rand_index] = static_cast<char>(rand())) == old_val) {
        }
        ASSERT_TRUE(MakeBlobCompromised(info.get()));
    }

    END_HELPER;
}

static bool EdgeAllocation(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;

    // Powers of two...
    for (size_t i = 1; i < 16; i++) {
        // -1, 0, +1 offsets...
        for (size_t j = -1; j < 2; j++) {
            fbl::unique_ptr<fs_test_utils::BlobInfo> info;
            ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, (1 << i) + j, &info));
            fbl::unique_fd fd;
            ASSERT_TRUE(MakeBlob(info.get(), &fd));
            ASSERT_EQ(unlink(info->path), 0);
            ASSERT_EQ(close(fd.release()), 0);
        }
    }
    END_HELPER;
}

static bool UmountWithOpenFile(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 16, &info));
    fbl::unique_fd fd;
    ASSERT_TRUE(MakeBlob(info.get(), &fd));

    // Intentionally don't close the file descriptor: Unmount anyway.
    ASSERT_TRUE(blobfsTest->Remount());
    // Just closing our local handle; the connection should be disconnected.
    ASSERT_EQ(close(fd.release()), -1);
    ASSERT_EQ(errno, EPIPE);

    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to open blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0, "Could not close blob");

    ASSERT_EQ(unlink(info->path), 0);
    END_HELPER;
}

static bool UmountWithMappedFile(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 16, &info));
    fbl::unique_fd fd;
    ASSERT_TRUE(MakeBlob(info.get(), &fd));

    void* addr = mmap(nullptr, info->size_data, PROT_READ, MAP_SHARED, fd.get(), 0);
    ASSERT_NONNULL(addr);
    ASSERT_EQ(close(fd.release()), 0);

    // Intentionally don't unmap the file descriptor: Unmount anyway.
    ASSERT_TRUE(blobfsTest->Remount());
    // Just closing our local handle; the connection should be disconnected.
    ASSERT_EQ(munmap(addr, info->size_data), 0);

    fd.reset(open(info->path, O_RDONLY));
    ASSERT_GE(fd.get(), 0, "Failed to open blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0, "Could not close blob");

    ASSERT_EQ(unlink(info->path), 0);
    END_HELPER;
}

static bool UmountWithOpenMappedFile(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 16, &info));
    fbl::unique_fd fd;
    ASSERT_TRUE(MakeBlob(info.get(), &fd));

    void* addr = mmap(nullptr, info->size_data, PROT_READ, MAP_SHARED, fd.get(), 0);
    ASSERT_NONNULL(addr);

    // Intentionally don't close the file descriptor: Unmount anyway.
    ASSERT_TRUE(blobfsTest->Remount());
    // Just closing our local handle; the connection should be disconnected.
    ASSERT_EQ(munmap(addr, info->size_data), 0);
    ASSERT_EQ(close(fd.release()), -1);
    ASSERT_EQ(errno, EPIPE);

    fd.reset(open(info->path, O_RDONLY));
    ASSERT_GE(fd.get(), 0, "Failed to open blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0, "Could not close blob");

    ASSERT_EQ(unlink(info->path), 0);
    END_HELPER;
}

static bool CreateUmountRemountSmall(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    for (size_t i = 10; i < 16; i++) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));

        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info.get(), &fd));
        // Close fd, unmount filesystem
        ASSERT_EQ(close(fd.release()), 0);

        ASSERT_TRUE(blobfsTest->Remount(), "Could not re-mount blobfs");

        fd.reset(open(info->path, O_RDONLY));
        ASSERT_TRUE(fd, "Failed to open blob");

        ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
        ASSERT_EQ(close(fd.release()), 0, "Could not close blob");
        ASSERT_EQ(unlink(info->path), 0);
    }

    END_HELPER;
}

static bool check_not_readable(int fd) {
    BEGIN_HELPER;
    struct pollfd fds;
    fds.fd = fd;
    fds.events = POLLIN;
    ASSERT_EQ(poll(&fds, 1, 10), 0, "Failed to wait for readable blob");

    char buf[1];
    ASSERT_EQ(lseek(fd, 0, SEEK_SET), 0);
    ASSERT_LT(read(fd, buf, sizeof(buf)), 0, "Blob should not be readable yet");
    END_HELPER;
}

static bool check_readable(int fd) {
    BEGIN_HELPER;
    struct pollfd fds;
    fds.fd = fd;
    fds.events = POLLIN;
    ASSERT_EQ(poll(&fds, 1, 10), 1, "Failed to wait for readable blob");
    ASSERT_EQ(fds.revents, POLLIN);

    char buf[1];
    ASSERT_EQ(lseek(fd, 0, SEEK_SET), 0);
    ASSERT_EQ(read(fd, buf, sizeof(buf)), sizeof(buf));
    END_HELPER;
}

static bool EarlyRead(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    // Check that we cannot read from the Blob until it has been fully written
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;

    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 17, &info));
    fbl::unique_fd fd(open(info->path, O_CREAT | O_EXCL | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");

    ASSERT_LT(open(info->path, O_CREAT | O_EXCL | O_RDWR), 0,
              "Should not be able to exclusively create twice");

    // This second fd should also not be readable
    fbl::unique_fd fd2(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd2, "Failed to create blob");

    ASSERT_TRUE(check_not_readable(fd.get()), "Should not be readable after open");
    ASSERT_TRUE(check_not_readable(fd2.get()), "Should not be readable after open");
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);
    ASSERT_TRUE(check_not_readable(fd.get()), "Should not be readable after alloc");
    ASSERT_TRUE(check_not_readable(fd2.get()), "Should not be readable after alloc");
    ASSERT_EQ(fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data), 0,
              "Failed to write Data");

    // Okay, NOW we can read.
    // Double check that attempting to read early didn't cause problems...
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd2.get(), info->data.get(), info->size_data));

    // Cool, everything is readable. What if we try accessing the blob status now?
    EXPECT_TRUE(check_readable(fd.get()));

    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_EQ(close(fd2.release()), 0);
    ASSERT_EQ(unlink(info->path), 0);
    END_HELPER;
}

static bool wait_readable(int fd) {
    BEGIN_HELPER;
    struct pollfd fds;
    fds.fd = fd;
    fds.events = POLLIN;
    ASSERT_EQ(poll(&fds, 1, 10000), 1, "Failed to wait for readable blob");
    ASSERT_EQ(fds.revents, POLLIN);
    END_HELPER;
}

// Check that we cannot read from the Blob until it has been fully written
static bool WaitForRead(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;

    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 17, &info));
    fbl::unique_fd fd(open(info->path, O_CREAT | O_EXCL | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");

    ASSERT_LT(open(info->path, O_CREAT | O_EXCL | O_RDWR), 0,
              "Should not be able to exclusively create twice");

    // Launch a background thread to wait for fd to become readable
    auto wait_until_readable = [](void* arg) {
        fbl::unique_fd fd(*static_cast<int*>(arg));
        if (!wait_readable(fd.get())) {
            return -1;
        }

        if (!check_readable(fd.get())) {
            return -1;
        }

        if (close(fd.release()) != 0) {
            return -1;
        }

        return 0;
    };

    int dupfd = dup(fd.get());
    thrd_t waiter_thread;
    thrd_create(&waiter_thread, wait_until_readable, &dupfd);
    int result;
    int success;

    {
        // In case the test fails, join the thread before returning from the test.
        auto join_thread = fbl::MakeAutoCall([&]() {
            success = thrd_join(waiter_thread, &result);
        });

        ASSERT_TRUE(check_not_readable(fd.get()), "Should not be readable after open");
        ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);
        ASSERT_TRUE(check_not_readable(fd.get()), "Should not be readable after alloc");
        ASSERT_EQ(fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data), 0,
                "Failed to write Data");

        // Cool, everything is readable. What if we try accessing the blob status now?
        EXPECT_TRUE(check_readable(fd.get()));
    }

    // Our background thread should have also completed successfully...
    ASSERT_EQ(success, thrd_success, "thrd_join failed");
    ASSERT_EQ(result, 0, "Unexpected result from background thread");

    // Double check that attempting to read early didn't cause problems...
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_EQ(unlink(info->path), 0);
    END_HELPER;
}

// Check that seeks during writing are ignored
static bool WriteSeekIgnored(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 17, &info));
    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);

    size_t n = 0;
    while (n != info->size_data) {
        off_t seek_pos = (rand() % info->size_data);
        ASSERT_EQ(lseek(fd.get(), seek_pos, SEEK_SET), seek_pos);
        ssize_t d = write(fd.get(), info->data.get(), info->size_data - n);
        ASSERT_GT(d, 0, "Data Write error");
        n += d;
    }

    // Double check that attempting to seek early didn't cause problems...
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_EQ(unlink(info->path), 0);
    END_HELPER;
}

// Try unlinking at a variety of times
static bool UnlinkTiming(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    // Unlink, close fd, re-open fd as new file
    auto full_unlink_reopen = [](fbl::unique_fd& fd, const char* path) {
        BEGIN_HELPER;
        ASSERT_EQ(unlink(path), 0);
        ASSERT_EQ(close(fd.release()), 0);
        fd.reset(open(path, O_CREAT | O_RDWR | O_EXCL));
        ASSERT_TRUE(fd, "Failed to recreate blob");
        END_HELPER;
    };

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 17, &info));

    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");

    // Unlink after first open
    ASSERT_TRUE(full_unlink_reopen(fd, info->path));

    // Unlink after init
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);
    ASSERT_TRUE(full_unlink_reopen(fd, info->path));

    // Unlink after first write
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);
    ASSERT_EQ(fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data), 0,
              "Failed to write Data");
    ASSERT_TRUE(full_unlink_reopen(fd, info->path));
    ASSERT_EQ(unlink(info->path), 0);
    ASSERT_EQ(close(fd.release()), 0);
    END_HELPER;
}

// Attempt using invalid operations
static bool InvalidOps(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    // First off, make a valid blob
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 12, &info));
    fbl::unique_fd fd;
    ASSERT_TRUE(MakeBlob(info.get(), &fd));
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));

    // Neat. Now, let's try some unsupported operations
    ASSERT_LT(rename(info->path, info->path), 0);
    ASSERT_LT(truncate(info->path, 0), 0);
    ASSERT_LT(utime(info->path, nullptr), 0);

    // Test that a blob fd cannot unmount the entire blobfs.
    zx_status_t status;
    fzl::FdioCaller caller(std::move(fd));
    ASSERT_EQ(fuchsia_io_DirectoryAdminUnmount(caller.borrow_channel(), &status), ZX_OK);
    ASSERT_EQ(status, ZX_ERR_ACCESS_DENIED);
    fd.reset(caller.release().release());

    // Access the file once more, after these operations
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(unlink(info->path), 0);
    ASSERT_EQ(close(fd.release()), 0);
    END_HELPER;
}

// Attempt operations on the root directory
static bool RootDirectory(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_fd dirfd(open(MOUNT_PATH "/.", O_RDONLY));
    ASSERT_TRUE(dirfd, "Cannot open root directory");

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 12, &info));

    // Test operations which should ONLY operate on Blobs
    ASSERT_LT(ftruncate(dirfd.get(), info->size_data), 0);

    char buf[8];
    ASSERT_LT(write(dirfd.get(), buf, 8), 0, "Should not write to directory");
    ASSERT_LT(read(dirfd.get(), buf, 8), 0, "Should not read from directory");

    // Should NOT be able to unlink root dir
    ASSERT_EQ(close(dirfd.release()), 0);
    ASSERT_LT(unlink(info->path), 0);
    END_HELPER;
}

bool TestPartialWrite(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info_complete;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info_partial;
    size_t size = 1 << 20;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, size, &info_complete));
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, size, &info_partial));

    // Partially write out first blob.
    fbl::unique_fd fd_partial(open(info_partial->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd_partial, "Failed to create blob");
    ASSERT_EQ(ftruncate(fd_partial.get(), size), 0);
    ASSERT_EQ(fs_test_utils::StreamAll(write, fd_partial.get(), info_partial->data.get(), size / 2), 0,
              "Failed to write Data");

    // Completely write out second blob.
    fbl::unique_fd fd_complete;
    ASSERT_TRUE(MakeBlob(info_complete.get(), &fd_complete));

    ASSERT_EQ(close(fd_complete.release()), 0);
    ASSERT_EQ(close(fd_partial.release()), 0);
    END_HELPER;
}

bool TestPartialWriteSleepRamdisk(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    if (gUseRealDisk) {
        fprintf(stderr, "Ramdisk required; skipping test\n");
        return true;
    }

    fbl::unique_ptr<fs_test_utils::BlobInfo> info_complete;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info_partial;
    size_t size = 1 << 20;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, size, &info_complete));
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, size, &info_partial));

    // Partially write out first blob.
    fbl::unique_fd fd_partial(open(info_partial->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd_partial, "Failed to create blob");
    ASSERT_EQ(ftruncate(fd_partial.get(), size), 0);
    ASSERT_EQ(fs_test_utils::StreamAll(write, fd_partial.get(), info_partial->data.get(), size / 2), 0,
              "Failed to write Data");

    // Completely write out second blob.
    fbl::unique_fd fd_complete;
    ASSERT_TRUE(MakeBlob(info_complete.get(), &fd_complete));

    ASSERT_EQ(syncfs(fd_complete.get()), 0);
    ASSERT_TRUE(blobfsTest->ToggleSleep());

    ASSERT_EQ(close(fd_complete.release()), 0);
    ASSERT_EQ(close(fd_partial.release()), 0);

    fd_complete.reset(open(info_complete->path, O_RDONLY));
    ASSERT_TRUE(fd_complete, "Failed to re-open blob");

    ASSERT_EQ(syncfs(fd_complete.get()), 0);
    ASSERT_TRUE(blobfsTest->ToggleSleep());

    ASSERT_TRUE(fs_test_utils::VerifyContents(fd_complete.get(), info_complete->data.get(), size));

    fd_partial.reset(open(info_partial->path, O_RDONLY));
    ASSERT_FALSE(fd_partial, "Should not be able to open invalid blob");
    ASSERT_EQ(close(fd_complete.release()), 0);
    END_HELPER;
}

bool TestAlternateWrite(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    size_t num_blobs = 1;
    size_t num_writes = 100;
    unsigned int seed = static_cast<unsigned int>(zx_ticks_get());
    fs_test_utils::BlobList bl(MOUNT_PATH);

    for (size_t i = 0; i < num_blobs; i++) {
        ASSERT_TRUE(bl.CreateBlob(&seed, num_writes));
    }

    for (size_t i = 0; i < num_blobs; i++) {
        ASSERT_TRUE(bl.ConfigBlob());
    }

    for (size_t i = 0; i < num_writes; i++) {
        for (size_t j = 0; j < num_blobs; j++) {
            ASSERT_TRUE(bl.WriteData());
        }
    }

    for (size_t i = 0; i < num_blobs; i++) {
        ASSERT_TRUE(bl.ReopenBlob());
    }

    bl.VerifyAll();

    bl.CloseAll();

    END_HELPER;
}

static bool TestHugeBlobRandom(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;

    // This blob is extremely large, and will remain large
    // on disk. It is not easily compressible.
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 2 * blobfs::WriteBufferSize(),
                                                  &info));

    fbl::unique_fd fd;
    ASSERT_TRUE(MakeBlob(info.get(), &fd));
    ASSERT_EQ(close(fd.release()), 0);

    // We can re-open and verify the Blob as read-only
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);

    // We cannot re-open the blob as writable
    fd.reset(open(info->path, O_RDWR | O_CREAT));
    ASSERT_FALSE(fd, "Shouldn't be able to re-create blob that exists");
    fd.reset(open(info->path, O_RDWR));
    ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");
    fd.reset(open(info->path, O_WRONLY));
    ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");

    // Force decompression by remounting, re-accessing blob.
    ASSERT_TRUE(blobfsTest->Remount());
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);

    ASSERT_EQ(unlink(info->path), 0);
    END_HELPER;
}

static bool TestHugeBlobCompressible(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;

    // This blob is extremely large, and will remain large
    // on disk, even though is very compressible.
    ASSERT_TRUE(fs_test_utils::GenerateBlob([](char* data, size_t length) {
        fs_test_utils::RandomFill(data, length / 2);
        data = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(data) + length / 2);
        memset(data, 'a', length / 2);
    }, MOUNT_PATH, 2 * blobfs::WriteBufferSize(), &info));

    fbl::unique_fd fd;
    ASSERT_TRUE(MakeBlob(info.get(), &fd));
    ASSERT_EQ(close(fd.release()), 0);

    // We can re-open and verify the Blob as read-only
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);

    // We cannot re-open the blob as writable
    fd.reset(open(info->path, O_RDWR | O_CREAT));
    ASSERT_FALSE(fd, "Shouldn't be able to re-create blob that exists");
    fd.reset(open(info->path, O_RDWR));
    ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");
    fd.reset(open(info->path, O_WRONLY));
    ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");

    // Force decompression by remounting, re-accessing blob.
    ASSERT_TRUE(blobfsTest->Remount());
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);

    ASSERT_EQ(unlink(info->path), 0);
    END_HELPER;
}

static bool CreateUmountRemountLarge(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fs_test_utils::BlobList bl(MOUNT_PATH);
    // TODO(smklein): Here, and elsewhere in this file, remove this source
    // of randomness to make the unit test deterministic -- fuzzing should
    // be the tool responsible for introducing randomness into the system.
    unsigned int seed = static_cast<unsigned int>(zx_ticks_get());
    unittest_printf("unmount_remount test using seed: %u\n", seed);

    // Do some operations...
    size_t num_ops = 5000;
    for (size_t i = 0; i < num_ops; ++i) {
        switch (rand_r(&seed) % 6) {
        case 0:
            ASSERT_TRUE(bl.CreateBlob(&seed));
            break;
        case 1:
            ASSERT_TRUE(bl.ConfigBlob());
            break;
        case 2:
            ASSERT_TRUE(bl.WriteData());
            break;
        case 3:
            ASSERT_TRUE(bl.ReadData());
            break;
        case 4:
            ASSERT_TRUE(bl.ReopenBlob());
            break;
        case 5:
            ASSERT_TRUE(bl.UnlinkBlob());
            break;
        }
    }

    // Close all currently opened nodes (REGARDLESS of their state)
    bl.CloseAll();

    // Unmount, remount
    ASSERT_TRUE(blobfsTest->Remount(), "Could not re-mount blobfs");

    // Reopen all (readable) blobs
    bl.OpenAll();

    // Verify state of all blobs
    bl.VerifyAll();

    // Close everything again
    bl.CloseAll();

    END_HELPER;
}

int unmount_remount_thread(void* arg) {
    fs_test_utils::BlobList* bl = static_cast<fs_test_utils::BlobList*>(arg);
    unsigned int seed = static_cast<unsigned int>(zx_ticks_get());
    unittest_printf("unmount_remount thread using seed: %u\n", seed);

    // Do some operations...
    size_t num_ops = 1000;
    for (size_t i = 0; i < num_ops; ++i) {
        switch (rand_r(&seed) % 6) {
        case 0:
            ASSERT_TRUE(bl->CreateBlob(&seed));
            break;
        case 1:
            ASSERT_TRUE(bl->ConfigBlob());
            break;
        case 2:
            ASSERT_TRUE(bl->WriteData());
            break;
        case 3:
            ASSERT_TRUE(bl->ReadData());
            break;
        case 4:
            ASSERT_TRUE(bl->ReopenBlob());
            break;
        case 5:
            ASSERT_TRUE(bl->UnlinkBlob());
            break;
        }
    }

    return 0;
}

static bool CreateUmountRemountLargeMultithreaded(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fs_test_utils::BlobList bl(MOUNT_PATH);

    size_t num_threads = 10;
    fbl::AllocChecker ac;
    fbl::Array<thrd_t> threads(new (&ac) thrd_t[num_threads](), num_threads);
    ASSERT_TRUE(ac.check());

    // Launch all threads
    for (size_t i = 0; i < num_threads; i++) {
        ASSERT_EQ(thrd_create(&threads[i], unmount_remount_thread, &bl),
                  thrd_success);
    }

    // Wait for all threads to complete.
    // Currently, threads will always return a successful status.
    for (size_t i = 0; i < num_threads; i++) {
        int res;
        ASSERT_EQ(thrd_join(threads[i], &res), thrd_success);
        ASSERT_EQ(res, 0);
    }

    // Close all currently opened nodes (REGARDLESS of their state)
    bl.CloseAll();

    // Unmount, remount
    ASSERT_TRUE(blobfsTest->Remount(), "Could not re-mount blobfs");

    // reopen all blobs
    bl.OpenAll();

    // verify all blob contents
    bl.VerifyAll();

    // close everything again
    bl.CloseAll();

    END_HELPER;
}

static bool NoSpace(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> last_info = nullptr;

    // Keep generating blobs until we run out of space
    size_t count = 0;
    while (true) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 17, &info));

        fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
        ASSERT_TRUE(fd, "Failed to create blob");
        int r = ftruncate(fd.get(), info->size_data);
        if (r < 0) {
            ASSERT_EQ(errno, ENOSPC, "Blobfs expected to run out of space");
            // We ran out of space, as expected. Can we allocate if we
            // unlink a previously allocated blob of the desired size?
            ASSERT_EQ(unlink(last_info->path), 0, "Unlinking old blob");
            ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0, "Re-init after unlink");

            // Yay! allocated successfully.
            ASSERT_EQ(close(fd.release()), 0);
            break;
        }
        ASSERT_EQ(fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data), 0,
                  "Failed to write Data");
        ASSERT_EQ(close(fd.release()), 0);
        last_info = std::move(info);

        if (++count % 50 == 0) {
            printf("Allocated %lu blobs\n", count);
        }
    }

    END_HELPER;
}

// The following test attempts to fragment the underlying blobfs partition
// assuming a trivial linear allocator. A more intelligent allocator
// may require modifications to this test.
static bool TestFragmentation(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    // Keep generating blobs until we run out of space, in a pattern of
    // large, small, large, small, large.
    //
    // At the end of  the test, we'll free the small blobs, and observe
    // if it is possible to allocate a larger blob. With a simple allocator
    // and no defragmentation, this would result in a NO_SPACE error.
    constexpr size_t kSmallSize = (1 << 16);
    constexpr size_t kLargeSize = (1 << 17);

    fbl::Vector<fbl::String> small_blobs;

    bool do_small_blob = true;
    size_t count = 0;
    while (true) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH,
                                                      do_small_blob ? kSmallSize : kLargeSize,
                                                      &info));
        fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
        ASSERT_TRUE(fd, "Failed to create blob");
        int r = ftruncate(fd.get(), info->size_data);
        if (r < 0) {
            ASSERT_EQ(ENOSPC, errno, "Blobfs expected to run out of space");
            break;
        }
        ASSERT_EQ(0, fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data),
                  "Failed to write Data");
        ASSERT_EQ(0, close(fd.release()));
        if (do_small_blob) {
            small_blobs.push_back(fbl::String(info->path));
        }

        do_small_blob = !do_small_blob;

        if (++count % 50 == 0) {
            printf("Allocated %lu blobs\n", count);
        }
    }

    // We have filled up the disk with both small and large blobs.
    // Observe that we cannot add another large blob.
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, kLargeSize, &info));

    // Calculate actual number of blocks required to store the blob (including the merkle tree).
    blobfs::Inode large_inode;
    large_inode.blob_size = kLargeSize;
    size_t kLargeBlocks = blobfs::MerkleTreeBlocks(large_inode)
                          + blobfs::BlobDataBlocks(large_inode);

    // We shouldn't have space (before we try allocating) ...
    BlobfsUsage usage;
    ASSERT_TRUE(blobfsTest->CheckInfo(&usage));
    ASSERT_LT(usage.total_bytes - usage.used_bytes, kLargeBlocks * blobfs::kBlobfsBlockSize);

    // ... and we don't have space (as we try allocating).
    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd);
    ASSERT_EQ(-1, ftruncate(fd.get(), info->size_data));
    ASSERT_EQ(ENOSPC, errno, "Blobfs expected to be out of space");

    // Unlink all small blobs -- except for the last one, since
    // we may have free trailing space at the end.
    for (size_t i = 0; i < small_blobs.size() - 1; i++) {
        ASSERT_EQ(0, unlink(small_blobs[i].c_str()), "Unlinking old blob");
    }

    // This asserts an assumption of our test: Freeing these blobs
    // should provde enough space.
    ASSERT_GT(kSmallSize * (small_blobs.size() - 1), kLargeSize);

    // Validate that we have enough space (before we try allocating)...
    ASSERT_TRUE(blobfsTest->CheckInfo(&usage));
    ASSERT_GE(usage.total_bytes - usage.used_bytes, kLargeBlocks * blobfs::kBlobfsBlockSize);

    // Now that blobfs supports extents, verify that we can still allocate
    // a large blob, even if it is fragmented.
    ASSERT_EQ(0, ftruncate(fd.get(), info->size_data));

    // Sanity check that we can write and read the fragmented blob.
    ASSERT_EQ(0, fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data));
    fbl::unique_ptr<char[]> buf(new char[info->size_data]);
    ASSERT_EQ(0, lseek(fd.get(), 0, SEEK_SET));
    ASSERT_EQ(0, fs_test_utils::StreamAll(read, fd.get(), buf.get(), info->size_data));
    ASSERT_EQ(0, memcmp(info->data.get(), buf.get(), info->size_data));
    ASSERT_EQ(0, close(fd.release()));

    // Sanity check that we can re-open and unlink the fragmented blob.
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd);
    ASSERT_EQ(0, unlink(info->path));
    ASSERT_EQ(0, close(fd.release()));

    END_HELPER;
}

static bool QueryDevicePath(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_fd dirfd(open(MOUNT_PATH "/.", O_RDONLY | O_ADMIN));
    ASSERT_TRUE(dirfd, "Cannot open root directory");

    char device_buffer[1024];
    char* device_path = static_cast<char*>(device_buffer);
    zx_status_t status;
    size_t path_len;
    fzl::FdioCaller caller(std::move(dirfd));
    ASSERT_EQ(fuchsia_io_DirectoryAdminGetDevicePath(caller.borrow_channel(), &status,
                                                     device_path, sizeof(device_buffer),
                                                     &path_len), ZX_OK);
    dirfd = caller.release();
    ASSERT_EQ(status, ZX_OK);
    ASSERT_GT(path_len, 0, "Device path not found");

    char actual_path[PATH_MAX];
    ASSERT_TRUE(blobfsTest->GetDevicePath(actual_path, PATH_MAX));
    ASSERT_EQ(strncmp(actual_path, device_path, path_len), 0, "Unexpected device path");
    ASSERT_EQ(close(dirfd.release()), 0);

    dirfd.reset(open(MOUNT_PATH "/.", O_RDONLY));
    ASSERT_TRUE(dirfd, "Cannot open root directory");
    caller.reset(std::move(dirfd));
    ASSERT_EQ(fuchsia_io_DirectoryAdminGetDevicePath(caller.borrow_channel(), &status,
                                                     device_path, sizeof(device_buffer),
                                                     &path_len), ZX_OK);
    dirfd = caller.release();
    ASSERT_EQ(status, ZX_ERR_ACCESS_DENIED);
    ASSERT_EQ(path_len, 0);
    ASSERT_EQ(close(dirfd.release()), 0);
    END_HELPER;
}

static bool TestReadOnly(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    // Mount the filesystem as read-write.
    // We can create new blobs.
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 10, &info));
    fbl::unique_fd blob_fd;
    ASSERT_TRUE(MakeBlob(info.get(), &blob_fd));
    ASSERT_TRUE(fs_test_utils::VerifyContents(blob_fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(blob_fd.release()), 0);

    blobfsTest->SetReadOnly(true);
    ASSERT_TRUE(blobfsTest->Remount());

    // We can read old blobs
    blob_fd.reset(open(info->path, O_RDONLY));
    ASSERT_GE(blob_fd.get(), 0);
    ASSERT_TRUE(fs_test_utils::VerifyContents(blob_fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(blob_fd.release()), 0);

    // We cannot create new blobs
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 10, &info));
    ASSERT_LT(open(info->path, O_CREAT | O_RDWR), 0);
    END_HELPER;
}

// This tests growing both additional inodes and blocks
static bool ResizePartition(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    ASSERT_EQ(blobfsTest->GetType(), FsTestType::kFvm);

    // Create 1000 blobs. Test slices are small enough that this will require both inodes and
    // blocks to be added
    for (size_t d = 0; d < 1000; d++) {
        if (d % 100 == 0) {
            printf("Creating blob: %lu\n", d);
        }

        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 64, &info));

        fbl::unique_fd fd;
        ASSERT_TRUE(MakeBlob(info.get(), &fd));
        ASSERT_EQ(close(fd.release()), 0);
    }

    // Remount partition
    ASSERT_TRUE(blobfsTest->Remount(), "Could not re-mount blobfs");

    DIR* dir = opendir(MOUNT_PATH);
    ASSERT_NONNULL(dir);
    unsigned entries_deleted = 0;
    char path[PATH_MAX];
    struct dirent* de;

    // Unlink all blobs
    while ((de = readdir(dir)) != nullptr) {
        if (entries_deleted % 100 == 0) {
            printf("Unlinking blob: %u\n", entries_deleted);
        }
        strcpy(path, MOUNT_PATH "/");
        strcat(path, de->d_name);
        ASSERT_EQ(unlink(path), 0);
        entries_deleted++;
    }

    printf("Completing test\n");
    ASSERT_EQ(closedir(dir), 0);
    ASSERT_EQ(entries_deleted, 1000);
    END_HELPER;
}

static bool CorruptAtMount(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    ASSERT_EQ(blobfsTest->GetType(), FsTestType::kFvm);
    ASSERT_TRUE(blobfsTest->Teardown());
    ASSERT_TRUE(blobfsTest->Reset());
    ASSERT_TRUE(blobfsTest->Init(FsTestState::kMinimal), "Mounting Blobfs");

    char device_path[PATH_MAX];
    ASSERT_TRUE(blobfsTest->GetDevicePath(device_path, PATH_MAX));
    ASSERT_EQ(mkfs(device_path, DISK_FORMAT_BLOBFS, launch_stdio_sync, &default_mkfs_options),
              ZX_OK);

    fbl::unique_fd fd(blobfsTest->GetFd());
    ASSERT_TRUE(fd, "Could not open ramdisk");
    fzl::UnownedFdioCaller caller(fd.get());

    // Manually shrink slice so FVM will differ from Blobfs.
    uint64_t offset = blobfs::kFVMNodeMapStart / kBlocksPerSlice;
    uint64_t length = 1;
    zx_status_t status;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(caller.borrow_channel(), offset,
                                                         length, &status), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    // Verify that shrink was successful.
    uint64_t start_slices[1];
    start_slices[0] = offset;
    fuchsia_hardware_block_volume_VsliceRange
            ranges[fuchsia_hardware_block_volume_MAX_SLICE_REQUESTS];
    size_t actual_ranges_count;

    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                caller.borrow_channel(), start_slices, fbl::count_of(start_slices), &status,
                ranges, &actual_ranges_count), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(actual_ranges_count, 1);
    ASSERT_FALSE(ranges[0].allocated);
    ASSERT_EQ(ranges[0].count,
              (blobfs::kFVMJournalStart - blobfs::kFVMNodeMapStart) / kBlocksPerSlice);

    // Attempt to mount the VPart. This should fail since slices are missing.
    mount_options_t options = default_mount_options;
    options.enable_journal = gEnableJournal;
    caller.reset();
    ASSERT_NE(mount(fd.release(), MOUNT_PATH, DISK_FORMAT_BLOBFS, &options,
                    launch_stdio_async), ZX_OK);

    fd.reset(blobfsTest->GetFd());
    ASSERT_TRUE(fd, "Could not open ramdisk");
    caller.reset(fd.get());

    // Manually grow slice count to twice what it was initially.
    length = 2;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(caller.borrow_channel(), offset,
                                                         length, &status), ZX_OK);
    ASSERT_EQ(status, ZX_OK);

    // Verify that extend was successful.
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                caller.borrow_channel(), start_slices, fbl::count_of(start_slices), &status,
                ranges, &actual_ranges_count), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(actual_ranges_count, 1);
    ASSERT_TRUE(ranges[0].allocated);
    ASSERT_EQ(ranges[0].count, 2);

    // Attempt to mount the VPart. This should succeed.
    caller.reset();
    ASSERT_EQ(mount(fd.release(), MOUNT_PATH, DISK_FORMAT_BLOBFS, &options,
                    launch_stdio_async), ZX_OK);

    ASSERT_EQ(umount(MOUNT_PATH), ZX_OK);
    fd.reset(blobfsTest->GetFd());
    ASSERT_TRUE(fd, "Could not open ramdisk");
    caller.reset(fd.get());

    // Verify that mount automatically removed extra slice.
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                caller.borrow_channel(), start_slices, fbl::count_of(start_slices), &status,
                ranges, &actual_ranges_count), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(actual_ranges_count, 1);
    ASSERT_TRUE(ranges[0].allocated);
    ASSERT_EQ(ranges[0].count, 1);
    END_HELPER;
}

typedef struct reopen_data {
    char path[PATH_MAX];
    std::atomic_bool complete;
} reopen_data_t;

int reopen_thread(void* arg) {
    reopen_data_t* dat = static_cast<reopen_data_t*>(arg);
    unsigned attempts = 0;
    while (!atomic_load(&dat->complete)) {
        fbl::unique_fd fd(open(dat->path, O_RDONLY));
        ASSERT_TRUE(fd);
        ASSERT_EQ(close(fd.release()), 0);
        attempts++;
    }

    printf("Reopened %u times\n", attempts);
    return 0;
}

// The purpose of this test is to repro the case where a blob is being retrieved from the blob hash
// at the same time it is being destructed, causing an invalid vnode to be returned. This can only
// occur when the client is opening a new fd to the blob at the same time it is being destructed
// after all writes to disk have completed.
// This test works best if a sleep is added at the beginning of fbl_recycle in VnodeBlob.
static bool CreateWriteReopen(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    size_t num_ops = 10;

    fbl::unique_ptr<fs_test_utils::BlobInfo> anchor_info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 10, &anchor_info));

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 10 * (1 << 20), &info));
    reopen_data_t dat;
    strcpy(dat.path, info->path);

    for (size_t i = 0; i < num_ops; i++) {
        printf("Running op %lu... ", i);
        fbl::unique_fd fd;
        fbl::unique_fd anchor_fd;
        atomic_store(&dat.complete, false);

        // Write both blobs to disk (without verification, so we can start reopening the blob asap)
        ASSERT_TRUE(MakeBlobUnverified(info.get(), &fd));
        ASSERT_TRUE(MakeBlobUnverified(anchor_info.get(), &anchor_fd));
        ASSERT_EQ(close(fd.release()), 0);

        int result;
        int success;
        thrd_t thread;
        ASSERT_EQ(thrd_create(&thread, reopen_thread, &dat), thrd_success);

        {
            // In case the test fails, always join the thread before returning from the test.
            auto join_thread = fbl::MakeAutoCall([&]() {
                atomic_store(&dat.complete, true);
                success = thrd_join(thread, &result);
            });

            // Sleep while the thread continually opens and closes the blob
            usleep(1000000);
            ASSERT_EQ(syncfs(anchor_fd.get()), 0);
        }

        ASSERT_EQ(success, thrd_success);
        ASSERT_EQ(result, 0);

        ASSERT_EQ(close(anchor_fd.release()), 0);
        ASSERT_EQ(unlink(info->path), 0);
        ASSERT_EQ(unlink(anchor_info->path), 0);
    }

    END_HELPER;
}

static bool TestCreateFailure(void) {
    BEGIN_TEST;
    BlobfsTest blobfsTest(FsTestType::kNormal);
    blobfsTest.SetStdio(false);
    ASSERT_TRUE(blobfsTest.Init(), "Mounting Blobfs");

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, blobfs::kBlobfsBlockSize, &info));

    size_t blocks = 0;

    // Attempt to create a blob, failing after each written block until the operations succeeds.
    // After each failure, check for disk consistency.
    while (true) {
        ASSERT_TRUE(blobfsTest.ToggleSleep(blocks));
        unittest_set_output_function(silent_printf, nullptr);
        fbl::unique_fd fd;

        // Blob creation may or may not succeed - as long as fsck passes, it doesn't matter.
        MakeBlob(info.get(), &fd);
        current_test_info->all_ok = true;

        // Resolve all transactions before waking the ramdisk.
        syncfs(fd.get());
        unittest_restore_output_function();
        ASSERT_TRUE(blobfsTest.ToggleSleep());

        // Force remount so journal will replay.
        ASSERT_TRUE(blobfsTest.ForceRemount());

        // Remount again to check fsck results.
        ASSERT_TRUE(blobfsTest.Remount());

        // Once file creation is successful, break out of the loop.
        fd.reset(open(info->path, O_RDONLY));
        if (fd) break;

        blocks++;
    }

    ASSERT_TRUE(blobfsTest.Teardown(), "Unmounting Blobfs");
    END_TEST;
}

static bool TestExtendFailure(void) {
    BEGIN_TEST;
    BlobfsTest blobfsTest(FsTestType::kFvm);
    blobfsTest.SetStdio(false);
    ASSERT_TRUE(blobfsTest.Init(), "Mounting Blobfs");

    BlobfsUsage original_usage;
    blobfsTest.CheckInfo(&original_usage);

    // Create a blob of the maximum size possible without causing an FVM extension.
    fbl::unique_ptr<fs_test_utils::BlobInfo> old_info;
    ASSERT_TRUE(
        fs_test_utils::GenerateRandomBlob(MOUNT_PATH,
                                          original_usage.total_bytes - blobfs::kBlobfsBlockSize,
                                          &old_info));

    fbl::unique_fd fd;
    ASSERT_TRUE(MakeBlob(old_info.get(), &fd));
    ASSERT_EQ(syncfs(fd.get()), 0);
    ASSERT_EQ(close(fd.release()), 0);

    // Ensure that an FVM extension did not occur.
    BlobfsUsage current_usage;
    blobfsTest.CheckInfo(&current_usage);
    ASSERT_EQ(current_usage.total_bytes, original_usage.total_bytes);

    // Generate another blob of the smallest size possible.
    fbl::unique_ptr<fs_test_utils::BlobInfo> new_info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, blobfs::kBlobfsBlockSize,
                                                  &new_info));

    // Since the FVM metadata covers a large range of blocks, it will take a while to test a
    // ramdisk failure after each individual block. Since we mostly care about what happens with
    // blobfs after the extension succeeds on the FVM side, test a maximum of |metadata_failures|
    // failures within the FVM metadata write itself.
    size_t metadata_size = fvm::MetadataSize(blobfsTest.GetDiskSize(), kTestFvmSliceSize);
    size_t metadata_blocks = metadata_size / blobfsTest.GetBlockSize();
    size_t metadata_failures = 16;
    size_t increment = metadata_blocks / fbl::min(metadata_failures, metadata_blocks);

    // Round down the metadata block count so we don't miss testing the transaction immediately
    // after the metadata write succeeds.
    metadata_blocks = fbl::round_down(metadata_blocks, increment);
    size_t blocks = 0;

    while (true) {
        ASSERT_TRUE(blobfsTest.ToggleSleep(blocks));
        unittest_set_output_function(silent_printf, nullptr);

        // Blob creation may or may not succeed - as long as fsck passes, it doesn't matter.
        MakeBlob(new_info.get(), &fd);
        current_test_info->all_ok = true;

        // Resolve all transactions before waking the ramdisk.
        syncfs(fd.get());

        unittest_restore_output_function();
        ASSERT_TRUE(blobfsTest.ToggleSleep());

        // Force remount so journal will replay.
        ASSERT_TRUE(blobfsTest.ForceRemount());

        // Remount again to check fsck results.
        ASSERT_TRUE(blobfsTest.Remount());

        // Check that the original blob still exists.
        fd.reset(open(old_info->path, O_RDONLY));
        ASSERT_TRUE(fd);

        // Once file creation is successful, break out of the loop.
        fd.reset(open(new_info->path, O_RDONLY));
        if (fd) {
            struct stat stats;
            ASSERT_EQ(fstat(fd.get(), &stats), 0);
            ASSERT_EQ(static_cast<uint64_t>(stats.st_size), old_info->size_data);
            break;
        }

        if (blocks >= metadata_blocks) {
            blocks++;
        } else {
            blocks += increment;
        }
    }

    // Ensure that an FVM extension occurred.
    blobfsTest.CheckInfo(&current_usage);
    ASSERT_GT(current_usage.total_bytes, original_usage.total_bytes);

    ASSERT_TRUE(blobfsTest.Teardown(), "Unmounting Blobfs");
    END_TEST;
}

static bool TestFailedWrite(BlobfsTest* blobfsTest) {
    BEGIN_TEST;

    if (gUseRealDisk) {
        fprintf(stderr, "Ramdisk required; skipping test\n");
        return true;
    }

    uint64_t block_size = blobfsTest->GetBlockSize();
    ASSERT_EQ(blobfs::kBlobfsBlockSize % block_size, 0);
    const uint64_t kDiskBlocksPerBlobfsBlock = blobfs::kBlobfsBlockSize / block_size;

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, blobfs::kBlobfsBlockSize, &info));

    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");
    // Truncate before sleeping the ramdisk. This is so potential FVM updates
    // do not interfere with the ramdisk block count.
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);

    // Journal:
    // - One Superblock block
    // - One Inode table block
    // - One Bitmap block
    //
    // Non-journal:
    // - One Inode table block
    // - One Data block
    constexpr uint64_t kBlockCountToWrite = 5;
    // Sleep after |kBlockCountToWrite - 1| blocks. This is 1 less than will be
    // needed to write out the entire blob. This ensures that writing the blob
    // will ultimately fail, but the write operation will return a successful
    // response.
    ASSERT_TRUE(blobfsTest->ToggleSleep(kDiskBlocksPerBlobfsBlock * (kBlockCountToWrite - 1)));
    ASSERT_EQ(write(fd.get(), info->data.get(), info->size_data),
              static_cast<ssize_t>(info->size_data));

    // Since the write operation ultimately failed when going out to disk,
    // syncfs will return a failed response.
    ASSERT_LT(syncfs(fd.get()), 0);
    ASSERT_EQ(errno, EPIPE);

    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, blobfs::kBlobfsBlockSize, &info));
    fd.reset(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");

    if (blobfsTest->GetType() == FsTestType::kFvm) {
        // On an FVM, truncate may either succeed or fail. If an FVM extend call is necessary,
        // it will fail since the ramdisk is asleep; otherwise, it will pass.
        ftruncate(fd.get(), info->size_data);
    } else {
        // Since truncate is done entirely in memory, a non-FVM truncate should always pass.
        ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);
    }

    // Since the ramdisk is asleep and our blobfs is aware of it due to the sync, write should fail.
    ASSERT_LT(write(fd.get(), info->data.get(), blobfs::kBlobfsBlockSize), 0);
    ASSERT_EQ(errno, EPIPE);

    // Wake the ramdisk and forcibly remount the BlobfsTest, forcing the journal to replay
    // and restore blobfs to a consistent state.
    ASSERT_TRUE(blobfsTest->ToggleSleep());
    ASSERT_TRUE(blobfsTest->ForceRemount());

    // Reset the ramdisk counts so we don't attempt to run ramdisk failure tests,
    // since we are already failing the ramdisk within this test.
    ASSERT_TRUE(blobfsTest->ToggleSleep());
    ASSERT_TRUE(blobfsTest->ToggleSleep());
    END_TEST;
}

static bool TestLargeBlob() {
    BEGIN_TEST;

    BlobfsTest blobfsTest(FsTestType::kNormal);

    // Create blobfs with enough data blocks to ensure 2 block bitmap blocks.
    blobfs::Superblock superblock;
    superblock.flags = 0;
    superblock.inode_count = blobfs::kBlobfsDefaultInodeCount;
    superblock.journal_block_count = blobfs::kDefaultJournalBlocks;
    superblock.data_block_count = 2 * blobfs::kBlobfsBlockBits;
    ASSERT_EQ(superblock.data_block_count % 2, 0);

    uint64_t blobfs_blocks = blobfs::TotalBlocks(superblock);
    uint64_t ramdisk_blocks = (blobfs_blocks * blobfs::kBlobfsBlockSize)
                              / blobfsTest.GetBlockSize();
    blobfsTest.SetBlockCount(ramdisk_blocks);

    ASSERT_TRUE(blobfsTest.Init());

    // Create (and delete) a blob large enough to overflow into the second bitmap block.
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    size_t blob_size = ((superblock.data_block_count / 2) + 1) * blobfs::kBlobfsBlockSize;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, blob_size, &info));

    fbl::unique_fd fd;
    ASSERT_TRUE(MakeBlob(info.get(), &fd));
    ASSERT_EQ(syncfs(fd.get()), 0);
    ASSERT_EQ(close(fd.release()), 0);
    ASSERT_EQ(unlink(info->path), 0);

    ASSERT_TRUE(blobfsTest.Teardown());
    END_TEST;
}

// TODO(ZX-2416): Add tests to manually corrupt journal entries/metadata.

BEGIN_TEST_CASE(blobfs_tests)
RUN_TESTS(MEDIUM, TestBasic)
RUN_TESTS(MEDIUM, TestUnallocatedBlob)
RUN_TESTS(MEDIUM, TestNullBlob)
RUN_TESTS(MEDIUM, TestCompressibleBlob)
RUN_TESTS(MEDIUM, TestMmap)
RUN_TESTS(MEDIUM, TestMmapUseAfterClose)
RUN_TESTS(MEDIUM, TestReaddir)
RUN_TESTS(MEDIUM, TestDiskTooSmall)
RUN_TEST_FVM(MEDIUM, TestQueryInfo)
RUN_TESTS(MEDIUM, TestGetAllocatedRegions)
RUN_TESTS(MEDIUM, UseAfterUnlink)
RUN_TESTS(MEDIUM, WriteAfterRead)
RUN_TESTS(MEDIUM, WriteAfterUnlink)
RUN_TESTS(MEDIUM, ReadTooLarge)
RUN_TESTS(MEDIUM, BadAllocation)
RUN_TESTS_SILENT(MEDIUM, CorruptedBlob)
RUN_TESTS_SILENT(MEDIUM, CorruptedDigest)
RUN_TESTS(MEDIUM, EdgeAllocation)
RUN_TESTS(MEDIUM, UmountWithOpenFile)
RUN_TESTS(MEDIUM, UmountWithMappedFile)
RUN_TESTS(MEDIUM, UmountWithOpenMappedFile)
RUN_TESTS(MEDIUM, CreateUmountRemountSmall)
RUN_TESTS(MEDIUM, EarlyRead)
RUN_TESTS(MEDIUM, WaitForRead)
RUN_TESTS(MEDIUM, WriteSeekIgnored)
RUN_TESTS(MEDIUM, UnlinkTiming)
RUN_TESTS(MEDIUM, InvalidOps)
RUN_TESTS(MEDIUM, RootDirectory)
RUN_TESTS(MEDIUM, TestPartialWrite)
RUN_TESTS(MEDIUM, TestPartialWriteSleepRamdisk)
RUN_TESTS(MEDIUM, TestAlternateWrite)
RUN_TESTS(LARGE, TestHugeBlobRandom)
RUN_TESTS(LARGE, TestHugeBlobCompressible)
RUN_TESTS(LARGE, CreateUmountRemountLarge)
RUN_TESTS(LARGE, CreateUmountRemountLargeMultithreaded)
RUN_TESTS(LARGE, NoSpace)
RUN_TESTS(LARGE, TestFragmentation)
RUN_TESTS(MEDIUM, QueryDevicePath)
RUN_TESTS(MEDIUM, TestReadOnly)
RUN_TEST_FVM(MEDIUM, ResizePartition)
RUN_TEST_FVM(MEDIUM, CorruptAtMount)
RUN_TESTS(LARGE, CreateWriteReopen)
RUN_TEST_MEDIUM(TestCreateFailure)
RUN_TEST_MEDIUM(TestExtendFailure)
RUN_TEST_LARGE(TestLargeBlob)
RUN_TESTS(SMALL, TestFailedWrite)
END_TEST_CASE(blobfs_tests)

// TODO(planders): revamp blobfs test options.
static void print_test_help(FILE* f) {
    fprintf(f,
            "  -d <blkdev>\n"
            "      Use block device <blkdev> instead of a ramdisk\n"
            "  -f <count>\n"
            "      For each test, run the test <count> additional times,\n"
            "        intentionally causing the underlying device driver to\n"
            "        'sleep' after a certain number of block writes.\n"
            "      After each additional test, the blobfs partition will be\n"
            "        remounted and checked for consistency via fsck.\n"
            "      If <count> is 0, the maximum number of tests are run.\n"
            "      This option is only valid when using a ramdisk.\n"
            "  -j\n"
            "      Disable the journal\n"
            "\n");
}

int main(int argc, char** argv) {
    unittest_register_test_help_printer(print_test_help);

    int i = 1;
    while (i < argc) {
        if (!strcmp(argv[i], "-d") && (i + 1 < argc)) {
            fbl::unique_fd fd(open(argv[i + 1], O_RDWR));
            if (!fd) {
                fprintf(stderr, "[fs] Could not open block device\n");
                return -1;
            }
            fzl::FdioCaller caller(std::move(fd));
            zx_status_t status;
            size_t path_len;
            zx_status_t io_status = fuchsia_device_ControllerGetTopologicalPath(
                    caller.borrow_channel(), &status, gRealDiskInfo.disk_path,
                    PATH_MAX - 1, &path_len);
            if (io_status != ZX_OK) {
                status = io_status;
            }
            if (status != ZX_OK) {
                fprintf(stderr, "[fs] Could not acquire topological path of block device\n");
                return -1;
            }
            gRealDiskInfo.disk_path[path_len] = 0;

            // If we previously tried running tests on this disk, it may
            // have created an FVM and failed. (Try to) clean up from previous state
            // before re-running.
            fvm_destroy(gRealDiskInfo.disk_path);

            fuchsia_hardware_block_BlockInfo block_info;
            io_status = fuchsia_hardware_block_BlockGetInfo(caller.borrow_channel(), &status,
                                                            &block_info);
            if (io_status != ZX_OK) {
                status = io_status;
            }

            if (status != ZX_OK) {
                fprintf(stderr, "[fs] Could not query block device info\n");
                return -1;
            }

            gUseRealDisk = true;
            gRealDiskInfo.blk_size = block_info.block_size;
            gRealDiskInfo.blk_count = block_info.block_count;

            size_t disk_size = gRealDiskInfo.blk_size * gRealDiskInfo.blk_count;
            if (disk_size < kBytesNormalMinimum) {
                fprintf(stderr, "Error: Insufficient disk space for tests");
                return -1;
            } else if (disk_size < kTotalBytesFvmMinimum) {
                fprintf(stderr, "Error: Insufficient disk space for FVM tests");
                return -1;
            }
            i += 2;
        } else if (!strcmp(argv[i], "-f") && (i + 1 < argc)) {
            gEnableRamdiskFailure = true;
            gRamdiskFailureLoops = atoi(argv[i + 1]);
            i += 2;
        } else if (!strcmp(argv[i], "-j")) {
            gEnableJournal = false;
            i++;
        } else {
            // Ignore options we don't recognize. See ulib/unittest/README.md.
            break;
        }
    }

    if (gUseRealDisk && gEnableRamdiskFailure) {
        fprintf(stderr, "Error: Ramdisk failure not allowed for real disk\n");
        return -1;
    }

    // Initialize tmpfs.
    async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
    if (loop.StartThread() != ZX_OK) {
        fprintf(stderr, "Error: Cannot initialize local tmpfs loop\n");
        return -1;
    }
    if (memfs_install_at(loop.dispatcher(), TMPFS_PATH) != ZX_OK) {
        fprintf(stderr, "Error: Cannot install local tmpfs\n");
        return -1;
    }

    return unittest_run_all_tests(argc, argv) ? 0 : -1;
}
