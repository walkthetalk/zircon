// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform/bus.h>
#include <soc/aml-s912/s912-gpio.h>
#include <soc/aml-s912/s912-hw.h>

#include "vim.h"
#include "vim-gpios.h"

namespace vim {
// DMC MMIO for display driver
static pbus_mmio_t vim_display_mmios[] = {
    {
        .base = S912_PRESET_BASE,
        .length = S912_PRESET_LENGTH,
    },
    {
        .base = S912_HDMITX_BASE,
        .length = S912_HDMITX_LENGTH,
    },
    {
        .base = S912_HIU_BASE,
        .length = S912_HIU_LENGTH,
    },
    {
        .base = S912_VPU_BASE,
        .length = S912_VPU_LENGTH,
    },
    {
        .base = S912_HDMITX_SEC_BASE,
        .length = S912_HDMITX_SEC_LENGTH,
    },
    {
        .base = S912_DMC_REG_BASE,
        .length = S912_DMC_REG_LENGTH,
    },
    {
        .base = S912_CBUS_REG_BASE,
        .length = S912_CBUS_REG_LENGTH,
    },
    {
        .base = S912_AUDOUT_BASE,
        .length = S912_AUDOUT_LEN,
    },
};

static const pbus_irq_t vim_display_irqs[] = {
    {
        .irq = S912_VIU1_VSYNC_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
    {
        .irq = S912_RDMA_DONE_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
};

static const pbus_bti_t vim_display_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = BTI_DISPLAY,
    },
    {
        .iommu_index = 0,
        .bti_id = BTI_AUDIO,
    },
};

static const zx_bind_inst_t root_match[] = {
    BI_MATCH(),
};
static const zx_bind_inst_t hpd_gpio_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_GPIO),
    BI_MATCH_IF(EQ, BIND_GPIO_PIN, GPIO_DISPLAY_HPD),
};
static const zx_bind_inst_t canvas_match[] = {
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_AMLOGIC_CANVAS),
};
static const zx_bind_inst_t sysmem_match[] = {
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_SYSMEM),
};
static const device_component_part_t hpd_gpio_component[] = {
    { countof(root_match), root_match },
    { countof(hpd_gpio_match), hpd_gpio_match },
};
static const device_component_part_t canvas_component[] = {
    { countof(root_match), root_match },
    { countof(canvas_match), canvas_match },
};
static const device_component_part_t sysmem_component[] = {
    { countof(root_match), root_match },
    { countof(sysmem_match), sysmem_match },
};
static const device_component_t components[] = {
    { countof(hpd_gpio_component), hpd_gpio_component },
    { countof(canvas_component), canvas_component },
    { countof(sysmem_component), sysmem_component },
};

zx_status_t Vim::DisplayInit() {
    zx_status_t status;
    pbus_dev_t display_dev = {};
    display_dev.name = "display";
    display_dev.vid = PDEV_VID_KHADAS;
    display_dev.pid = PDEV_PID_VIM2;
    display_dev.did = PDEV_DID_VIM_DISPLAY;
    display_dev.mmio_list = vim_display_mmios;
    display_dev.mmio_count = countof(vim_display_mmios);
    display_dev.irq_list = vim_display_irqs;
    display_dev.irq_count = countof(vim_display_irqs);
    display_dev.bti_list = vim_display_btis;
    display_dev.bti_count = countof(vim_display_btis);

    // enable this #if 0 in order to enable the SPDIF out pin for VIM2 (GPIO H4, pad M22)
#if 0
    gpio_set_alt_function(&bus->gpio, S912_SPDIF_H4, S912_SPDIF_H4_OUT_FN);
#endif

    if ((status = pbus_.CompositeDeviceAdd(&display_dev, components, countof(components), UINT32_MAX)) != ZX_OK) {
        zxlogf(ERROR, "DisplayInit: pbus_device_add() failed for display: %d\n", status);
        return status;
    }

    return ZX_OK;
}
} //namespace vim
