// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/binding.h>

zx_status_t not_supported_bind(void* ctx, zx_device_t* parent) {
    return ZX_ERR_NOT_SUPPORTED;
}

static zx_driver_ops_t bind_fail_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = not_supported_bind,
};

ZIRCON_DRIVER_BEGIN(bind_fail, bind_fail_driver_ops, "zircon", "0.1", 2)
    BI_ABORT_IF_AUTOBIND,
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_TEST),
ZIRCON_DRIVER_END(bind_fail)
