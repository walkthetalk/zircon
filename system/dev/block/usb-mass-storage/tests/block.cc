// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "../block.h"
#include "../usb-mass-storage.h"
#include <fbl/string.h>
#include <lib/fake_ddk/fake_ddk.h>

#include <unittest/unittest.h>

namespace {

struct Context {
    ums::UmsBlockDevice* dev;
    fbl::String name;
    block_info_t info;
    block_op_t* op;
    zx_status_t status;
    ums::Transaction* txn;
};

class Binder : public fake_ddk::Bind {
public:
    zx_status_t DeviceRemove(zx_device_t* dev) {
        Context* context = reinterpret_cast<Context*>(dev);
        context->dev->DdkRelease();
        return ZX_OK;
    }
    zx_status_t DeviceAdd(zx_driver_t* drv, zx_device_t* parent, device_add_args_t* args,
                          zx_device_t** out) {
        *out = parent;
        Context* context = reinterpret_cast<Context*>(parent);
        context->name = fbl::String(args->name);
        return ZX_OK;
    }
};

static void BlockCallback(void* ctx, zx_status_t status, block_op_t* op) {
    Context* context = reinterpret_cast<Context*>(ctx);
    context->status = status;
    context->op = op;
}

bool UmsBlockDeviceConstructorTest() {
    BEGIN_TEST;
    Binder ddk;
    Context context;
    ums::UmsBlockDevice dev(reinterpret_cast<zx_device_t*>(&context), 5,
                            [&](ums::Transaction* txn) { context.txn = txn; });
    context.dev = &dev;
    ums::BlockDeviceParameters params = {};
    params.lun = 5;
    EXPECT_TRUE(params == dev.GetBlockDeviceParameters(),
                "Parameters must be set to user-provided values.");
    dev.Adopt();
    EXPECT_TRUE(dev.Release(), "Expected to free the device");
    END_TEST;
}

bool UmsBlockDeviceAddTest() {
    BEGIN_TEST;
    Binder ddk;
    Context context;
    ums::UmsBlockDevice dev(reinterpret_cast<zx_device_t*>(&context), 5,
                            [&](ums::Transaction* txn) { context.txn = txn; });
    context.dev = &dev;
    ums::BlockDeviceParameters params = {};
    params.lun = 5;
    EXPECT_TRUE(params == dev.GetBlockDeviceParameters(),
                "Parameters must be set to user-provided values.");
    dev.Adopt();
    EXPECT_EQ(ZX_OK, dev.Add(), "Expected Add to succeed");
    EXPECT_EQ(ZX_OK, dev.DdkRemove(), "Expected DdkRemove to succeed");
    EXPECT_TRUE(dev.Release(), "Expected to free the device");
    END_TEST;
}

bool UmsBlockDeviceGetSizeTest() {
    BEGIN_TEST;
    Binder ddk;
    Context context;
    ums::UmsBlockDevice dev(reinterpret_cast<zx_device_t*>(&context), 5,
                            [&](ums::Transaction* txn) { context.txn = txn; });
    context.dev = &dev;
    ums::BlockDeviceParameters params = {};
    params.lun = 5;
    dev.Adopt();
    EXPECT_TRUE(params == dev.GetBlockDeviceParameters(),
                "Parameters must be set to user-provided values.");
    EXPECT_EQ(ZX_OK, dev.Add(), "Expected Add to succeed");
    EXPECT_TRUE(fbl::String("lun-005") == context.name);
    params = dev.GetBlockDeviceParameters();
    params.block_size = 15;
    params.total_blocks = 700;
    context.info.block_size = params.block_size;
    context.info.block_count = params.total_blocks;
    dev.SetBlockDeviceParameters(params);
    EXPECT_EQ(params.block_size * params.total_blocks, dev.DdkGetSize());
    EXPECT_EQ(ZX_OK, dev.DdkRemove(), "Expected DdkRemove to succeed");
    EXPECT_TRUE(dev.Release(), "Expected to free the device");
    END_TEST;
}

bool UmsBlockDeviceNotSupportedTest() {
    BEGIN_TEST;
    Binder ddk;
    Context context;
    ums::UmsBlockDevice dev(reinterpret_cast<zx_device_t*>(&context), 5,
                            [&](ums::Transaction* txn) { context.txn = txn; });
    context.dev = &dev;
    dev.Adopt();
    EXPECT_EQ(ZX_OK, dev.Add(), "Expected Add to succeed");
    EXPECT_TRUE(fbl::String("lun-005") == context.name);
    ums::Transaction txn;
    txn.op.command = BLOCK_OP_MASK;
    dev.BlockImplQueue(&txn.op, BlockCallback, &context);
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, context.status);
    EXPECT_EQ(ZX_OK, dev.DdkRemove(), "Expected DdkRemove to succeed");
    EXPECT_TRUE(dev.Release(), "Expected to free the device");
    END_TEST;
}

