// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/bt/hci.h>
#include <ddk/protocol/serial.h>
#include <endian.h>
#include <fuchsia/hardware/bluetooth/c/fidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <threads.h>
#include <zircon/assert.h>
#include <zircon/status.h>
#include <zircon/threads.h>

// TODO: how can we parameterize this?
#define TARGET_BAUD_RATE    2000000

#define MAC_ADDR_LEN 6

// TODO: Determine firmware name based on controller version.
#define FIRMWARE_PATH "BCM4345C5.hcd"

#define FIRMWARE_DOWNLOAD_DELAY ZX_MSEC(50)

// Hardcoded. Better to parameterize on chipset.
// Broadcom chips need a few hundred msec delay
// after firmware load
#define BAUD_RATE_SWITCH_DELAY ZX_MSEC(200)

typedef struct {
    uint16_t opcode;
    uint8_t parameter_total_size;
} __PACKED hci_command_header_t;

typedef struct {
    uint8_t event_code;
    uint8_t parameter_total_size;
} __PACKED hci_event_header_t;

typedef struct {
    hci_event_header_t header;
    uint8_t num_hci_command_packets;
    uint16_t command_opcode;
    uint8_t return_code;
} __PACKED hci_command_complete_t;

typedef struct {
    hci_event_header_t header;
    uint8_t num_hci_command_packets;
    uint16_t command_opcode;
    uint8_t return_code;
    uint8_t bdaddr[MAC_ADDR_LEN];
} __PACKED hci_read_bdaddr_command_complete_t;

// HCI reset command
const hci_command_header_t RESET_CMD = {
    .opcode =  0x0c03,
    .parameter_total_size = 0,
};

// vendor command to begin firmware download
const hci_command_header_t START_FIRMWARE_DOWNLOAD_CMD = {
    .opcode =  0xfc2e,
    .parameter_total_size = 0,
};

// HCI command to read BDADDR from controller
const hci_command_header_t READ_BDADDR_CMD = {
    .opcode =  0x1009,
    .parameter_total_size = 0,
};

typedef struct {
    hci_command_header_t header;
    uint16_t unused;
    uint32_t baud_rate;
} __PACKED bcm_set_baud_rate_cmd_t;
#define BCM_SET_BAUD_RATE_CMD   0xfc18

typedef struct {
    hci_command_header_t header;
    uint8_t bdaddr[MAC_ADDR_LEN];
} __PACKED bcm_set_bdaddr_cmd_t;
#define BCM_SET_BDADDR_CMD   0xfc01

#define HCI_EVT_COMMAND_COMPLETE    0x0e

typedef struct {
    zx_device_t* zxdev;
    zx_device_t* transport_dev;
    bt_hci_protocol_t hci;
    serial_protocol_t serial;
    zx_handle_t command_channel;
    bool is_uart;   // true if underlying transport is UART
} bcm_hci_t;

static zx_status_t bcm_hci_get_protocol(void* ctx, uint32_t proto_id, void* out_proto) {
    if (proto_id != ZX_PROTOCOL_BT_HCI) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    bcm_hci_t* hci = ctx;
    bt_hci_protocol_t* hci_proto = out_proto;

    // Forward the underlying bt-transport ops.
    hci_proto->ops = hci->hci.ops;
    hci_proto->ctx = hci->hci.ctx;

    return ZX_OK;
}

static void bcm_hci_unbind(void* ctx) {
    bcm_hci_t* hci = ctx;

    device_remove(hci->zxdev);
}

static void bcm_hci_release(void* ctx) {
    bcm_hci_t* hci = ctx;

    if (hci->command_channel != ZX_HANDLE_INVALID) {
        zx_handle_close(hci->command_channel);
    }

    free(hci);
}

zx_status_t fidl_bt_hci_open_command_channel(void* ctx, zx_handle_t channel) {
    bcm_hci_t* hci = ctx;
    zx_status_t status = bt_hci_open_command_channel(&hci->hci, channel);
    if (status != ZX_OK) {
        zx_handle_close(channel);
    }
    return status;
}

zx_status_t fidl_bt_hci_open_acl_data_channel(void* ctx, zx_handle_t channel) {
    bcm_hci_t* hci = ctx;
    zx_status_t status = bt_hci_open_acl_data_channel(&hci->hci, channel);
    if (status != ZX_OK) {
        zx_handle_close(channel);
    }
    return status;
}

