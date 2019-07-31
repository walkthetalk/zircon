// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "netboot.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <threads.h>
#include <unistd.h>

#include <fuchsia/device/manager/c/fidl.h>
#include <fuchsia/kernel/c/fidl.h>
#include <inet6/inet6.h>
#include <inet6/netifc.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/directory.h>
#include <zircon/boot/netboot.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include "netsvc.h"
#include "netcp.h"
#include "paver.h"
#include "zbi.h"

static uint32_t last_cookie = 0;
static uint32_t last_cmd = 0;
static uint32_t last_arg = 0;
static uint32_t last_ack_cmd = 0;
static uint32_t last_ack_arg = 0;

#define PAGE_ROUNDUP(x) ((x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define MAX_ADVERTISE_DATA_LEN 256

bool xfer_active = false;

struct nbfilecontainer_t {
    nbfile file;
    zx_handle_t data; // handle to vmo that backs netbootfile.
};

static nbfilecontainer_t nbkernel;
static nbfilecontainer_t nbbootdata;
static nbfilecontainer_t nbcmdline;

// Pointer to the currently active transfer.
static nbfile* active;

static zx_status_t nbfilecontainer_init(size_t size, nbfilecontainer_t* target) {
    zx_status_t st = ZX_OK;

    assert(target);

    // De-init the container if it's already initialized.
    if (target->file.data) {
        // For now, I can't see a valid reason that a client would send the same
        // filename twice.
        // We'll handle this case gracefully, but we'll print a warning to the
        // console just in case it was a mistake.
        printf("netbootloader: warning, reusing a previously initialized container\n");

        // Unmap the vmo from the address space.
        st = zx_vmar_unmap(zx_vmar_root_self(), reinterpret_cast<uintptr_t>(target->file.data),
                           target->file.size);
        if (st != ZX_OK) {
            printf("netbootloader: failed to unmap existing vmo, st = %d\n", st);
            return st;
        }

        zx_handle_close(target->data);

        target->file.offset = 0;
        target->file.size = 0;
        target->file.data = 0;
    }

    st = zx_vmo_create(size, 0, &target->data);
    if (st != ZX_OK) {
        printf("netbootloader: Could not create a netboot vmo of size = %lu "
               "retcode = %d\n",
               size, st);
        return st;
    }
    zx_object_set_property(target->data, ZX_PROP_NAME, "netboot", 7);

    uintptr_t buffer;
    st = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, target->data, 0,
                     size, &buffer);
    if (st != ZX_OK) {
        printf("netbootloader: failed to map data vmo for buffer, st = %d\n", st);
        zx_handle_close(target->data);
        return st;
    }

    target->file.offset = 0;
    target->file.size = size;
    target->file.data = reinterpret_cast<uint8_t*>(buffer);

    return ZX_OK;
}

nbfile* netboot_get_buffer(const char* name, size_t size) {
    zx_status_t st = ZX_OK;
    nbfilecontainer_t* result;

    if (!strcmp(name, NB_KERNEL_FILENAME)) {
        result = &nbkernel;
    } else if (!strcmp(name, NB_RAMDISK_FILENAME)) {
        result = &nbbootdata;
    } else if (!strcmp(name, NB_CMDLINE_FILENAME)) {
        result = &nbcmdline;
    } else {
        return NULL;
    }

    st = nbfilecontainer_init(size, result);
    if (st != ZX_OK) {
        printf("netbootloader: failed to initialize file container for "
               "file = '%s', retcode = %d\n",
               name, st);
        return NULL;
    }

    return &result->file;
}

void netboot_advertise(const char* nodename) {
    // Don't advertise if a transfer is active.
    if (xfer_active)
        return;

    struct {
        nbmsg msg;
        char data[MAX_ADVERTISE_DATA_LEN];
    } packet_data;
    nbmsg* msg = &packet_data.msg;
    msg->magic = NB_MAGIC;
    msg->cookie = 0;
    msg->cmd = NB_ADVERTISE;
    msg->arg = NB_VERSION_CURRENT;

    snprintf(packet_data.data, MAX_ADVERTISE_DATA_LEN, "version=%s;nodename=%s",
             BOOTLOADER_VERSION, nodename);
    const size_t data_len = strlen(packet_data.data) + 1;
    udp6_send(&packet_data, sizeof(nbmsg) + data_len, &ip6_ll_all_nodes, NB_ADVERT_PORT, NB_SERVER_PORT,
              false);
}

