// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <zircon/types.h>

#include <ddk/device.h>
#include <ddk/protocol/clock.h>
#include <ddk/protocol/gpio.h>
#include <lib/device-protocol/platform-device.h>
#include <ddk/protocol/platform/device.h>
#include <lib/mmio/mmio.h>
#include <dev/pci/designware/atu-cfg.h>
#include <fbl/unique_ptr.h>

#include <optional>

#include "aml-pcie.h"

namespace pcie {
namespace aml {

class AmlPcieDevice {
  public:
    AmlPcieDevice(zx_device_t* parent) : parent_(parent) {}
    ~AmlPcieDevice() = default;

    zx_status_t Init();

  private:
    enum {
        kClk81,
        kClkPcieA,
        kClkPort,
        kClockCount,
    };

    zx_status_t InitProtocols();
    zx_status_t InitMmios();
    zx_status_t InitMetadata();

    zx_device_t* parent_;
    zx_device_t* dev_;

    // Protocols
    pdev_protocol_t pdev_;
    clock_protocol_t clks_[kClockCount];
    gpio_protocol_t gpio_;

    // MMIO Buffers
    std::optional<ddk::MmioBuffer> dbi_;
    std::optional<ddk::MmioBuffer> cfg_;
    std::optional<ddk::MmioBuffer> rst_;
    std::optional<ddk::MmioBuffer> pll_;

    // Pinned MMIO Buffers
    std::optional<ddk::MmioPinnedBuffer> dbi_pinned_;

    // Device Metadata
    iatu_translation_entry_t atu_cfg_;
    iatu_translation_entry_t atu_io_;
    iatu_translation_entry_t atu_mem_;

    fbl::unique_ptr<AmlPcie> pcie_;
};

} // namespace aml
} // namespace pcie
