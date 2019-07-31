// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-thermal.h"

#include <string.h>

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddktl/protocol/composite.h>
#include <fbl/auto_call.h>
#include <fbl/unique_ptr.h>
#include <soc/aml-common/aml-thermal.h>
#include <zircon/syscalls/port.h>

namespace thermal {
namespace {

enum {
    COMPONENT_SCPI,
    COMPONENT_GPIO_FAN_0,
    COMPONENT_GPIO_FAN_1,
    COMPONENT_COUNT,
};

} // namespace

zx_status_t AmlThermal::Create(void* ctx, zx_device_t* device) {
    zxlogf(INFO, "aml_thermal: driver begin...\n");
    zx_status_t status;

    ddk::CompositeProtocolClient composite(device);
    if (!composite.is_valid()) {
        THERMAL_ERROR("could not get composite protocol\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_device_t* components[COMPONENT_COUNT];
    size_t actual;
    composite.GetComponents(components, COMPONENT_COUNT, &actual);
    if (actual != COMPONENT_COUNT) {
        THERMAL_ERROR("could not get components\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    scpi_protocol_t scpi_proto;
    status = device_get_protocol(components[COMPONENT_SCPI], ZX_PROTOCOL_SCPI, &scpi_proto);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not get scpi protocol: %d\n", status);
        return status;
    }

    gpio_protocol_t fan0_gpio_proto;
    status = device_get_protocol(components[COMPONENT_GPIO_FAN_0], ZX_PROTOCOL_GPIO,
                                 &fan0_gpio_proto);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not get fan0 gpio protocol: %d\n", status);
        return status;
    }

    gpio_protocol_t fan1_gpio_proto;
    status = device_get_protocol(components[COMPONENT_GPIO_FAN_1], ZX_PROTOCOL_GPIO,
                                 &fan1_gpio_proto);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not get fan1 gpio protocol: %d\n", status);
        return status;
    }

    ddk::ScpiProtocolClient scpi(&scpi_proto);
    uint32_t sensor_id;
    status = scpi.GetSensor("aml_thermal", &sensor_id);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not thermal get sensor: %d\n", status);
        return status;
    }

    zx::port port;
    status = zx::port::create(0, &port);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not configure port: %d\n", status);
        return status;
    }

    auto thermal = std::make_unique<AmlThermal>(device, fan0_gpio_proto, fan1_gpio_proto,
                                                scpi_proto, sensor_id, std::move(port));

    status = thermal->DdkAdd("vim-thermal", DEVICE_ADD_INVISIBLE);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not add driver: %d\n", status);
        return status;
    }

    // Perform post-construction initialization before device is made visible.
    status = thermal->Init(components[COMPONENT_SCPI]);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not initialize thermal driver: %d\n", status);
        thermal->DdkRemove();
        return status;
    }

    thermal->DdkMakeVisible();

    // devmgr is now in charge of this device.
    __UNUSED auto _ = thermal.release();
    return ZX_OK;
}

zx_status_t AmlThermal::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
    return fuchsia_hardware_thermal_Device_dispatch(this, txn, msg, &fidl_ops);
}

zx_status_t AmlThermal::GetInfo(fidl_txn_t* txn) {
    return fuchsia_hardware_thermal_DeviceGetInfo_reply(txn, ZX_ERR_NOT_SUPPORTED, nullptr);
}

zx_status_t AmlThermal::GetDeviceInfo(fidl_txn_t* txn) {
    return fuchsia_hardware_thermal_DeviceGetDeviceInfo_reply(txn, ZX_OK, &info_);
}

zx_status_t AmlThermal::GetDvfsInfo(fuchsia_hardware_thermal_PowerDomain power_domain,
                                    fidl_txn_t* txn) {
    if (power_domain >= fuchsia_hardware_thermal_MAX_DVFS_DOMAINS) {
        return fuchsia_hardware_thermal_DeviceGetDvfsInfo_reply(txn, ZX_ERR_INVALID_ARGS, nullptr);
    }

    scpi_opp_t opps = {};
    auto status = scpi_.GetDvfsInfo(static_cast<uint8_t>(power_domain), &opps);
    return fuchsia_hardware_thermal_DeviceGetDvfsInfo_reply(txn, status, &opps);
}

zx_status_t AmlThermal::GetTemperature(fidl_txn_t* txn) {
    return fuchsia_hardware_thermal_DeviceGetTemperature_reply(txn, ZX_OK, temperature_);
}

zx_status_t AmlThermal::GetStateChangeEvent(fidl_txn_t* txn) {
    return fuchsia_hardware_thermal_DeviceGetStateChangeEvent_reply(txn, ZX_ERR_NOT_SUPPORTED,
                                                                    ZX_HANDLE_INVALID);
}

