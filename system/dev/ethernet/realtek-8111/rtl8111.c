// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/io-buffer.h>
#include <ddk/mmio-buffer.h>
#include <ddk/protocol/ethernet.h>
#include <ddk/protocol/pci.h>
#include <lib/device-protocol/pci.h>
#include <hw/reg.h>

#include <zircon/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "rtl8111.h"

#define WRITE32(a, v)  writel(v, edev->mmio.vaddr + (a))
#define READ32(a)      readl(edev->mmio.vaddr + (a))
#define WRITE16(a, v)  writew(v, edev->mmio.vaddr + (a))
#define READ16(a)      readw(edev->mmio.vaddr + (a))
#define WRITE8(a, v)   writeb(v, edev->mmio.vaddr + (a))
#define READ8(a)       readb(edev->mmio.vaddr + (a))

#define HI32(val) (((val) >> 32) & 0xffffffff)
#define LO32(val) ((val) & 0xffffffff)

typedef struct eth_desc {
    uint32_t status1;
    uint32_t status2;
    uint64_t data_addr;
} eth_desc_t;

typedef struct ethernet_device {
    zx_device_t* zxdev;
    mtx_t lock;
    mtx_t tx_lock;
    cnd_t tx_cond;
    pci_protocol_t pci;
    zx_handle_t irqh;
    mmio_buffer_t mmio;
    thrd_t irq_thread;
    zx_handle_t btih;
    io_buffer_t buffer;

    eth_desc_t* txd_ring;
    uint64_t txd_phys_addr;
    int txd_idx;
    void* txb;

    eth_desc_t* rxd_ring;
    uint64_t rxd_phys_addr;
    int rxd_idx;
    void* rxb;

    uint8_t mac[6];
    bool online;

    ethernet_ifc_protocol_t ifc;
} ethernet_device_t;

static void rtl8111_init_buffers(ethernet_device_t* edev) {
    zxlogf(TRACE, "rtl8111: Initializing buffers\n");
    edev->txd_ring = io_buffer_virt(&edev->buffer);
    edev->txd_phys_addr = io_buffer_phys(&edev->buffer);
    edev->txd_idx = 0;
    edev->txb = io_buffer_virt(&edev->buffer) + (2 * ETH_DESC_RING_SIZE);

    edev->rxd_ring = io_buffer_virt(&edev->buffer) + ETH_DESC_RING_SIZE;
    edev->rxd_phys_addr = io_buffer_phys(&edev->buffer) + ETH_DESC_RING_SIZE;
    edev->rxd_idx = 0;
    edev->rxb = edev->txb + (ETH_BUF_SIZE * ETH_BUF_COUNT);

    uint64_t txb_phys = io_buffer_phys(&edev->buffer) + (2 * ETH_DESC_RING_SIZE);
    uint64_t rxb_phys = txb_phys + (ETH_BUF_COUNT * ETH_BUF_SIZE);
    for (int i = 0; i < ETH_BUF_COUNT; i++) {
        bool is_end = i == (ETH_BUF_COUNT - 1);
        edev->rxd_ring[i].status1 = RX_DESC_OWN | (is_end ? RX_DESC_EOR : 0) | ETH_BUF_SIZE;
        edev->rxd_ring[i].status2 = 0;
        edev->rxd_ring[i].data_addr = rxb_phys;

        edev->txd_ring[i].status1 = 0;
        edev->txd_ring[i].status2 = 0;
        edev->txd_ring[i].data_addr = txb_phys;

        rxb_phys += ETH_BUF_SIZE;
        txb_phys += ETH_BUF_SIZE;
    }
}

