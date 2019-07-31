// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/paver/paver.h>

#include <fs-management/fvm.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <zxtest/zxtest.h>

#include "test/test-utils.h"

namespace {

using devmgr_integration_test::IsolatedDevmgr;
using devmgr_integration_test::RecursiveWaitForFile;

constexpr size_t kSliceSize = kBlockSize * 2;
constexpr uint8_t kFvmType[GPT_GUID_LEN] = GUID_FVM_VALUE;

} // namespace

class FvmTest : public zxtest::Test {
public:
    FvmTest() {
        devmgr_launcher::Args args;
        args.sys_device_driver = IsolatedDevmgr::kSysdevDriver;
        args.driver_search_paths.push_back("/boot/driver");
        args.use_system_svchost = true;
        args.disable_block_watcher = true;
        ASSERT_EQ(IsolatedDevmgr::Create(std::move(args), &devmgr_), ZX_OK);

        BlockDevice::Create(devmgr_.devfs_root(), kFvmType, &device_);
        ASSERT_TRUE(device_);
    }

    int borrow_fd() {
        return device_->fd();
    }

    fbl::unique_fd fd() {
        return fbl::unique_fd(dup(device_->fd()));
    }

private:
    IsolatedDevmgr devmgr_;
    std::unique_ptr<BlockDevice> device_;
};

TEST_F(FvmTest, FormatFvmEmpty) {
    fbl::unique_fd fvm_part = FvmPartitionFormat(fd(), kSliceSize, paver::BindOption::Reformat);
    ASSERT_TRUE(fvm_part.is_valid());
}

TEST_F(FvmTest, TryBindEmpty) {
    fbl::unique_fd fvm_part = FvmPartitionFormat(fd(), kSliceSize, paver::BindOption::TryBind);
    ASSERT_TRUE(fvm_part.is_valid());
}

TEST_F(FvmTest, TryBindAlreadyFormatted) {
    ASSERT_OK(fvm_init(borrow_fd(), kSliceSize));
    fbl::unique_fd fvm_part = FvmPartitionFormat(fd(), kSliceSize, paver::BindOption::TryBind);
    ASSERT_TRUE(fvm_part.is_valid());
}

TEST_F(FvmTest, TryBindAlreadyBound) {
    fbl::unique_fd fvm_part = FvmPartitionFormat(fd(), kSliceSize, paver::BindOption::Reformat);
    ASSERT_TRUE(fvm_part.is_valid());

    fvm_part = FvmPartitionFormat(fd(), kSliceSize, paver::BindOption::TryBind);
    ASSERT_TRUE(fvm_part.is_valid());
}

TEST_F(FvmTest, TryBindAlreadyFormattedWrongSliceSize) {
    ASSERT_OK(fvm_init(borrow_fd(), kSliceSize * 2));
    fbl::unique_fd fvm_part = FvmPartitionFormat(fd(), kSliceSize, paver::BindOption::TryBind);
    ASSERT_TRUE(fvm_part.is_valid());
}
