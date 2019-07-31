// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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

#include "astro.h"

namespace astro {

static const pbus_dev_t rtc_dev = []() {
    pbus_dev_t dev = {};
    dev.name = "rtc";
    dev.vid = PDEV_VID_GENERIC;
    dev.pid = PDEV_PID_GENERIC;
    dev.did = PDEV_DID_RTC_FALLBACK;
    return dev;
}();

uint32_t Astro::GetBoardRev() {
    uint32_t board_rev;
    uint8_t id0, id1, id2;

    gpio_impl_.ConfigIn(GPIO_HW_ID0, GPIO_NO_PULL);
    gpio_impl_.ConfigIn(GPIO_HW_ID1, GPIO_NO_PULL);
    gpio_impl_.ConfigIn(GPIO_HW_ID2, GPIO_NO_PULL);
    gpio_impl_.Read(GPIO_HW_ID0, &id0);
    gpio_impl_.Read(GPIO_HW_ID1, &id1);
    gpio_impl_.Read(GPIO_HW_ID2, &id2);
    board_rev = id0 + (id1 << 1) + (id2 << 2);

    if (board_rev >= MAX_SUPPORTED_REV) {
        // We have detected a new board rev. Print this warning just in case the
        // new board rev requires additional support that we were not aware of
        zxlogf(INFO, "Unsupported board revision detected (%d)\n", board_rev);
    }

    return board_rev;
}

int Astro::Thread() {
    zx_status_t status;

    // Sysmem is started early so zx_vmo_create_contiguous() works.
    if ((status = SysmemInit()) != ZX_OK) {
        zxlogf(ERROR, "%s: SysmemInit() failed: %d\n", __func__, status);
        return status;
    }

    if ((status = GpioInit()) != ZX_OK) {
        zxlogf(ERROR, "%s: GpioInit() failed: %d\n", __func__, status);
        return status;
    }

    // Once gpio is up and running, let's populate board revision
    pbus_board_info_t info;
    info.board_revision = GetBoardRev();
    status = pbus_.SetBoardInfo(&info);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: PBusSetBoardInfo failed: %d\n", __func__, status);
    }
    zxlogf(INFO, "Detected board rev 0x%x\n", info.board_revision);

    if ((status = ButtonsInit()) != ZX_OK) {
        zxlogf(ERROR, "ButtonsInit failed: %d\n", status);
    }

    if ((status = I2cInit()) != ZX_OK) {
        zxlogf(ERROR, "I2cInit failed: %d\n", status);
    }

    if ((status = MaliInit()) != ZX_OK) {
        zxlogf(ERROR, "MaliInit failed: %d\n", status);
    }

    if ((status = UsbInit()) != ZX_OK) {
        zxlogf(ERROR, "UsbInit failed: %d\n", status);
    }

    if ((status = TouchInit()) != ZX_OK) {
        zxlogf(ERROR, "TouchInit failed: %d\n", status);
    }

    if ((status = DisplayInit()) != ZX_OK) {
        zxlogf(ERROR, "DisplayInit failed: %d\n", status);
    }

    if ((status = CanvasInit()) != ZX_OK) {
        zxlogf(ERROR, "CanvasInit failed: %d\n", status);
    }

    if ((status = TeeInit()) != ZX_OK) {
        zxlogf(ERROR, "TeeInit failed: %d\n", status);
    }

    if ((status = VideoInit()) != ZX_OK) {
        zxlogf(ERROR, "VideoInit failed: %d\n", status);
    }

    if ((status = pbus_.DeviceAdd(&rtc_dev)) != ZX_OK) {
        zxlogf(ERROR, "%s: DeviceAdd failed - RTC: %d\n", __func__, status);
    }

    if ((status = RawNandInit()) != ZX_OK) {
        zxlogf(ERROR, "RawNandInit failed: %d\n", status);
    }

    if ((status = SdioInit()) != ZX_OK) {
        zxlogf(ERROR, "SdioInit failed: %d\n", status);
    }

    if ((status = LightInit()) != ZX_OK) {
        zxlogf(ERROR, "LightInit failed: %d\n", status);
    }

    if ((status = ClkInit()) != ZX_OK) {
        zxlogf(ERROR, "ClkInit failed: %d\n", status);
    }

    if ((status = ThermalInit()) != ZX_OK) {
        zxlogf(ERROR, "ThermalInit failed: %d\n", status);
    }

    if ((status = AudioInit()) != ZX_OK) {
        zxlogf(ERROR, "AudioInit failed: %d\n", status);
    }

    // This function includes some non-trivial delays, so lets run this last
    // to avoid slowing down the rest of the boot.
    if ((status = BluetoothInit()) != ZX_OK) {
        zxlogf(ERROR, "BluetoothInit failed: %d\n", status);
    }

    return ZX_OK;
}

zx_status_t Astro::Start() {
    int rc = thrd_create_with_name(&thread_, [](void* arg) -> int {
                              return reinterpret_cast<Astro*>(arg)->Thread();
                          }, this, "astro-start-thread");
    if (rc != thrd_success) {
        return ZX_ERR_INTERNAL;
    }
    return ZX_OK;
}

void Astro::DdkRelease() {
    delete this;
}

zx_status_t Astro::Create(void* ctx, zx_device_t* parent) {
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
    auto board = fbl::make_unique_checked<Astro>(&ac, parent, &pbus, &iommu);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    status = board->DdkAdd("astro", DEVICE_ADD_NON_BINDABLE);
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

static zx_driver_ops_t astro_driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = Astro::Create;
    return ops;
}();

} // namespace astro

ZIRCON_DRIVER_BEGIN(aml_bus, astro::astro_driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PBUS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GOOGLE),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_ASTRO),
ZIRCON_DRIVER_END(astro)