bool UmsBlockDeviceReadTest() {
    BEGIN_TEST;
    Binder ddk;
    Context context;
    ums::UmsBlockDevice dev(reinterpret_cast<zx_device_t*>(&context), 5,
                            [&](ums::Transaction* txn) { context.txn = txn; });
    context.dev = &dev;
    dev.Adopt();
    EXPECT_EQ(ZX_OK, dev.Add(), "Expected Add to succeed");
    EXPECT_TRUE(fbl::String("lun-005") == context.name);
    ums::Transaction txn;
    txn.op.command = BLOCK_OP_READ;
    dev.BlockImplQueue(&txn.op, BlockCallback, &context);
    EXPECT_EQ(ZX_OK, dev.DdkRemove(), "Expected DdkRemove to succeed");
    EXPECT_TRUE(dev.Release(), "Expected to free the device");
    END_TEST;
}

bool UmsBlockDeviceWriteTest() {
    BEGIN_TEST;
    Binder ddk;
    Context context;
    ums::UmsBlockDevice dev(reinterpret_cast<zx_device_t*>(&context), 5,
                            [&](ums::Transaction* txn) { context.txn = txn; });
    context.dev = &dev;
    dev.Adopt();
    EXPECT_EQ(ZX_OK, dev.Add(), "Expected Add to succeed");
    EXPECT_TRUE(fbl::String("lun-005") == context.name);
    ums::Transaction txn;
    txn.op.command = BLOCK_OP_WRITE;
    dev.BlockImplQueue(&txn.op, BlockCallback, &context);
    EXPECT_EQ(&txn, context.txn);
    EXPECT_EQ(ZX_OK, dev.DdkRemove(), "Expected DdkRemove to succeed");
    EXPECT_TRUE(dev.Release(), "Expected to free the device");
    END_TEST;
}

bool UmsBlockDeviceFlushTest() {
    BEGIN_TEST;
    Binder ddk;
    Context context;
    ums::UmsBlockDevice dev(reinterpret_cast<zx_device_t*>(&context), 5,
                            [&](ums::Transaction* txn) { context.txn = txn; });
    context.dev = &dev;
    dev.Adopt();
    EXPECT_EQ(ZX_OK, dev.Add(), "Expected Add to succeed");
    EXPECT_TRUE(fbl::String("lun-005") == context.name);
    ums::Transaction txn;
    txn.op.command = BLOCK_OP_FLUSH;
    dev.BlockImplQueue(&txn.op, BlockCallback, &context);
    EXPECT_EQ(&txn, context.txn);
    EXPECT_EQ(ZX_OK, dev.DdkRemove(), "Expected DdkRemove to succeed");
    EXPECT_TRUE(dev.Release(), "Expected to free the device");
    END_TEST;
}

} // namespace

BEGIN_TEST_CASE(usb_mass_storage_tests)
RUN_NAMED_TEST("UMS block device constructor sets paramaters appropriately",
               UmsBlockDeviceConstructorTest)
RUN_NAMED_TEST("BlockImplQueue with invalid OPCODE type returns an error",
               UmsBlockDeviceNotSupportedTest)
RUN_NAMED_TEST("BlockImplQueue read OPCODE initiates a read transaction", UmsBlockDeviceReadTest)
RUN_NAMED_TEST("write OPCODE initiates a write transaction", UmsBlockDeviceWriteTest)
RUN_NAMED_TEST("flush OPCODE initiates a flush transaction", UmsBlockDeviceFlushTest)

END_TEST_CASE(usb_mass_storage_tests)