static void nb_open(const char* filename, uint32_t cookie, uint32_t arg, const ip6_addr_t* saddr,
                    uint16_t sport, uint16_t dport) {
    nbmsg m;
    m.magic = NB_MAGIC;
    m.cookie = cookie;
    m.cmd = NB_ACK;
    m.arg = netcp_open(filename, arg, NULL);
    udp6_send(&m, sizeof(m), saddr, sport, dport, false);
}

struct netfilemsg {
    nbmsg hdr;
    uint8_t data[1024];
};

static void nb_read(uint32_t cookie, uint32_t arg, const ip6_addr_t* saddr, uint16_t sport,
                    uint16_t dport) {
    static netfilemsg m = {
        .hdr =
            {
                .magic = NB_MAGIC,
                .cookie = 0,
                .cmd = NB_ACK,
                .arg = 0,
                .data = {},
            },
        .data = {},
    };
    static size_t msg_size = 0;
    static uint32_t blocknum = static_cast<uint32_t>(-1);
    if (arg == blocknum) {
        // Request to resend last message, verify that the cookie is unchanged
        if (cookie != m.hdr.cookie) {
            m.hdr.arg = -EIO;
            m.hdr.cookie = cookie;
            msg_size = sizeof(m.hdr);
        }
    } else if (arg == 0 || arg == blocknum + 1) {
        ssize_t result = netcp_read(&m.data, sizeof(m.data));
        if (result < 0) {
            m.hdr.arg = static_cast<uint32_t>(result);
            msg_size = sizeof(m.hdr);
        } else {
            // Note that the response does not use actual size as the argument,
            // it matches the *requested* size. Actual size can be determined by
            // the packet size.
            m.hdr.arg = arg;
            msg_size = sizeof(m.hdr) + result;
        }
        m.hdr.cookie = cookie;
        blocknum = arg;
    } else {
        // Ignore bogus read requests -- host will timeout if they're confused
        return;
    }
    udp6_send(&m, msg_size, saddr, sport, dport, false);
}

static void nb_write(const char* data, size_t len, uint32_t cookie, uint32_t arg,
                     const ip6_addr_t* saddr, uint16_t sport, uint16_t dport) {
    static nbmsg m = {
        .magic = NB_MAGIC,
        .cookie = 0,
        .cmd = NB_ACK,
        .arg = 0,
        .data = {},
    };
    static uint32_t blocknum = static_cast<uint32_t>(-1);
    if (arg == blocknum) {
        // Request to repeat last write, verify that cookie is unchanged
        if (cookie != m.cookie) {
            m.arg = -EIO;
        }
    } else if (arg == 0 || arg == blocknum + 1) {
        ssize_t result = netcp_write(data, len);
        m.arg = static_cast<uint32_t>(result > 0 ? 0 : result);
        blocknum = arg;
    }
    m.cookie = cookie;
    udp6_send(&m, sizeof(m), saddr, sport, dport, false);
}

static void nb_close(uint32_t cookie, const ip6_addr_t* saddr, uint16_t sport, uint16_t dport) {
    nbmsg m;
    m.magic = NB_MAGIC;
    m.cookie = cookie;
    m.cmd = NB_ACK;
    m.arg = netcp_close();
    udp6_send(&m, sizeof(m), saddr, sport, dport, false);
}

static zx_status_t do_dmctl_mexec() {
    zx_handle_t kernel, bootdata;
    // TODO(scottmg): range check nbcmdline.file.size rather than just casting.
    zx_status_t status =
        netboot_prepare_zbi(nbkernel.data, nbbootdata.data, nbcmdline.file.data,
                            static_cast<uint32_t>(nbcmdline.file.size), &kernel, &bootdata);
    if (status != ZX_OK) {
        return status;
    }

    zx::channel local, remote;
    status = zx::channel::create(0, &local, &remote);
    if (status != ZX_OK) {
        return ZX_ERR_INTERNAL;
    }

    status = fdio_service_connect("/svc/fuchsia.kernel.MexecBroker", remote.release());
    if (status != ZX_OK) {
        return ZX_ERR_INTERNAL;
    }

    status = fuchsia_kernel_MexecBrokerPerformMexec(local.get(), kernel, bootdata);
    if (status != ZX_OK) {
        return ZX_ERR_INTERNAL;
    }

    // Wait for the world to end.
    zx_nanosleep(ZX_TIME_INFINITE);
    return ZX_ERR_INTERNAL;
}

