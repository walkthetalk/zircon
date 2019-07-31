// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_DISPLAY_ASTRO_DISPLAY_VPU_H_
#define ZIRCON_SYSTEM_DEV_DISPLAY_ASTRO_DISPLAY_VPU_H_

#include <ddk/protocol/platform/device.h>
#include <lib/device-protocol/platform-device.h>
#include <lib/mmio/mmio.h>
#include <zircon/compiler.h>

#include <optional>

#include "common.h"
#include "vpu-regs.h"

namespace astro_display {

class Vpu {
 public:
  Vpu() {}
  zx_status_t Init(zx_device_t* parent);
  // This function powers on VPU related blocks. The function contains undocumented
  // register and/or power-on sequences.
  void PowerOn();
  // This function powers off VPU related blocks. The function contains undocumented
  // register and/or power-off sequences.
  void PowerOff();
  // This function sets up default video post processing unit. It contains undocumented
  // registers and/or initialization sequences
  void VppInit();

 private:
  // This function configures the VPU-related clocks. It contains undocumented registers
  // and/or clock initialization sequences
  void ConfigureClock();

  std::optional<ddk::MmioBuffer> vpu_mmio_;
  std::optional<ddk::MmioBuffer> hhi_mmio_;
  std::optional<ddk::MmioBuffer> aobus_mmio_;
  std::optional<ddk::MmioBuffer> cbus_mmio_;
  pdev_protocol_t pdev_ = {};

  bool initialized_ = false;
};
}  // namespace astro_display

#endif  // ZIRCON_SYSTEM_DEV_DISPLAY_ASTRO_DISPLAY_VPU_H_
