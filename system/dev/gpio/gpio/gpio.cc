// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/metadata.h>
#include <ddk/metadata/gpio.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_call.h>
#include <fbl/unique_ptr.h>
#include <zircon/types.h>

#include "gpio.h"

namespace gpio {

zx_status_t GpioDevice::GpioConfigIn(uint32_t flags) {
    return gpio_.ConfigIn(pin_, flags);
}

zx_status_t GpioDevice::GpioConfigOut(uint8_t initial_value) {
    return gpio_.ConfigOut(pin_, initial_value);
}

zx_status_t GpioDevice::GpioSetAltFunction(uint64_t function) {
    return gpio_.SetAltFunction(pin_, function);
}

zx_status_t GpioDevice::GpioRead(uint8_t* out_value) {
    return gpio_.Read(pin_, out_value);
}

zx_status_t GpioDevice::GpioWrite(uint8_t value) {
    return gpio_.Write(pin_, value);
}

zx_status_t GpioDevice::GpioGetInterrupt(uint32_t flags, zx::interrupt* out_irq) {
    return gpio_.GetInterrupt(pin_, flags, out_irq);
}

zx_status_t GpioDevice::GpioReleaseInterrupt() {
    return gpio_.ReleaseInterrupt(pin_);
}

zx_status_t GpioDevice::GpioSetPolarity(gpio_polarity_t polarity) {
    return gpio_.SetPolarity(pin_, polarity);
}

void GpioDevice::DdkUnbind() {
    DdkRemove();
}

void GpioDevice::DdkRelease() {
    delete this;
}

zx_status_t GpioDevice::Create(void* ctx, zx_device_t* parent) {
    gpio_impl_protocol_t gpio;
    auto status = device_get_protocol(parent, ZX_PROTOCOL_GPIO_IMPL, &gpio);
    if (status != ZX_OK) {
        return status;
    }

    size_t metadata_size;
    status = device_get_metadata_size(parent, DEVICE_METADATA_GPIO_PINS, &metadata_size);
    if (status != ZX_OK) {
        return status;
    }
    auto pin_count = metadata_size / sizeof(gpio_pin_t);

    fbl::AllocChecker ac;
    std::unique_ptr<gpio_pin_t[]> pins(new (&ac) gpio_pin_t[pin_count]);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    size_t actual;
    status = device_get_metadata(parent, DEVICE_METADATA_GPIO_PINS, pins.get(), metadata_size,
                                 &actual);
    if (status != ZX_OK) {
        return status;
    }
    if (actual != metadata_size) {
        return ZX_ERR_INTERNAL;
    }

    for (uint32_t i = 0; i < pin_count; i++) {
        auto pin = pins[i].pin;
        fbl::AllocChecker ac;
        std::unique_ptr<GpioDevice> dev(new (&ac) GpioDevice(parent, &gpio, pin));
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }

        char name[20];
        snprintf(name, sizeof(name), "gpio-%u", pin);
        zx_device_prop_t props[] = {
            { BIND_GPIO_PIN, 0, pin },
        };

        status = dev->DdkAdd(name, 0, props, countof(props));
        if (status != ZX_OK) {
            return status;
        }

        // dev is now owned by devmgr.
        __UNUSED auto ptr = dev.release();
    }

    return ZX_OK;
}

static constexpr zx_driver_ops_t driver_ops = [](){
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = GpioDevice::Create;
    return ops;
}();


} // namespace gpio

ZIRCON_DRIVER_BEGIN(gpio, gpio::driver_ops, "zircon", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_GPIO_IMPL),
ZIRCON_DRIVER_END(gpio)