static void rtl8111_init_regs(ethernet_device_t* edev) {
    zxlogf(TRACE, "rtl8111: Initializing registers\n");

    // C+CR needs to be configured first - enable rx VLAN detagging and checksum offload
    WRITE16(RTL_CPLUSCR, READ16(RTL_CPLUSCR) | RTL_CPLUSCR_RXVLAN | RTL_CPLUSCR_RXCHKSUM);

    // Reset the controller and wait for the operation to finish
    WRITE8(RTL_CR, READ8(RTL_CR) | RTL_CR_RST);
    while (READ8(RTL_CR) & RTL_CR_RST) {
        zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
    }

    // Unlock the configuration registers
    WRITE8(RTL_9436CR, (READ8(RTL_9436CR) & RTL_9436CR_EEM_MASK) | RTL_9436CR_EEM_UNLOCK);

    // Set the tx and rx maximum packet size
    WRITE8(RTL_MTPS, (READ8(RTL_MTPS) & RTL_MTPS_MTPS_MASK) | ROUNDUP(ETH_BUF_SIZE, 128) / 128);
    WRITE16(RTL_RMS, (READ16(RTL_RMS) & RTL_RMS_RMS_MASK) | ETH_BUF_SIZE);

    // Set the rx/tx descriptor ring addresses
    WRITE32(RTL_RDSAR_LOW, LO32(edev->rxd_phys_addr));
    WRITE32(RTL_RDSAR_HIGH, HI32(edev->rxd_phys_addr));
    WRITE32(RTL_TNPDS_LOW, LO32(edev->txd_phys_addr));
    WRITE32(RTL_TNPDS_HIGH, HI32(edev->txd_phys_addr));

    // Set the interframe gap and max DMA burst size in the tx config register
    uint32_t tcr = READ32(RTL_TCR) & ~(RTL_TCR_IFG_MASK | RTL_TCR_MXDMA_MASK);
    WRITE32(RTL_TCR, tcr | RTL_TCR_IFG96 | RTL_TCR_MXDMA_UNLIMITED);

    // Disable interrupts except link change and rx-ok and then clear all interrupts
    WRITE16(RTL_IMR, (READ16(RTL_IMR) & ~RTL_INT_MASK) | RTL_INT_LINKCHG | RTL_INT_ROK);
    WRITE16(RTL_ISR, 0xffff);

    // Lock the configuration registers and enable rx/tx
    WRITE8(RTL_9436CR, (READ8(RTL_9436CR) & RTL_9436CR_EEM_MASK) | RTL_9436CR_EEM_LOCK);
    WRITE8(RTL_CR, READ8(RTL_CR) | RTL_CR_RE | RTL_CR_TE);

    // Configure the max dma burst, what types of packets we accept, and the multicast filter
    uint32_t rcr = READ32(RTL_RCR) & ~(RTL_RCR_MXDMA_MASK | RTL_RCR_ACCEPT_MASK);
    WRITE32(RTL_RCR, rcr | RTL_RCR_MXDMA_UNLIMITED | RTL_RCR_AB | RTL_RCR_AM | RTL_RCR_APM);
    WRITE32(RTL_MAR7, 0xffffffff); // Accept all multicasts
    WRITE32(RTL_MAR3, 0xffffffff);

    // Read the MAC and link status
    uint32_t n = READ32(RTL_MAC0);
    memcpy(edev->mac, &n, 4);
    n = READ32(RTL_MAC1);
    memcpy(edev->mac + 4, &n, 2);

    edev->online = READ8(RTL_PHYSTATUS) & RTL_PHYSTATUS_LINKSTS;

    zxlogf(INFO, "rtl111: mac address=%02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
            edev->mac[0], edev->mac[1], edev->mac[2],
            edev->mac[3], edev->mac[4], edev->mac[5],
            edev->online ? "online" : "offline");
}