zx_status_t AmlThermal::GetStateChangePort(fidl_txn_t* txn) {
    zx::port dup;
    zx_status_t status = port_.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup);
    return fuchsia_hardware_thermal_DeviceGetStateChangePort_reply(txn, status, dup.release());
}

zx_status_t AmlThermal::SetTrip(uint32_t id, uint32_t temp, fidl_txn_t* txn) {
    return fuchsia_hardware_thermal_DeviceSetTrip_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

zx_status_t AmlThermal::GetDvfsOperatingPoint(fuchsia_hardware_thermal_PowerDomain power_domain,
                                              fidl_txn_t* txn) {
    if (power_domain == fuchsia_hardware_thermal_PowerDomain_BIG_CLUSTER_POWER_DOMAIN) {
        return fuchsia_hardware_thermal_DeviceGetDvfsOperatingPoint_reply(
            txn, ZX_OK, static_cast<uint16_t>(cur_bigcluster_opp_idx_));
    } else if (power_domain == fuchsia_hardware_thermal_PowerDomain_LITTLE_CLUSTER_POWER_DOMAIN) {
        return fuchsia_hardware_thermal_DeviceGetDvfsOperatingPoint_reply(
            txn, ZX_OK, static_cast<uint16_t>(cur_littlecluster_opp_idx_));
    }

    return fuchsia_hardware_thermal_DeviceGetDvfsOperatingPoint_reply(txn, ZX_ERR_INVALID_ARGS, 0);
}

zx_status_t AmlThermal::SetDvfsOperatingPoint(uint16_t op_idx,
                                              fuchsia_hardware_thermal_PowerDomain power_domain,
                                              fidl_txn_t* txn) {
    zx_status_t status = ZX_OK;
    if (power_domain == fuchsia_hardware_thermal_PowerDomain_BIG_CLUSTER_POWER_DOMAIN) {
        if (op_idx != cur_bigcluster_opp_idx_) {
            status = scpi_.SetDvfsIdx(static_cast<uint8_t>(power_domain), op_idx);
        }
        cur_bigcluster_opp_idx_ = op_idx;
    } else if (power_domain == fuchsia_hardware_thermal_PowerDomain_LITTLE_CLUSTER_POWER_DOMAIN) {
        if (op_idx != cur_littlecluster_opp_idx_) {
            status = scpi_.SetDvfsIdx(static_cast<uint8_t>(power_domain), op_idx);
        }
        cur_littlecluster_opp_idx_ = op_idx;
    } else {
        status = ZX_ERR_INVALID_ARGS;
    }

    return fuchsia_hardware_thermal_DeviceSetDvfsOperatingPoint_reply(txn, status);
}

zx_status_t AmlThermal::GetFanLevel(fidl_txn_t* txn) {
    return fuchsia_hardware_thermal_DeviceGetFanLevel_reply(txn, ZX_OK, fan_level_);
}

zx_status_t AmlThermal::SetFanLevel(uint32_t fan_level, fidl_txn_t* txn) {
    return fuchsia_hardware_thermal_DeviceSetFanLevel_reply(
        txn, SetFanLevel(static_cast<FanLevel>(fan_level)));
}

void AmlThermal::JoinWorkerThread() {
    const auto status = thrd_join(worker_, nullptr);
    if (status != thrd_success) {
        THERMAL_ERROR("worker thread failed: %d\n", status);
    }
}

void AmlThermal::DdkRelease() {
    if (worker_) {
        JoinWorkerThread();
    }
    delete this;
}

void AmlThermal::DdkUnbind() {
    sync_completion_signal(&quit_);
    DdkRemove();
}

zx_status_t AmlThermal::Init(zx_device_t* dev) {
    auto status = fan0_gpio_.ConfigOut(0);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not configure FAN_CTL0 gpio: %d\n", status);
        return status;
    }

    status = fan1_gpio_.ConfigOut(0);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not configure FAN_CTL1 gpio: %d\n", status);
        return status;
    }

    size_t read;
    status = device_get_metadata(dev, DEVICE_METADATA_THERMAL_CONFIG, &info_,
                                 sizeof(fuchsia_hardware_thermal_ThermalDeviceInfo), &read);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not read device metadata: %d\n", status);
        return status;
    } else if (read != sizeof(fuchsia_hardware_thermal_ThermalDeviceInfo)) {
        THERMAL_ERROR("could not read device metadata\n");
        return ZX_ERR_NO_MEMORY;
    }

    status = scpi_.GetDvfsInfo(fuchsia_hardware_thermal_PowerDomain_BIG_CLUSTER_POWER_DOMAIN,
                               &info_.opps[0]);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not get bigcluster dvfs opps: %d\n", status);
        return status;
    }

    status = scpi_.GetDvfsInfo(fuchsia_hardware_thermal_PowerDomain_LITTLE_CLUSTER_POWER_DOMAIN,
                               &info_.opps[1]);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not get littlecluster dvfs opps: %d\n", status);
        return status;
    }

    auto start_thread = [](void* arg) { return static_cast<AmlThermal*>(arg)->Worker(); };
    status = thrd_create_with_name(&worker_, start_thread, this, "aml_thermal_notify_thread");
    if (status != ZX_OK) {
        THERMAL_ERROR("could not start worker thread: %d\n", status);
        return status;
    }

    return ZX_OK;
}

