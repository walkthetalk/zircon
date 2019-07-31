// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/metadata.h>
#include <ddk/metadata/spi.h>
#include <ddk/platform-defs.h>
#include <fbl/algorithm.h>
#include <lib/mmio/mmio.h>

#include <soc/aml-t931/t931-gpio.h>
#include <soc/aml-t931/t931-spi.h>

#include "sherlock.h"
#include "sherlock-gpios.h"

#define HHI_SPICC_CLK_CNTL (0xf7 * 4)
#define spicc_0_clk_sel_fclk_div2 (4 << 7)
#define spicc_0_clk_en            (1 << 6)
#define spicc_0_clk_div(x)        ((x) - 1)

namespace sherlock {

static const pbus_mmio_t spi_mmios[] = {
    {
        .base = T931_SPICC0_BASE,
        .length = 0x44,
    },
    {
        .base = T931_SPICC1_BASE,
        .length = 0x44,
    },
};

static const pbus_irq_t spi_irqs[] = {
    {
        .irq = T931_SPICC0_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
    {
        .irq = T931_SPICC1_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
};

static const spi_channel_t spi_channels[] = {
    // Thread SPI
    {
        .bus_id = SHERLOCK_SPICC0,
        .cs = 0, // index into matching chip-select map
        .vid = PDEV_VID_NORDIC,
        .pid = PDEV_PID_NORDIC_NRF52840,
        .did = PDEV_DID_NORDIC_THREAD
    }
};

static const amlspi_cs_map_t spi_cs_map[] = {
    {
        .bus_id = SHERLOCK_SPICC0,
        .cs_count = 1,
        .cs = { 0 } // index into components list
    },
    {
        .bus_id = SHERLOCK_SPICC1,
        .cs_count = 0,
        .cs = {}
    }
};

static const pbus_metadata_t spi_metadata[] = {
    {
        .type = DEVICE_METADATA_SPI_CHANNELS,
        .data_buffer = spi_channels,
        .data_size = sizeof spi_channels,
    },
    {
        .type = DEVICE_METADATA_AMLSPI_CS_MAPPING,
        .data_buffer = &spi_cs_map,
        .data_size = sizeof spi_cs_map
    }
};

static pbus_dev_t spi_dev = []() {
    pbus_dev_t dev = {};
    dev.name = "spi";
    dev.vid = PDEV_VID_AMLOGIC;
    dev.pid = PDEV_PID_GENERIC;
    dev.did = PDEV_DID_AMLOGIC_SPI;
    dev.mmio_list = spi_mmios;
    dev.mmio_count = countof(spi_mmios);
    dev.irq_list = spi_irqs;
    dev.irq_count = countof(spi_irqs);
    dev.metadata_list = spi_metadata;
    dev.metadata_count = countof(spi_metadata);
    return dev;
}();

// composite binding rules
static const zx_bind_inst_t root_match[] = {
    BI_MATCH(),
};
static constexpr zx_bind_inst_t gpio_spicc0_ss0_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_GPIO),
    BI_MATCH_IF(EQ, BIND_GPIO_PIN, GPIO_SPICC0_SS0),
};
static constexpr device_component_part_t gpio_spicc0_ss0_component[] = {
    { fbl::count_of(root_match), root_match },
    { fbl::count_of(gpio_spicc0_ss0_match), gpio_spicc0_ss0_match },
};
static constexpr device_component_t components[] = {
    { fbl::count_of(gpio_spicc0_ss0_component), gpio_spicc0_ss0_component },
};

zx_status_t Sherlock::SpiInit() {
    // setup pinmux for the SPI bus
    // SPI_A
    gpio_impl_.SetAltFunction(T931_GPIOC(0), 5); // MOSI
    gpio_impl_.SetAltFunction(T931_GPIOC(1), 5); // MISO
    gpio_impl_.ConfigOut(GPIO_SPICC0_SS0, 1);    // SS0
    gpio_impl_.SetAltFunction(T931_GPIOC(3), 5); // SCLK

    // TODO(ZX-4230): fix this clock enable block when the clock driver can handle the dividers
    {
        // Please do not use get_root_resource() in new code. See ZX-1467.
        zx::unowned_resource resource(get_root_resource());
        std::optional<ddk::MmioBuffer> buf;
        zx_status_t status = ddk::MmioBuffer::Create(T931_HIU_BASE, T931_HIU_LENGTH, *resource,
                                        ZX_CACHE_POLICY_UNCACHED_DEVICE, &buf);
        if (status != ZX_OK) {
            zxlogf(ERROR, "%s: MmioBuffer::Create failed %d\n", __func__, status);
            return status;
        }

        // SPICC0 clock enable
        buf->Write32(spicc_0_clk_sel_fclk_div2 | spicc_0_clk_en | spicc_0_clk_div(10),
                    HHI_SPICC_CLK_CNTL);
    }

    zx_status_t status = pbus_.CompositeDeviceAdd(&spi_dev, components, fbl::count_of(components),
                                                  UINT32_MAX);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DeviceAdd failed %d\n", __func__, status);
        return status;
    }

    return ZX_OK;
}

} // namespace sherlock
