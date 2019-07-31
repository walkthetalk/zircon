// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/protocol/usb.h>
#include <ddk/protocol/usb/composite.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>
#include <zircon/hw/usb.h>
#ifdef __cplusplus
#include <ddktl/protocol/usb.h>
#include <optional>
#endif

__BEGIN_CDECLS

// helper function for claiming additional interfaces that satisfy the want_interface predicate,
// want_interface will be passed the supplied arg
zx_status_t usb_claim_additional_interfaces(
    usb_composite_protocol_t* comp,
    bool (*want_interface)(usb_interface_descriptor_t*, void*),
    void* arg);

// Utilities for iterating through descriptors within a device's USB configuration descriptor
typedef struct {
    uint8_t* desc;     // start of configuration descriptor
    uint8_t* desc_end; // end of configuration descriptor
    uint8_t* current;  // current position in configuration descriptor
} usb_desc_iter_t;

// initializes a usb_desc_iter_t
zx_status_t usb_desc_iter_init(usb_protocol_t* usb, usb_desc_iter_t* iter);

// Clones a usb_desc_iter_t
zx_status_t usb_desc_iter_clone(const usb_desc_iter_t* src, usb_desc_iter_t* dest);

// releases resources in a usb_desc_iter_t
void usb_desc_iter_release(usb_desc_iter_t* iter);

// resets iterator to the beginning
void usb_desc_iter_reset(usb_desc_iter_t* iter);

// returns the next descriptor
usb_descriptor_header_t* usb_desc_iter_next(usb_desc_iter_t* iter);

// returns the next descriptor without incrementing the iterator
usb_descriptor_header_t* usb_desc_iter_peek(usb_desc_iter_t* iter);

// returns the next interface descriptor, optionally skipping alternate interfaces
usb_interface_descriptor_t* usb_desc_iter_next_interface(usb_desc_iter_t* iter, bool skip_alt);

// returns the next endpoint descriptor within the current interface
usb_endpoint_descriptor_t* usb_desc_iter_next_endpoint(usb_desc_iter_t* iter);

// returns the next ss-companion descriptor within the current interface
usb_ss_ep_comp_descriptor_t* usb_desc_iter_next_ss_ep_comp(usb_desc_iter_t* iter);

static inline zx_status_t usb_get_descriptor(const usb_protocol_t* usb, uint8_t request_type,
                                             uint16_t type, uint16_t index, void* data,
                                             size_t length, zx_time_t timeout, size_t* out_length) {
    return usb_control_in(usb, request_type | USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                          (uint16_t)(type << 8 | index), 0, timeout, data, length, out_length);
}

static inline zx_status_t usb_get_status(const usb_protocol_t* usb, uint8_t request_type,
                                         uint16_t index, void* data, size_t length,
                                         zx_time_t timeout, size_t* out_length) {
    return usb_control_in(usb, request_type | USB_DIR_IN, USB_REQ_GET_STATUS, 0, index, timeout,
                          data, length, out_length);
}

static inline zx_status_t usb_set_feature(const usb_protocol_t* usb, uint8_t request_type,
                                          uint16_t feature, uint16_t index, zx_time_t timeout) {
    return usb_control_out(usb, request_type, USB_REQ_SET_FEATURE, feature, index, timeout,
                           NULL, 0);
}

static inline zx_status_t usb_clear_feature(const usb_protocol_t* usb, uint8_t request_type,
                                            uint16_t feature, uint16_t index, zx_time_t timeout) {
    return usb_control_out(usb, request_type, USB_REQ_CLEAR_FEATURE, feature, index, timeout,
                           NULL, 0);
}

__END_CDECLS

#ifdef __cplusplus
namespace usb {

typedef struct {
    usb_endpoint_descriptor_t descriptor;
    usb_ss_ep_comp_descriptor_t ss_companion;
    // True if ss_companion is populated.
    bool has_companion;
} usb_iter_endpoint_descriptor_t;

class UsbDevice : public ddk::UsbProtocolClient {
public:
    UsbDevice() {}
    UsbDevice(const usb_protocol_t* proto)
        : UsbProtocolClient(proto) {}

    UsbDevice(zx_device_t* parent)
        : UsbProtocolClient(parent) {
    }
    zx_status_t ClearFeature(uint8_t request_type,
                             uint16_t feature, uint16_t index, zx_time_t timeout) {
        usb_protocol_t proto;
        GetProto(&proto);
        return usb_clear_feature(&proto, request_type, feature, index, timeout);
    }
    zx_status_t GetDescriptor(uint8_t request_type,
                              uint16_t type, uint16_t index, void* data,
                              size_t length, zx_time_t timeout, size_t* out_length) {
        usb_protocol_t proto;
        GetProto(&proto);
        return usb_get_descriptor(&proto, request_type, type, index, data, length, timeout,
                                  out_length);
    }
    zx_status_t GetStatus(uint8_t request_type,
                          uint16_t index, void* data, size_t length,
                          zx_time_t timeout, size_t* out_length) {
        usb_protocol_t proto;
        GetProto(&proto);
        return usb_get_status(&proto, request_type, index, data, length, timeout, out_length);
    }
    zx_status_t SetFeature(int8_t request_type,
                           uint16_t feature, uint16_t index, zx_time_t timeout) {
        usb_protocol_t proto;
        GetProto(&proto);
        return usb_set_feature(&proto, request_type, feature, index, timeout);
    }
};

// Interface is owned by an iterator of an InterfaceList.  It is possible to enumerate all USB
// endpoint descriptors by using the standard iterator interface, or a for loop such as
// for (auto endpoint:interface) { ... } where interface is an instance of an Interface.
// Interfaces must not outlive their original InterfaceLists.
class Interface {
private:
    class iterator_impl;

public:
    using iterator = iterator_impl;
    using const_iterator = iterator_impl;

