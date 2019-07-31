// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fuchsia/hardware/input/c/fidl.h>
#include <hid/hid.h>
#include <hid/usages.h>
#include <lib/fzl/fdio.h>
#include <zircon/syscalls.h>

#include <port/port.h>

#include <utility>

#include "keyboard.h"

#define LOW_REPEAT_KEY_FREQ 250000000
#define HIGH_REPEAT_KEY_FREQ 50000000

static int modifiers_from_keycode(uint8_t keycode) {
    switch (keycode) {
    case HID_USAGE_KEY_LEFT_SHIFT:
        return MOD_LSHIFT;
    case HID_USAGE_KEY_RIGHT_SHIFT:
        return MOD_RSHIFT;
    case HID_USAGE_KEY_LEFT_ALT:
        return MOD_LALT;
    case HID_USAGE_KEY_RIGHT_ALT:
        return MOD_RALT;
    case HID_USAGE_KEY_LEFT_CTRL:
        return MOD_LCTRL;
    case HID_USAGE_KEY_RIGHT_CTRL:
        return MOD_RCTRL;
    }
    return 0;
}

static void set_caps_lock_led(int keyboard_fd, bool caps_lock) {
    // The following bit to set is specified in "Device Class Definition
    // for Human Interface Devices (HID)", Version 1.11,
    // http://www.usb.org/developers/hidpage/HID1_11.pdf.  Zircon leaves
    // USB keyboards in boot mode, so the relevant section is Appendix B,
    // "Boot Interface Descriptors", "B.1 Protocol 1 (Keyboard)".
    const uint8_t kUsbCapsLockBit = 1 << 1;
    const uint8_t report_body[1] = { static_cast<uint8_t>(caps_lock ? kUsbCapsLockBit : 0) };

    // Temporarily wrap keyboard_fd, we will release it after the call so we don't close it.
    fzl::FdioCaller caller{fbl::unique_fd(keyboard_fd)};
    zx_status_t call_status;
    zx_status_t status = fuchsia_hardware_input_DeviceSetReport(
        caller.borrow_channel(), fuchsia_hardware_input_ReportType_OUTPUT, 0, report_body,
        sizeof(report_body), &call_status);
    caller.release().release();
    if (status != ZX_OK || call_status != ZX_OK) {
#if !BUILD_FOR_TEST
        printf("fuchsia.hardware.input.Device.SetReport() failed (returned %d, %d)\n", status,
               call_status);
#endif
    }
}

struct vc_input {
    port_fd_handler_t fh;
    port_handler_t th;
    zx_handle_t timer;

    keypress_handler_t handler;
    int fd;

    uint8_t previous_report_buf[8];
    uint8_t report_buf[8];
    hid_keys_t state[2];
    int cur_idx;
    int prev_idx;
    int modifiers;
    uint64_t repeat_interval;
    bool repeat_enabled;
};

// returns true if key was pressed and none were released
bool vc_input_process(vc_input_t* vi, uint8_t report[8]) {
    bool do_repeat = false;

    // process the key
    uint8_t keycode;
    hid_keys_t keys;

    hid_kbd_parse_report(report, &vi->state[vi->cur_idx]);

    hid_kbd_pressed_keys(&vi->state[vi->prev_idx], &vi->state[vi->cur_idx], &keys);
    hid_for_every_key(&keys, keycode) {
        if (keycode == HID_USAGE_KEY_ERROR_ROLLOVER) {
            return false;
        }
        vi->modifiers |= modifiers_from_keycode(keycode);
        if (keycode == HID_USAGE_KEY_CAPSLOCK) {
            vi->modifiers ^= MOD_CAPSLOCK;
            set_caps_lock_led(vi->fd, vi->modifiers & MOD_CAPSLOCK);
        }
        vi->handler(keycode, vi->modifiers);
        do_repeat = true;
    }

    hid_kbd_released_keys(&vi->state[vi->prev_idx], &vi->state[vi->cur_idx], &keys);
    hid_for_every_key(&keys, keycode) {
        vi->modifiers &= ~modifiers_from_keycode(keycode);
        do_repeat = false;
    }

    // swap key states
    vi->cur_idx = 1 - vi->cur_idx;
    vi->prev_idx = 1 - vi->prev_idx;

    return do_repeat;
}

#if !BUILD_FOR_TEST
static void vc_input_destroy(vc_input_t* vi) {
    port_cancel(&port, &vi->th);
    if (vi->fd >= 0) {
        port_fd_handler_done(&vi->fh);
        close(vi->fd);
    }
    zx_handle_close(vi->timer);
    free(vi);
}