static int irq_thread(void* arg) {
    ethernet_device_t* edev = arg;
    while (1) {
        zx_status_t r;
        r = zx_interrupt_wait(edev->irqh, NULL);
        if (r != ZX_OK) {
            zxlogf(TRACE, "rtl8111: irq wait failed: %d\n", r);
            break;
        }

        mtx_lock(&edev->lock);

        uint16_t isr = READ16(RTL_ISR);
        if (isr & RTL_INT_LINKCHG) {
            bool was_online = edev->online;
            bool online = READ8(RTL_PHYSTATUS) & RTL_PHYSTATUS_LINKSTS;
            if (online != was_online) {
                zxlogf(INFO, "rtl8111: link %s\n", online ? "online" : "offline");
                edev->online = online;
                if (edev->ifc.ops) {
                    ethernet_ifc_status(&edev->ifc, online ? ETHERNET_STATUS_ONLINE : 0);
                }
            }
        }
        if (isr & RTL_INT_TOK) {
            cnd_signal(&edev->tx_cond);
        }
        if (isr & RTL_INT_ROK) {
            eth_desc_t* rxd;
            while (!((rxd = edev->rxd_ring + edev->rxd_idx)->status1 & RX_DESC_OWN)) {
                if (edev->ifc.ops) {
                    size_t len = rxd->status1 & RX_DESC_LEN_MASK;
                    ethernet_ifc_recv(
                        &edev->ifc, edev->rxb + (edev->rxd_idx * ETH_BUF_SIZE), len, 0);
                } else {
                    zxlogf(ERROR, "rtl8111: No ethmac callback, dropping packet\n");
                }

                bool is_end = edev->rxd_idx == (ETH_BUF_COUNT - 1);
                rxd->status1 = RX_DESC_OWN | (is_end ? RX_DESC_EOR : 0) | ETH_BUF_SIZE;

                edev->rxd_idx = (edev->rxd_idx + 1) % ETH_BUF_COUNT;
            }
        }

        WRITE16(RTL_ISR, 0xffff);

        mtx_unlock(&edev->lock);
    }
    return 0;
}

static zx_status_t rtl8111_query(void* ctx, uint32_t options, ethernet_info_t* info) {
    ethernet_device_t* edev = ctx;

    if (options) {
        return ZX_ERR_INVALID_ARGS;
    }

    memset(info, 0, sizeof(*info));
    info->mtu = ETH_BUF_SIZE;
    memcpy(info->mac, edev->mac, sizeof(edev->mac));
    info->netbuf_size = sizeof(ethernet_netbuf_t);

    return ZX_OK;
}

static void rtl8111_stop(void* ctx) {
    ethernet_device_t* edev = ctx;
    mtx_lock(&edev->lock);
    edev->ifc.ops = NULL;
    mtx_unlock(&edev->lock);
}

static zx_status_t rtl8111_start(void* ctx, const ethernet_ifc_protocol_t* ifc) {
    ethernet_device_t* edev = ctx;
    zx_status_t status = ZX_OK;

    mtx_lock(&edev->lock);
    if (edev->ifc.ops) {
        status = ZX_ERR_BAD_STATE;
    } else {
        edev->ifc = *ifc;
        ethernet_ifc_status(&edev->ifc, edev->online ? ETHERNET_STATUS_ONLINE : 0);
    }
    mtx_unlock(&edev->lock);

    return status;
}

