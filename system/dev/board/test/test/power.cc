// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform/bus.h>
#include <ddk/protocol/platform/device.h>
#include <ddktl/device.h>
#include <ddktl/protocol/powerimpl.h>

#define DRIVER_NAME "test-power"

namespace power {

class TestPowerDevice;
using DeviceType = ddk::Device<TestPowerDevice, ddk::Unbindable>;

class TestPowerDevice : public DeviceType,
                       public ddk::PowerImplProtocol<TestPowerDevice, ddk::base_protocol> {
public:
    static zx_status_t Create(zx_device_t* parent);

    explicit TestPowerDevice(zx_device_t* parent)
        : DeviceType(parent) {}

    zx_status_t Create(std::unique_ptr<TestPowerDevice>* out);
    zx_status_t Init();

    // Methods required by the ddk mixins
    void DdkUnbind();
    void DdkRelease();

    zx_status_t PowerImplEnablePowerDomain(uint32_t index);
    zx_status_t PowerImplDisablePowerDomain(uint32_t index);
    zx_status_t PowerImplGetPowerDomainStatus(uint32_t index,
                                              power_domain_status_t* out_status);
    zx_status_t PowerImplGetSupportedVoltageRange(uint32_t index, uint32_t* min_voltage,
                                                  uint32_t* max_voltage);

    zx_status_t PowerImplRequestVoltage(uint32_t index, uint32_t voltage, uint32_t* actual_voltage);
    zx_status_t PowerImplReadPmicCtrlReg(uint32_t index, uint32_t addr, uint32_t* value);
    zx_status_t PowerImplWritePmicCtrlReg(uint32_t index, uint32_t addr, uint32_t value);

private:
    // For testing PMIC register read/write
    uint32_t last_index_ = 0;
    uint32_t last_addr_ = 0;
    uint32_t last_value_ = 0;
};

zx_status_t TestPowerDevice::Init() {
    pbus_protocol_t pbus;
    auto status = device_get_protocol(parent(), ZX_PROTOCOL_PBUS, &pbus);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: ZX_PROTOCOL_PBUS not available %d\n", __func__, status);
        return status;
    }
    power_impl_protocol_t power_proto = {
        .ops = &power_impl_protocol_ops_,
        .ctx = this,
    };
    status = pbus_register_protocol(&pbus, ZX_PROTOCOL_POWER_IMPL, &power_proto,
                                    sizeof(power_proto));
     if (status != ZX_OK) {
        zxlogf(ERROR, "%s pbus_register_protocol failed %d\n", __func__, status);
         return status;
     }
     return ZX_OK;
}

zx_status_t TestPowerDevice::Create(zx_device_t* parent) {
    auto dev = std::make_unique<TestPowerDevice>(parent);
    pdev_protocol_t pdev;
    zx_status_t status;

    zxlogf(INFO, "TestPowerDevice::Create: %s \n", DRIVER_NAME);

    status = device_get_protocol(parent, ZX_PROTOCOL_PDEV, &pdev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: could not get ZX_PROTOCOL_PDEV\n", __func__);
        return status;
    }

    status = dev->DdkAdd("test-power");
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DdkAdd failed: %d\n", __func__, status);
        return status;
    }
    // devmgr is now in charge of dev.
    auto ptr = dev.release();

    return ptr->Init();
}

void TestPowerDevice::DdkUnbind() {}

void TestPowerDevice::DdkRelease() {
    delete this;
}

zx_status_t TestPowerDevice::PowerImplEnablePowerDomain(uint32_t index) {
    //TODO(ravoorir): to be implemented
    return ZX_OK;
}

zx_status_t TestPowerDevice::PowerImplDisablePowerDomain(uint32_t index) {
    //TODO(ravoorir): to be implemented
    return ZX_OK;
}

zx_status_t TestPowerDevice::PowerImplGetPowerDomainStatus(uint32_t index,
                                                           power_domain_status_t* out_status) {
    //TODO(ravoorir): to be implemented
    return ZX_OK;
}

zx_status_t TestPowerDevice::PowerImplGetSupportedVoltageRange(uint32_t index,
                                                               uint32_t* min_voltage,
                                                               uint32_t* max_voltage) {
    //TODO(ravoorir): to be implemented
    return ZX_OK;
}

zx_status_t TestPowerDevice::PowerImplRequestVoltage(uint32_t index, uint32_t voltage,
                                                     uint32_t* actual_voltage) {
    //TODO(ravoorir): to be implemented
    return ZX_OK;
}


zx_status_t TestPowerDevice::PowerImplWritePmicCtrlReg(uint32_t index,
                                                       uint32_t addr, uint32_t value) {
    // Save most recent write for read.
    last_index_ = index;
    last_addr_ = addr;
    last_value_ = value;

    return ZX_OK;
}

zx_status_t TestPowerDevice::PowerImplReadPmicCtrlReg(uint32_t index,
                                                      uint32_t addr, uint32_t* value) {
    if (index == last_index_ && addr == last_addr_) {
        *value = last_value_;
        return ZX_OK;
    }
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t test_power_bind(void* ctx, zx_device_t* parent) {
    return TestPowerDevice::Create(parent);
}

constexpr zx_driver_ops_t driver_ops = [](){
    zx_driver_ops_t driver_ops = {};
    driver_ops.version = DRIVER_OPS_VERSION;
    driver_ops.bind = test_power_bind;
    return driver_ops;
}();

} // namespace power

ZIRCON_DRIVER_BEGIN(test_power, power::driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_TEST),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_PBUS_TEST),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_TEST_POWER),
ZIRCON_DRIVER_END(test_power)
