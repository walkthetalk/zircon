// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sherlock.h"

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform/bus.h>

#include <soc/aml-t931/t931-hw.h>

#include <limits.h>

namespace sherlock {

static const pbus_mmio_t sherlock_canvas_mmios[] = {
    {
        .base = T931_DMC_BASE,
        .length = T931_DMC_LENGTH,
    },
};

static const pbus_bti_t sherlock_canvas_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = BTI_CANVAS,
    },
};

static pbus_dev_t canvas_dev = []() {
    pbus_dev_t dev = {};
    dev.name = "canvas";
    dev.vid = PDEV_VID_AMLOGIC;
    dev.pid = PDEV_PID_GENERIC;
    dev.did = PDEV_DID_AMLOGIC_CANVAS;
    dev.mmio_list = sherlock_canvas_mmios;
    dev.mmio_count = countof(sherlock_canvas_mmios);
    dev.bti_list = sherlock_canvas_btis;
    dev.bti_count = countof(sherlock_canvas_btis);
    return dev;
}();

zx_status_t Sherlock::CanvasInit() {
    zx_status_t status = pbus_.ProtocolDeviceAdd(ZX_PROTOCOL_AMLOGIC_CANVAS, &canvas_dev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "Sherlock::CanvasInit: pbus_device_add failed: %d\n", status);
        return status;
    }
    return status;
}

}  // namespace sherlock
