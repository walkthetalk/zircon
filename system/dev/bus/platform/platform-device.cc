// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform-device.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <fbl/function.h>
#include <zircon/syscalls/resource.h>

#include "platform-bus.h"

namespace platform_bus {

zx_status_t PlatformDevice::Create(const pbus_dev_t* pdev, zx_device_t* parent, PlatformBus* bus,
                                   fbl::unique_ptr<platform_bus::PlatformDevice>* out) {
    fbl::AllocChecker ac;
    fbl::unique_ptr<platform_bus::PlatformDevice> dev(
        new (&ac) platform_bus::PlatformDevice(parent, bus, pdev));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    auto status = dev->Init(pdev);
    if (status != ZX_OK) {
        return status;
    }
    out->swap(dev);
    return ZX_OK;
}

PlatformDevice::PlatformDevice(zx_device_t* parent, PlatformBus* bus, const pbus_dev_t* pdev)
    : PlatformDeviceType(parent), bus_(bus), vid_(pdev->vid), pid_(pdev->pid),
      did_(pdev->did) {
    strlcpy(name_, pdev->name, sizeof(name_));
}

zx_status_t PlatformDevice::Init(const pbus_dev_t* pdev) {
    return resources_.Init(pdev);
}

// Create a resource and pass it back to the proxy along with necessary metadata
// to create/map the VMO in the driver process.
zx_status_t PlatformDevice::RpcGetMmio(uint32_t index, zx_paddr_t* out_paddr, size_t* out_length,
                                       zx_handle_t* out_handle, uint32_t* out_handle_count) {
    if (index >= resources_.mmio_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }
    const auto& root_rsrc = bus_->GetResource();
    if (!root_rsrc->is_valid()) {
        return ZX_ERR_NO_RESOURCES;
    }

    const pbus_mmio_t& mmio = resources_.mmio(index);
    zx::resource resource;
    char rsrc_name[ZX_MAX_NAME_LEN];
    snprintf(rsrc_name, ZX_MAX_NAME_LEN - 1, "%s.pbus[%u]", name_, index);
    zx_status_t status = zx::resource::create(*root_rsrc, ZX_RSRC_KIND_MMIO, mmio.base,
                                              mmio.length, rsrc_name, sizeof(rsrc_name), &resource);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: pdev_rpc_get_mmio: zx_resource_create failed: %d\n", name_, status);
        return status;
    }

    *out_paddr = mmio.base;
    *out_length = mmio.length;
    *out_handle_count = 1;
    *out_handle = resource.release();
    return ZX_OK;
}

