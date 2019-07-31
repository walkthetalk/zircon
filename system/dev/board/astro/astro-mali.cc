// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform/bus.h>
#include <soc/aml-s905d2/s905d2-hw.h>
#include <hw/reg.h>

#include "astro.h"

namespace astro {

static const pbus_mmio_t mali_mmios[] = {
    {
        .base = S905D2_MALI_BASE,
        .length = S905D2_MALI_LENGTH,
    },
    {
        .base = S905D2_HIU_BASE,
        .length = S905D2_HIU_LENGTH,
    },
    {
        .base = S905D2_RESET_BASE,
        .length = S905D2_RESET_LENGTH,
    },
};

static const pbus_irq_t mali_irqs[] = {
    {
        .irq = S905D2_MALI_IRQ_PP,
        .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    },
    {
        .irq = S905D2_MALI_IRQ_GPMMU,
        .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    },
    {
        .irq = S905D2_MALI_IRQ_GP,
        .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    },
};

static pbus_bti_t mali_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = 0,
    },
};

static const pbus_dev_t mali_dev = []() {
    pbus_dev_t dev = {};
    dev.name = "mali";
    dev.vid = PDEV_VID_AMLOGIC;
    dev.pid = PDEV_PID_AMLOGIC_S905D2;
    dev.did = PDEV_DID_AMLOGIC_MALI_INIT;
    dev.mmio_list = mali_mmios;
    dev.mmio_count = countof(mali_mmios);
    dev.irq_list = mali_irqs;
    dev.irq_count = countof(mali_irqs);
    dev.bti_list = mali_btis;
    dev.bti_count = countof(mali_btis);
    return dev;
}();

zx_status_t Astro::MaliInit() {

    // Populate the BTI information
    mali_btis[0].iommu_index = 0;
    mali_btis[0].bti_id      = BTI_MALI;

    zx_status_t status = pbus_.DeviceAdd(&mali_dev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DeviceAdd failed: %d\n", __func__, status);
        return status;
    }
    return status;
}

} // namespace astro
