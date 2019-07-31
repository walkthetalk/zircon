// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "rndishost.h"

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/ethernet.h>
#include <ddk/protocol/usb.h>
#include <usb/usb.h>
#include <usb/usb-request.h>
#include <zircon/hw/usb/cdc.h>
#include <zircon/hw/usb.h>
#include <zircon/listnode.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define READ_REQ_COUNT 8
#define WRITE_REQ_COUNT 4
#define ETH_HEADER_SIZE 4

#define ETHERNET_MAX_TRANSMIT_DELAY 100
#define ETHERNET_MAX_RECV_DELAY 100
#define ETHERNET_TRANSMIT_DELAY 10
#define ETHERNET_RECV_DELAY 10
#define ETHERNET_INITIAL_TRANSMIT_DELAY 0
#define ETHERNET_INITIAL_RECV_DELAY 0

typedef struct {
    zx_device_t* zxdev;
    zx_device_t* usb_zxdev;
    usb_protocol_t usb;

    uint8_t mac_addr[6];
    uint8_t control_intf;
    uint32_t request_id;
    uint32_t mtu;

    uint8_t bulk_in_addr;
    uint8_t bulk_out_addr;

    list_node_t free_read_reqs;
    list_node_t free_write_reqs;

    uint64_t rx_endpoint_delay; // wait time between 2 recv requests
    uint64_t tx_endpoint_delay; // wait time between 2 transmit requests

    // Interface to the ethernet layer.
    ethernet_ifc_protocol_t ifc;

    thrd_t thread;
    bool thread_started;
    size_t parent_req_size;

    mtx_t mutex;
} rndishost_t;

static void dump_buffer(void* buf) {
    uint8_t* p = buf;
    for (int i = 0; i < RNDIS_BUFFER_SIZE; i += 4) {
        if (i != 0 && i % 24 == 0) {
            zxlogf(DEBUG1, "\n");
        }
        zxlogf(DEBUG1, "%08x ", p[i] | p[i + 1] << 8 | p[i + 2] << 16 | p[i + 3] << 24);
    }
    zxlogf(DEBUG1, "\n");
}

static bool command_succeeded(void* buf, uint32_t type, uint32_t length) {
    rndis_header_complete* header = buf;
    if (header->msg_type != type) {
        zxlogf(DEBUG1, "Bad type: Actual: %x, Expected: %x.\n",
               header->msg_type, type);
        return false;
    }
    if (header->msg_length != length) {
        zxlogf(DEBUG1, "Bad length: Actual: %d, Expected: %d.\n",
               header->msg_length, length);
        return false;
    }
    if (header->status != RNDIS_STATUS_SUCCESS) {
        zxlogf(DEBUG1, "Bad status: %x.\n", header->status);
        return false;
    }
    return true;
}

static zx_status_t rndis_command(rndishost_t* eth, void* buf) {
    rndis_header* header = buf;
    uint32_t request_id = eth->request_id++;
    header->request_id = request_id;

    zx_status_t status;
    status = usb_control_out(&eth->usb, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                             USB_CDC_SEND_ENCAPSULATED_COMMAND,
                             0, eth->control_intf, RNDIS_CONTROL_TIMEOUT, buf, header->msg_length);

    if (status < 0) {
        return status;
    }

    status = usb_control_in(&eth->usb, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                            USB_CDC_GET_ENCAPSULATED_RESPONSE,
                            0, eth->control_intf, RNDIS_CONTROL_TIMEOUT,
                            buf, RNDIS_BUFFER_SIZE, NULL);

    if (header->request_id != request_id) {
        return ZX_ERR_IO_DATA_INTEGRITY;
    }

    return status;
}

static void rndishost_recv(rndishost_t* eth, usb_request_t* request) {
    size_t len = request->response.actual;

    uint8_t* read_data;
    zx_status_t status = usb_request_mmap(request, (void*)&read_data);
    if (status != ZX_OK) {
        zxlogf(ERROR, "rndishost receive: usb_request_mmap failed: %d\n", status);
        return;
    }

    while (len > sizeof(rndis_packet_header)) {
        rndis_packet_header* header = (rndis_packet_header*)read_data;

        // The |data_offset| field contains the offset to the payload measured from the start of
        // the field itself.
        size_t data_offset = offsetof(rndis_packet_header, data_offset) + header->data_offset;

        if (header->msg_type != RNDIS_PACKET_MSG || len < header->msg_length ||
            len < data_offset + header->data_length) {
            zxlogf(DEBUG1, "rndis bad packet\n");
            return;
        }

        if (header->data_length == 0) {
            // No more data.
            return;
        }

        ethernet_ifc_recv(&eth->ifc, read_data + data_offset, header->data_length, 0);

        read_data += header->msg_length;
        len -= header->msg_length;
    }
}

