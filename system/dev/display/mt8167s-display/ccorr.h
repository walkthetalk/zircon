// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_DISPLAY_MT8167S_DISPLAY_CCORR_H_
#define ZIRCON_SYSTEM_DEV_DISPLAY_MT8167S_DISPLAY_CCORR_H_
#include <ddk/protocol/platform/device.h>
#include <ddktl/device.h>
#include <fbl/unique_ptr.h>
#include <hwreg/mmio.h>
#include <lib/device-protocol/platform-device.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/bti.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>

#include <optional>

#include "common.h"
#include "registers-ccorr.h"

namespace mt8167s_display {

// [Ovl] --> [Color] --> [CCorr] --> [AAL] --> [Gamma] --> [Dither] --> [RDMA] --> [DSI]
//
// The Color Correction engine is responsible for overall RGB color mixture that
// would fit the characteristics of the target panel.

class Ccorr {
 public:
  Ccorr(uint32_t height, uint32_t width) : height_(height), width_(width) {
    ZX_ASSERT(height_ < kMaxHeight);
    ZX_ASSERT(width_ < kMaxWidth);
  }
  zx_status_t Init(zx_device_t* parent);
  zx_status_t Config();
  void PrintRegisters();

 private:
  fbl::unique_ptr<ddk::MmioBuffer> ccorr_mmio_;
  pdev_protocol_t pdev_ = {nullptr, nullptr};
  const uint32_t height_;  // Display height
  const uint32_t width_;   // Display width
  bool initialized_ = false;
};

}  // namespace mt8167s_display

#endif  // ZIRCON_SYSTEM_DEV_DISPLAY_MT8167S_DISPLAY_CCORR_H_
