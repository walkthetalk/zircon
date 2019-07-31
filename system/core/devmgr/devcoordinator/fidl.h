// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_FIDL_H_
#define ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_FIDL_H_

#include <lib/zx/channel.h>
#include <lib/zx/vmo.h>

namespace devmgr {

class CompositeDevice;
class Devhost;
struct Device;

// Methods for composing FIDL RPCs to the devhosts
zx_status_t dh_send_remove_device(const Device* dev);
zx_status_t dh_send_create_device(Device* dev, Devhost* dh, zx::channel rpc, zx::vmo driver,
                                  const char* args, zx::handle rpc_proxy);
zx_status_t dh_send_create_device_stub(Device* dev, Devhost* dh, zx::channel rpc,
                                       uint32_t protocol_id);
zx_status_t dh_send_bind_driver(const Device* dev, const char* libname, zx::vmo driver);
zx_status_t dh_send_connect_proxy(const Device* dev, zx::channel proxy);
zx_status_t dh_send_suspend(const Device* dev, uint32_t flags);
zx_status_t dh_send_unbind(const Device* dev);
zx_status_t dh_send_complete_compatibility_tests(const Device* dev, zx_status_t test_status_);
zx_status_t dh_send_create_composite_device(Devhost* dh, const Device* composite_dev,
                                            const CompositeDevice& composite,
                                            const uint64_t* component_local_ids, zx::channel rpc);

}  // namespace devmgr

#endif  // ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_FIDL_H_
