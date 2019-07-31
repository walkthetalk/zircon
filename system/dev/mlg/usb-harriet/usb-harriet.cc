// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "usb-harriet.h"

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/protocol/usb.h>
#include <fbl/alloc_checker.h>
#include <fbl/unique_ptr.h>

namespace {

static constexpr uint16_t GOOGLE_USB_VID = 0x18D1;
static constexpr uint16_t HARRIET_USB_PID = 0x9302;

}  // namespace

namespace usb_harriet {

zx_status_t Harriet::Bind() {
    zx_status_t status = DdkAdd("usb-harriet");
    if (status != ZX_OK) {
       return status;
    }
    return ZX_OK;
}

// static
zx_status_t Harriet::Create(zx_device_t* parent) {
    usb::UsbDevice usb(parent);
    if (!usb.is_valid()) {
        return ZX_ERR_PROTOCOL_NOT_SUPPORTED;
    }

    std::optional<usb::InterfaceList> intfs;
    zx_status_t status = usb::InterfaceList::Create(usb, true, &intfs);
    if (status != ZX_OK) {
        return status;
    }
    auto intf = intfs->begin();
    const usb_interface_descriptor_t* intf_desc = intf->descriptor();
    if (intf == intfs->end()) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    uint8_t intf_num = intf_desc->bInterfaceNumber;
    zxlogf(TRACE, "found intf %u\n", intf_num);

    for (auto ep_desc : *intf) {
        uint8_t ep_type = usb_ep_type(&ep_desc.descriptor);
        switch(ep_type) {
        case USB_ENDPOINT_BULK:
        case USB_ENDPOINT_INTERRUPT:
            zxlogf(TRACE, "%s %s EP 0x%x\n",
                   ep_type == USB_ENDPOINT_BULK ? "BULK" : "INTERRUPT",
                   usb_ep_direction(&ep_desc.descriptor) == USB_ENDPOINT_OUT ? "OUT" : "IN",
                   ep_desc.descriptor.bEndpointAddress);
            break;
        default:
            zxlogf(TRACE, "found additional unexpected EP, type: %u addr 0x%x\n",
                   ep_type, ep_desc.descriptor.bEndpointAddress);
        }
    }

    fbl::AllocChecker ac;
    auto dev = fbl::make_unique_checked<Harriet>(&ac, parent, usb);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    status = dev->Bind();
    if (status == ZX_OK) {
        // Intentionally leak as it is now held by DevMgr.
        __UNUSED auto ptr = dev.release();
    }
    return status;
}

zx_status_t harriet_bind(void* ctx, zx_device_t* parent) {
    zxlogf(TRACE, "harriet_bind\n");
    return usb_harriet::Harriet::Create(parent);
}

static constexpr zx_driver_ops_t harriet_driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = harriet_bind;
    return ops;
}();

}  // namespace usb

// clang-format off
ZIRCON_DRIVER_BEGIN(usb_harriet, usb_harriet::harriet_driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_USB),
    BI_ABORT_IF(NE, BIND_USB_VID, GOOGLE_USB_VID),
    BI_MATCH_IF(EQ, BIND_USB_PID, HARRIET_USB_PID),
ZIRCON_DRIVER_END(usb_harriet)
// clang-format on
