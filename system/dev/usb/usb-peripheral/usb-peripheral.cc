// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "usb-peripheral.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/metadata.h>
#include <ddk/protocol/usb/dci.h>
#include <ddk/protocol/usb/function.h>
#include <ddk/protocol/usb/modeswitch.h>
#include <ddk/usb-peripheral-config.h>
#include <ddktl/fidl.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/unique_ptr.h>
#include <zircon/device/usb-peripheral.h>
#include <zircon/hw/usb.h>
#include <zircon/hw/usb/cdc.h>
#include <zircon/listnode.h>

#include "usb-function.h"

namespace usb_peripheral {

zx_status_t UsbPeripheral::Create(void* ctx, zx_device_t* parent) {
    fbl::AllocChecker ac;
    auto device = fbl::make_unique_checked<UsbPeripheral>(&ac, parent);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    auto status = device->Init();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the device.
    __UNUSED auto* dummy = device.release();
    return ZX_OK;
}

void UsbPeripheral::RequestComplete(usb_request_t* req) {
    fbl::AutoLock l(&pending_requests_lock_);
    usb::UnownedRequest<void> request(req, dci_.GetRequestSize());

    pending_requests_.erase(&request);
    l.release();
    request.Complete(request.request()->response.status, request.request()->response.actual);
}

void UsbPeripheral::UsbPeripheralRequestQueue(usb_request_t* usb_request,
                                              const usb_request_complete_t* complete_cb) {
    if (shutting_down_) {
        usb_request_complete(usb_request, ZX_ERR_IO_NOT_PRESENT, 0, complete_cb);
        return;
    }
    fbl::AutoLock l(&pending_requests_lock_);
    usb::UnownedRequest<void> request(usb_request, *complete_cb, dci_.GetRequestSize());
    __UNUSED usb_request_complete_t completion;
    completion.ctx = this;
    completion.callback = [](void* ctx, usb_request_t* req) {
        reinterpret_cast<UsbPeripheral*>(ctx)->RequestComplete(req);
    };
    pending_requests_.push_back(&request);
    l.release();
    dci_.RequestQueue(request.take(), &completion);
}

zx_status_t UsbPeripheral::Init() {
    // Parent must support DCI protocol. USB Mode Switch is optional.
    if (!dci_.is_valid()) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    // Starting USB mode is determined from device metadata.
    // We read initial value and store it in dev->usb_mode, but do not actually
    // enable it until after all of our functions have bound.
    size_t actual;
    auto status = device_get_metadata(parent(), DEVICE_METADATA_USB_MODE, &usb_mode_,
                                      sizeof(usb_mode_), &actual);
    if (status == ZX_ERR_NOT_FOUND) {
        fbl::AutoLock lock(&lock_);
        // Assume peripheral mode by default.
        usb_mode_ = USB_MODE_PERIPHERAL;
    } else if (status != ZX_OK || actual != sizeof(usb_mode_)) {
        zxlogf(ERROR, "%s: DEVICE_METADATA_USB_MODE failed\n", __func__);
        return status;
    }
    // Set DCI mode to USB_MODE_NONE until we are ready
    if (ums_.is_valid()) {
        ums_.SetMode(USB_MODE_NONE);
    }
    parent_request_size_ = usb::UnownedRequest<void>::RequestSize(dci_.GetRequestSize());

    status = DdkAdd("usb-peripheral", DEVICE_ADD_NON_BINDABLE);
    if (status != ZX_OK) {
        return status;
    }

    dci_.SetInterface(this, &usb_dci_interface_protocol_ops_);
    size_t metasize = 0;
    status = device_get_metadata_size(parent(), DEVICE_METADATA_USB_CONFIG, &metasize);
    if (status != ZX_OK) {
        return ZX_OK;
    }
    constexpr auto alignment = []() {
        return alignof(UsbConfig) > __STDCPP_DEFAULT_NEW_ALIGNMENT__
                   ? alignof(UsbConfig)
                   : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    }();
    fbl::AllocChecker ac;
    UsbConfig* config = reinterpret_cast<UsbConfig*>(
        new (std::align_val_t(alignment), &ac) unsigned char[metasize]);
    if (!ac.check()) {
        return ZX_OK;
    }
    fbl::AutoCall call([=]() { delete[] reinterpret_cast<char*>(config); });
    const uint32_t key = DEVICE_METADATA_USB_CONFIG;
    status = device_get_metadata(parent(), key, config, metasize, &metasize);
    if (status != ZX_OK) {
        return ZX_OK;
    }
    device_desc_.idVendor = config->vid;
    device_desc_.idProduct = config->pid;

    size_t max_str_len = strnlen(config->manufacturer, sizeof(config->manufacturer));
    status = AllocStringDesc(fbl::String(config->manufacturer, max_str_len),
                             &device_desc_.iManufacturer);
    if (status != ZX_OK) {
        return status;
    }

    max_str_len = strnlen(config->product, sizeof(config->product));
    status = AllocStringDesc(fbl::String(config->product, max_str_len), &device_desc_.iProduct);
    if (status != ZX_OK) {
        return status;
    }

    max_str_len = strnlen(config->serial, sizeof(config->serial));
    status = AllocStringDesc(fbl::String(config->serial, max_str_len), &device_desc_.iSerialNumber);
    if (status != ZX_OK) {
        return status;
    }
    SetDefaultConfig(reinterpret_cast<FunctionDescriptor*>(config->functions),
                     (metasize - sizeof(UsbConfig)) / sizeof(FunctionDescriptor));
    return ZX_OK;
}

zx_status_t UsbPeripheral::AllocStringDesc(fbl::String desc, uint8_t* out_index) {
    fbl::AutoLock lock(&lock_);

    if (strings_.size() >= MAX_STRINGS) {
        return ZX_ERR_NO_RESOURCES;
    }
    strings_.push_back(std::move(desc));

    // String indices are 1-based.
    *out_index = static_cast<uint8_t>(strings_.size());
    return ZX_OK;
}

zx_status_t UsbPeripheral::ValidateFunction(fbl::RefPtr<UsbFunction> function, void* descriptors,
                                            size_t length, uint8_t* out_num_interfaces) {
    auto* intf_desc = static_cast<usb_interface_descriptor_t*>(descriptors);
    if (intf_desc->bDescriptorType != USB_DT_INTERFACE ||
            intf_desc->bLength != sizeof(usb_interface_descriptor_t)) {
        zxlogf(ERROR, "%s: first descriptor not an interface descriptor\n", __func__);
        return ZX_ERR_INVALID_ARGS;
    }

    auto* end = reinterpret_cast<const usb_descriptor_header_t*>(static_cast<uint8_t*>(descriptors)
                                                                 + length);
    auto* header = reinterpret_cast<const usb_descriptor_header_t*>(descriptors);

    while (header < end) {
        if (header->bDescriptorType == USB_DT_INTERFACE) {
            auto* desc = reinterpret_cast<const usb_interface_descriptor_t*>(header);
            if (desc->bInterfaceNumber >= countof(interface_map_) ||
                interface_map_[desc->bInterfaceNumber] != function) {
                zxlogf(ERROR, "usb_func_set_interface: bInterfaceNumber %u\n",
                       desc->bInterfaceNumber);
                return ZX_ERR_INVALID_ARGS;
            }
            if (desc->bAlternateSetting == 0) {
                if (*out_num_interfaces == UINT8_MAX) {
                    return ZX_ERR_INVALID_ARGS;
                }
                (*out_num_interfaces)++;
            }
        } else if (header->bDescriptorType == USB_DT_ENDPOINT) {
            auto* desc = reinterpret_cast<const usb_endpoint_descriptor_t*>(header);
            auto index = EpAddressToIndex(desc->bEndpointAddress);
            if (index == 0 || index >= countof(endpoint_map_) ||
                endpoint_map_[index] != function) {
                zxlogf(ERROR, "usb_func_set_interface: bad endpoint address 0x%X\n",
                       desc->bEndpointAddress);
                return ZX_ERR_INVALID_ARGS;
            }
        }

        if (header->bLength == 0) {
            zxlogf(ERROR, "usb_func_set_interface: zero length descriptor\n");
            return ZX_ERR_INVALID_ARGS;
        }
        header = reinterpret_cast<const usb_descriptor_header_t*>(
            reinterpret_cast<const uint8_t*>(header) + header->bLength);
    }

    return ZX_OK;
}

zx_status_t UsbPeripheral::FunctionRegistered() {
    fbl::AutoLock lock(&lock_);

    if (config_desc_.size() != 0) {
        zxlogf(ERROR, "%s: already have configuration descriptor!\n", __func__);
        return ZX_ERR_BAD_STATE;
    }

    // Check to see if we have all our functions registered.
    // If so, we can build our configuration descriptor and tell the DCI driver we are ready.
    size_t length = sizeof(usb_configuration_descriptor_t);
    for (size_t i = 0; i < functions_.size(); i++) {
        auto* function = functions_[i].get();
        size_t descriptors_length;
        if (function->GetDescriptors(&descriptors_length) != nullptr) {
            length += descriptors_length;
        } else {
            // Need to wait for more functions to register.
            return ZX_OK;
        }
    }

    // build our configuration descriptor
    fbl::AllocChecker ac;
    auto* config_desc_bytes = new (&ac) uint8_t[length];
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    auto* config_desc = reinterpret_cast<usb_configuration_descriptor_t*>(config_desc_bytes);

    config_desc->bLength = sizeof(*config_desc);
    config_desc->bDescriptorType = USB_DT_CONFIG;
    config_desc->wTotalLength = htole16(length);
    config_desc->bNumInterfaces = 0;
    config_desc->bConfigurationValue = 1;
    config_desc->iConfiguration = 0;
    // TODO(voydanoff) add a way to configure bmAttributes and bMaxPower
    config_desc->bmAttributes = USB_CONFIGURATION_SELF_POWERED | USB_CONFIGURATION_RESERVED_7;
    config_desc->bMaxPower = 0;

    uint8_t* dest = reinterpret_cast<uint8_t*>(config_desc + 1);
    for (size_t i = 0; i < functions_.size(); i++) {
        auto* function = functions_[i].get();
        size_t descriptors_length;
        auto* descriptors = function->GetDescriptors(&descriptors_length);
        memcpy(dest, descriptors, descriptors_length);
        dest += descriptors_length;
        config_desc->bNumInterfaces = static_cast<uint8_t>(config_desc->bNumInterfaces +
                                                           function->GetNumInterfaces());
    }
    config_desc_.reset(config_desc_bytes, length);

    zxlogf(TRACE, "usb_device_function_registered functions_registered = true\n");
    functions_registered_ = true;
    if (listener_) {
        ::llcpp::fuchsia::hardware::usb::peripheral::Events::Call::FunctionRegistered(
            zx::unowned_channel(listener_.get()));
    }
    return DeviceStateChanged();
}

zx_status_t UsbPeripheral::AllocInterface(fbl::RefPtr<UsbFunction> function,
                                          uint8_t* out_intf_num) {
    fbl::AutoLock lock(&lock_);

    for (uint8_t i = 0; i < countof(interface_map_); i++) {
        if (interface_map_[i] == nullptr) {
            interface_map_[i] = function;
            *out_intf_num = i;
            return ZX_OK;
        }
    }

    return ZX_ERR_NO_RESOURCES;
}

zx_status_t UsbPeripheral::AllocEndpoint(fbl::RefPtr<UsbFunction> function, uint8_t direction,
                                         uint8_t* out_address) {
    uint8_t start, end;

    if (direction == USB_DIR_OUT) {
        start = OUT_EP_START;
        end = OUT_EP_END;
    } else if (direction == USB_DIR_IN) {
        start = IN_EP_START;
        end = IN_EP_END;
    } else {
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::AutoLock lock(&lock_);
    for (uint8_t index = start; index <= end; index++) {
        if (endpoint_map_[index] == nullptr) {
            endpoint_map_[index] = function;
            *out_address = EpIndexToAddress(index);
            return ZX_OK;
        }
    }

    return ZX_ERR_NO_RESOURCES;
}

zx_status_t UsbPeripheral::GetDescriptor(uint8_t request_type, uint16_t value, uint16_t index,
                                         void* buffer, size_t length, size_t* out_actual) {
    uint8_t type = request_type & USB_TYPE_MASK;

    if (type != USB_TYPE_STANDARD) {
        zxlogf(ERROR, "%s unsupported value: %d index: %d\n", __func__, value, index);
        return ZX_ERR_NOT_SUPPORTED;
    }

    fbl::AutoLock lock(&lock_);

    auto desc_type = static_cast<uint8_t>(value >> 8);
    if (desc_type == USB_DT_DEVICE && index == 0) {
        if (device_desc_.bLength == 0) {
            zxlogf(ERROR, "%s: device descriptor not set\n", __func__);
            return ZX_ERR_INTERNAL;
        }
        if (length > sizeof(device_desc_)) length = sizeof(device_desc_);
        memcpy(buffer, &device_desc_, length);
        *out_actual = length;
        return ZX_OK;
    } else if (desc_type == USB_DT_CONFIG && index == 0) {
        if (config_desc_.size() == 0) {
            zxlogf(ERROR, "%s: configuration descriptor not set\n", __func__);
            return ZX_ERR_INTERNAL;
        }
        auto desc_length = config_desc_.size();
        if (length > desc_length) {
            length = desc_length;
        }
        memcpy(buffer, config_desc_.get(), length);
        *out_actual = length;
        return ZX_OK;
    }
    else if (value >> 8 == USB_DT_STRING) {
        uint8_t desc[255];
        auto* header = reinterpret_cast<usb_descriptor_header_t*>(desc);
        header->bDescriptorType = USB_DT_STRING;

        auto string_index = static_cast<uint8_t>(value & 0xFF);
        if (string_index == 0) {
            // special case - return language list
            header->bLength = 4;
            desc[2] = 0x09;     // language ID
            desc[3] = 0x04;
        } else {
            // String indices are 1-based.
            string_index--;
            if (string_index >= strings_.size()) {
                return ZX_ERR_INVALID_ARGS;
            }
            const char* string = strings_[string_index].c_str();
            unsigned index = 2;

            // convert ASCII to UTF16
            if (string) {
                while (*string && index < sizeof(desc) - 2) {
                    desc[index++] = *string++;
                    desc[index++] = 0;
                }
            }
            header->bLength = static_cast<uint8_t>(index);
        }

        if (header->bLength < length) length = header->bLength;
        memcpy(buffer, desc, length);
        *out_actual = length;
        return ZX_OK;
    }

    zxlogf(ERROR, "%s unsupported value: %d index: %d\n", __func__, value, index);
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t UsbPeripheral::SetConfiguration(uint8_t configuration) {
    bool configured = configuration > 0;

    fbl::AutoLock lock(&lock_);

    for (size_t i = 0; i < functions_.size(); i++) {
        auto* function = functions_[i].get();
        auto status = function->SetConfigured(configured, speed_);
        if (status != ZX_OK && configured) {
            return status;
        }
    }

    configuration_ = configuration;

    return ZX_OK;
}

zx_status_t UsbPeripheral::SetInterface(uint8_t interface, uint8_t alt_setting) {
    if (interface >= countof(interface_map_)) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    auto function = interface_map_[interface];
    if (function != nullptr) {
        return function->SetInterface(interface, alt_setting);
    }
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t UsbPeripheral::AddFunction(FunctionDescriptor desc) {
    fbl::AutoLock lock(&lock_);

    if (functions_bound_) {
        return ZX_ERR_BAD_STATE;
    }

    fbl::AllocChecker ac;
    auto function = fbl::MakeRefCountedChecked<UsbFunction>(&ac, zxdev(), this, std::move(desc));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    functions_.push_back(function);

    return ZX_OK;
}

zx_status_t UsbPeripheral::BindFunctions() {
    fbl::AutoLock lock(&lock_);
    if (functions_bound_) {
        zxlogf(ERROR, "%s: already bound!\n", __func__);
        return ZX_ERR_BAD_STATE;
    }

    if (device_desc_.bLength == 0) {
        zxlogf(ERROR, "%s: device descriptor not set\n", __func__);
        return ZX_ERR_BAD_STATE;
    }
    if (functions_.size() == 0) {
        zxlogf(ERROR, "%s: no functions to bind\n", __func__);
        return ZX_ERR_BAD_STATE;
    }

    zxlogf(TRACE, "%s: functions_bound = true\n", __func__);
    functions_bound_ = true;
    return DeviceStateChanged();
}

zx_status_t UsbPeripheral::ClearFunctions() {
    fbl::AutoLock lock(&lock_);
    shutting_down_ = true;
    for (size_t i = 0; i < 256; i++) {
        dci_.CancelAll(static_cast<uint8_t>(i));
    }
    for (size_t i = 0; i < functions_.size(); i++) {
        auto* function = functions_[i].get();
        if (function->zxdev()) {
            function->DdkRemove();
        }
    }
    shutting_down_ = false;
    functions_.reset();
    config_desc_.reset();
    functions_bound_ = false;
    functions_registered_ = false;

    for (size_t i = 0; i < countof(interface_map_); i++) {
        interface_map_[i].reset();
    }
    for (size_t i = 0; i < countof(endpoint_map_); i++) {
        endpoint_map_[i].reset();
    }
    strings_.reset();

    return DeviceStateChanged();
}

zx_status_t UsbPeripheral::AddFunctionDevices() {
    zxlogf(TRACE, "%s\n", __func__);
    if (function_devs_added_) {
        return ZX_OK;
    }

    for (unsigned i = 0; i < functions_.size(); i++) {
        auto function = functions_[i];
        char name[16];
        snprintf(name, sizeof(name), "function-%03u", i);

        auto& desc = function->GetFunctionDescriptor();

        zx_device_prop_t props[] = {
            { BIND_PROTOCOL, 0, ZX_PROTOCOL_USB_FUNCTION },
            { BIND_USB_CLASS, 0, desc.interface_class },
            { BIND_USB_SUBCLASS, 0, desc.interface_subclass },
            { BIND_USB_PROTOCOL, 0, desc.interface_protocol },
            { BIND_USB_VID, 0, device_desc_.idVendor },
            { BIND_USB_PID, 0, device_desc_.idProduct },
        };

        auto status = function->DdkAdd(name, 0, props, countof(props));
        if (status != ZX_OK) {
            zxlogf(ERROR, "usb_dev_bind_functions add_device failed %d\n", status);
            return status;
        }
        // Hold a reference while devmgr has a pointer to the function.
        function->AddRef();
    }

    function_devs_added_ = true;
    return ZX_OK;
}

void UsbPeripheral::RemoveFunctionDevices() {
    zxlogf(TRACE, "%s\n", __func__);

    for (size_t i = 0; i < functions_.size(); i++) {
        auto* function = functions_[i].get();
        // Here we remove the function from the DDK device tree,
        // but the storage for the function remains on our function list.
        function->DdkRemove();
    }

    config_desc_.reset();
    functions_registered_ = false;
    function_devs_added_ = false;
}

zx_status_t UsbPeripheral::DeviceStateChanged() {
    zxlogf(TRACE, "%s usb_mode: %d dci_usb_mode: %d\n", __func__, usb_mode_, dci_usb_mode_);

    usb_mode_t new_dci_usb_mode = dci_usb_mode_;
    bool add_function_devs = (usb_mode_ == USB_MODE_PERIPHERAL && functions_bound_);
    zx_status_t status = ZX_OK;

    if (usb_mode_ == USB_MODE_PERIPHERAL) {
        if (functions_registered_) {
            // switch DCI to device mode
            new_dci_usb_mode = USB_MODE_PERIPHERAL;
        } else {
            new_dci_usb_mode = USB_MODE_NONE;
        }
    } else {
        new_dci_usb_mode = usb_mode_;
    }

    if (add_function_devs) {
        // publish child devices if necessary
        if (!function_devs_added_) {
            status = AddFunctionDevices();
            if (status != ZX_OK) {
                return status;
            }
        }
    }

    if (dci_usb_mode_ != new_dci_usb_mode) {
        zxlogf(TRACE, "%s: set DCI mode %d\n", __func__, new_dci_usb_mode);
        if (ums_.is_valid()) {
            status = ums_.SetMode(new_dci_usb_mode);
            if (status != ZX_OK) {
                ums_.SetMode(USB_MODE_NONE);
                new_dci_usb_mode = USB_MODE_NONE;
            }
        }
        dci_usb_mode_ = new_dci_usb_mode;
    }

    if (!add_function_devs && function_devs_added_) {
        RemoveFunctionDevices();
    }

    return status;
}

zx_status_t UsbPeripheral::UsbDciInterfaceControl(const usb_setup_t* setup,
                                                  const void* write_buffer, size_t write_size,
                                                  void* read_buffer, size_t read_size,
                                                  size_t* out_read_actual) {
    uint8_t request_type = setup->bmRequestType;
    uint8_t direction = request_type & USB_DIR_MASK;
    uint8_t request = setup->bRequest;
    uint16_t value = le16toh(setup->wValue);
    uint16_t index = le16toh(setup->wIndex);
    uint16_t length = le16toh(setup->wLength);

    if (direction == USB_DIR_IN && length > read_size) {
        return ZX_ERR_BUFFER_TOO_SMALL;
    } else if (direction == USB_DIR_OUT && length > write_size) {
        return ZX_ERR_BUFFER_TOO_SMALL;
    }
    if ((write_size > 0 && write_buffer == NULL) || (read_size > 0 && read_buffer == NULL)) {
        return ZX_ERR_INVALID_ARGS;
    }

    zxlogf(TRACE, "usb_dev_control type: 0x%02X req: %d value: %d index: %d length: %d\n",
            request_type, request, value, index, length);

    switch (request_type & USB_RECIP_MASK) {
    case USB_RECIP_DEVICE:
        // handle standard device requests
        if ((request_type & (USB_DIR_MASK | USB_TYPE_MASK)) == (USB_DIR_IN | USB_TYPE_STANDARD) &&
            request == USB_REQ_GET_DESCRIPTOR) {
            return GetDescriptor(request_type, value, index, read_buffer, length, out_read_actual);
        } else if (request_type == (USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
                   request == USB_REQ_SET_CONFIGURATION && length == 0) {
            return SetConfiguration(static_cast<uint8_t>(value));
        } else if (request_type == (USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
                   request == USB_REQ_GET_CONFIGURATION && length > 0) {
            *static_cast<uint8_t*>(read_buffer) = configuration_;
            *out_read_actual = sizeof(uint8_t);
            return ZX_OK;
        } else {
            // Delegate to one of the function drivers.
            // USB_RECIP_DEVICE should only be used when there is a single active interface.
            // But just to be conservative, try all the available interfaces.
            for (size_t i = 0; i < countof(interface_map_); i++) {
                auto function = interface_map_[index];
                if (function != nullptr) {
                    auto status = function->Control(setup, write_buffer, write_size, read_buffer,
                                                    read_size, out_read_actual);
                    if (status == ZX_OK) {
                        return ZX_OK;
                    }
                }
            }
        }
        break;
    case USB_RECIP_INTERFACE: {
        if (request_type == (USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE) &&
                   request == USB_REQ_SET_INTERFACE && length == 0) {
            return SetInterface(static_cast<uint8_t>(index), static_cast<uint8_t>(value));
        } else {
            if (index >= countof(interface_map_)) {
                return ZX_ERR_OUT_OF_RANGE;
            }
            // delegate to the function driver for the interface
            auto function = interface_map_[index];
            if (function != nullptr) {
                return function->Control(setup, write_buffer, write_size, read_buffer, read_size,
                                         out_read_actual);
            }
        }
        break;
    }
    case USB_RECIP_ENDPOINT: {
        // delegate to the function driver for the endpoint
        index = EpAddressToIndex(static_cast<uint8_t>(index));
        if (index == 0 || index >= USB_MAX_EPS) {
            return ZX_ERR_INVALID_ARGS;
        }
        if (index >= countof(endpoint_map_)) {
            return ZX_ERR_OUT_OF_RANGE;
        }
        auto function = endpoint_map_[index];
        if (function != nullptr) {
            return function->Control(setup, write_buffer, write_size, read_buffer, read_size,
                                     out_read_actual);
        }
        break;
    }
    case USB_RECIP_OTHER:
        // TODO(voydanoff) - how to handle this?
    default:
        break;
    }

    return ZX_ERR_NOT_SUPPORTED;
}

void UsbPeripheral::UsbDciInterfaceSetConnected(bool connected) {
    bool was_connected = connected;
    {
      fbl::AutoLock lock(&lock_);
      std::swap(connected_, was_connected);
    }

    if (was_connected != connected) {
        if (!connected) {
            for (size_t i = 0; i < functions_.size(); i++) {
                auto* function = functions_[i].get();
                function->SetConfigured(false, USB_SPEED_UNDEFINED);
            }
        }
    }
}

void UsbPeripheral::UsbDciInterfaceSetSpeed(usb_speed_t speed) {
    speed_ = speed;
}

void UsbPeripheral::SetDeviceDescriptor(DeviceDescriptor desc,
                                        SetDeviceDescriptorCompleter::Sync completer) {
    zx_status_t status;
    if (desc.bNumConfigurations != 1) {
        zxlogf(ERROR, "usb_device_ioctl: bNumConfigurations: %u, only 1 supported\n",
                desc.bNumConfigurations);
        status = ZX_ERR_INVALID_ARGS;
    } else {
        device_desc_.bLength = sizeof(usb_device_descriptor_t);
        device_desc_.bDescriptorType = USB_DT_DEVICE;
        device_desc_.bcdUSB = desc.bcdUSB;
        device_desc_.bDeviceClass = desc.bDeviceClass;
        device_desc_.bDeviceSubClass = desc.bDeviceSubClass;
        device_desc_.bDeviceProtocol = desc.bDeviceProtocol;
        device_desc_.bMaxPacketSize0 = desc.bMaxPacketSize0;
        device_desc_.idVendor = desc.idVendor;
        device_desc_.idProduct = desc.idProduct;
        device_desc_.bcdDevice = desc.bcdDevice;
        device_desc_.iManufacturer = desc.iManufacturer;
        device_desc_.iProduct = desc.iProduct;
        device_desc_.iSerialNumber = desc.iSerialNumber;
        device_desc_.bNumConfigurations = desc.bNumConfigurations;
        status = ZX_OK;
    }

    completer.Reply(status);
}

void UsbPeripheral::AllocStringDesc(fidl::StringView name,
                                    AllocStringDescCompleter::Sync completer) {
    uint8_t index = 0;
    auto status = AllocStringDesc(fbl::String(name.data(), name.size()), &index);
    completer.Reply(status, index);
}

void UsbPeripheral::AddFunction(FunctionDescriptor desc, AddFunctionCompleter::Sync completer) {
    completer.Reply(AddFunction(std::move(desc)));
}

void UsbPeripheral::BindFunctions(BindFunctionsCompleter::Sync completer) {
    completer.Reply(BindFunctions());
}

void UsbPeripheral::ClearFunctions(ClearFunctionsCompleter::Sync completer) {
    zxlogf(TRACE, "%s\n", __func__);

    completer.Reply(ClearFunctions());
}

void UsbPeripheral::GetMode(GetModeCompleter::Sync completer) {
    fbl::AutoLock lock(&lock_);

    uint32_t mode = usb_mode_;

    completer.Reply(ZX_OK, mode);
}

int UsbPeripheral::ListenerCleanupThread() {
    zx_signals_t observed = 0;
    listener_.wait_one(ZX_CHANNEL_PEER_CLOSED | __ZX_OBJECT_HANDLE_CLOSED, zx::time::infinite(),
                       &observed);
    fbl::AutoLock l(&lock_);
    listener_.reset();
    return 0;
}

void UsbPeripheral::SetStateChangeListener(zx::channel listener,
                                           SetStateChangeListenerCompleter::Sync completer) {
    // This code is wrapped in a loop
    // to prevent a race condition in the event that multiple
    // clients try to set the handle at once.
    while (1) {
        fbl::AutoLock lock(&lock_);
        if (listener_.is_valid() && thread_) {
            thrd_t thread = thread_;
            thread_ = 0;
            lock.release();
            int output;
            thrd_join(thread, &output);
            continue;
        }
        if (listener_.is_valid()) {
            completer.Close(ZX_ERR_BAD_STATE);
            return;
        }
        if (thread_) {
            int output;
            thrd_t thread = thread_;
            thread_ = 0;
            lock.release();
            // We now own the thread, but not the listener.
            thrd_join(thread, &output);
            // Go back and try to re-set the listener_.
            // another caller may have tried to do this while we were blocked on thrd_join.
            continue;
        }
        listener_ = std::move(listener);
        if (thrd_create(
                &thread_,
                [](void* arg) -> int {
                    return reinterpret_cast<UsbPeripheral*>(arg)->ListenerCleanupThread();
                },
                reinterpret_cast<void*>(this)) != thrd_success) {
            listener_.reset();
            completer.Close(ZX_ERR_INTERNAL);
            return;
        }
        return;
    }
}

void UsbPeripheral::SetMode(uint32_t mode, SetModeCompleter::Sync completer) {
    fbl::AutoLock lock(&lock_);
    usb_mode_ = mode;
    completer.Reply(DeviceStateChanged());
}

zx_status_t UsbPeripheral::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
    DdkTransaction transaction(txn);
    ::llcpp::fuchsia::hardware::usb::peripheral::Device::Dispatch(this, msg, &transaction);
    return transaction.Status();
}

void UsbPeripheral::DdkUnbind() {
    zxlogf(TRACE, "%s\n", __func__);
    ClearFunctions();
    DdkRemove();
}

void UsbPeripheral::DdkRelease() {
    zxlogf(TRACE, "%s\n", __func__);
    {
        fbl::AutoLock l(&lock_);
        if (listener_) {
            listener_.reset();
        }
    }
    if (thread_) {
        int output;
        thrd_join(thread_, &output);
        thread_ = 0;
    }
    delete this;
}

zx_status_t UsbPeripheral::SetDefaultConfig(FunctionDescriptor* descriptors, size_t length) {
    device_desc_.bLength = sizeof(usb_device_descriptor_t),
    device_desc_.bDescriptorType = USB_DT_DEVICE;
    device_desc_.bcdUSB = htole16(0x0200);
    device_desc_.bDeviceClass = 0;
    device_desc_.bDeviceSubClass = 0;
    device_desc_.bDeviceProtocol = 0;
    device_desc_.bMaxPacketSize0 = 64;
    device_desc_.bcdDevice = htole16(0x0100);
    device_desc_.bNumConfigurations = 1;

    zx_status_t status = ZX_OK;
    for (size_t i = 0; i < length; i++) {
        status = AddFunction(std::move(*(descriptors + i)));
        if (status != ZX_OK) {
            return status;
        }
    }

    if (status != ZX_OK)
        return status;

    return BindFunctions();
}

static constexpr zx_driver_ops_t ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = UsbPeripheral::Create;
    return ops;
}();

} // namespace usb_peripheral

// clang-format off
ZIRCON_DRIVER_BEGIN(usb_device, usb_peripheral::ops, "zircon", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_USB_DCI),
ZIRCON_DRIVER_END(usb_device)