zx_status_t fidl_bt_hci_open_snoop_channel(void* ctx, zx_handle_t channel) {
    bcm_hci_t* hci = ctx;
    zx_status_t status = bt_hci_open_snoop_channel(&hci->hci, channel);
    if (status != ZX_OK) {
        zx_handle_close(channel);
    }
    return status;
}

static fuchsia_hardware_bluetooth_Hci_ops_t fidl_ops = {
    .OpenCommandChannel = fidl_bt_hci_open_command_channel,
    .OpenAclDataChannel = fidl_bt_hci_open_acl_data_channel,
    .OpenSnoopChannel = fidl_bt_hci_open_snoop_channel,
};

static zx_status_t fuchsia_bt_hci_message_instance(void* ctx, fidl_msg_t* msg, fidl_txn_t* txn) {
    return fuchsia_hardware_bluetooth_Hci_dispatch(ctx, txn, msg, &fidl_ops);
}

static zx_protocol_device_t bcm_hci_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .get_protocol = bcm_hci_get_protocol,
    .message = fuchsia_bt_hci_message_instance,
    .unbind = bcm_hci_unbind,
    .release = bcm_hci_release,
};


static zx_status_t bcm_hci_send_command(bcm_hci_t* hci, const hci_command_header_t* command,
                                        size_t length, void* out_buf, size_t out_buf_len) {
    #define CHAN_READ_BUF_LEN 257
    uint8_t read_buf[CHAN_READ_BUF_LEN];
    if (out_buf_len > CHAN_READ_BUF_LEN) {
        zxlogf(ERROR, "bcm_hci_send_command provided |out_buf| is too large");
        return ZX_ERR_INVALID_ARGS;
    }

    // send HCI command
    zx_status_t status = zx_channel_write(hci->command_channel, 0, command, length, NULL, 0);
    if (status != ZX_OK) {
        zxlogf(ERROR, "bcm_hci_send_command zx_channel_write failed %s\n",
               zx_status_get_string(status));
        return status;
    }

    // wait for an HCI Command Complete event
    uint32_t actual;

    do {
        status = zx_channel_read(hci->command_channel, 0, read_buf, NULL, CHAN_READ_BUF_LEN, 0,
                                 &actual, NULL);
        if (status == ZX_ERR_SHOULD_WAIT) {
            zx_object_wait_one(hci->command_channel, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                               zx_deadline_after(ZX_SEC(5)), NULL);
        }
    } while (status == ZX_ERR_SHOULD_WAIT);

    if (status != ZX_OK) {
        zxlogf(ERROR, "bcm_hci_send_command zx_channel_read failed %s\n",
               zx_status_get_string(status));
        return status;
    }

    hci_event_header_t* header = (hci_event_header_t *)read_buf;
    if (header->event_code != HCI_EVT_COMMAND_COMPLETE || header->parameter_total_size
            != sizeof(hci_command_complete_t)  - sizeof(hci_event_header_t)) {
        zxlogf(ERROR, "bcm_hci_send_command did not receive command complete\n");
        return ZX_ERR_INTERNAL;
    }
    hci_command_complete_t* event = (hci_command_complete_t *)read_buf;
    if (event->return_code != 0) {
        zxlogf(ERROR, "bcm_hci_send_command got command complete error %u\n", event->return_code);
        return ZX_ERR_INTERNAL;
    }

    if (out_buf) {
      memcpy(out_buf, read_buf, out_buf_len);
    }

    return ZX_OK;
}

static zx_status_t bcm_hci_set_baud_rate(bcm_hci_t* hci, uint32_t baud_rate) {
    bcm_set_baud_rate_cmd_t command = {
        .header = {
            .opcode =  BCM_SET_BAUD_RATE_CMD,
            .parameter_total_size = sizeof(bcm_set_baud_rate_cmd_t) - sizeof(hci_command_header_t),
        },
        .unused = 0,
        .baud_rate = htole32(baud_rate),
    };

    zx_status_t status = bcm_hci_send_command(hci, &command.header, sizeof(command), NULL, 0);
    if (status != ZX_OK) {
        return status;
    }

    return serial_config(&hci->serial, TARGET_BAUD_RATE, SERIAL_SET_BAUD_RATE_ONLY);
}

