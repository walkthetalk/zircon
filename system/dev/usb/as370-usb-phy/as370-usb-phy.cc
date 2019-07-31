// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "as370-usb-phy.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <soc/as370/as370-reset.h>
#include <soc/as370/as370-usb.h>
#include <fbl/algorithm.h>
#include <fbl/unique_ptr.h>
#include <hw/reg.h>

namespace as370_usb_phy {

void UsbPhy::ResetPhy() {
    auto* mmio = &*reset_mmio_;

    auto reset = as370::GblPerifStickyResetN::Get().ReadFrom(mmio);
    reset.set_usbOtgPhyreset(0).WriteTo(mmio);
    reset.set_usbOtgPrstn(1).WriteTo(mmio);
    usleep(10);
    reset.set_usbOtgHresetn(1).WriteTo(mmio);
    usleep(100);
}

zx_status_t UsbPhy::InitPhy() {
    auto* mmio = &*usbphy_mmio_;

    as370::USB_PHY_CTRL0::Get()
        .FromValue(0)
        .set_value(0x0EB35E84).
        WriteTo(mmio);
    as370::USB_PHY_CTRL1::Get()
        .FromValue(0)
        .set_value(0x80E9F004)
        .WriteTo(mmio);

    ResetPhy();

    uint32_t count = 10000;
    while (count) {
        if (as370::USB_PHY_RB::Get().ReadFrom(mmio).clk_rdy()) {
            break;
        }
        usleep(1);
        count--;
    }

    return count ? 0 : ZX_ERR_TIMED_OUT;

    return ZX_OK;
}

zx_status_t UsbPhy::Create(void* ctx, zx_device_t* parent) {
    auto dev = std::make_unique<UsbPhy>(parent);
    auto status = dev->Init();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the device.
    __UNUSED auto* dummy = dev.release();
    return ZX_OK;
}

zx_status_t UsbPhy::AddDwc2Device() {
    if (dwc2_device_) {
        zxlogf(ERROR, "UsbPhy::AddDwc2Device: device already exists!\n");
        return ZX_ERR_BAD_STATE;
    }

    fbl::AllocChecker ac;
    dwc2_device_ = fbl::make_unique_checked<Dwc2Device>(&ac, zxdev());
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    zx_device_prop_t props[] = {
        {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_GENERIC},
        {BIND_PLATFORM_DEV_PID, 0, PDEV_PID_GENERIC},
        {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_USB_DWC2},
    };

    return dwc2_device_->DdkAdd("dwc2", 0, props, countof(props), ZX_PROTOCOL_USB_PHY);
}

zx_status_t UsbPhy::RemoveDwc2Device() {
    if (dwc2_device_ == nullptr) {
        zxlogf(ERROR, "UsbPhy::RemoveDwc2Device: device does not exist!\n");
        return ZX_ERR_BAD_STATE;
    }

    // devmgr will own the device until it is destroyed.
    __UNUSED auto* dev = dwc2_device_.release();
    dev->DdkRemove();

    return ZX_OK;
}

zx_status_t UsbPhy::Init() {
    if (!pdev_.is_valid()) {
        zxlogf(ERROR, "UsbPhy::Init: could not get platform device protocol\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    auto status = pdev_.MapMmio(0, &usbphy_mmio_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "UsbPhy::Init: MapMmio failed for usbphy_mmio_\n");
        return status;
    }
    status = pdev_.MapMmio(1, &reset_mmio_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "UsbPhy::Init: MapMmio failed for reset_mmio_\n");
        return status;
    }

    status = InitPhy();
    if (status != ZX_OK) {
        zxlogf(ERROR, "UsbPhy::Init: InitPhy() failed\n");
        return status;
    }

    status = DdkAdd("as370-usb-phy", DEVICE_ADD_NON_BINDABLE);
    if (status != ZX_OK) {
        zxlogf(ERROR, "UsbPhy::Init: DdkAdd() failed\n");
        return status;
    }

    AddDwc2Device();

    return ZX_OK;
}

void UsbPhy::DdkUnbind() {
    RemoveDwc2Device();
    DdkRemove();
}

void UsbPhy::DdkRelease() {
    delete this;
}

static constexpr zx_driver_ops_t driver_ops = [](){
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = UsbPhy::Create;
    return ops;
}();

} // namespace as370_usb_phy

ZIRCON_DRIVER_BEGIN(as370_usb_phy, as370_usb_phy::driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_SYNAPTICS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_SYNAPTICS_AS370),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_AS370_USB_PHY),
ZIRCON_DRIVER_END(as370_usb_phy)