static void rndis_read_complete(void* ctx, usb_request_t* request) {
    rndishost_t* eth = (rndishost_t*)ctx;

    if (request->response.status == ZX_ERR_IO_NOT_PRESENT) {
        usb_request_release(request);
        return;
    }

    mtx_lock(&eth->mutex);
    if (request->response.status == ZX_ERR_IO_REFUSED) {
        zxlogf(TRACE, "rndis_read_complete usb_reset_endpoint\n");
        usb_reset_endpoint(&eth->usb, eth->bulk_in_addr);
    } else if (request->response.status == ZX_ERR_IO_INVALID) {
        zxlogf(TRACE, "rndis_read_complete Slowing down the requests by %d usec"
                      " and resetting the recv endpoint\n",
               ETHERNET_RECV_DELAY);
        if (eth->rx_endpoint_delay < ETHERNET_MAX_RECV_DELAY) {
            eth->rx_endpoint_delay += ETHERNET_RECV_DELAY;
        }
        usb_reset_endpoint(&eth->usb, eth->bulk_in_addr);
    }
    if ((request->response.status == ZX_OK) && eth->ifc.ops) {
        rndishost_recv(eth, request);
    } else {
        zxlogf(DEBUG1, "rndis read complete: bad status = %d\n", request->response.status);
    }

    // TODO: Only usb_request_queue if the device is online.
    zx_nanosleep(zx_deadline_after(ZX_USEC(eth->rx_endpoint_delay)));
    usb_request_complete_t complete = {
        .callback = rndis_read_complete,
        .ctx = eth,
    };
    usb_request_queue(&eth->usb, request, &complete);

    mtx_unlock(&eth->mutex);
}

static void rndis_write_complete(void* ctx, usb_request_t* request) {
    rndishost_t* eth = (rndishost_t*)ctx;

    if (request->response.status == ZX_ERR_IO_NOT_PRESENT) {
        zxlogf(ERROR, "rndis_write_complete zx_err_io_not_present\n");
        usb_request_release(request);
        return;
    }

    mtx_lock(&eth->mutex);
    if (request->response.status == ZX_ERR_IO_REFUSED) {
        zxlogf(TRACE, "rndishost usb_reset_endpoint\n");
        usb_reset_endpoint(&eth->usb, eth->bulk_out_addr);
    } else if (request->response.status == ZX_ERR_IO_INVALID) {
        zxlogf(TRACE, "rndis_write_complete Slowing down the requests by %d usec"
                      " and resetting the transmit endpoint\n",
               ETHERNET_TRANSMIT_DELAY);
        if (eth->tx_endpoint_delay < ETHERNET_MAX_TRANSMIT_DELAY) {
            eth->tx_endpoint_delay += ETHERNET_TRANSMIT_DELAY;
        }
        usb_reset_endpoint(&eth->usb, eth->bulk_out_addr);
    }

    zx_status_t status = usb_req_list_add_tail(&eth->free_write_reqs, request,
                                               eth->parent_req_size);
    ZX_DEBUG_ASSERT(status == ZX_OK);
    mtx_unlock(&eth->mutex);
}

static void rndishost_free(rndishost_t* eth) {
    usb_request_t* txn;
    while ((txn = usb_req_list_remove_head(&eth->free_read_reqs, eth->parent_req_size)) != NULL) {
        usb_request_release(txn);
    }
    while ((txn = usb_req_list_remove_head(&eth->free_write_reqs, eth->parent_req_size)) != NULL) {
        usb_request_release(txn);
    }
    free(eth);
}