    Interface() = delete;

    const usb_interface_descriptor_t* descriptor() const { return descriptor_; }

    iterator begin() const;
    const_iterator cbegin() const;
    iterator end() const;
    const_iterator cend() const;

    friend class InterfaceList;

private:
    Interface(const usb_desc_iter_t& iter, const usb_interface_descriptor_t* descriptor)
        : iter_(iter),
          descriptor_(descriptor) {}

    class iterator_impl {
    public:
        friend class Interface;

        iterator_impl(const usb_desc_iter_t& iter, const usb_iter_endpoint_descriptor_t& endpoint)
            : iter_(iter),
              endpoint_(endpoint) {}

        bool operator==(const iterator_impl& other) const {
            const usb_endpoint_descriptor_t* a = &endpoint_.descriptor;
            const usb_endpoint_descriptor_t* b = &other.endpoint_.descriptor;
            // Note that within a configuration, endpoint addresses are unique.
            return (a->bEndpointAddress == b->bEndpointAddress);
        }
        bool operator!=(const iterator_impl& other) const { return !(*this == other); }

        iterator_impl operator++(int) {
            iterator_impl ret(*this);
            ++(*this);
            return ret;
        }

        iterator_impl& operator++() {
            endpoint_ = {};
            ReadEp(&iter_, &endpoint_);
            return *this;
        }

        const usb_iter_endpoint_descriptor_t* endpoint() const { return &endpoint_; }
        const usb_iter_endpoint_descriptor_t& operator*() const { return endpoint_; }
        const usb_iter_endpoint_descriptor_t* operator->() const { return &endpoint_; }

    private:
        // Using the given iter, read the next endpoint descriptor(s).
        static void ReadEp(usb_desc_iter_t* iter, usb_iter_endpoint_descriptor_t* out);

        usb_desc_iter_t iter_;
        usb_iter_endpoint_descriptor_t endpoint_;
    };

    // Advances iter_ to the next usb_interface_descriptor_t.
    void Next(bool skip_alt);

    usb_desc_iter_t iter_;
    const usb_interface_descriptor_t* descriptor_;
};

// An InterfaceList can be used for enumerating USB interfaces and endpoints.  It implements a
// standard C++ iterator interface, which can be used with a for loop. such as
// for(interface:interface_list) { ... }.
//
// The InterfaceList will enumerate interfaces in the client, and will skip any alternate interfaces
// if skip_alt is true (see page 268 of the USB 2.0 specification for more information).
//
// Example Usage #1:
//     std::optional<InterfaceList> my_list;
//     status = InterfaceList::Create(my_client, true, &my_list);
//     if (status != ZX_OK) {
//         ...
//     }
//
//     for (auto& interface : *my_list) {
//         for (auto& endpoint : interface) {
//             ...
//         }
//     }
//
// Example Usage #2:
//     std::optional<InterfaceList> my_list;
//     status = InterfaceList::Create(my_client, true, &my_list);
//     if (status != ZX_OK) {
//         ...
//     }
//
//     auto my_iter = my_list->begin(); // or cbegin().
//     do {
//         auto ep_iter = my_iter->begin();
//         do {
//             ...
//         } while (++ep_iter != my_iter->end())
//     } while (++my_iter != my_list->end())
class InterfaceList {
private:
    class iterator_impl;

public:
    using iterator = iterator_impl;
    using const_iterator = iterator_impl;

    InterfaceList() = delete;

    InterfaceList(const usb_desc_iter_t& iter, bool skip_alt)
        : iter_(iter),
          skip_alt_(skip_alt) {}

    InterfaceList(InterfaceList&&) = delete;
    InterfaceList& operator=(InterfaceList&&) = delete;

    ~InterfaceList() {
        if (iter_.desc) {
            usb_desc_iter_release(&iter_);
        }
    }

    static zx_status_t Create(const ddk::UsbProtocolClient& client, bool skip_alt,
                              std::optional<InterfaceList>* out);

    iterator begin() const;
    const_iterator cbegin() const;
    iterator end() const;
    const_iterator cend() const;

private:
    class iterator_impl {
    public:
        iterator_impl(const usb_desc_iter_t& iter, bool skip_alt,
                      const usb_interface_descriptor_t* descriptor)
            : skip_alt_(skip_alt),
              interface_(iter, descriptor) {}

        bool operator==(const iterator_impl& other) const {
            return interface_.descriptor_ == other.interface_.descriptor_;
        }
        bool operator!=(const iterator_impl& other) const { return !(*this == other); }

        iterator_impl operator++(int) {
            iterator_impl ret(*this);
            ++(*this);
            return ret;
        }

        iterator_impl& operator++() {
            interface_.Next(skip_alt_);
            return *this;
        }

        const Interface* get() const { return &interface_; }
        const Interface& operator*() const { return interface_; }
        const Interface* operator->() const { return &interface_; }

    private:
        const bool skip_alt_;
        Interface interface_;
    };

    usb_desc_iter_t iter_{};
    bool skip_alt_;
};

} // namespace usb
#endif
