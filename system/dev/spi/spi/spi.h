// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddktl/device.h>
#include <ddktl/protocol/spiimpl.h>
#include <fbl/ref_ptr.h>
#include <fbl/vector.h>
#include "spi-child.h"

namespace spi {

class SpiDevice;
using SpiDeviceType = ddk::Device<SpiDevice, ddk::Unbindable>;

class SpiDevice : public SpiDeviceType {
public:
    SpiDevice(zx_device_t* parent, const spi_impl_protocol_t* spi, uint32_t bus_id)
        : SpiDeviceType(parent), spi_(spi), bus_id_(bus_id) {}

    static zx_status_t Create(void* ctx, zx_device_t* parent);

    void DdkUnbind();
    void DdkRelease();

private:
    void AddChildren();

    fbl::Vector<fbl::RefPtr<SpiChild>> children_;
    const ddk::SpiImplProtocolClient spi_;
    uint32_t bus_id_;
};

} // namespace spi