static zx_status_t rndishost_query(void* ctx, uint32_t options, ethernet_info_t* info) {
    rndishost_t* eth = (rndishost_t*)ctx;

    if (options) {
        return ZX_ERR_INVALID_ARGS;
    }

    memset(info, 0, sizeof(*info));
    info->mtu = eth->mtu;
    memcpy(info->mac, eth->mac_addr, sizeof(eth->mac_addr));
    info->netbuf_size = sizeof(ethernet_netbuf_t);

    return ZX_OK;
}

static void rndishost_stop(void* ctx) {
    rndishost_t* eth = (rndishost_t*)ctx;
    mtx_lock(&eth->mutex);
    eth->ifc.ops = NULL;
    mtx_unlock(&eth->mutex);
}

static zx_status_t rndishost_start(void* ctx, const ethernet_ifc_protocol_t* ifc) {
    rndishost_t* eth = (rndishost_t*)ctx;
    zx_status_t status = ZX_OK;

    mtx_lock(&eth->mutex);
    if (eth->ifc.ops) {
        status = ZX_ERR_ALREADY_BOUND;
    } else {
        eth->ifc = *ifc;
        // TODO: Check that the device is online before sending ETH_STATUS_ONLINE.
        ethernet_ifc_status(&eth->ifc, ETHERNET_STATUS_ONLINE);
    }
    mtx_unlock(&eth->mutex);

    return status;
}

static zx_status_t rndishost_queue_tx(void* ctx, uint32_t options, ethernet_netbuf_t* netbuf) {
    size_t length = netbuf->data_size;
    rndishost_t* eth = (rndishost_t*)ctx;
    const uint8_t* byte_data = netbuf->data_buffer;
    zx_status_t status = ZX_OK;

    mtx_lock(&eth->mutex);

    usb_request_t* req = usb_req_list_remove_head(&eth->free_write_reqs, eth->parent_req_size);
    if (req == NULL) {
        zxlogf(TRACE, "rndishost dropped a packet\n");
        status = ZX_ERR_NO_RESOURCES;
        goto done;
    }

    if (length + sizeof(rndis_packet_header) > RNDIS_MAX_XFER_SIZE) {
        zxlogf(TRACE, "rndishost attempted to send a packet that's too large.\n");
        status = usb_req_list_add_tail(&eth->free_write_reqs, req, eth->parent_req_size);
        ZX_DEBUG_ASSERT(status == ZX_OK);
        status = ZX_ERR_INVALID_ARGS;
        goto done;
    }

    rndis_packet_header header;
    uint8_t* header_data = (uint8_t*)&header;
    memset(header_data, 0, sizeof(rndis_packet_header));
    header.msg_type = RNDIS_PACKET_MSG;
    header.msg_length = sizeof(rndis_packet_header) + length;
    // The offset should be given from the beginning of the data_offset field.
    // So subtract 8 bytes for msg_type and msg_length.
    header.data_offset = sizeof(rndis_packet_header) - 8;
    header.data_length = length;

    usb_request_copy_to(req, header_data, sizeof(rndis_packet_header), 0);
    ssize_t bytes_copied = usb_request_copy_to(req, byte_data, length,
                                               sizeof(rndis_packet_header));
    req->header.length = sizeof(rndis_packet_header) + length;
    if (bytes_copied < 0) {
        printf("rndishost: failed to copy data into send txn (error %zd)\n", bytes_copied);
        status = usb_req_list_add_tail(&eth->free_write_reqs, req, eth->parent_req_size);
        ZX_DEBUG_ASSERT(status == ZX_OK);
        goto done;
    }
    zx_nanosleep(zx_deadline_after(ZX_USEC(eth->tx_endpoint_delay)));
    usb_request_complete_t complete = {
        .callback = rndis_write_complete,
        .ctx = eth,
    };
    usb_request_queue(&eth->usb, req, &complete);

done:
    mtx_unlock(&eth->mutex);
    return status;
}

static void rndishost_unbind(void* ctx) {
    rndishost_t* eth = (rndishost_t*)ctx;
    device_remove(eth->zxdev);
}

static void rndishost_release(void* ctx) {
    rndishost_t* eth = (rndishost_t*)ctx;
    mtx_lock(&eth->mutex);
    bool should_join = eth->thread_started;
    mtx_unlock(&eth->mutex);
    if (should_join) {
        thrd_join(eth->thread, NULL);
    }
    rndishost_free(eth);
}

