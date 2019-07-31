// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddktl/device.h>
#include <ddktl/protocol/amlogiccanvas.h>
#include <ddktl/protocol/clock.h>
#include <ddktl/protocol/codec.h>
#include <ddktl/protocol/ethernet/board.h>
#include <ddktl/protocol/gpio.h>
#include <ddktl/protocol/i2c.h>
#include <ddktl/protocol/mipicsi.h>
#include <ddktl/protocol/platform/device.h>
#include <ddktl/protocol/power.h>
#include <ddktl/protocol/sysmem.h>
#include <ddktl/protocol/usb/modeswitch.h>
#include <lib/zx/channel.h>

#include "proxy-protocol.h"

namespace component {

class ComponentProxy;
using ComponentProxyBase = ddk::Device<ComponentProxy, ddk::Unbindable, ddk::GetProtocolable>;

class ComponentProxy : public ComponentProxyBase,
                       public ddk::AmlogicCanvasProtocol<ComponentProxy>,
                       public ddk::ClockProtocol<ComponentProxy>,
                       public ddk::EthBoardProtocol<ComponentProxy>,
                       public ddk::GpioProtocol<ComponentProxy>,
                       public ddk::I2cProtocol<ComponentProxy>,
                       public ddk::MipiCsiProtocol<ComponentProxy>,
                       public ddk::CodecProtocol<ComponentProxy>,
                       public ddk::PDevProtocol<ComponentProxy>,
                       public ddk::PowerProtocol<ComponentProxy>,
                       public ddk::SysmemProtocol<ComponentProxy>,
                       public ddk::UsbModeSwitchProtocol<ComponentProxy> {
public:
    ComponentProxy(zx_device_t* parent, zx::channel rpc)
        : ComponentProxyBase(parent), rpc_(std::move(rpc)) {}

    static zx_status_t Create(void* ctx, zx_device_t* parent, const char* name,
                              const char* args, zx_handle_t raw_rpc);

    zx_status_t DdkGetProtocol(uint32_t, void*);
    void DdkUnbind();
    void DdkRelease();

    zx_status_t Rpc(const ProxyRequest* req, size_t req_length, ProxyResponse* resp,
                    size_t resp_length, const zx_handle_t* in_handles, size_t in_handle_count,
                    zx_handle_t* out_handles, size_t out_handle_count, size_t* out_actual);

    zx_status_t Rpc(const ProxyRequest* req, size_t req_length, ProxyResponse* resp,
                    size_t resp_length) {
        return Rpc(req, req_length, resp, resp_length, nullptr, 0, nullptr, 0, nullptr);
    }

    zx_status_t AmlogicCanvasConfig(zx::vmo vmo, size_t offset, const canvas_info_t* info,
                                    uint8_t* out_canvas_idx);
    zx_status_t AmlogicCanvasFree(uint8_t canvas_idx);
    zx_status_t ClockEnable();
    zx_status_t ClockDisable();
    zx_status_t ClockIsEnabled(bool* out_enabled);
    zx_status_t ClockSetRate(uint64_t hz);
    zx_status_t ClockQuerySupportedRate(uint64_t max_rate, uint64_t* out_max_supported_rate);
    zx_status_t ClockGetRate(uint64_t* out_current_rate);
    zx_status_t EthBoardResetPhy();
    zx_status_t GpioConfigIn(uint32_t flags);
    zx_status_t GpioConfigOut(uint8_t initial_value);
    zx_status_t GpioSetAltFunction(uint64_t function);
    zx_status_t GpioRead(uint8_t* out_value);
    zx_status_t GpioWrite(uint8_t value);
    zx_status_t GpioGetInterrupt(uint32_t flags, zx::interrupt* out_irq);
    zx_status_t GpioReleaseInterrupt();
    zx_status_t GpioSetPolarity(gpio_polarity_t polarity);
    void I2cTransact(const i2c_op_t* op_list, size_t op_count, i2c_transact_callback callback,
                     void* cookie);
    zx_status_t I2cGetMaxTransferSize(size_t* out_size);
    zx_status_t I2cGetInterrupt(uint32_t flags, zx::interrupt* out_irq);
    zx_status_t PDevGetMmio(uint32_t index, pdev_mmio_t* out_mmio);
    zx_status_t PDevGetInterrupt(uint32_t index, uint32_t flags, zx::interrupt* out_irq);
    zx_status_t PDevGetBti(uint32_t index, zx::bti* out_bti);
    zx_status_t PDevGetSmc(uint32_t index, zx::resource* out_smc);
    zx_status_t PDevGetDeviceInfo(pdev_device_info_t* out_info);
    zx_status_t PDevGetBoardInfo(pdev_board_info_t* out_info);
    zx_status_t PDevDeviceAdd(uint32_t index, const device_add_args_t* args,
                              zx_device_t** out_device);
    zx_status_t PDevGetProtocol(uint32_t proto_id, uint32_t index, void* out_out_protocol_buffer,
                                size_t out_protocol_size, size_t* out_out_protocol_actual);
    zx_status_t PowerEnablePowerDomain();
    zx_status_t PowerDisablePowerDomain();
    zx_status_t PowerGetPowerDomainStatus(power_domain_status_t* out_status);
    zx_status_t PowerGetSupportedVoltageRange(uint32_t* min_voltage, uint32_t* max_voltage);
    zx_status_t PowerRequestVoltage(uint32_t _voltage, uint32_t* actual_voltage);
    zx_status_t PowerWritePmicCtrlReg(uint32_t reg_addr, uint32_t value);
    zx_status_t PowerReadPmicCtrlReg(uint32_t reg_addr, uint32_t* out_value);
    zx_status_t SysmemConnect(zx::channel allocator2_request);
    zx_status_t SysmemRegisterHeap(uint64_t heap, zx::channel heap_connection);
    zx_status_t MipiCsiInit(const mipi_info_t* mipi_info,
                            const mipi_adap_info_t* adap_info);
    zx_status_t MipiCsiDeInit();

    void CodecReset(codec_reset_callback callback, void* cookie);
    void CodecGetInfo(codec_get_info_callback callback, void* cookie);
    void CodecIsBridgeable(codec_is_bridgeable_callback callback, void* cookie);
    void CodecSetBridgedMode(bool enable_bridged_mode, codec_set_bridged_mode_callback callback,
                             void* cookie);
    void CodecGetDaiFormats(codec_get_dai_formats_callback callback, void* cookie);
    void CodecSetDaiFormat(const dai_format_t* format, codec_set_dai_format_callback callback,
                           void* cookie);
    void CodecGetGainFormat(codec_get_gain_format_callback callback, void* cookie);
    void CodecGetGainState(codec_get_gain_state_callback callback, void* cookie);
    void CodecSetGainState(const gain_state_t* gain_state, codec_set_gain_state_callback callback,
                           void* cookie);
    void CodecGetPlugState(codec_get_plug_state_callback callback, void* cookie);

    // USB Mode Switch
    zx_status_t UsbModeSwitchSetMode(usb_mode_t mode);

private:
    zx::channel rpc_;
};

} // namespace component
