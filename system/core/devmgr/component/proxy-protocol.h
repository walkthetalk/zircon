// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

namespace component {

// Maximum transfer size we can proxy.
static constexpr size_t kProxyMaxTransferSize = 4096;
// TODO(andresoportus): remove these restrictions.
static constexpr size_t kMaxDaiFormats = 8;
static constexpr size_t kMaxChannelsToUse = 8;
static constexpr size_t kMaxChannels = 8;
static constexpr size_t kMaxSampleFormats = 8;
static constexpr size_t kMaxJustifyFormats = 8;
static constexpr size_t kMaxRates = 8;
static constexpr size_t kMaxBitsPerChannel = 8;
static constexpr size_t kMaxBitsPerSample = 8;
static constexpr size_t kMaxCodecStringSize = 64;

/// Header for RPC requests.
struct ProxyRequest {
    uint32_t txid;
    uint32_t proto_id;
};

/// Header for RPC responses.
struct ProxyResponse {
    uint32_t txid;
    zx_status_t status;
};

// ZX_PROTOCOL_PDEV proxy support.
enum class PdevOp {
    GET_MMIO,
    GET_INTERRUPT,
    GET_BTI,
    GET_SMC,
    GET_DEVICE_INFO,
    GET_BOARD_INFO,
};

struct PdevProxyRequest {
    ProxyRequest header;
    PdevOp op;
    uint32_t index;
    uint32_t flags;
};

struct PdevProxyResponse {
    ProxyResponse header;
    zx_off_t offset;
    size_t size;
    uint32_t flags;
    pdev_device_info_t device_info;
    pdev_board_info_t board_info;
};

// Maximum metadata size that can be returned via PDEV_DEVICE_GET_METADATA.
static constexpr uint32_t PROXY_MAX_METADATA_SIZE =
    (kProxyMaxTransferSize - sizeof(PdevProxyResponse));

struct rpc_pdev_metadata_rsp_t {
    PdevProxyResponse pdev;
    uint8_t metadata[PROXY_MAX_METADATA_SIZE];
};

// ZX_PROTOCOL_MIPI_CSI proxy support.
enum class MipiCsiOp {
    INIT,
    DEINIT,
};

struct MipiCsiProxyRequest {
    ProxyRequest header;
    MipiCsiOp op;
    mipi_info_t mipi_info;
    mipi_adap_info_t adap_info;
};

// ZX_PROTOCOL_CODEC proxy support.
enum class CodecOp {
    RESET,
    GET_INFO,
    IS_BRIDGEABLE,
    SET_BRIDGED_MODE,
    GET_GAIN_FORMAT,
    GET_GAIN_STATE,
    SET_GAIN_STATE,
    GET_DAI_FORMATS,
    SET_DAI_FORMAT,
    GET_PLUG_STATE,
};

struct CodecProxyRequest {
    ProxyRequest header;
    CodecOp op;
};

struct CodecIsBridgeableProxyResponse {
    ProxyResponse header;
    bool supports_bridged_mode;
};

struct CodecSetBridgedProxyRequest {
    ProxyRequest header;
    CodecOp op;
    bool enable_bridged_mode;
};

struct CodecGainFormatProxyResponse {
    ProxyResponse header;
    gain_format_t format;
};

struct CodecGainStateProxyRequest {
    ProxyRequest header;
    CodecOp op;
    gain_state_t state;
};

struct CodecGainStateProxyResponse {
    ProxyResponse header;
    gain_state_t state;
};

struct CodecDaiFormatsProxyResponse {
    ProxyResponse header;
    dai_supported_formats_t formats[kMaxDaiFormats];
    uint32_t number_of_channels[kMaxChannels];
    sample_format_t formats_list[kMaxSampleFormats];
    justify_format_t justify_formats[kMaxJustifyFormats];
    uint32_t frame_rates_list[kMaxRates];
    uint8_t bits_per_channel[kMaxBitsPerChannel];
    uint8_t bits_per_sample_list[kMaxBitsPerSample];
};

struct CodecDaiFormatProxyRequest {
    ProxyRequest header;
    CodecOp op;
    dai_format_t format;
    uint32_t channels_to_use[kMaxChannelsToUse];
};

struct CodecPlugStateProxyResponse {
    ProxyResponse header;
    plug_state_t plug_state;
};

struct CodecInfoProxyResponse {
    ProxyResponse header;
    char unique_id[kMaxCodecStringSize];
    char manufacturer[kMaxCodecStringSize];
    char product_name[kMaxCodecStringSize];
};

// ZX_PROTOCOL_GPIO proxy support.
enum class GpioOp {
    CONFIG_IN,
    CONFIG_OUT,
    SET_ALT_FUNCTION,
    READ,
    WRITE,
    GET_INTERRUPT,
    RELEASE_INTERRUPT,
    SET_POLARITY,
};

struct GpioProxyRequest {
    ProxyRequest header;
    GpioOp op;
    uint32_t flags;
    uint32_t polarity;
    uint64_t alt_function;
    uint8_t value;
};

struct GpioProxyResponse {
    ProxyResponse header;
    uint8_t value;
};

// ZX_PROTOCOL_CLOCK proxy support.
enum class ClockOp {
    ENABLE,
    DISABLE,
    IS_ENABLED,
    SET_RATE,
    QUERY_SUPPORTED_RATE,
    GET_RATE,
};

struct ClockProxyRequest {
    ProxyRequest header;
    ClockOp op;
    uint64_t rate;
};

struct ClockProxyResponse {
    ProxyResponse header;
    bool is_enabled;
    uint64_t rate;
};

// ZX_PROTOCOL_POWER proxy support.
enum class PowerOp {
    ENABLE,
    DISABLE,
    GET_STATUS,
    GET_SUPPORTED_VOLTAGE_RANGE,
    REQUEST_VOLTAGE,
    WRITE_PMIC_CTRL_REG,
    READ_PMIC_CTRL_REG,
};

struct PowerProxyRequest {
    ProxyRequest header;
    PowerOp op;
    uint32_t set_voltage;
    uint32_t reg_addr;
    uint32_t reg_value;
};

struct PowerProxyResponse {
    ProxyResponse header;
    power_domain_status_t status;
    uint32_t min_voltage;
    uint32_t max_voltage;
    uint32_t actual_voltage;
    uint32_t reg_value;
};

// ZX_PROTOCOL_SYSMEM proxy support.
enum class SysmemOp {
    CONNECT,
    REGISTER_HEAP,
};

struct SysmemProxyRequest {
    ProxyRequest header;
    SysmemOp op;
    uint64_t heap;
};

// ZX_PROTOCOL_AMLOGIC_CANVAS proxy support.
enum class AmlogicCanvasOp {
    CONFIG,
    FREE,
};

struct AmlogicCanvasProxyRequest {
    ProxyRequest header;
    AmlogicCanvasOp op;
    size_t offset;
    canvas_info_t info;
    uint8_t canvas_idx;
};

struct AmlogicCanvasProxyResponse {
    ProxyResponse header;
    uint8_t canvas_idx;
};

// ZX_PROTOCOL_ETH_BOARD proxy support.
enum class EthBoardOp {
    RESET_PHY,
};

struct EthBoardProxyRequest {
    ProxyRequest header;
    EthBoardOp op;
};

// ZX_PROTOCOL_I2C proxy support.
enum class I2cOp {
    TRANSACT,
    GET_MAX_TRANSFER_SIZE,
    GET_INTERRUPT,
};

struct I2cProxyRequest {
    ProxyRequest header;
    I2cOp op;
    size_t op_count;
    uint32_t flags;
};

struct I2cProxyResponse {
    ProxyResponse header;
    size_t size;
};

struct I2cProxyOp {
    size_t length;
    bool is_read;
    bool stop;
};

// ZX_PROTOCOL_USB_MODE_SWITCH proxy support.
enum class UsbModeSwitchOp {
    SET_MODE,
};

struct UsbModeSwitchProxyRequest {
    ProxyRequest header;
    UsbModeSwitchOp op;
    usb_mode_t mode;
};

} // namespace component