static zx_status_t rndishost_set_param(void *ctx, uint32_t param, int32_t value, const void* data,
                                       size_t data_size) {
    return ZX_ERR_NOT_SUPPORTED;
}

static ethernet_impl_protocol_ops_t ethernet_impl_ops = {
    .query = rndishost_query,
    .stop = rndishost_stop,
    .start = rndishost_start,
    .queue_tx = rndishost_queue_tx,
    .set_param = rndishost_set_param,
};

static zx_protocol_device_t rndishost_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .unbind = rndishost_unbind,
    .release = rndishost_release,
};

static int rndis_start_thread(void* arg) {
    rndishost_t* eth = (rndishost_t*)arg;
    void* buf = malloc(RNDIS_BUFFER_SIZE);
    memset(buf, 0, RNDIS_BUFFER_SIZE);

    // Send an initialization message to the device.
    rndis_init* init = buf;
    init->msg_type = RNDIS_INITIALIZE_MSG;
    init->msg_length = sizeof(rndis_init);
    init->major_version = RNDIS_MAJOR_VERSION;
    init->minor_version = RNDIS_MINOR_VERSION;
    init->max_xfer_size = RNDIS_MAX_XFER_SIZE;

    zx_status_t status = rndis_command(eth, buf);
    if (status < 0) {
        zxlogf(ERROR, "rndishost bad status on initial message. %d\n", status);
        goto fail;
    }

    rndis_init_complete* init_cmplt = buf;
    if (!command_succeeded(buf, RNDIS_INITIALIZE_CMPLT, sizeof(*init_cmplt))) {
        zxlogf(ERROR, "rndishost initialization failed.\n");
        status = ZX_ERR_IO;
        goto fail;
    }
    eth->mtu = init_cmplt->max_xfer_size;

    // Check the PHY, this is optional and may not be supported by the device.
    uint32_t* phy;
    memset(buf, 0, RNDIS_BUFFER_SIZE);
    rndis_query* query = buf;
    query->msg_type = RNDIS_QUERY_MSG;
    query->msg_length = sizeof(rndis_query) + sizeof(*phy);
    query->oid = OID_GEN_PHYSICAL_MEDIUM;
    query->info_buffer_length = sizeof(*phy);
    query->info_buffer_offset = RNDIS_QUERY_BUFFER_OFFSET;
    status = rndis_command(eth, buf);
    if (status == ZX_OK) {
        // TODO: Do something with this information.
        rndis_query_complete* phy_query_cmplt = buf;
        if (command_succeeded(buf,
                              RNDIS_QUERY_CMPLT,
                              sizeof(*phy_query_cmplt) + phy_query_cmplt->info_buffer_length)) {
            // The offset given in the reply is from the beginning of the request_id
            // field. So, add 8 for the msg_type and msg_length fields.
            phy = buf + 8 + phy_query_cmplt->info_buffer_offset;
        }
    }

    // Query the device for a MAC address.
    memset(buf, 0, RNDIS_BUFFER_SIZE);
    query->msg_type = RNDIS_QUERY_MSG;
    query->msg_length = sizeof(rndis_query) + 48;
    query->oid = OID_802_3_PERMANENT_ADDRESS;
    query->info_buffer_length = 48;
    query->info_buffer_offset = RNDIS_QUERY_BUFFER_OFFSET;
    status = rndis_command(eth, buf);
    if (status < 0) {
        zxlogf(ERROR, "Couldn't get device physical address\n");
        goto fail;
    }

    rndis_query_complete* mac_query_cmplt = buf;
    if (!command_succeeded(buf, RNDIS_QUERY_CMPLT, sizeof(*mac_query_cmplt) +
                                                       mac_query_cmplt->info_buffer_length)) {
        zxlogf(ERROR, "rndishost MAC query failed.\n");
        status = ZX_ERR_IO;
        goto fail;
    }
    uint8_t* mac_addr = buf + 8 + mac_query_cmplt->info_buffer_offset;
    memcpy(eth->mac_addr, mac_addr, sizeof(eth->mac_addr));
    zxlogf(INFO, "rndishost MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
           eth->mac_addr[0], eth->mac_addr[1], eth->mac_addr[2],
           eth->mac_addr[3], eth->mac_addr[4], eth->mac_addr[5]);

    // Enable data transfers
    memset(buf, 0, RNDIS_BUFFER_SIZE);
    rndis_set* set = buf;
    set->msg_type = RNDIS_SET_MSG;
    set->msg_length = sizeof(rndis_set) + 4; // 4 bytes for the filter
    set->oid = OID_GEN_CURRENT_PACKET_FILTER;
    set->info_buffer_length = 4;
    // Offset should begin at oid, so subtract 8 bytes for msg_type and msg_length.
    set->info_buffer_offset = sizeof(rndis_set) - 8;
    uint8_t* filter = buf + sizeof(rndis_set);
    *filter = RNDIS_PACKET_TYPE_DIRECTED |
              RNDIS_PACKET_TYPE_BROADCAST |
              RNDIS_PACKET_TYPE_ALL_MULTICAST |
              RNDIS_PACKET_TYPE_PROMISCUOUS;
    status = rndis_command(eth, buf);
    if (status < 0) {
        zxlogf(ERROR, "Couldn't set the packet filter.\n");
        goto fail;
    }

    if (!command_succeeded(buf, RNDIS_SET_CMPLT, sizeof(rndis_set_complete))) {
        zxlogf(ERROR, "rndishost set filter failed.\n");
        status = ZX_ERR_IO;
        goto fail;
    }

    // Queue read requests
    mtx_lock(&eth->mutex);
    usb_request_t* txn;
    usb_request_complete_t complete = {
        .callback = rndis_read_complete,
        .ctx = eth,
    };
    while ((txn = usb_req_list_remove_head(&eth->free_read_reqs, eth->parent_req_size)) != NULL) {
        usb_request_queue(&eth->usb, txn, &complete);
    }
    mtx_unlock(&eth->mutex);

    free(buf);
    device_make_visible(eth->zxdev);
    return ZX_OK;

fail:
    free(buf);
    device_remove(eth->zxdev);
    return status;
}

