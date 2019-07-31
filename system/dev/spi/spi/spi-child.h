// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/metadata/spi.h>
#include <ddktl/device.h>
#include <ddktl/protocol/spiimpl.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <fuchsia/hardware/spi/llcpp/fidl.h>
#include <lib/fidl-utils/bind.h>

namespace spi {

class SpiChild;
using SpiChildType = ddk::Device<SpiChild, ddk::Messageable>;

class SpiChild : public SpiChildType,
                 public fbl::RefCounted<SpiChild>,
                 public llcpp::fuchsia::hardware::spi::Device::Interface {
public:
    SpiChild(zx_device_t* parent, ddk::SpiImplProtocolClient spi,
             const spi_channel_t* channel)
        : SpiChildType(parent), spi_(spi), cs_(channel->cs) {}

    zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn);
    void DdkUnbind();
    void DdkRelease();

    void Transmit(fidl::VectorView<uint8_t> data, TransmitCompleter::Sync completer) override;
    void Receive(uint32_t size, ReceiveCompleter::Sync completer) override;
    void Exchange(fidl::VectorView<uint8_t> txdata, ExchangeCompleter::Sync completer) override;

private:
    const ddk::SpiImplProtocolClient spi_;
    const uint32_t cs_;
};

} // namespace spi
