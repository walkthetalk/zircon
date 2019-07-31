// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <ddk/driver.h>
#include <ddk/binding.h>
#include <ddk/debug.h>
#include <fbl/alloc_checker.h>

#include "block_device.h"

namespace {

zx_status_t FtlDriverBind(void* ctx, zx_device_t* parent) {
    zxlogf(INFO, "FTL: Binding. Version 1.0.11\n");
    fbl::AllocChecker checker;
    std::unique_ptr<ftl::BlockDevice> device(new (&checker) ftl::BlockDevice(parent));
    if (!checker.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    zx_status_t status = device->Bind();
    if (status == ZX_OK) {
        // devmgr is now in charge of the device.
        __UNUSED ftl::BlockDevice* dummy = device.release();
    }
    return status;
}

}  // namespace

static constexpr zx_driver_ops_t ftl_driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = FtlDriverBind;
    return ops;
}();

ZIRCON_DRIVER_BEGIN(ftl, ftl_driver_ops, "zircon", "0.1", 2)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_NAND),
    BI_MATCH_IF(EQ, BIND_NAND_CLASS, fuchsia_hardware_nand_Class_FTL)
ZIRCON_DRIVER_END(ftl)
