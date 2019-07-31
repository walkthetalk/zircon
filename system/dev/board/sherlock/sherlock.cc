// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sherlock.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/gpio.h>
#include <ddk/protocol/platform/device.h>
#include <fbl/algorithm.h>
#include <fbl/unique_ptr.h>

namespace sherlock {

static pbus_dev_t rtc_dev = []() {
    pbus_dev_t dev = {};
    dev.name = "rtc";
    dev.vid = PDEV_VID_GENERIC;
    dev.pid = PDEV_PID_GENERIC;
    dev.did = PDEV_DID_RTC_FALLBACK;
    return dev;
}();

zx_status_t Sherlock::Create(void* ctx, zx_device_t* parent) {
    pbus_protocol_t pbus;
    iommu_protocol_t iommu;

    auto status = device_get_protocol(parent, ZX_PROTOCOL_PBUS, &pbus);
    if (status != ZX_OK) {
        return status;
    }

    status = device_get_protocol(parent, ZX_PROTOCOL_IOMMU, &iommu);
    if (status != ZX_OK) {
        return status;
    }

    fbl::AllocChecker ac;
    auto board = fbl::make_unique_checked<Sherlock>(&ac, parent, &pbus, &iommu);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    status = board->DdkAdd("sherlock", DEVICE_ADD_NON_BINDABLE);
    if (status != ZX_OK) {
        return status;
    }

    // Start up our protocol helpers and platform devices.
    status = board->Start();
    if (status == ZX_OK) {
        // devmgr is now in charge of the device.
        __UNUSED auto* dummy = board.release();
    }
    return status;
}

int Sherlock::Thread() {
    // Load protocol implementation drivers first.
    if (SysmemInit() != ZX_OK) {
        zxlogf(ERROR, "SysmemInit() failed\n");
        return -1;
    }

    if (GpioInit() != ZX_OK) {
        zxlogf(ERROR, "GpioInit() failed\n");
        return -1;
    }

    if (BoardInit() != ZX_OK) {
        zxlogf(ERROR, "BoardInit() failed\n");
        return -1;
    }

    if (ClkInit() != ZX_OK) {
        zxlogf(ERROR, "ClkInit() failed\n");
        return -1;
    }

    if (I2cInit() != ZX_OK) {
        zxlogf(ERROR, "I2cInit() failed\n");
    }

    if (SpiInit() != ZX_OK) {
        zxlogf(ERROR, "SpiInit() failed\n");
    }

    if (CanvasInit() != ZX_OK) {
        zxlogf(ERROR, "CanvasInit() failed\n");
    }

    if (ThermalInit() != ZX_OK) {
        zxlogf(ERROR, "ThermalInit() failed\n");
    }

    if (DisplayInit() != ZX_OK) {
        zxlogf(ERROR, "DisplayInit()failed\n");
    }

    // Then the platform device drivers.
    if (UsbInit() != ZX_OK) {
        zxlogf(ERROR, "UsbInit() failed\n");
    }

    if (EmmcInit() != ZX_OK) {
        zxlogf(ERROR, "EmmcInit() failed\n");
    }

    // The BMC43458 chip requires this hardware clock for bluetooth and wifi.
    // Called here to avoid a dep. between sdio and bluetooth init order.
    if (BCM43458LpoClockInit() != ZX_OK) {
        zxlogf(ERROR, "Bcm43458LpoClockInit() failed\n");
    }

    if (SdioInit() != ZX_OK) {
        zxlogf(ERROR, "SdioInit() failed\n");
    }

    if (BluetoothInit() != ZX_OK) {
        zxlogf(ERROR, "BluetoothInit() failed\n");
    }

    if (CameraInit() != ZX_OK) {
        zxlogf(ERROR, "CameraInit() failed\n");
    }

    if (TeeInit() != ZX_OK) {
        zxlogf(ERROR, "TeeInit() failed\n");
    }

    if (VideoInit() != ZX_OK) {
        zxlogf(ERROR, "VideoInit() failed\n");
    }

    if (MaliInit() != ZX_OK) {
        zxlogf(ERROR, "MaliInit() failed\n");
    }

    if (ButtonsInit() != ZX_OK) {
        zxlogf(ERROR, "ButtonsInit() failed\n");
    }

    if (AudioInit() != ZX_OK) {
        zxlogf(ERROR, "AudioInit() failed\n");
    }

    if (TouchInit() != ZX_OK) {
        zxlogf(ERROR, "TouchInit() failed\n");
        return -1;
    }

    if (LightInit() != ZX_OK) {
        zxlogf(ERROR, "LightInit() failed\n");
        return -1;
    }

    zx_status_t status = pbus_.DeviceAdd(&rtc_dev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DeviceAdd failed for RTC - error %d\n", __func__, status);
        return -1;
    }

    return 0;
}

zx_status_t Sherlock::Start() {
    int rc = thrd_create_with_name(&thread_,
                                   [](void* arg) -> int {
                                       return reinterpret_cast<Sherlock*>(arg)->Thread();
                                   },
                                   this,
                                   "sherlock-start-thread");
    if (rc != thrd_success) {
        return ZX_ERR_INTERNAL;
    }
    return ZX_OK;
}

void Sherlock::DdkRelease() {
    delete this;
}

static constexpr zx_driver_ops_t driver_ops = [](){
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = Sherlock::Create;
    return ops;
}();

} // namespace sherlock

ZIRCON_DRIVER_BEGIN(sherlock, sherlock::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PBUS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GOOGLE),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_SHERLOCK),
ZIRCON_DRIVER_END(sherlock)