static zx_status_t bcm_hci_set_bdaddr(bcm_hci_t* hci, uint8_t bdaddr[MAC_ADDR_LEN]) {
    bcm_set_bdaddr_cmd_t command = {
        .header = {
            .opcode =  BCM_SET_BDADDR_CMD,
            .parameter_total_size = sizeof(bcm_set_bdaddr_cmd_t) - sizeof(hci_command_header_t),
        },
        .bdaddr = {
            // HCI expects little endian. Swap bytes
            bdaddr[5], bdaddr[4], bdaddr[3], bdaddr[2], bdaddr[1], bdaddr[0]
        },
    };

    return bcm_hci_send_command(hci, &command.header, sizeof(command), NULL, 0);
}

static zx_status_t bcm_get_bdaddr_from_bootloader(bcm_hci_t* hci, uint8_t macaddr[MAC_ADDR_LEN]) {
    uint8_t bootloader_macaddr[8];
    size_t actual_len;
    zx_status_t status = device_get_metadata(hci->zxdev, DEVICE_METADATA_MAC_ADDRESS,
        bootloader_macaddr, sizeof(bootloader_macaddr),
        &actual_len);
    if (status != ZX_OK) {
        return status;
    } else if (actual_len < MAC_ADDR_LEN) {
        return ZX_ERR_INTERNAL;
    }
    memcpy(macaddr, bootloader_macaddr, MAC_ADDR_LEN);
    zxlogf(INFO, "bcm-hci: got bootloader mac address %02x:%02x:%02x:%02x:%02x:%02x\n",
           macaddr[0], macaddr[1], macaddr[2], macaddr[3], macaddr[4], macaddr[5]);

    return ZX_OK;
}

static int bcm_hci_start_thread(void* arg) {
    bcm_hci_t* hci = arg;
    zx_handle_t fw_vmo;

    zx_handle_t theirs;
    zx_status_t status = zx_channel_create(0, &hci->command_channel, &theirs);
    if (status != ZX_OK) {
        goto fail;
    }
    status = bt_hci_open_command_channel(&hci->hci, theirs);
    if (status != ZX_OK) {
        goto fail;
    }

    // Send Reset command
    status = bcm_hci_send_command(hci, &RESET_CMD, sizeof(RESET_CMD), NULL, 0);
    if (status != ZX_OK) {
        goto fail;
    }

    if (hci->is_uart) {
        // switch baud rate to TARGET_BAUD_RATE
        status = bcm_hci_set_baud_rate(hci, TARGET_BAUD_RATE);
        if (status != ZX_OK) {
            goto fail;
        }
    }

    size_t fw_size;
    status = load_firmware(hci->zxdev, FIRMWARE_PATH, &fw_vmo, &fw_size);
    if (status == ZX_OK) {
        status = bcm_hci_send_command(hci, &START_FIRMWARE_DOWNLOAD_CMD,
                                      sizeof(START_FIRMWARE_DOWNLOAD_CMD), NULL, 0);
        if (status != ZX_OK) {
            goto fail;
        }

        // give time for placing firmware in download mode
        zx_nanosleep(zx_deadline_after(FIRMWARE_DOWNLOAD_DELAY));

        zx_off_t offset = 0;
        while (offset < fw_size) {
            uint8_t buffer[255 + 3];

            size_t remaining = fw_size - offset;
            size_t read_amount = (remaining > sizeof(buffer) ? sizeof(buffer) : remaining);

            if (read_amount < 3) {
                zxlogf(ERROR, "short HCI command in firmware download\n");
                status = ZX_ERR_INTERNAL;
                goto vmo_close_fail;
            }

            status = zx_vmo_read(fw_vmo, buffer, offset, read_amount);
            if (status != ZX_OK) {
                goto vmo_close_fail;
            }

            hci_command_header_t* header = (hci_command_header_t *)buffer;
            size_t length = header->parameter_total_size + sizeof(*header);
            if (read_amount < length) {
                zxlogf(ERROR, "short HCI command in firmware download\n");
                status = ZX_ERR_INTERNAL;
                goto vmo_close_fail;
            }
            status = bcm_hci_send_command(hci, header, length, NULL, 0);
            if (status != ZX_OK) {
                zxlogf(ERROR, "bcm_hci_send_command failed in firmware download: %s\n",
                       zx_status_get_string(status));
                goto vmo_close_fail;
            }
            offset += length;
        }

        zx_handle_close(fw_vmo);

        if (hci->is_uart) {
            // firmware switched us back to 115200. switch back to TARGET_BAUD_RATE
            status = serial_config(&hci->serial, 115200, SERIAL_SET_BAUD_RATE_ONLY);
            if (status != ZX_OK) {
                goto fail;
            }

            // switch baud rate to TARGET_BAUD_RATE after DELAY
            zx_nanosleep(zx_deadline_after(BAUD_RATE_SWITCH_DELAY));
            status = bcm_hci_set_baud_rate(hci, TARGET_BAUD_RATE);
            if (status != ZX_OK) {
                goto fail;
            }
        }
        zxlogf(INFO, "bcm-hci: firmware loaded\n");
    } else {
        zxlogf(ERROR, "bcm-hci: no firmware file found\n");
    }

    // set BDADDR to value in bootloader
    uint8_t macaddr[MAC_ADDR_LEN];
    status = bcm_get_bdaddr_from_bootloader(hci, macaddr);
    if (status == ZX_OK) {
        // send Set BDADDR command
        status = bcm_hci_set_bdaddr(hci, macaddr);
        if (status != ZX_OK) {
            goto fail;
        }
    } else {
        // log error and fallback mac address
        hci_read_bdaddr_command_complete_t event;
        memset(&event, 0, sizeof(event));
        char fallback_addr[18] = "<unknown>";
        zx_status_t read_cmd_status = bcm_hci_send_command(hci, &READ_BDADDR_CMD,
                                                           sizeof(READ_BDADDR_CMD),
                                                           (void *)(&event), sizeof(event));
        if (read_cmd_status == ZX_OK) {
            // HCI returns data as little endian. Swap bytes
            snprintf(fallback_addr, 18, "%02x:%02x:%02x:%02x:%02x:%02x", event.bdaddr[5],
                     event.bdaddr[4], event.bdaddr[3], event.bdaddr[2], event.bdaddr[1],
                     event.bdaddr[0]);
        }
        zxlogf(ERROR, "bcm-hci: error getting mac address from bootloader: %s. "
               "Fallback address: %s.\n",
               zx_status_get_string(status), fallback_addr);
    }

    // We're done with the command channel. Close it so that it can be opened by
    // the host stack after the device becomes visible.
    zx_handle_close(hci->command_channel);
    hci->command_channel = ZX_HANDLE_INVALID;

    device_make_visible(hci->zxdev);
    return 0;

vmo_close_fail:
    zx_handle_close(fw_vmo);
fail:
    zxlogf(ERROR, "bcm_hci_start_thread: device initialization failed: %s\n",
           zx_status_get_string(status));

    device_remove(hci->zxdev);
    return -1;
}

