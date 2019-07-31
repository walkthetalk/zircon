// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/device.h>
#include <stdarg.h>
#include <stdio.h>
#include <zircon/compiler.h>
#include <zircon/device/vfs.h>

#include <utility>

#include "devhost.h"
#include "scheduler_profile.h"

using namespace devmgr;

// These are the API entry-points from drivers
// They must take the devhost_api_lock before calling devhost_* internals
//
// Driver code MUST NOT directly call devhost_* APIs

// LibDriver Device Interface

#define ALLOWED_FLAGS \
  (DEVICE_ADD_NON_BINDABLE | DEVICE_ADD_INSTANCE | \
   DEVICE_ADD_MUST_ISOLATE | DEVICE_ADD_INVISIBLE | DEVICE_ADD_ALLOW_MULTI_COMPOSITE)

__EXPORT zx_status_t device_add_from_driver(zx_driver_t* drv, zx_device_t* parent,
                                            device_add_args_t* args, zx_device_t** out) {
  zx_status_t r;
  fbl::RefPtr<zx_device_t> dev;

  if (!parent) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::RefPtr<zx_device> parent_ref(parent);

  if (!args || args->version != DEVICE_ADD_ARGS_VERSION) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (!args->ops || args->ops->version != DEVICE_OPS_VERSION) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (args->flags & ~ALLOWED_FLAGS) {
    return ZX_ERR_INVALID_ARGS;
  }
  if ((args->flags & DEVICE_ADD_INSTANCE) &&
      (args->flags & (DEVICE_ADD_MUST_ISOLATE | DEVICE_ADD_INVISIBLE))) {
    return ZX_ERR_INVALID_ARGS;
  }

  // If the device will be added in the same devhost and visible,
  // we can connect the client immediately after adding the device.
  // Otherwise we will pass this channel to the devcoordinator via devhost_device_add.
  zx::channel client_remote(args->client_remote);

  {
    ApiAutoLock lock;
    r = devhost_device_create(drv, args->name, args->ctx, args->ops, &dev);
    if (r != ZX_OK) {
      return r;
    }
    if (args->proto_id) {
      dev->protocol_id = args->proto_id;
      dev->protocol_ops = args->proto_ops;
    }
    if (args->flags & DEVICE_ADD_NON_BINDABLE) {
      dev->flags |= DEV_FLAG_UNBINDABLE;
    }
    if (args->flags & DEVICE_ADD_INVISIBLE) {
      dev->flags |= DEV_FLAG_INVISIBLE;
    }
    if (args->flags & DEVICE_ADD_ALLOW_MULTI_COMPOSITE) {
      dev->flags |= DEV_FLAG_ALLOW_MULTI_COMPOSITE;
    }

    // out must be set before calling devhost_device_add().
    // devhost_device_add() may result in child devices being created before it returns,
    // and those children may call ops on the device before device_add() returns.
    // This leaked-ref will be accounted below.
    if (out) {
      *out = dev.get();
    }

    if (args->flags & DEVICE_ADD_MUST_ISOLATE) {
      r = devhost_device_add(dev, parent_ref, args->props, args->prop_count, args->proxy_args,
                             std::move(client_remote));
    } else if (args->flags & DEVICE_ADD_INSTANCE) {
      dev->flags |= DEV_FLAG_INSTANCE | DEV_FLAG_UNBINDABLE;
      r = devhost_device_add(dev, parent_ref, nullptr, 0, nullptr,
                             zx::channel() /* client_remote */);
    } else {
      bool pass_client_remote = args->flags & DEVICE_ADD_INVISIBLE;
      r = devhost_device_add(dev, parent_ref, args->props, args->prop_count, nullptr,
                             pass_client_remote ? std::move(client_remote) : zx::channel());
    }
    if (r != ZX_OK) {
      if (out) {
        *out = nullptr;
      }
      dev.reset();
    }
  }

  if (dev && client_remote.is_valid()) {
    // This needs to be called outside the ApiAutoLock, as device_open will be called
    devhost_device_connect(dev, ZX_FS_RIGHT_READABLE | ZX_FS_RIGHT_WRITABLE,
                           std::move(client_remote));

    // Leak the reference that was written to |out|, it will be recovered in device_remove().
    // For device instances we mimic the behavior of |open| by not leaking the reference,
    // effectively passing owenership to the new connection.
    if (!(args->flags & DEVICE_ADD_INSTANCE)) {
      __UNUSED auto ptr = dev.leak_ref();
    }
  } else {
    // Leak the reference that was written to |out|, it will be recovered in device_remove().
    __UNUSED auto ptr = dev.leak_ref();
  }

  return r;
}

__EXPORT zx_status_t device_remove(zx_device_t* dev) {
  ApiAutoLock lock;
  // This recovers the leaked reference that happened in
  // device_add_from_driver() above.
  auto dev_ref = fbl::internal::MakeRefPtrNoAdopt(dev);
  return devhost_device_remove(std::move(dev_ref));
}

__EXPORT zx_status_t device_rebind(zx_device_t* dev) {
  ApiAutoLock lock;
  fbl::RefPtr<zx_device_t> dev_ref(dev);
  return devhost_device_rebind(dev_ref);
}

__EXPORT void device_make_visible(zx_device_t* dev) {
  ApiAutoLock lock;
  fbl::RefPtr<zx_device_t> dev_ref(dev);
  devhost_make_visible(dev_ref);
}