static zx_status_t rtl8111_queue_tx(void* ctx, uint32_t options, ethernet_netbuf_t* netbuf) {
    size_t length = netbuf->data_size;
    if (length > ETH_BUF_SIZE) {
        zxlogf(ERROR, "rtl8111: Unsupported packet length %zu\n", length);
        return ZX_ERR_INVALID_ARGS;
    }
    ethernet_device_t* edev = ctx;

    mtx_lock(&edev->tx_lock);

    if (edev->txd_ring[edev->txd_idx].status1 & TX_DESC_OWN) {
        mtx_lock(&edev->lock);
        WRITE16(RTL_IMR, READ16(RTL_IMR) | RTL_INT_TOK);
        WRITE16(RTL_ISR, RTL_INT_TOK);

        while (edev->txd_ring[edev->txd_idx].status1 & TX_DESC_OWN) {
            zxlogf(TRACE, "rtl8111: Waiting for buffer\n");
            cnd_wait(&edev->tx_cond, &edev->lock);
        }

        WRITE16(RTL_IMR, READ16(RTL_IMR) & ~RTL_INT_TOK);
        mtx_unlock(&edev->lock);
    }

    memcpy(edev->txb + (edev->txd_idx * ETH_BUF_SIZE), netbuf->data_buffer, length);

    bool is_end = edev->txd_idx == (ETH_BUF_COUNT - 1);
    edev->txd_ring[edev->txd_idx].status1 =
        (is_end ? TX_DESC_EOR : 0) | length | TX_DESC_OWN | TX_DESC_FS | TX_DESC_LS;

    WRITE8(RTL_TPPOLL, READ8(RTL_TPPOLL) | RTL_TPPOLL_NPQ);

    edev->txd_idx = (edev->txd_idx + 1) % ETH_BUF_COUNT;

    mtx_unlock(&edev->tx_lock);
    return ZX_OK;
}

static zx_status_t rtl8111_set_promisc(ethernet_device_t* edev, bool on) {
    if (on) {
        WRITE16(RTL_RCR, READ16(RTL_RCR | RTL_RCR_AAP));
    } else {
        WRITE16(RTL_RCR, READ16(RTL_RCR & ~RTL_RCR_AAP));
    }

    return ZX_OK;
}

static zx_status_t rtl8111_set_param(void *ctx, uint32_t param, int32_t value, const void* data,
                                     size_t data_size) {
    ethernet_device_t* edev = ctx;
    zx_status_t status = ZX_OK;

    mtx_lock(&edev->lock);

    switch (param) {
    case ETHERNET_SETPARAM_PROMISC:
        status = rtl8111_set_promisc(edev, (bool)value);
        break;
    default:
        status = ZX_ERR_NOT_SUPPORTED;
    }
    mtx_unlock(&edev->lock);

    return status;
}

static ethernet_impl_protocol_ops_t ethernet_impl_ops = {
    .query = rtl8111_query,
    .stop = rtl8111_stop,
    .start = rtl8111_start,
    .queue_tx = rtl8111_queue_tx,
    .set_param = rtl8111_set_param,
};

static void rtl8111_release(void* ctx) {
    ethernet_device_t* edev = ctx;

    WRITE8(RTL_CR, READ8(RTL_CR) | RTL_CR_RST);
    pci_enable_bus_master(&edev->pci, false);

    zx_handle_close(edev->irqh);
    thrd_join(edev->irq_thread, NULL);

    mmio_buffer_release(&edev->mmio);

    io_buffer_release(&edev->buffer);
    zx_handle_close(edev->btih);

    free(edev);
}

static zx_protocol_device_t device_ops = {
    .version = DEVICE_OPS_VERSION,
    .release = rtl8111_release,
};