static zx_status_t bcm_hci_bind(void* ctx, zx_device_t* device) {
    bcm_hci_t* hci = calloc(1, sizeof(bcm_hci_t));
    if (!hci) {
        return ZX_ERR_NO_MEMORY;
    }

    zx_status_t status = device_get_protocol(device, ZX_PROTOCOL_BT_HCI, &hci->hci);
    if (status != ZX_OK) {
        zxlogf(ERROR, "bcm_hci_bind: get protocol ZX_PROTOCOL_BT_HCI failed\n");
        return status;
    }
    status = device_get_protocol(device, ZX_PROTOCOL_SERIAL, &hci->serial);
    if (status == ZX_OK) {
        hci->is_uart = true;
    }

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "bcm-hci",
        .ctx = hci,
        .ops = &bcm_hci_device_proto,
        .proto_id = ZX_PROTOCOL_BT_HCI,
        .flags = DEVICE_ADD_INVISIBLE,
    };

    hci->transport_dev = device;

    status = device_add(device, &args, &hci->zxdev);
    if (status != ZX_OK) {
        bcm_hci_release(hci);
        return status;
    }

    // create thread to continue initialization
    thrd_t t;
    int thrd_rc = thrd_create_with_name(&t, bcm_hci_start_thread, hci, "bcm_hci_start_thread");
    if (thrd_rc != thrd_success) {
        device_remove(hci->zxdev);
        bcm_hci_release(hci);
        return thrd_status_to_zx_status(thrd_rc);
    }

    return ZX_OK;
}

static zx_driver_ops_t bcm_hci_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = bcm_hci_bind,
};

// clang-format off
ZIRCON_DRIVER_BEGIN(bcm_hci, bcm_hci_driver_ops, "zircon", "0.1", 2)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_BT_TRANSPORT),
    BI_MATCH_IF(EQ, BIND_SERIAL_VID, PDEV_VID_BROADCOM),
ZIRCON_DRIVER_END(bcm_hci)
