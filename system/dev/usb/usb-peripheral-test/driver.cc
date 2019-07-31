// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "driver.h"
#include <assert.h>
#include <fbl/alloc_checker.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <fbl/auto_lock.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>
#include <hw/arch_ops.h>
#include <optional>
#include <zircon/device/usb-peripheral-test.h>
#include <zircon/device/usb-peripheral.h>
#include <zircon/listnode.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>

namespace usb_function_test {

zx_status_t UsbTest::Init() {
    zx_status_t status = device_get_protocol(parent(), ZX_PROTOCOL_USB_FUNCTION, &function_);
    if (status != ZX_OK) {
        return status;
    }

    parent_req_size_ = function_.GetRequestSize();

    status = function_.AllocInterface(&descriptors_.intf.bInterfaceNumber);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: usb_function_alloc_interface failed\n", __func__);
        return status;
    }

    status = function_.AllocEp(USB_DIR_OUT, &bulk_out_addr_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: usb_function_alloc_ep failed\n", __func__);
        return status;
    }
    status = function_.AllocEp(USB_DIR_IN, &bulk_in_addr_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: usb_function_alloc_ep failed\n", __func__);
        return status;
    }
    status = function_.AllocEp(USB_DIR_IN, &intr_addr_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: usb_function_alloc_ep failed\n", __func__);
        return status;
    }

    descriptors_.bulk_out_ep.bEndpointAddress = bulk_out_addr_;
    descriptors_.bulk_in_ep.bEndpointAddress = bulk_in_addr_;
    descriptors_.intr_ep.bEndpointAddress = intr_addr_;

    // Allocate bulk out usb requests.
    std::optional<usb::Request<void>> req;
    fbl::AutoLock lock(&lock_);
    for (size_t i = 0; i < BULK_TX_COUNT; i++) {
        status = usb::Request<void>::Alloc(&req, BULK_REQ_SIZE, bulk_out_addr_, parent_req_size_);
        if (status != ZX_OK) {
            return status;
        }
        bulk_out_reqs_.push_next(std::move(*req));
        ZX_DEBUG_ASSERT(status == ZX_OK);
    }
    // Allocate bulk in usb requests.
    for (size_t i = 0; i < BULK_RX_COUNT; i++) {
        status = usb::Request<void>::Alloc(&req, BULK_REQ_SIZE, bulk_in_addr_, parent_req_size_);
        if (status != ZX_OK) {
            return status;
        }
        bulk_in_reqs_.push_next(std::move(*req));
        ZX_DEBUG_ASSERT(status == ZX_OK);
    }

    // Allocate interrupt requests.
    for (size_t i = 0; i < INTR_COUNT; i++) {
        status = usb::Request<void>::Alloc(&req, INTR_REQ_SIZE, intr_addr_, parent_req_size_);
        if (status != ZX_OK) {
            return status;
        }
        intr_reqs_.push_next(std::move(*req));
        ZX_DEBUG_ASSERT(status == ZX_OK);
    }
    DdkAdd("usb-function-test", DEVICE_ADD_NON_BINDABLE);

    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: add_device failed %d\n", __func__, status);
        return status;
    }

    function_.SetInterface(this, &usb_function_interface_protocol_ops_);

    return ZX_OK;
}

void UsbTest::TestIntrComplete(usb_request_t* req) {
    zxlogf(LTRACE, "%s %d %ld\n", __func__, req->response.status, req->response.actual);

    fbl::AutoLock lock(&lock_);
    intr_reqs_.push(usb::Request<void>(req, parent_req_size_));
}

void UsbTest::TestBulkOutComplete(usb_request_t* req) {
    zxlogf(LTRACE, "%s %d %ld\n", __func__, req->response.status, req->response.actual);

    if (req->response.status == ZX_ERR_IO_NOT_PRESENT) {
        fbl::AutoLock lock(&lock_);
        bulk_out_reqs_.push_next(usb::Request<void>(req, parent_req_size_));
        return;
    }
    if (req->response.status == ZX_OK) {
        lock_.Acquire();
        std::optional<usb::Request<void>> in_req = bulk_in_reqs_.pop();
        lock_.Release();
        if (in_req) {
            // Send data back to host.
            void* buffer;
            usb_request_mmap(req, &buffer);
            (*in_req).CopyTo(buffer, req->response.actual, 0);
            req->header.length = req->response.actual;

            usb_request_complete_t complete = {
                .callback =
                    [](void* ctx, usb_request_t* req) {
                        static_cast<UsbTest*>(ctx)->TestBulkInComplete(req);
                    },
                .ctx = this,
            };
            hw_mb();
            usb_request_cache_flush(in_req->request(), 0, in_req->request()->response.actual);
            function_.RequestQueue(in_req->take(), &complete);
        } else {
            zxlogf(ERROR, "%s: no bulk in request available\n", __func__);
        }
    } else {
        zxlogf(ERROR, "%s: usb_read_complete called with status %d\n", __func__,
               req->response.status);
    }

    // Requeue read.
    usb_request_complete_t complete = {
        .callback =
            [](void* ctx, usb_request_t* req) {
                static_cast<UsbTest*>(ctx)->TestBulkOutComplete(req);
            },
        .ctx = this,
    };
    function_.RequestQueue(req, &complete);
}

void UsbTest::TestBulkInComplete(usb_request_t* req) {
    zxlogf(LTRACE, "%s %d %ld\n", __func__, req->response.status, req->response.actual);

    fbl::AutoLock lock(&lock_);
    bulk_in_reqs_.push(usb::Request<void>(req, parent_req_size_));
}