static zx_status_t rtl8111_bind(void* ctx, zx_device_t* dev) {
    zxlogf(TRACE, "rtl8111: binding device\n");

    zx_status_t r;
    ethernet_device_t* edev;
    if ((edev = calloc(1, sizeof(ethernet_device_t))) == NULL) {
        return ZX_ERR_NO_MEMORY;
    }
    mtx_init(&edev->lock, mtx_plain);
    mtx_init(&edev->tx_lock, mtx_plain);
    cnd_init(&edev->tx_cond);

    if ((r = device_get_protocol(dev, ZX_PROTOCOL_PCI, &edev->pci)) != ZX_OK) {
        zxlogf(ERROR, "rtl8111: no pci protocol\n");
        goto fail;
    }

    uint32_t irq_cnt = 0;
    if ((pci_query_irq_mode(&edev->pci, ZX_PCIE_IRQ_MODE_MSI, &irq_cnt) == ZX_OK) &&
        (pci_set_irq_mode(&edev->pci, ZX_PCIE_IRQ_MODE_MSI, 1) == ZX_OK)) {
        zxlogf(TRACE, "rtl8111: using MSI mode\n");
    } else if ((pci_query_irq_mode(&edev->pci, ZX_PCIE_IRQ_MODE_LEGACY, &irq_cnt) == ZX_OK) &&
               (pci_set_irq_mode(&edev->pci, ZX_PCIE_IRQ_MODE_LEGACY, 1) == ZX_OK)) {
        zxlogf(TRACE, "rtl8111: using legacy irq mode\n");
    } else {
        zxlogf(ERROR, "rtl8111: failed to configure irqs\n");
        r = ZX_ERR_INTERNAL;
        goto fail;
    }

    r = pci_map_interrupt(&edev->pci, 0, &edev->irqh);
    if (r != ZX_OK) {
        zxlogf(ERROR, "rtl8111: failed to map irq %d\n", r);
        goto fail;
    }

    r = pci_map_bar_buffer(&edev->pci, 2u, ZX_CACHE_POLICY_UNCACHED_DEVICE, &edev->mmio);
    if (r != ZX_OK) {
        zxlogf(ERROR, "rtl8111: cannot map io %d\n", r);
        goto fail;
    }

    if ((r = pci_enable_bus_master(&edev->pci, true)) != ZX_OK) {
        zxlogf(ERROR, "rtl8111: cannot enable bus master %d\n", r);
        goto fail;
    }

    if ((r = pci_get_bti(&edev->pci, 0, &edev->btih)) != ZX_OK) {
        zxlogf(ERROR, "rtl8111: could not get bti %d\n", r);
        goto fail;
    }

    uint32_t mac_version = READ32(RTL_TCR) & 0x7cf00000;
    zxlogf(TRACE, "rtl8111: version 0x%08x\n", mac_version);

    // TODO(stevensd): Don't require a contiguous buffer
    uint32_t alloc_size = ((ETH_BUF_SIZE + ETH_DESC_ELT_SIZE) * ETH_BUF_COUNT) * 2;
    r = io_buffer_init(&edev->buffer, edev->btih, alloc_size, IO_BUFFER_RW | IO_BUFFER_CONTIG);
    if (r != ZX_OK) {
        zxlogf(ERROR, "rtl8111: cannot alloc io-buffer %d\n", r);
        goto fail;
    }

    rtl8111_init_buffers(edev);
    rtl8111_init_regs(edev);

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "rtl8111",
        .ctx = edev,
        .ops = &device_ops,
        .proto_id = ZX_PROTOCOL_ETHERNET_IMPL,
        .proto_ops = &ethernet_impl_ops,
    };

    if ((r = device_add(dev, &args, &edev->zxdev)) != ZX_OK) {
        zxlogf(ERROR, "rtl8111: failed to add device %d\n", r);
        goto fail;
    }

    r = thrd_create_with_name(&edev->irq_thread, irq_thread, edev, "rtl-irq-thread");
    if (r < 0) {
        zxlogf(ERROR, "rtl8111: failed to create irq thread %d\n", r);
        device_remove(edev->zxdev);
        return ZX_OK; // The cleanup will be done in release
    }

    zxlogf(TRACE, "rtl8111: bind successful\n");

    return ZX_OK;

fail:
    io_buffer_release(&edev->buffer);
    mmio_buffer_release(&edev->mmio);
    if (edev->btih) {
        zx_handle_close(edev->btih);
    }
    if (edev->irqh) {
        zx_handle_close(edev->irqh);
    }
    free(edev);
    return r != ZX_OK ? r : ZX_ERR_INTERNAL;
}

static zx_driver_ops_t rtl8111_ethernet_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = rtl8111_bind,
};

// clang-format off
ZIRCON_DRIVER_BEGIN(realtek_rtl8111, rtl8111_ethernet_driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PCI),
    BI_ABORT_IF(NE, BIND_PCI_VID, REALTEK_VID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, RTL8111_DID),
ZIRCON_DRIVER_END(realtek_rtl8111)