static zx_status_t reboot() {
    zx::channel local, remote;
    zx_status_t status = zx::channel::create(0, &local, &remote);
    if (status != ZX_OK) {
        return ZX_ERR_INTERNAL;
    }

    status = fdio_service_connect("/svc/fuchsia.device.manager.Administrator", remote.release());
    if (status != ZX_OK) {
        return ZX_ERR_INTERNAL;
    }

    zx_status_t call_status;
    status = fuchsia_device_manager_AdministratorSuspend(local.get(),
                                                         fuchsia_device_manager_SUSPEND_FLAG_REBOOT,
                                                         &call_status);
    if (status != ZX_OK) {
        return status;
    }

    return call_status;
}

static void bootloader_recv(void* data, size_t len, const ip6_addr_t* daddr, uint16_t dport,
                            const ip6_addr_t* saddr, uint16_t sport) {
    nbmsg* msg = reinterpret_cast<nbmsg*>(data);
    nbmsg ack;

    bool do_transmit = true;
    bool do_boot = false;
    bool do_reboot = false;

    if (dport != NB_SERVER_PORT)
        return;

    if (len < sizeof(nbmsg))
        return;
    len -= sizeof(nbmsg);

    if ((last_cookie == msg->cookie) && (last_cmd == msg->cmd) && (last_arg == msg->arg)) {
        // host must have missed the ack. resend
        ack.magic = NB_MAGIC;
        ack.cookie = last_cookie;
        ack.cmd = last_ack_cmd;
        ack.arg = last_ack_arg;
        goto transmit;
    }

    ack.cmd = NB_ACK;
    ack.arg = 0;

    switch (msg->cmd) {
    case NB_COMMAND:
        if (len == 0)
            return;
        msg->data[len - 1] = 0;
        break;
    case NB_SEND_FILE:
        xfer_active = true;
        if (len == 0)
            return;
        msg->data[len - 1] = 0;
        for (size_t i = 0; i < (len - 1); i++) {
            if ((msg->data[i] < ' ') || (msg->data[i] > 127)) {
                msg->data[i] = '.';
            }
        }
        active = netboot_get_buffer(reinterpret_cast<const char*>(msg->data), msg->arg);
        if (active) {
            active->offset = 0;
            ack.arg = msg->arg;
            size_t prefix_len = strlen(NB_FILENAME_PREFIX);
            const char* filename;
            if (!strncmp(reinterpret_cast<char*>(msg->data), NB_FILENAME_PREFIX, prefix_len)) {
                filename = &(reinterpret_cast<const char*>(msg->data))[prefix_len];
            } else {
                filename = reinterpret_cast<const char*>(msg->data);
            }
            printf("netboot: Receive File '%s'...\n", filename);
        } else {
            printf("netboot: Rejected File '%s'...\n", reinterpret_cast<char*>(msg->data));
            ack.cmd = NB_ERROR_BAD_FILE;
        }
        break;

    case NB_DATA:
    case NB_LAST_DATA:
        xfer_active = true;
        if (active == 0) {
            printf("netboot: > received chunk before NB_FILE\n");
            return;
        }
        if (msg->arg != active->offset) {
            // printf("netboot: < received chunk at offset %d but current offset is %zu\n",
            // msg->arg, active->offset);
            ack.arg = static_cast<uint32_t>(active->offset);
            ack.cmd = NB_ACK;
        } else if ((active->offset + len) > active->size) {
            ack.cmd = NB_ERROR_TOO_LARGE;
            ack.arg = msg->arg;
        } else {
            memcpy(active->data + active->offset, msg->data, len);
            active->offset += len;
            ack.cmd = msg->cmd == NB_LAST_DATA ? NB_FILE_RECEIVED : NB_ACK;
            if (msg->cmd != NB_LAST_DATA) {
                do_transmit = false;
            } else {
                xfer_active = false;
            }
        }
        break;
    case NB_BOOT:
        // Wait for the paver to complete
        while (netsvc::Paver::Get()->InProgress()) {
            thrd_yield();
        }
        if (netsvc::Paver::Get()->exit_code() != 0) {
            printf("netboot: detected paver error: %d\n", netsvc::Paver::Get()->exit_code());
            netsvc::Paver::Get()->reset_exit_code();
            break;
        }
        do_boot = true;
        printf("netboot: Boot Kernel...\n");
        break;
    case NB_REBOOT:
        // Wait for the paver to complete
        while (netsvc::Paver::Get()->InProgress()) {
            thrd_yield();
        }
        if (netsvc::Paver::Get()->exit_code() != 0) {
            printf("netboot: detected paver error: %d\n", netsvc::Paver::Get()->exit_code());
            netsvc::Paver::Get()->reset_exit_code();
            break;
        }
        do_reboot = true;
        printf("netboot: Reboot ...\n");
        break;
    default:
        // We don't have a handler for this command, let netsvc handle it.
        do_transmit = false;
    }

    last_cookie = msg->cookie;
    last_cmd = msg->cmd;
    last_arg = msg->arg;
    last_ack_cmd = ack.cmd;
    last_ack_arg = ack.arg;

    ack.cookie = msg->cookie;
    ack.magic = NB_MAGIC;
transmit:
    if (do_transmit) {
        udp6_send(&ack, sizeof(ack), saddr, sport, NB_SERVER_PORT, false);
    }

    if (do_boot) {
        zx_status_t status = do_dmctl_mexec();
        if (status != ZX_OK) {
            // TODO: This will return before the system actually mexecs.
            // We can't pass an event to wait on here because fdio
            // has a limit of 3 handles, and we're already using
            // all 3 to pass boot parameters.
            printf("netboot: Boot failed. status = %d\n", status);
        }
    }

    if (do_reboot) {
        zx_status_t status = reboot();
        if (status != ZX_OK) {
            printf("netboot: Reboot failed. status = %d\n", status);
        }
    }
}

