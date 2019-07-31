// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <threads.h>

#include <ddktl/device.h>
#include <lib/device-protocol/i2c-channel.h>
#include <fbl/mutex.h>
#include <hid/bma253.h>
#include <lib/simplehid/simplehid.h>
#include <zircon/thread_annotations.h>

namespace accel {

class Bma253;
using DeviceType = ddk::Device<Bma253>;

class Bma253 : public DeviceType, public ddk::HidbusProtocol<Bma253, ddk::base_protocol> {
public:
    virtual ~Bma253() {}

    static zx_status_t Create(void* ctx, zx_device_t* parent);

    void DdkRelease() { delete this; }

    zx_status_t HidbusQuery(uint32_t options, hid_info_t* out_info);
    zx_status_t HidbusStart(const hidbus_ifc_protocol_t* ifc) { return simple_hid_.HidbusStart(ifc); }
    void HidbusStop() { simple_hid_.HidbusStop(); }
    zx_status_t HidbusGetDescriptor(hid_description_type_t desc_type, void** out_data_buffer,
                                    size_t* data_size);
    zx_status_t HidbusGetReport(hid_report_type_t rpt_type, uint8_t rpt_id, void* out_data_buffer,
                                size_t data_size, size_t* out_data_actual);
    zx_status_t HidbusSetReport(hid_report_type_t rpt_type, uint8_t rpt_id, const void* data_buffer,
                                size_t data_size);
    zx_status_t HidbusGetIdle(uint8_t rpt_id, uint8_t* out_duration);
    zx_status_t HidbusSetIdle(uint8_t rpt_id, uint8_t duration);
    zx_status_t HidbusGetProtocol(hid_protocol_t* out_protocol);
    zx_status_t HidbusSetProtocol(hid_protocol_t protocol);

    // Visible for testing.
    Bma253(zx_device_t* parent, ddk::I2cChannel i2c, zx::port port)
        : DeviceType(parent), i2c_(i2c) {
        simple_hid_ = simplehid::SimpleHid<bma253_input_rpt_t>(
            std::move(port),
            [this](bma253_input_rpt_t* report) {
                return GetInputReport(report);
            }
        );
    }

    // Visible for testing.
    zx_status_t Init();

private:
    zx_status_t GetInputReport(bma253_input_rpt_t* report);

    fbl::Mutex i2c_lock_;
    ddk::I2cChannel i2c_ TA_GUARDED(i2c_lock_);

    simplehid::SimpleHid<bma253_input_rpt_t> simple_hid_;
};

}  // namespace accel
