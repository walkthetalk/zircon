// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>
#include <ddktl/protocol/usb/dci.h>
#include <ddktl/protocol/usb/function.h>
#include <ddktl/protocol/usb/modeswitch.h>
#include <fbl/array.h>
#include <fbl/mutex.h>
#include <fbl/ref_ptr.h>
#include <fbl/string.h>
#include <fbl/vector.h>
#include <fuchsia/hardware/usb/peripheral/llcpp/fidl.h>
#include <lib/zx/channel.h>
#include <usb/request-cpp.h>

/*
    THEORY OF OPERATION

    This driver is responsible for USB in the peripheral role, that is,
    acting as a USB device to a USB host.
    It serves as the central point of coordination for the peripheral role.
    It is configured via ioctls in the fuchsia.hardware.usb.peripheral FIDL interface
    (which is used by the usbctl command line program).
    Based on this configuration, it creates one or more devmgr devices with protocol
    ZX_PROTOCOL_USB_FUNCTION. These devices are bind points for USB function drivers,
    which implement USB interfaces for particular functions (like USB ethernet or mass storage).
    This driver also binds to a device with protocol ZX_PROTOCOL_USB_DCI
    (Device Controller Interface) which is implemented by a driver for the actual
    USB controller hardware for the peripheral role.

    There are several steps needed to initialize and start USB in the peripheral role.
    The first step is setting up the USB configuration via the FIDL interface.
    SetDeviceDescriptor() sets the USB device descriptor to be presented
    to the host during enumeration.
    Next, AddFunction() can be called one or more times to add descriptors for the USB functions
    to be included in the USB configuration.
    Finally after all the functions have been added, BindFunctions() tells this driver
    that configuration is complete and it is now possible to build the configuration descriptor.
    Once we get to this point, UsbPeripheral.functions_bound_ is set to true.

    Independent of this configuration process, the FIDL SetMode() message can be used
    to configure the role of the USB controller. If the role is set to USB_MODE_PERIPHERAL
    and functions_bound_ is true, then we are ready to start USB in peripheral role.
    At this point, we create DDK devices for our list of functions.
    When the function drivers bind to these functions, they register an interface of type
    usb_function_interface_protocol_t with this driver via the usb_function_register() API.
    Once all of the function drivers have registered themselves this way,
    UsbPeripheral.functions_registered_ is set to true.

    if the usb mode is set to USB_MODE_PERIPHERAL and functions_registered_ is true,
    we are now finally ready to operate in the peripheral role.
    At this point we can inform the DCI driver to start running in peripheral role
    by calling usb_mode_switch_set_mode(USB_MODE_PERIPHERAL) on its ZX_PROTOCOL_USB_MODE_SWITCH
    interface. Now the USB controller hardware is up and running as a USB peripheral.

    Teardown of the peripheral role one of two ways:
    First, the FIDL ClearFunctions() message will reset this device's list of USB functions.
    Second, the USB mode can be set to something other than USB_MODE_PERIPHERAL.
    In this second case, we will remove the DDK devices for the USB functions
    so the function drivers will unbind, but the USB configuration remains ready to go
    for when the USB mode is switched back to USB_MODE_PERIPHERAL.
*/

namespace usb_peripheral {

class UsbFunction;

using ::llcpp::fuchsia::hardware::usb::peripheral::DeviceDescriptor;
using ::llcpp::fuchsia::hardware::usb::peripheral::FunctionDescriptor;

class UsbPeripheral;
using UsbPeripheralType = ddk::Device<UsbPeripheral, ddk::Unbindable, ddk::Messageable>;

// This is the main class for the USB peripheral role driver.
// It binds against the USB DCI driver device and manages a list of UsbFunction devices,
// one for each USB function in the peripheral role configuration.
class UsbPeripheral : public UsbPeripheralType,
                      public ddk::EmptyProtocol<ZX_PROTOCOL_USB_PERIPHERAL>,
                      public ddk::UsbDciInterfaceProtocol<UsbPeripheral>,
                      public ::llcpp::fuchsia::hardware::usb::peripheral::Device::Interface {
public:
    UsbPeripheral(zx_device_t* parent) : UsbPeripheralType(parent), dci_(parent), ums_(parent) {}

    static zx_status_t Create(void* ctx, zx_device_t* parent);

    // Device protocol implementation.
    zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn);
    void DdkUnbind();
    void DdkRelease();

    // UsbDciInterface implementation.
    zx_status_t UsbDciInterfaceControl(const usb_setup_t* setup, const void* write_buffer,
                                       size_t write_size, void* out_read_buffer, size_t read_size,
                                       size_t* out_read_actual);
    void UsbDciInterfaceSetConnected(bool connected);
    void UsbDciInterfaceSetSpeed(usb_speed_t speed);

    // FIDL messages
    void SetDeviceDescriptor(DeviceDescriptor desc,
                             SetDeviceDescriptorCompleter::Sync completer) override;
    void AllocStringDesc(fidl::StringView name, AllocStringDescCompleter::Sync completer) override;
    void AddFunction(FunctionDescriptor desc, AddFunctionCompleter::Sync completer) override;
    void BindFunctions(BindFunctionsCompleter::Sync completer) override;
    void ClearFunctions(ClearFunctionsCompleter::Sync completer) override;
    void GetMode(GetModeCompleter::Sync completer) override;
    void SetMode(uint32_t mode, SetModeCompleter::Sync completer) override;
    void SetStateChangeListener(zx::channel listener,
                                SetStateChangeListenerCompleter::Sync completer) override;