zx_status_t AmlThermal::NotifyThermalDaemon(uint32_t trip_index) const {
    zx_port_packet_t pkt;
    pkt.key = trip_index;
    pkt.type = ZX_PKT_TYPE_USER;
    return port_.queue(&pkt);
}

zx_status_t AmlThermal::SetFanLevel(FanLevel level) {
    // Levels per individual system fan.
    uint8_t fan0_level;
    uint8_t fan1_level;

    switch (level) {
    case FAN_L0:
        fan0_level = 0;
        fan1_level = 0;
        break;
    case FAN_L1:
        fan0_level = 1;
        fan1_level = 0;
        break;
    case FAN_L2:
        fan0_level = 0;
        fan1_level = 1;
        break;
    case FAN_L3:
        fan0_level = 1;
        fan1_level = 1;
        break;
    default:
        THERMAL_ERROR("unknown fan level: %d\n", level);
        return ZX_ERR_INVALID_ARGS;
    }

    auto status = fan0_gpio_.Write(fan0_level);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not set FAN_CTL0 level: %d\n", status);
        return status;
    }

    status = fan1_gpio_.Write(fan1_level);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not set FAN_CTL1 level: %d\n", status);
        return status;
    }

    fan_level_ = level;
    return ZX_OK;
}

int AmlThermal::Worker() {
    zx_status_t status;
    uint32_t trip_pt = 0;
    const uint32_t trip_limit = info_.num_trip_points - 1;
    bool crit = false;
    bool signal = false;

    // Notify thermal daemon of initial settings.
    status = NotifyThermalDaemon(trip_pt);
    if (status != ZX_OK) {
        THERMAL_ERROR("could not notify thermal daemon: %d\n", status);
        return status;
    }

    do {
        status = scpi_.GetSensorValue(sensor_id_, &temperature_);
        if (status != ZX_OK) {
            THERMAL_ERROR("could not read temperature: %d\n", status);
            return status;
        }

        signal = true;
        if (trip_pt != trip_limit && temperature_ >= info_.trip_point_info[trip_pt + 1].up_temp) {
            trip_pt++; // Triggered next trip point.
        } else if (trip_pt && temperature_ < info_.trip_point_info[trip_pt].down_temp) {
            if (trip_pt == trip_limit) {
                // A prev trip point triggered, so the temperature is falling
                // down below the critical temperature.  Make a note of that.
                crit = false;
            }
            trip_pt--; // Triggered prev trip point.
        } else if (trip_pt == trip_limit && temperature_ >= info_.critical_temp && !crit) {
            // The device temperature is crossing the critical temperature, set
            // the CPU freq to the lowest possible setting to ensure the
            // temperature doesn't rise any further.
            crit = true;
            status = scpi_.SetDvfsIdx(
                fuchsia_hardware_thermal_PowerDomain_BIG_CLUSTER_POWER_DOMAIN, 0);
            if (status != ZX_OK) {
                THERMAL_ERROR("unable to set DVFS OPP for Big cluster\n");
                return status;
            }

            status = scpi_.SetDvfsIdx(
                fuchsia_hardware_thermal_PowerDomain_LITTLE_CLUSTER_POWER_DOMAIN, 0);
            if (status != ZX_OK) {
                THERMAL_ERROR("unable to set DVFS OPP for Little cluster\n");
                return status;
            }
        } else {
            signal = false;
        }

        if (signal) {
            // Notify the thermal daemon about which trip point triggered.
            status = NotifyThermalDaemon(trip_pt);
            if (status != ZX_OK) {
                THERMAL_ERROR("could not notify thermal daemon: %d\n", status);
                return status;
            }
        }

    } while (sync_completion_wait(&quit_, duration_.get()) == ZX_ERR_TIMED_OUT);
    return ZX_OK;
}

static constexpr zx_driver_ops_t driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = AmlThermal::Create;
    return ops;
}();

} // namespace thermal

ZIRCON_DRIVER_BEGIN(aml_thermal, thermal::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_AMLOGIC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_AMLOGIC_S912),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_AMLOGIC_THERMAL),
ZIRCON_DRIVER_END(aml_thermal)