static zx_status_t vc_timer_cb(port_handler_t* ph, zx_signals_t signals, uint32_t evt) {
    vc_input_t* vi = containerof(ph, vc_input_t, th);

    vc_input_process(vi, vi->previous_report_buf);
    vc_input_process(vi, vi->report_buf);

    // increase repeat rate if we're not yet at the fastest rate
    if ((vi->repeat_interval = vi->repeat_interval * 3 / 4) < HIGH_REPEAT_KEY_FREQ) {
        vi->repeat_interval = HIGH_REPEAT_KEY_FREQ;
    }

    zx_timer_set(vi->timer, zx_deadline_after(vi->repeat_interval), 0);

    return ZX_OK;
}

static zx_status_t vc_input_cb(port_fd_handler_t* fh, unsigned pollevt, uint32_t evt) {
    vc_input_t* vi = containerof(fh, vc_input_t, fh);
    ssize_t r;

    if (!(pollevt & POLLIN)) {
        r = ZX_ERR_PEER_CLOSED;
    } else {
        memcpy(vi->previous_report_buf, vi->report_buf, sizeof(vi->report_buf));
        r = read(vi->fd, vi->report_buf, sizeof(vi->report_buf));
    }
    if (r <= 0) {
        vc_input_destroy(vi);
        return ZX_ERR_STOP;
    }
    if ((size_t)(r) != sizeof(vi->report_buf)) {
        vi->repeat_interval = ZX_TIME_INFINITE;
        return ZX_OK;
    }

    if (vc_input_process(vi, vi->report_buf) && vi->repeat_enabled) {
        vi->repeat_interval = LOW_REPEAT_KEY_FREQ;
        zx_timer_set(vi->timer, zx_deadline_after(vi->repeat_interval), 0);
    } else {
        zx_timer_cancel(vi->timer);
    }
    return ZX_OK;
}
#endif

zx_status_t vc_input_create(vc_input_t** out, keypress_handler_t handler, int fd) {
    vc_input_t* vi = reinterpret_cast<vc_input_t*>(calloc(1, sizeof(vc_input_t)));
    if (vi == NULL) {
        return ZX_ERR_NO_MEMORY;
    }

    vi->fd = fd;
    vi->handler = handler;

    vi->cur_idx = 0;
    vi->prev_idx = 1;
    vi->modifiers = 0;
    vi->repeat_interval = ZX_TIME_INFINITE;
    vi->repeat_enabled = true;

    char* flag = getenv("virtcon.keyrepeat");
    if (flag && (!strcmp(flag, "0") || !strcmp(flag, "false"))) {
        printf("vc: Key repeat disabled\n");
        vi->repeat_enabled = false;
    }

#if !BUILD_FOR_TEST
    zx_status_t r;
    if ((r = zx_timer_create(ZX_TIMER_SLACK_LATE, ZX_CLOCK_MONOTONIC, &vi->timer)) < 0) {
        free(vi);
        return r;
    }

    vi->fh.func = vc_input_cb;
    if ((r = port_fd_handler_init(&vi->fh, fd, POLLIN | POLLHUP | POLLRDHUP)) < 0) {
        zx_handle_close(vi->timer);
        free(vi);
        return r;
    }

    if ((r = port_wait(&port, &vi->fh.ph)) < 0) {
        port_fd_handler_done(&vi->fh);
        zx_handle_close(vi->timer);
        free(vi);
        return r;
    }

    vi->th.handle = vi->timer;
    vi->th.waitfor = ZX_TIMER_SIGNALED;
    vi->th.func = vc_timer_cb;
    port_wait(&port, &vi->th);
#endif

    *out = vi;
    return ZX_OK;
}

#if !BUILD_FOR_TEST
zx_status_t new_input_device(int fd, keypress_handler_t handler) {
    // test to see if this is a device we can read
    uint32_t proto = fuchsia_hardware_input_BootProtocol_NONE;

    // Temporarily wrap fd, we will release it after the call so we don't close it.
    fzl::FdioCaller caller{fbl::unique_fd(fd)};
    zx_status_t status =
        fuchsia_hardware_input_DeviceGetBootProtocol(caller.borrow_channel(), &proto);
    caller.release().release();
    if ((status != ZX_OK) || (proto != fuchsia_hardware_input_BootProtocol_KBD)) {
        // skip devices that aren't keyboards
        close(fd);
        return ZX_ERR_NOT_SUPPORTED;
    }

    vc_input_t* vi;
    if ((status = vc_input_create(&vi, handler, fd)) < 0) {
        close(fd);
    }
    return status;
}
#endif
