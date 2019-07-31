// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <atomic>
#include <lib/device-protocol/platform-device.h>
#include <ddk/protocol/platform/device.h>
#include <ddktl/device.h>
#include <fuchsia/hardware/thermal/c/fidl.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/interrupt.h>
#include <optional>
#include <threads.h>

namespace thermal {

// This class represents a temperature sensor
// which is on the S905D2 core.
class AmlTSensor {

public:
    DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AmlTSensor);
    AmlTSensor(){}
    uint32_t ReadTemperature();
    zx_status_t InitSensor(zx_device_t* parent,
                           fuchsia_hardware_thermal_ThermalDeviceInfo thermal_config);
    zx_status_t GetStateChangePort(zx_handle_t* port);
    ~AmlTSensor();

private:
    int TripPointIrqHandler();
    zx_status_t InitPdev(zx_device_t* parent);
    uint32_t TempToCode(uint32_t temp, bool trend);
    uint32_t CodeToTemp(uint32_t temp_code);
    void SetRebootTemperature(uint32_t temp);
    zx_status_t InitTripPoints();
    zx_status_t NotifyThermalDaemon();
    void UpdateFallThresholdIrq(uint32_t irq);
    void UpdateRiseThresholdIrq(uint32_t irq);
    uint32_t trim_info_;
    pdev_protocol_t pdev_;
    std::optional<ddk::MmioBuffer> pll_mmio_;
    std::optional<ddk::MmioBuffer> ao_mmio_;
    std::optional<ddk::MmioBuffer> hiu_mmio_;
    zx::interrupt tsensor_irq_;
    thrd_t irq_thread_;
    std::atomic<bool> running_;
    zx_handle_t port_;
    fuchsia_hardware_thermal_ThermalDeviceInfo thermal_config_;
    uint32_t current_trip_idx_ = 0;
};
} // namespace thermal
