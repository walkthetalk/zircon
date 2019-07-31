// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "netsvc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

#include <inet6/inet6.h>
#include <inet6/netifc.h>

#include <lib/fdio/io.h>
#include <lib/fdio/spawn.h>
#include <zircon/boot/netboot.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/log.h>
#include <zircon/time.h>

#include "device_id.h"
#include "debuglog.h"
#include "netboot.h"
#include "tftp.h"

#define FILTER_IPV6 1

static bool g_netbootloader = false;

static const char* g_nodename = "zircon";

bool netbootloader() { return g_netbootloader; }
const char* nodename() { return g_nodename; }

void udp6_recv(void* data, size_t len, const ip6_addr_t* daddr, uint16_t dport,
               const ip6_addr_t* saddr, uint16_t sport) {

    bool mcast = (memcmp(daddr, &ip6_ll_all_nodes, sizeof(ip6_addr_t)) == 0);

    switch (dport) {
    case NB_SERVER_PORT:
        netboot_recv(data, len, mcast, daddr, dport, saddr, sport);
        break;
    case DEBUGLOG_ACK_PORT:
        debuglog_recv(data, len, mcast);
        break;
    case NB_TFTP_INCOMING_PORT:
    case NB_TFTP_OUTGOING_PORT:
        tftp_recv(data, len, daddr, dport, saddr, sport);
        break;
    }
}

void netifc_recv(void* data, size_t len) {
    eth_recv(data, len);
}

bool netifc_send_pending() {
    if (!tftp_has_pending()) {
        return false;
    }
    tftp_send_next();
    return tftp_has_pending();
}

void update_timeouts() {
    zx_time_t now = zx_clock_get_monotonic();
    zx_time_t next_timeout = (debuglog_next_timeout() < tftp_next_timeout())
                                 ? debuglog_next_timeout()
                                 : tftp_next_timeout();
    if (next_timeout != ZX_TIME_INFINITE) {
        uint32_t ms = static_cast<uint32_t>(
            (next_timeout < now) ? 0 : (zx_time_sub_time(next_timeout, now)) / ZX_MSEC(1));
        netifc_set_timer(ms);
    }
}

static const char* zedboot_banner = "              _ _                 _   \n"
                                    "             | | |               | |  \n"
                                    "  _______  __| | |__   ___   ___ | |_ \n"
                                    " |_  / _ \\/ _` | '_ \\ / _ \\ / _ \\| __|\n"
                                    "  / /  __/ (_| | |_) | (_) | (_) | |_ \n"
                                    " /___\\___|\\__,_|_.__/ \\___/ \\___/ \\__|\n"
                                    "                                      \n"
                                    "\n";

int main(int argc, char** argv) {
    unsigned char mac[6];
    uint16_t mtu;
    char device_id[DEVICE_ID_MAX];
    bool print_nodename_and_exit = false;

    if (debuglog_init() < 0) {
        return -1;
    }

    const char* interface = NULL;
    bool nodename_provided = false;
    bool should_advertise = false;
    while (argc > 1) {
        if (!strncmp(argv[1], "--netboot", 9)) {
            g_netbootloader = true;
        } else if (!strncmp(argv[1], "--advertise", 11)) {
            should_advertise = true;
        } else if (!strncmp(argv[1], "--interface", 11)) {
            if (argc < 3) {
                printf("netsvc: fatal error: missing argument to --interface\n");
                return -1;
            }
            interface = argv[2];
            // Advance args one position. The second arg will be advanced below.
            argv++;
            argc--;
        } else if (!strncmp(argv[1], "--nodename", 10)) {
            print_nodename_and_exit = true;
        } else {
            g_nodename = argv[1];
            nodename_provided = true;
        }
        argv++;
        argc--;
    }
    if (interface != NULL) {
        printf("netsvc: looking for interface %s\n", interface);
    }

    for (;;) {
        if (netifc_open(interface, /*quiet=*/print_nodename_and_exit) != 0) {
            printf("netsvc: fatal error initializing network\n");
            return -1;
        }

        // Use mac address to generate unique nodename unless one was provided.
        if (!nodename_provided) {
            netifc_get_info(mac, &mtu);
            device_id_get(mac, device_id);
            g_nodename = device_id;
            if (print_nodename_and_exit) {
                printf("%s\n", g_nodename);
                return 0;
            }
        }

        if (g_netbootloader) {
            printf("%szedboot: version: %s\n\n", zedboot_banner, BOOTLOADER_VERSION);
        }

        printf("netsvc: nodename='%s'\n", g_nodename);
        if (!should_advertise) {
            printf("netsvc: will not advertise\n");
        }
        printf("netsvc: start\n");
        for (;;) {
            if (g_netbootloader && should_advertise) {
                netboot_advertise(g_nodename);
            }

            update_timeouts();

            if (netifc_poll()) {
                printf("netsvc: netifc_poll() failed - terminating\n");
                break;
            }
            zx_time_t now = zx_clock_get_monotonic();
            if (now > debuglog_next_timeout()) {
                debuglog_timeout_expired();
            }
            if (now > tftp_next_timeout()) {
                tftp_timeout_expired();
            }
        }
        netifc_close();
    }

    return 0;
}