__EXPORT zx_status_t device_get_profile(zx_device_t* dev, uint32_t priority, const char* name,
                                        zx_handle_t* out_profile) {
  return devhost_get_scheduler_profile(priority, name, out_profile);
}

__EXPORT const char* device_get_name(zx_device_t* dev) { return dev->name; }

__EXPORT zx_device_t* device_get_parent(zx_device_t* dev) {
  // The caller should not hold on to this past the lifetime of |dev|.
  return dev->parent.get();
}

struct GenericProtocol {
  void* ops;
  void* ctx;
};

__EXPORT zx_status_t device_get_protocol(const zx_device_t* dev, uint32_t proto_id, void* out) {
  auto proto = static_cast<GenericProtocol*>(out);
  if (dev->ops->get_protocol) {
    return dev->ops->get_protocol(dev->ctx, proto_id, out);
  }
  if ((proto_id == dev->protocol_id) && (dev->protocol_ops != nullptr)) {
    proto->ops = dev->protocol_ops;
    proto->ctx = dev->ctx;
    return ZX_OK;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

__EXPORT void device_state_clr_set(zx_device_t* dev, zx_signals_t clearflag, zx_signals_t setflag) {
  dev->event.signal(clearflag, setflag);
}

__EXPORT zx_off_t device_get_size(zx_device_t* dev) { return dev->GetSizeOp(); }

__EXPORT zx_status_t device_read(zx_device_t* dev, void* buf, size_t count, zx_off_t off,
                                 size_t* actual) {
  return dev->ReadOp(buf, count, off, actual);
}

__EXPORT zx_status_t device_write(zx_device_t* dev, const void* buf, size_t count, zx_off_t off,
                                  size_t* actual) {
  return dev->WriteOp(buf, count, off, actual);
}

// LibDriver Misc Interfaces

namespace devmgr {
extern zx_handle_t root_resource_handle;
}  // namespace devmgr

// Please do not use get_root_resource() in new code. See ZX-1467.
__EXPORT zx_handle_t get_root_resource() { return root_resource_handle; }

__EXPORT zx_status_t load_firmware(zx_device_t* dev, const char* path, zx_handle_t* fw,
                                   size_t* size) {
  ApiAutoLock lock;
  fbl::RefPtr<zx_device_t> dev_ref(dev);
  return devhost_load_firmware(dev_ref, path, fw, size);
}

// Interface Used by DevHost RPC Layer

zx_status_t device_bind(const fbl::RefPtr<zx_device_t>& dev, const char* drv_libname) {
  ApiAutoLock lock;
  return devhost_device_bind(dev, drv_libname);
}

zx_status_t device_unbind(const fbl::RefPtr<zx_device_t>& dev) {
  ApiAutoLock lock;
  return devhost_device_unbind(dev);
}

zx_status_t device_run_compatibility_tests(const fbl::RefPtr<zx_device_t>& dev,
                                           int64_t hook_wait_time) {
    ApiAutoLock lock;
    return devhost_device_run_compatibility_tests(dev, hook_wait_time);
}

zx_status_t device_open(const fbl::RefPtr<zx_device_t>& dev, fbl::RefPtr<zx_device_t>* out,
                        uint32_t flags) {
  ApiAutoLock lock;
  return devhost_device_open(dev, out, flags);
}

// This function is intended to consume the reference produced by device_open()
zx_status_t device_close(fbl::RefPtr<zx_device_t> dev, uint32_t flags) {
  ApiAutoLock lock;
  return devhost_device_close(std::move(dev), flags);
}

__EXPORT zx_status_t device_get_metadata(zx_device_t* dev, uint32_t type, void* buf, size_t buflen,
                                         size_t* actual) {
  ApiAutoLock lock;
  auto dev_ref = fbl::WrapRefPtr(dev);
  return devhost_get_metadata(dev_ref, type, buf, buflen, actual);
}

__EXPORT zx_status_t device_get_metadata_size(zx_device_t* dev, uint32_t type, size_t* out_size) {
  ApiAutoLock lock;
  auto dev_ref = fbl::WrapRefPtr(dev);
  return devhost_get_metadata_size(dev_ref, type, out_size);
}

__EXPORT zx_status_t device_add_metadata(zx_device_t* dev, uint32_t type, const void* data,
                                         size_t length) {
  ApiAutoLock lock;
  auto dev_ref = fbl::WrapRefPtr(dev);
  return devhost_add_metadata(dev_ref, type, data, length);
}

__EXPORT zx_status_t device_publish_metadata(zx_device_t* dev, const char* path, uint32_t type,
                                             const void* data, size_t length) {
  ApiAutoLock lock;
  auto dev_ref = fbl::WrapRefPtr(dev);
  return devhost_publish_metadata(dev_ref, path, type, data, length);
}

__EXPORT zx_status_t device_add_composite(zx_device_t* dev, const char* name,
                                          const zx_device_prop_t* props, size_t props_count,
                                          const device_component_t* components,
                                          size_t components_count,
                                          uint32_t coresident_device_index) {
  ApiAutoLock lock;
  auto dev_ref = fbl::WrapRefPtr(dev);
  return devhost_device_add_composite(dev_ref, name, props, props_count, components,
                                      components_count, coresident_device_index);
}