static zx_status_t rndishost_bind(void* ctx, zx_device_t* device) {
    usb_protocol_t usb;
    zx_status_t status = device_get_protocol(device, ZX_PROTOCOL_USB, &usb);
    if (status != ZX_OK) {
        return status;
    }

    // Find our endpoints.
    usb_desc_iter_t iter;
    status = usb_desc_iter_init(&usb, &iter);
    if (status < 0) {
        return status;
    }

    // We should have two interfaces: the CDC classified interface the bulk in
    // and out endpoints, and the RNDIS interface for control. The RNDIS
    // interface will be classified as USB_CLASS_WIRELESS when the device is
    // used for tethering.
    // TODO: Figure out how to handle other RNDIS use cases.
    usb_interface_descriptor_t* intf = usb_desc_iter_next_interface(&iter, false);
    uint8_t bulk_in_addr = 0;
    uint8_t bulk_out_addr = 0;
    uint8_t intr_addr = 0;
    uint8_t control_intf = 0;
    while (intf) {
        if (intf->bInterfaceClass == USB_CLASS_WIRELESS) {
            control_intf = intf->bInterfaceNumber;
            if (intf->bNumEndpoints != 1) {
                usb_desc_iter_release(&iter);
                return ZX_ERR_NOT_SUPPORTED;
            }
            usb_endpoint_descriptor_t* endp = usb_desc_iter_next_endpoint(&iter);
            while (endp) {
                if (usb_ep_direction(endp) == USB_ENDPOINT_IN &&
                    usb_ep_type(endp) == USB_ENDPOINT_INTERRUPT) {
                    intr_addr = endp->bEndpointAddress;
                }
                endp = usb_desc_iter_next_endpoint(&iter);
            }
        } else if (intf->bInterfaceClass == USB_CLASS_CDC) {
            if (intf->bNumEndpoints != 2) {
                usb_desc_iter_release(&iter);
                return ZX_ERR_NOT_SUPPORTED;
            }
            usb_endpoint_descriptor_t* endp = usb_desc_iter_next_endpoint(&iter);
            while (endp) {
                if (usb_ep_direction(endp) == USB_ENDPOINT_OUT) {
                    if (usb_ep_type(endp) == USB_ENDPOINT_BULK) {
                        bulk_out_addr = endp->bEndpointAddress;
                    }
                } else if (usb_ep_direction(endp) == USB_ENDPOINT_IN) {
                    if (usb_ep_type(endp) == USB_ENDPOINT_BULK) {
                        bulk_in_addr = endp->bEndpointAddress;
                    }
                }
                endp = usb_desc_iter_next_endpoint(&iter);
            }
        } else {
            usb_desc_iter_release(&iter);
            return ZX_ERR_NOT_SUPPORTED;
        }
        intf = usb_desc_iter_next_interface(&iter, false);
    }
    usb_desc_iter_release(&iter);

    if (!bulk_in_addr || !bulk_out_addr || !intr_addr) {
        zxlogf(ERROR, "rndishost couldn't find endpoints\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    rndishost_t* eth = calloc(1, sizeof(rndishost_t));
    if (!eth) {
        return ZX_ERR_NO_MEMORY;
    }

    list_initialize(&eth->free_read_reqs);
    list_initialize(&eth->free_write_reqs);

    mtx_init(&eth->mutex, mtx_plain);

    eth->usb_zxdev = device;
    eth->control_intf = control_intf;
    eth->bulk_in_addr = bulk_in_addr;
    eth->bulk_out_addr = bulk_out_addr;
    eth->ifc.ops = NULL;
    memcpy(&eth->usb, &usb, sizeof(eth->usb));

    eth->parent_req_size = usb_get_request_size(&eth->usb);
    uint64_t req_size = eth->parent_req_size + sizeof(usb_req_internal_t);

    for (int i = 0; i < READ_REQ_COUNT; i++) {
        usb_request_t* req;
        zx_status_t alloc_result = usb_request_alloc(&req, RNDIS_BUFFER_SIZE, bulk_in_addr,
                                                     req_size);
        if (alloc_result != ZX_OK) {
            status = alloc_result;
            goto fail;
        }
        status = usb_req_list_add_head(&eth->free_read_reqs, req, eth->parent_req_size);
        ZX_DEBUG_ASSERT(status == ZX_OK);
    }
    for (int i = 0; i < WRITE_REQ_COUNT; i++) {
        usb_request_t* req;
        // TODO: Allocate based on mtu.
        zx_status_t alloc_result = usb_request_alloc(&req, RNDIS_BUFFER_SIZE, bulk_out_addr,
                                                     req_size);
        if (alloc_result != ZX_OK) {
            status = alloc_result;
            goto fail;
        }
        status = usb_req_list_add_head(&eth->free_write_reqs, req, eth->parent_req_size);
        ZX_DEBUG_ASSERT(status == ZX_OK);
    }

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "rndishost",
        .ctx = eth,
        .ops = &rndishost_device_proto,
        .proto_id = ZX_PROTOCOL_ETHERNET_IMPL,
        .proto_ops = &ethernet_impl_ops,
        .flags = DEVICE_ADD_INVISIBLE,
    };

    mtx_lock(&eth->mutex);
    status = device_add(eth->usb_zxdev, &args, &eth->zxdev);
    if (status < 0) {
        mtx_unlock(&eth->mutex);
        zxlogf(ERROR, "rndishost: failed to create device: %d\n", status);
        goto fail;
    }

    eth->thread_started = true;
    int ret = thrd_create_with_name(&eth->thread, rndis_start_thread,
                                    eth, "rndishost_start_thread");
    if (ret != thrd_success) {
        eth->thread_started = false;
        mtx_unlock(&eth->mutex);
        device_remove(eth->zxdev);
        return ZX_ERR_NO_RESOURCES;
    }

    mtx_unlock(&eth->mutex);
    return ZX_OK;

fail:
    zxlogf(ERROR, "rndishost_bind failed: %d\n", status);
    rndishost_free(eth);
    return status;
}

static zx_driver_ops_t rndis_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = rndishost_bind,
};

// TODO: Make sure we can bind to all RNDIS use cases. USB_CLASS_WIRELESS only
// covers the tethered device case.
// clang-format off
ZIRCON_DRIVER_BEGIN(rndishost, rndis_driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_USB),
    BI_ABORT_IF(NE, BIND_USB_CLASS, USB_CLASS_WIRELESS),
    BI_ABORT_IF(NE, BIND_USB_SUBCLASS, RNDIS_SUBCLASS),
    BI_MATCH_IF(EQ, BIND_USB_PROTOCOL, RNDIS_PROTOCOL),
ZIRCON_DRIVER_END(rndishost)
