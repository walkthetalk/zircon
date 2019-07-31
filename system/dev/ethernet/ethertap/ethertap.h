// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_ETHERNET_ETHERTAP_ETHERTAP_H_
#define ZIRCON_SYSTEM_DEV_ETHERNET_ETHERTAP_ETHERTAP_H_

#include <ddk/device.h>
#include <ddk/protocol/test.h>
#include <ddktl/device.h>
#include <ddktl/protocol/ethernet.h>
#include <ddktl/protocol/test.h>
#include <fbl/mutex.h>
#include <fbl/unique_ptr.h>
#include <fuchsia/hardware/ethertap/c/fidl.h>
#include <lib/zx/socket.h>
#include <threads.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

namespace eth {

class TapCtl : public ddk::Device<TapCtl, ddk::Messageable> {
public:
    TapCtl(zx_device_t* device);

    static zx_status_t Create(void* ctx, zx_device_t* parent);
    void DdkRelease();
    zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn);
    zx_status_t
    OpenDevice(const char* name, const fuchsia_hardware_ethertap_Config* config, zx::channel device);
};

class TapDevice : public ddk::Device<TapDevice, ddk::Unbindable>,
                  public ddk::EthernetImplProtocol<TapDevice, ddk::base_protocol> {
public:
    TapDevice(zx_device_t* device,
              const fuchsia_hardware_ethertap_Config* config,
              zx::channel server);

    void DdkRelease();
    void DdkUnbind();

    zx_status_t EthernetImplQuery(uint32_t options, ethernet_info_t* info);
    void EthernetImplStop();
    zx_status_t EthernetImplStart(const ethernet_ifc_protocol_t* ifc);
    zx_status_t EthernetImplQueueTx(uint32_t options, ethernet_netbuf_t* netbuf);
    zx_status_t EthernetImplSetParam(uint32_t param, int32_t value, const void* data,
                               size_t data_size);
    // No DMA capability, so return invalid handle for get_bti
    void EthernetImplGetBti(zx::bti* bti);
    int Thread();
    zx_status_t Reply(zx_txid_t, const fidl_msg_t* msg);

    zx_status_t Recv(const uint8_t* buffer, uint32_t length);
    void UpdateLinkStatus(bool online);

private:
    // ethertap options
    uint32_t options_ = 0;

    // ethermac fields
    uint32_t features_ = 0;
    uint32_t mtu_ = 0;
    uint8_t mac_[6] = {};

    fbl::Mutex lock_;
    bool dead_ = false;
    ddk::EthernetIfcProtocolClient ethernet_client_ __TA_GUARDED(lock_);

    // Only accessed from Thread, so not locked.
    bool online_ = false;
    zx::channel channel_;

    thrd_t thread_;
};

} // namespace eth

#endif // ZIRCON_SYSTEM_DEV_ETHERNET_ETHERTAP_ETHERTAP_H_