// Create a resource and pass it back to the proxy along with necessary metadata
// to create the IRQ in the driver process.
zx_status_t PlatformDevice::RpcGetInterrupt(uint32_t index, uint32_t* out_irq, uint32_t* out_mode,
                                            zx_handle_t* out_handle, uint32_t* out_handle_count) {
    if (index >= resources_.irq_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const auto& root_rsrc = bus_->GetResource();
    if (!root_rsrc->is_valid()) {
        return ZX_ERR_NO_RESOURCES;
    }

    zx::resource resource;
    const pbus_irq_t& irq = resources_.irq(index);
    uint32_t options = ZX_RSRC_KIND_IRQ | ZX_RSRC_FLAG_EXCLUSIVE;
    char rsrc_name[ZX_MAX_NAME_LEN];
    snprintf(rsrc_name, ZX_MAX_NAME_LEN - 1, "%s.pbus[%u]", name_, index);
    zx_status_t status = zx::resource::create(*root_rsrc, options, irq.irq, 1, rsrc_name,
                                              sizeof(rsrc_name), &resource);
    if (status != ZX_OK) {
        return status;
    }

    *out_irq = irq.irq;
    *out_mode = irq.mode;
    *out_handle_count = 1;
    *out_handle = resource.release();
    return ZX_OK;
}

zx_status_t PlatformDevice::RpcGetBti(uint32_t index, zx_handle_t* out_handle,
                                      uint32_t* out_handle_count) {
    if (index >= resources_.bti_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const pbus_bti_t& bti = resources_.bti(index);

    zx::bti out_bti;
    zx_status_t status = bus_->IommuGetBti(bti.iommu_index, bti.bti_id, &out_bti);
    *out_handle = out_bti.release();

    if (status == ZX_OK) {
        *out_handle_count = 1;
    }

    return status;
}

zx_status_t PlatformDevice::RpcGetSmc(uint32_t index, zx_handle_t* out_handle,
                                      uint32_t* out_handle_count) {
    if (index >= resources_.smc_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const auto& root_rsrc = bus_->GetResource();
    if (!root_rsrc->is_valid()) {
        return ZX_ERR_NO_RESOURCES;
    }

    zx::resource resource;
    const pbus_smc_t& smc = resources_.smc(index);
    uint32_t options = ZX_RSRC_KIND_SMC;
    if (smc.exclusive)
        options |= ZX_RSRC_FLAG_EXCLUSIVE;
    char rsrc_name[ZX_MAX_NAME_LEN];
    snprintf(rsrc_name, ZX_MAX_NAME_LEN - 1, "%s.pbus[%u]", name_, index);
    zx_status_t status = zx::resource::create(*root_rsrc, options,
                                              smc.service_call_num_base, smc.count, rsrc_name,
                                              sizeof(rsrc_name), &resource);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: pdev_rpc_get_smc: zx_resource_create failed: %d\n", name_, status);
        return status;
    }

    *out_handle_count = 1;
    *out_handle = resource.release();
    return ZX_OK;
}

zx_status_t PlatformDevice::RpcGetDeviceInfo(pdev_device_info_t* out_info) {
    pdev_device_info_t info = {
        .vid = vid_,
        .pid = pid_,
        .did = did_,
        .mmio_count = static_cast<uint32_t>(resources_.mmio_count()),
        .irq_count = static_cast<uint32_t>(resources_.irq_count()),
        .gpio_count = static_cast<uint32_t>(resources_.gpio_count()),
        .clk_count = static_cast<uint32_t>(resources_.clk_count()),
        .bti_count = static_cast<uint32_t>(resources_.bti_count()),
        .smc_count = static_cast<uint32_t>(resources_.smc_count()),
        .metadata_count = static_cast<uint32_t>(resources_.metadata_count() +
                                                resources_.boot_metadata_count()),
        .reserved = {},
        .name = {},
    };
    static_assert(sizeof(info.name) == sizeof(name_), "");
    memcpy(info.name, name_, sizeof(out_info->name));
    memcpy(out_info, &info, sizeof(info));

    return ZX_OK;
}

zx_status_t PlatformDevice::RpcGetMetadata(uint32_t index, uint32_t* out_type, uint8_t* buf,
                                           uint32_t buf_size, uint32_t* actual) {
    if (index >= resources_.metadata_count() + resources_.boot_metadata_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    if (index < resources_.metadata_count()) {
        auto& metadata = resources_.metadata(index);
        if (metadata.data_size > buf_size) {
            return ZX_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(buf, metadata.data_buffer, metadata.data_size);
        *out_type = metadata.type;
        *actual = static_cast<uint32_t>(metadata.data_size);
        return ZX_OK;
    }

    // boot_metadata indices follow metadata indices.
    index -= static_cast<uint32_t>(resources_.metadata_count());

    auto& metadata = resources_.boot_metadata(index);
    zx::vmo vmo;
    uint32_t length;
    zx_status_t status = bus_->GetBootItem(metadata.zbi_type, metadata.zbi_extra, &vmo, &length);
    if (status != ZX_OK) {
        return status;
    } else if (length > buf_size) {
        return ZX_ERR_BUFFER_TOO_SMALL;
    }

    status = vmo.read(buf, 0, length);
    if (status != ZX_OK) {
        return status;
    }
    *out_type = metadata.zbi_type;
    *actual = length;
    return ZX_OK;
}

zx_status_t PlatformDevice::RpcGpioConfigIn(uint32_t index, uint32_t flags) {
    if (bus_->gpio() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.gpio_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return bus_->gpio()->ConfigIn(resources_.gpio(index).gpio, flags);
}

zx_status_t PlatformDevice::RpcGpioConfigOut(uint32_t index, uint8_t initial_value) {
    if (bus_->gpio() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.gpio_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return bus_->gpio()->ConfigOut(resources_.gpio(index).gpio, initial_value);
}

zx_status_t PlatformDevice::RpcGpioSetAltFunction(uint32_t index, uint64_t function) {
    if (bus_->gpio() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.gpio_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return bus_->gpio()->SetAltFunction(resources_.gpio(index).gpio, function);
}

zx_status_t PlatformDevice::RpcGpioRead(uint32_t index, uint8_t* out_value) {
    if (bus_->gpio() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.gpio_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return bus_->gpio()->Read(resources_.gpio(index).gpio, out_value);
}

zx_status_t PlatformDevice::RpcGpioWrite(uint32_t index, uint8_t value) {
    if (bus_->gpio() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.gpio_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return bus_->gpio()->Write(resources_.gpio(index).gpio, value);
}

zx_status_t PlatformDevice::RpcGpioGetInterrupt(uint32_t index, uint32_t flags,
                                                zx_handle_t* out_handle,
                                                uint32_t* out_handle_count) {
    if (bus_->gpio() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.gpio_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    zx::interrupt irq;
    zx_status_t status = bus_->gpio()->GetInterrupt(resources_.gpio(index).gpio, flags, &irq);
    if (status == ZX_OK) {
        *out_handle = irq.release();
        *out_handle_count = 1;
    }
    return status;
}

zx_status_t PlatformDevice::RpcGpioReleaseInterrupt(uint32_t index) {
    if (bus_->gpio() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.gpio_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }
    return bus_->gpio()->ReleaseInterrupt(resources_.gpio(index).gpio);
}

zx_status_t PlatformDevice::RpcGpioSetPolarity(uint32_t index, uint32_t flags) {
    if (bus_->gpio() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.gpio_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }
    return bus_->gpio()->SetPolarity(resources_.gpio(index).gpio, flags);
}

zx_status_t PlatformDevice::RpcClockEnable(uint32_t index) {
    if (bus_->clk() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.clk_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return bus_->clk()->Enable(resources_.clk(index).clk);
}

zx_status_t PlatformDevice::RpcClockDisable(uint32_t index) {
    if (bus_->clk() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.clk_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return bus_->clk()->Disable(resources_.clk(index).clk);
}

zx_status_t PlatformDevice::RpcClockIsEnabled(uint32_t index, bool* result) {
    if (bus_->clk() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.clk_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return bus_->clk()->IsEnabled(resources_.clk(index).clk, result);
}

zx_status_t PlatformDevice::RpcClockSetRate(uint32_t index, uint64_t rate) {
    if (bus_->clk() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.clk_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return bus_->clk()->SetRate(resources_.clk(index).clk, rate);
}

zx_status_t PlatformDevice::RpcClockQuerySupportedRate(uint32_t index, uint64_t max_rate,
                                                       uint64_t* out_rate) {
    if (bus_->clk() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.clk_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return bus_->clk()->QuerySupportedRate(resources_.clk(index).clk, max_rate, out_rate);
}

zx_status_t PlatformDevice::RpcClockGetRate(uint32_t index, uint64_t* out_current_rate) {
    if (bus_->clk() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= resources_.clk_count()) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return bus_->clk()->GetRate(resources_.clk(index).clk, out_current_rate);
}

zx_status_t PlatformDevice::RpcSysmemConnect(zx::channel allocator_request) {
    if (bus_->sysmem() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    return bus_->sysmem()->Connect(std::move(allocator_request));
}

zx_status_t PlatformDevice::RpcCanvasConfig(zx::vmo vmo, size_t offset, const canvas_info_t* info,
                                            uint8_t* out_canvas_idx) {
    if (bus_->canvas() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    return bus_->canvas()->Config(std::move(vmo), offset, info, out_canvas_idx);
}

zx_status_t PlatformDevice::RpcCanvasFree(uint8_t canvas_index) {
    if (bus_->canvas() == nullptr) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    return bus_->canvas()->Free(canvas_index);
}

zx_status_t PlatformDevice::DdkRxrpc(zx_handle_t channel) {
    if (channel == ZX_HANDLE_INVALID) {
        // proxy device has connected
        return ZX_OK;
    }

    uint8_t req_buf[PROXY_MAX_TRANSFER_SIZE];
    uint8_t resp_buf[PROXY_MAX_TRANSFER_SIZE];
    auto* req_header = reinterpret_cast<platform_proxy_req_t*>(&req_buf);
    auto* resp_header = reinterpret_cast<platform_proxy_rsp_t*>(&resp_buf);
    uint32_t actual;
    zx_handle_t req_handles[ZX_CHANNEL_MAX_MSG_HANDLES];
    zx_handle_t resp_handles[ZX_CHANNEL_MAX_MSG_HANDLES];
    uint32_t req_handle_count;
    uint32_t resp_handle_count = 0;

    auto status = zx_channel_read(channel, 0, &req_buf, req_handles, sizeof(req_buf),
                                  fbl::count_of(req_handles), &actual, &req_handle_count);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_rxrpc: zx_channel_read failed %d\n", status);
        return status;
    }

    resp_header->txid = req_header->txid;
    uint32_t resp_len;

    switch (req_header->proto_id) {
    case ZX_PROTOCOL_PDEV: {
        auto req = reinterpret_cast<rpc_pdev_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu (PDEV)\n", __func__, actual, sizeof(*req));
            return ZX_ERR_INTERNAL;
        }
        auto resp = reinterpret_cast<rpc_pdev_rsp_t*>(&resp_buf);
        resp_len = sizeof(*resp);

        switch (req_header->op) {
        case PDEV_GET_MMIO:
            status = RpcGetMmio(req->index, &resp->paddr, &resp->length, resp_handles,
                                &resp_handle_count);
            break;
        case PDEV_GET_INTERRUPT:
            status = RpcGetInterrupt(req->index, &resp->irq, &resp->mode, resp_handles,
                                     &resp_handle_count);
            break;
        case PDEV_GET_BTI:
            status = RpcGetBti(req->index, resp_handles, &resp_handle_count);
            break;
        case PDEV_GET_SMC:
            status = RpcGetSmc(req->index, resp_handles, &resp_handle_count);
            break;
        case PDEV_GET_DEVICE_INFO:
            status = RpcGetDeviceInfo(&resp->device_info);
            break;
        case PDEV_GET_BOARD_INFO:
            status = bus_->PBusGetBoardInfo(&resp->board_info);
            break;
        case PDEV_GET_METADATA: {
            auto resp = reinterpret_cast<rpc_pdev_metadata_rsp_t*>(resp_buf);
            static_assert(sizeof(*resp) == sizeof(resp_buf), "");
            auto buf_size = static_cast<uint32_t>(sizeof(resp_buf) - sizeof(*resp_header));
            status = RpcGetMetadata(req->index, &resp->pdev.metadata_type, resp->metadata,
                                    buf_size, &resp->pdev.metadata_length);
            resp_len += resp->pdev.metadata_length;
            break;
        }
        default:
            zxlogf(ERROR, "%s: unknown pdev op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    case ZX_PROTOCOL_GPIO: {
        auto req = reinterpret_cast<rpc_gpio_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu (GPIO)\n", __func__, actual, sizeof(*req));
            return ZX_ERR_INTERNAL;
        }
        auto resp = reinterpret_cast<rpc_gpio_rsp_t*>(&resp_buf);
        resp_len = sizeof(*resp);

        switch (req_header->op) {
        case GPIO_CONFIG_IN:
            status = RpcGpioConfigIn(req->index, req->flags);
            break;
        case GPIO_CONFIG_OUT:
            status = RpcGpioConfigOut(req->index, req->value);
            break;
        case GPIO_SET_ALT_FUNCTION:
            status = RpcGpioSetAltFunction(req->index, req->alt_function);
            break;
        case GPIO_READ:
            status = RpcGpioRead(req->index, &resp->value);
            break;
        case GPIO_WRITE:
            status = RpcGpioWrite(req->index, req->value);
            break;
        case GPIO_GET_INTERRUPT:
            status = RpcGpioGetInterrupt(req->index, req->flags, resp_handles,
                                         &resp_handle_count);
            break;
        case GPIO_RELEASE_INTERRUPT:
            status = RpcGpioReleaseInterrupt(req->index);
            break;
        case GPIO_SET_POLARITY:
            status = RpcGpioSetPolarity(req->index, req->polarity);
            break;
        default:
            zxlogf(ERROR, "%s: unknown GPIO op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    case ZX_PROTOCOL_CLOCK: {
        auto req = reinterpret_cast<rpc_clk_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu (CLOCK)\n", __func__, actual,
                   sizeof(*req));
            return ZX_ERR_INTERNAL;
        }
        auto resp = reinterpret_cast<rpc_clk_rsp_t*>(&resp_buf);
        resp_len = sizeof(*resp);

        switch (req_header->op) {
        case CLK_ENABLE:
            status = RpcClockEnable(req->index);
            break;
        case CLK_DISABLE:
            status = RpcClockDisable(req->index);
            break;
        case CLK_IS_ENABLED:
            status = RpcClockIsEnabled(req->index, &resp->is_enabled);
            break;
        case CLK_SET_RATE:
            status = RpcClockSetRate(req->index, req->rate);
            break;
        case CLK_QUERY_SUPPORTED_RATE:
            status = RpcClockQuerySupportedRate(req->index, req->rate, &resp->rate);
            break;
        case CLK_GET_RATE:
            status = RpcClockGetRate(req->index, &resp->rate);
            break;
        default:
            zxlogf(ERROR, "%s: unknown clk op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    case ZX_PROTOCOL_SYSMEM: {
        auto req = reinterpret_cast<platform_proxy_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu (SYSMEM)\n", __func__, actual,
                   sizeof(*req));
            return ZX_ERR_INTERNAL;
        }
        if (req_handle_count != 1) {
            zxlogf(ERROR, "%s received %u handles, expecting 1 (SYSMEM)\n", __func__,
                   req_handle_count);
            return ZX_ERR_INTERNAL;
        }
        resp_len = sizeof(*resp_header);

        switch (req_header->op) {
        case SYSMEM_CONNECT:
            status = RpcSysmemConnect(zx::channel(req_handles[0]));
            break;
        default:
            zxlogf(ERROR, "%s: unknown sysmem op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    case ZX_PROTOCOL_AMLOGIC_CANVAS: {
        auto req = reinterpret_cast<rpc_amlogic_canvas_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu (CANVAS)\n", __func__, actual,
                   sizeof(*req));
            return ZX_ERR_INTERNAL;
        }
        auto resp = reinterpret_cast<rpc_amlogic_canvas_rsp_t*>(&resp_buf);
        resp_len = sizeof(*resp);

        switch (req_header->op) {
        case AMLOGIC_CANVAS_CONFIG:
            if (req_handle_count != 1) {
                zxlogf(ERROR, "%s received %u handles, expecting 1 (CANVAS CONFIG) \n", __func__,
                       req_handle_count);
                return ZX_ERR_INTERNAL;
            }
            status = RpcCanvasConfig(zx::vmo(req_handles[0]), req->offset, &req->info,
                                     &resp->canvas_idx);
            break;
        case AMLOGIC_CANVAS_FREE:
            if (req_handle_count != 0) {
                zxlogf(ERROR, "%s received %u handles, expecting 0 (CANVAS FREE) \n", __func__,
                       req_handle_count);
                return ZX_ERR_INTERNAL;
            }
            status = RpcCanvasFree(req->canvas_idx);
            break;
        default:
            zxlogf(ERROR, "%s: unknown CANVAS op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    default:
        zxlogf(ERROR, "%s: unknown protocol %u\n", __func__, req_header->proto_id);
        return ZX_ERR_INTERNAL;
    }

    // set op to match request so zx_channel_write will return our response
    resp_header->status = status;
    status = zx_channel_write(channel, 0, resp_header, resp_len,
                              (resp_handle_count ? resp_handles : nullptr), resp_handle_count);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_rxrpc: zx_channel_write failed %d\n", status);
    }
    return status;
}

void PlatformDevice::DdkRelease() {
    delete this;
}

zx_status_t PlatformDevice::Start() {
    char name[ZX_DEVICE_NAME_MAX];
    if (vid_ == PDEV_VID_GENERIC && pid_ == PDEV_PID_GENERIC && did_ == PDEV_DID_KPCI) {
        strlcpy(name, "pci", sizeof(name));
    } else {
        snprintf(name, sizeof(name), "%02x:%02x:%01x", vid_, pid_, did_);
    }
    char argstr[64];
    snprintf(argstr, sizeof(argstr), "pdev:%s,", name);

    // Platform devices run in their own devhosts.
    uint32_t device_add_flags = DEVICE_ADD_MUST_ISOLATE;

    const size_t metadata_count = resources_.metadata_count();
    const size_t boot_metadata_count = resources_.boot_metadata_count();
    if (metadata_count > 0 || boot_metadata_count > 0) {
        // Keep device invisible until after we add its metadata.
        device_add_flags |= DEVICE_ADD_INVISIBLE;
    }

    zx_device_prop_t props[] = {
        {BIND_PLATFORM_DEV_VID, 0, vid_},
        {BIND_PLATFORM_DEV_PID, 0, pid_},
        {BIND_PLATFORM_DEV_DID, 0, did_},
    };
    zx_status_t status = DdkAdd(name, device_add_flags, props, fbl::count_of(props),
                                ZX_PROTOCOL_PDEV, argstr);
    if (status != ZX_OK) {
        return status;
    }

    if (metadata_count > 0 || boot_metadata_count > 0) {
        for (size_t i = 0; i < metadata_count; i++) {
            const auto& metadata = resources_.metadata(i);
            status = DdkAddMetadata(metadata.type, metadata.data_buffer, metadata.data_size);
            if (status != ZX_OK) {
                DdkRemove();
                return status;
            }
        }

        for (size_t i = 0; i < boot_metadata_count; i++) {
            const auto& metadata = resources_.boot_metadata(i);
            fbl::Array<uint8_t> data;
            status = bus_->GetBootItem(metadata.zbi_type, metadata.zbi_extra, &data);
            if (status == ZX_OK) {
                status = DdkAddMetadata(metadata.zbi_type, data.get(), data.size());
            }
            if (status != ZX_OK) {
                zxlogf(WARN, "%s failed to add metadata for new device\n", __func__);
            }
        }

        DdkMakeVisible();
    }

    return ZX_OK;
}

} // namespace platform_bus