void UsbTest::UsbFunctionInterfaceGetDescriptors(void* buffer, size_t buffer_size,
                                                 size_t* out_actual) {
    size_t length = sizeof(descriptors_);
    if (length > buffer_size) {
        length = buffer_size;
    }
    memcpy(buffer, &descriptors_, length);
    *out_actual = length;
}

zx_status_t UsbTest::UsbFunctionInterfaceControl(const usb_setup_t* setup, const void* write_buffer,
                                                 size_t write_size, void* read_buffer,
                                                 size_t read_size, size_t* out_read_actual) {
    size_t length = le16toh(setup->wLength);

    zxlogf(TRACE, "%s\n", __func__);
    if (setup->bmRequestType == (USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE) &&
        setup->bRequest == USB_PERIPHERAL_TEST_SET_DATA) {
        if (length > sizeof(test_data_)) {
            length = sizeof(test_data_);
        }
        memcpy(test_data_, write_buffer, length);
        test_data_length_ = length;
        return ZX_OK;
    } else if (setup->bmRequestType == (USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_INTERFACE) &&
               setup->bRequest == USB_PERIPHERAL_TEST_GET_DATA) {
        if (length > test_data_length_) {
            length = test_data_length_;
        }
        memcpy(read_buffer, test_data_, length);
        *out_read_actual = length;
        return ZX_OK;
    } else if (setup->bmRequestType == (USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE) &&
               setup->bRequest == USB_PERIPHERAL_TEST_SEND_INTERUPT) {
        lock_.Acquire();
        std::optional<usb::Request<void>> req = intr_reqs_.pop();
        lock_.Release();
        if (!req) {
            zxlogf(ERROR, "%s: no interrupt request available\n", __func__);
            // TODO(voydanoff) maybe stall in this case?
            return ZX_OK;
        }
        req->CopyTo(test_data_, test_data_length_, 0);
        req->request()->header.length = test_data_length_;

        usb_request_complete_t complete = {
            .callback =
                [](void* ctx, usb_request_t* req) {
                    static_cast<UsbTest*>(ctx)->TestIntrComplete(req);
                },
            .ctx = this,
        };
        function_.RequestQueue(req->take(), &complete);
        return ZX_OK;
    } else {
        return ZX_ERR_NOT_SUPPORTED;
    }
}

zx_status_t UsbTest::UsbFunctionInterfaceSetConfigured(bool configured, usb_speed_t speed) {
    zxlogf(TRACE, "%s: %d %d\n", __func__, configured, speed);
    zx_status_t status;

    if (configured) {
        if ((status = function_.ConfigEp(&descriptors_.intr_ep, NULL)) != ZX_OK ||
            (status = function_.ConfigEp(&descriptors_.bulk_out_ep, NULL)) != ZX_OK ||
            (status = function_.ConfigEp(&descriptors_.bulk_in_ep, NULL)) != ZX_OK) {
            zxlogf(ERROR, "%s: function_.ConfigEp( failed\n", __func__);
            return status;
        }
    } else {
        function_.DisableEp(bulk_out_addr_);
        function_.DisableEp(bulk_in_addr_);
        function_.DisableEp(intr_addr_);
    }
    configured_ = configured;

    if (configured) {
        // Queue our OUT requests.
        fbl::AutoLock lock(&lock_);
        std::optional<usb::Request<void>> req;
        while ((req = bulk_out_reqs_.pop())) {
            usb_request_complete_t complete = {
                .callback =
                    [](void* ctx, usb_request_t* req) {
                        static_cast<UsbTest*>(ctx)->TestBulkOutComplete(req);
                    },
                .ctx = this,
            };
            function_.RequestQueue(req->take(), &complete);
        }
    }

    return ZX_OK;
}

zx_status_t UsbTest::UsbFunctionInterfaceSetInterface(uint8_t interface, uint8_t alt_setting) {
    return ZX_ERR_NOT_SUPPORTED;
}

void UsbTest::DdkUnbind() {
    zxlogf(TRACE, "%s\n", __func__);
    DdkRemove();
}

void UsbTest::DdkRelease() {
    zxlogf(TRACE, "%s\n", __func__);
    delete this;
}

zx_status_t UsbTest::Create(void* ctx, zx_device_t* parent) {
    zxlogf(INFO, "%s\n", __func__);
    fbl::AllocChecker ac;
    std::unique_ptr<UsbTest> test(new (&ac) UsbTest(parent));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    zx_status_t status = test->Init();
    if (status != ZX_OK) {
        return status;
    }
    // The DDK now owns the test.
    __UNUSED UsbTest* unused = test.release();
    return ZX_OK;
}

zx_driver_ops_t driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = UsbTest::Create;
    return ops;
}();

} // namespace usb_function_test

// clang-format off
ZIRCON_DRIVER_BEGIN(usb_function_test, usb_function_test::driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_USB_FUNCTION),
    BI_ABORT_IF(NE, BIND_USB_VID, GOOGLE_USB_VID),
    BI_MATCH_IF(EQ, BIND_USB_PID, GOOGLE_USB_FUNCTION_TEST_PID),
    BI_MATCH_IF(EQ, BIND_USB_PID, GOOGLE_USB_CDC_AND_FUNCTION_TEST_PID),
ZIRCON_DRIVER_END(usb_function_test)
