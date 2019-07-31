// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <threads.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/metadata.h>
#include <ddk/metadata/i2c.h>
#include <fbl/alloc_checker.h>
#include <fbl/mutex.h>
#include <lib/sync/completion.h>
#include <zircon/types.h>

#include "i2c.h"
#include "i2c-child.h"

namespace i2c {

void I2cDevice::DdkUnbind() {
    DdkRemove();
}

void I2cDevice::DdkRelease() {
    delete this;
}

zx_status_t I2cDevice::Create(void* ctx, zx_device_t* parent) {
    i2c_impl_protocol_t i2c;
    auto status = device_get_protocol(parent, ZX_PROTOCOL_I2C_IMPL, &i2c);
    if (status != ZX_OK) {
        return status;
    }

    fbl::AllocChecker ac;
    std::unique_ptr<I2cDevice> device(new (&ac) I2cDevice(parent, &i2c));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    status = device->Init(&i2c);
    if (status != ZX_OK) {
        return status;
    }

    status = device->DdkAdd("i2c");
    if (status != ZX_OK) {
        return status;
    }

    device->AddChildren();

    __UNUSED auto* dummy = device.release();

    return ZX_OK;
}

zx_status_t I2cDevice::Init(ddk::I2cImplProtocolClient i2c) {
    uint32_t bus_count = i2c.GetBusCount();
    if (!bus_count) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    fbl::AllocChecker ac;
    i2c_buses_.reserve(bus_count, &ac);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    for (uint32_t i = 0; i < bus_count; i++) {
        auto i2c_bus = fbl::MakeRefCountedChecked<I2cBus>(&ac, i2c, i);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }

        auto status = i2c_bus->Start();
        if (status != ZX_OK) {
            return status;
        }

        i2c_buses_.push_back(std::move(i2c_bus));
    }

    return ZX_OK;
}

void I2cDevice::AddChildren() {
    size_t metadata_size;
    auto status = device_get_metadata_size(zxdev(), DEVICE_METADATA_I2C_CHANNELS, &metadata_size);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: device_get_metadata_size failed %d\n", __func__, status);
        return;
    }
    auto channel_count = metadata_size / sizeof(i2c_channel_t);

    fbl::AllocChecker ac;
    std::unique_ptr<i2c_channel_t[]> channels(new (&ac) i2c_channel_t[channel_count]);
    if (!ac.check()) {
        zxlogf(ERROR, "%s: out of memory\n", __func__);
        return;
    }

    size_t actual;
    status = device_get_metadata(zxdev(), DEVICE_METADATA_I2C_CHANNELS, channels.get(),
                                 metadata_size, &actual);
    if (status != ZX_OK || actual != metadata_size) {
        zxlogf(ERROR, "%s: device_get_metadata failed %d\n", __func__, status);
        return;
    }

    for (uint32_t i = 0; i < channel_count; i++) {
        const auto& channel = channels[i];
        const auto bus_id = channel.bus_id;
        const auto address = channel.address;
        const auto vid = channel.vid;
        const auto pid = channel.pid;
        const auto did = channel.did;

        if (bus_id >= i2c_buses_.size()) {
            zxlogf(ERROR, "%s: bus_id %u out of range\n", __func__, bus_id);
            return;
        }

        fbl::AllocChecker ac;
        std::unique_ptr<I2cChild> dev(new (&ac) I2cChild(zxdev(), i2c_, i2c_buses_[bus_id],
                                      &channel));
        if (!ac.check()) {
            zxlogf(ERROR, "%s: out of memory\n", __func__);
            return;
        }

        char name[20];
        snprintf(name, sizeof(name), "i2c-%u-%u", bus_id, address);

        if (vid || pid || did) {
            zx_device_prop_t props[] = {
                { BIND_I2C_BUS_ID, 0, bus_id },
                { BIND_I2C_ADDRESS, 0, address },
                { BIND_PLATFORM_DEV_VID, 0, vid },
                { BIND_PLATFORM_DEV_PID, 0, pid },
                { BIND_PLATFORM_DEV_DID, 0, did },
            };

            status = dev->DdkAdd(name, 0, props, countof(props));
        } else {
            zx_device_prop_t props[] = {
                { BIND_I2C_BUS_ID, 0, bus_id },
                { BIND_I2C_ADDRESS, 0, address },
            };

            status = dev->DdkAdd(name, 0, props, countof(props));
        }

        if (status != ZX_OK) {
            zxlogf(ERROR, "%s: DdkAdd failed %d\n", __func__, status);
            return;
        }

        // dev is now owned by devmgr.
        __UNUSED auto ptr = dev.release();
    }
}

static constexpr zx_driver_ops_t driver_ops = [](){
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = I2cDevice::Create;
    return ops;
}();

} // namespace i2c

ZIRCON_DRIVER_BEGIN(i2c, i2c::driver_ops, "zircon", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_I2C_IMPL),
ZIRCON_DRIVER_END(i2c)