void netboot_recv(void* data, size_t len, bool is_mcast, const ip6_addr_t* daddr, uint16_t dport,
                  const ip6_addr_t* saddr, uint16_t sport) {
    nbmsg* msg = reinterpret_cast<nbmsg*>(data);
    // Not enough bytes to be a message
    if ((len < sizeof(*msg)) || (msg->magic != NB_MAGIC)) {
        return;
    }
    len -= sizeof(*msg);

    if (len && msg->cmd != NB_DATA && msg->cmd != NB_LAST_DATA) {
        msg->data[len - 1] = '\0';
    }

    switch (msg->cmd) {
    case NB_QUERY: {
        if (strcmp(reinterpret_cast<char*>(msg->data), "*") &&
            strcmp(reinterpret_cast<char*>(msg->data), nodename())) {
            break;
        }
        size_t dlen = strlen(nodename()) + 1;
        char buf[1024 + sizeof(nbmsg)];
        if ((dlen + sizeof(nbmsg)) > sizeof(buf)) {
            return;
        }
        msg->cmd = NB_ACK;
        memcpy(buf, msg, sizeof(nbmsg));
        memcpy(buf + sizeof(nbmsg), nodename(), dlen);
        udp6_send(buf, sizeof(nbmsg) + dlen, saddr, sport, dport, false);
        break;
    }
    case NB_SHELL_CMD:
        if (!is_mcast) {
            netboot_run_cmd(reinterpret_cast<char*>(msg->data));
            return;
        }
        break;
    case NB_OPEN:
        nb_open(reinterpret_cast<char*>(msg->data), msg->cookie, msg->arg, saddr, sport, dport);
        break;
    case NB_READ:
        nb_read(msg->cookie, msg->arg, saddr, sport, dport);
        break;
    case NB_WRITE:
        len--; // NB NUL-terminator is not part of the data
        nb_write(reinterpret_cast<char*>(msg->data), len, msg->cookie, msg->arg, saddr, sport,
                 dport);
        break;
    case NB_CLOSE:
        nb_close(msg->cookie, saddr, sport, dport);
        break;
    default:
        // If the bootloader is enabled, then let it have a crack at the
        // incoming packets as well.
        if (netbootloader()) {
            bootloader_recv(data, len + sizeof(nbmsg), daddr, dport, saddr, sport);
        }
    }
}