    zx_status_t SetFunctionInterface(fbl::RefPtr<UsbFunction> function,
                                     const usb_function_interface_protocol_t* interface);
    zx_status_t AllocInterface(fbl::RefPtr<UsbFunction> function, uint8_t* out_intf_num);
    zx_status_t AllocEndpoint(fbl::RefPtr<UsbFunction> function, uint8_t direction,
                              uint8_t* out_address);
    zx_status_t AllocStringDesc(fbl::String desc, uint8_t* out_index);
    zx_status_t ValidateFunction(fbl::RefPtr<UsbFunction> function, void* descriptors, size_t length,
                                 uint8_t* out_num_interfaces);
    zx_status_t FunctionRegistered();

    inline const ddk::UsbDciProtocolClient& dci() const { return dci_; }
    inline size_t ParentRequestSize() const { return parent_request_size_; }
    void UsbPeripheralRequestQueue(usb_request_t* usb_request,
                                   const usb_request_complete_t* complete_cb);

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(UsbPeripheral);

    static constexpr uint8_t MAX_INTERFACES = 32;
    static constexpr uint8_t MAX_STRINGS = 255;

    // OUT endpoints are in range 1 - 15, IN endpoints are in range 17 - 31.
    static constexpr uint8_t OUT_EP_START = 1;
    static constexpr uint8_t OUT_EP_END = 15;
    static constexpr uint8_t IN_EP_START = 17;
    static constexpr uint8_t IN_EP_END = 31;

    // For mapping bEndpointAddress value to/from index in range 0 - 31.
    static inline uint8_t EpAddressToIndex(uint8_t addr) {
        return static_cast<uint8_t>(((addr) & 0xF) | (((addr) & 0x80) >> 3));
    }
    static inline uint8_t EpIndexToAddress(uint8_t index) {
        return static_cast<uint8_t>(((index) & 0xF) | (((index) & 0x10) << 3));
    }

    zx_status_t Init();
    zx_status_t AddFunction(FunctionDescriptor desc);
    zx_status_t BindFunctions();
    zx_status_t ClearFunctions();
    zx_status_t DeviceStateChanged() __TA_REQUIRES(lock_);
    zx_status_t AddFunctionDevices() __TA_REQUIRES(lock_);
    void RemoveFunctionDevices() __TA_REQUIRES(lock_);
    zx_status_t GetDescriptor(uint8_t request_type, uint16_t value, uint16_t index, void* buffer,
                              size_t length, size_t* out_actual);
    zx_status_t SetConfiguration(uint8_t configuration);
    zx_status_t SetInterface(uint8_t interface, uint8_t alt_setting);
    zx_status_t SetDefaultConfig(FunctionDescriptor* descriptors, size_t length);
    int ListenerCleanupThread();
    void RequestComplete(usb_request_t* req);

    // Our parent's DCI protocol.
    const ddk::UsbDciProtocolClient dci_;
    // Our parent's optional USB switch protocol.
    const ddk::UsbModeSwitchProtocolClient ums_;
    // USB device descriptor set via ioctl_usb_peripheral_set_device_desc()
    usb_device_descriptor_t device_desc_ = {};
    // USB configuration descriptor, synthesized from our functions' descriptors.
    fbl::Array<uint8_t> config_desc_;
    // Map from interface number to function.
    fbl::RefPtr<UsbFunction> interface_map_[MAX_INTERFACES];
    // Map from endpoint index to function.
    fbl::RefPtr<UsbFunction> endpoint_map_[USB_MAX_EPS];
    // Strings for USB string descriptors.
    fbl::Vector<fbl::String> strings_ __TA_GUARDED(lock_);
    // List of usb_function_t.
    fbl::Vector<fbl::RefPtr<UsbFunction>> functions_;
    // mutex for protecting our state
    fbl::Mutex lock_;
    // Current USB mode set via ioctl_usb_peripheral_set_mode()
    usb_mode_t usb_mode_ __TA_GUARDED(lock_) = USB_MODE_NONE;
    // Our parent's USB mode.
    usb_mode_t dci_usb_mode_ __TA_GUARDED(lock_) = USB_MODE_NONE;
    // Set if ioctl_usb_peripheral_bind_functions() has been called
    // and we have a complete list of our function.
    bool functions_bound_ __TA_GUARDED(lock_) = false;
    // True if all our functions have registered their usb_function_interface_protocol_t.
    bool functions_registered_ __TA_GUARDED(lock_) = false;
    // True if we have added child devices for our functions.
    bool function_devs_added_ __TA_GUARDED(lock_) = false;
    // True if we are connected to a host,
    bool connected_ __TA_GUARDED(lock_) = false;
    // True if we are shutting down/clearing functions
    bool shutting_down_ = false;
    // Current configuration number selected via USB_REQ_SET_CONFIGURATION
    // (will be 0 or 1 since we currently do not support multiple configurations).
    uint8_t configuration_;
    // USB connection speed.
    usb_speed_t speed_;
    // Size of our parent's usb_request_t.
    size_t parent_request_size_;
    // Registered listener
    zx::channel listener_;

    thrd_t thread_ = 0;

    bool cache_enabled_ = true;
    bool cache_report_enabled_ = true;

    fbl::Mutex pending_requests_lock_;
    usb::UnownedRequestList<void> pending_requests_ __TA_GUARDED(pending_requests_lock_);
};

} // namespace usb_peripheral
