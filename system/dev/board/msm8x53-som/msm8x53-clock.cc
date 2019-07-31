// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform/bus.h>
#include <soc/msm8x53/msm8x53-clock.h>
#include <soc/msm8x53/msm8x53-gpio.h>
#include <soc/msm8x53/msm8x53-hw.h>

#include "msm8x53.h"

namespace board_msm8x53 {

namespace {

constexpr pbus_mmio_t clock_mmios[] = {
    {
        .base = msm8x53::kCcBase,
        .length = msm8x53::kCcSize,
    },
};

constexpr pbus_dev_t clock_dev = []() {
    pbus_dev_t result{};

    result.name = "gcc-clock";
    result.vid = PDEV_VID_QUALCOMM;
    result.pid = PDEV_PID_QUALCOMM_MSM8X53;
    result.did = PDEV_DID_QUALCOMM_CLOCK;
    result.mmio_list = clock_mmios;
    result.mmio_count = countof(clock_mmios);

    return result;
}();


}  // namespace

zx_status_t Msm8x53::ClockInit() {

    zx_status_t status = pbus_.ProtocolDeviceAdd(ZX_PROTOCOL_CLOCK_IMPL, &clock_dev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: ProtocolDeviceAdd failed %d\n", __func__, status);
        return status;
    }

    return ZX_OK;
}

} // namespace board_msm8x53
