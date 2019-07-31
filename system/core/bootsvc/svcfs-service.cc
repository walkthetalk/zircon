// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "svcfs-service.h"

#include <fuchsia/boot/c/fidl.h>
#include <lib/fidl-async/bind.h>
#include <lib/zx/job.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/status.h>

#include "util.h"

namespace {

struct ArgumentsData {
  zx::vmo vmo;
  size_t size;
};

zx_status_t ArgumentsGet(void* ctx, fidl_txn_t* txn) {
  auto data = static_cast<const ArgumentsData*>(ctx);
  zx::vmo dup;
  zx_status_t status = data->vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup);
  if (status != ZX_OK) {
    printf("bootsvc: Failed to duplicate boot arguments VMO: %s\n", zx_status_get_string(status));
    return status;
  }
  return fuchsia_boot_ArgumentsGet_reply(txn, dup.release(), data->size);
}

constexpr fuchsia_boot_Arguments_ops kArgumentsOps = {
    .Get = ArgumentsGet,
};

zx_status_t FactoryItemsGet(void* ctx, uint32_t extra, fidl_txn_t* txn) {
  auto map = static_cast<bootsvc::FactoryItemMap*>(ctx);
  auto it = map->find(extra);
  if (it == map->end()) {
    return fuchsia_boot_FactoryItemsGet_reply(txn, ZX_HANDLE_INVALID, 0);
  }

  // Erase the entry after reading.
  zx_handle_t vmo_handle = it->second.vmo.release();
  uint32_t length = it->second.length;
  map->erase(it);

  return fuchsia_boot_FactoryItemsGet_reply(txn, vmo_handle, length);
}

constexpr fuchsia_boot_FactoryItems_ops kFactoryItemsOps = {
    .Get = FactoryItemsGet,
};

struct ItemsData {
  zx::vmo vmo;
  bootsvc::ItemMap map;
};

zx_status_t ItemsGet(void* ctx, uint32_t type, uint32_t extra, fidl_txn_t* txn) {
  auto data = static_cast<const ItemsData*>(ctx);
  auto it = data->map.find(bootsvc::ItemKey{type, extra});
  if (it == data->map.end()) {
    return fuchsia_boot_ItemsGet_reply(txn, ZX_HANDLE_INVALID, 0);
  }
  auto& item = it->second;
  auto buf = std::make_unique<uint8_t[]>(item.length);
  zx_status_t status = data->vmo.read(buf.get(), item.offset, item.length);
  if (status != ZX_OK) {
    printf("bootsvc: Failed to read from boot image VMO: %s\n", zx_status_get_string(status));
    return status;
  }
  zx::vmo payload;
  status = zx::vmo::create(item.length, 0, &payload);
  if (status != ZX_OK) {
    printf("bootsvc: Failed to create payload VMO: %s\n", zx_status_get_string(status));
    return status;
  }
  status = payload.write(buf.get(), 0, item.length);
  if (status != ZX_OK) {
    printf("bootsvc: Failed to write to payload VMO: %s\n", zx_status_get_string(status));
    return status;
  }
  return fuchsia_boot_ItemsGet_reply(txn, payload.release(), item.length);
}

constexpr fuchsia_boot_Items_ops kItemsOps = {
    .Get = ItemsGet,
};

zx_status_t LogGet(void* ctx, fidl_txn_t* txn) {
  auto log = static_cast<const zx::debuglog*>(ctx);
  zx::debuglog dup;
  zx_status_t status = log->duplicate(ZX_RIGHT_SAME_RIGHTS, &dup);
  if (status != ZX_OK) {
    printf("bootsvc: Failed to duplicate kernel log: %s\n", zx_status_get_string(status));
    return status;
  }
  return fuchsia_boot_LogGet_reply(txn, dup.release());
}

constexpr fuchsia_boot_Log_ops kLogOps = {
    .Get = LogGet,
};

zx_status_t RootJobGet(void* ctx, fidl_txn_t* txn) {
  zx::job dup;
  zx_status_t status = zx::job::default_job()->duplicate(ZX_RIGHT_SAME_RIGHTS, &dup);
  if (status != ZX_OK) {
    printf("bootsvc: Failed to duplicate root job: %s\n", zx_status_get_string(status));
    return status;
  }
  return fuchsia_boot_RootJobGet_reply(txn, dup.release());
}

constexpr fuchsia_boot_RootJob_ops kRootJobOps = {
    .Get = RootJobGet,
};

zx_status_t RootResourceGet(void* ctx, fidl_txn_t* txn) {
  auto root_resource_handle = static_cast<const zx::resource*>(ctx);
  zx::resource root_resource_dup;
  zx_status_t status = root_resource_handle->duplicate(ZX_RIGHT_SAME_RIGHTS, &root_resource_dup);
  if (status != ZX_OK) {
    printf("bootsvc: Failed to duplicate root resource handle: %s\n", zx_status_get_string(status));
    return status;
  }
  return fuchsia_boot_RootResourceGet_reply(txn, root_resource_dup.release());
}

constexpr fuchsia_boot_RootResource_ops kRootResourceOps = {
    .Get = RootResourceGet,
};

}  // namespace

namespace bootsvc {

fbl::RefPtr<SvcfsService> SvcfsService::Create(async_dispatcher_t* dispatcher) {
  return fbl::AdoptRef(new SvcfsService(dispatcher));
}

SvcfsService::SvcfsService(async_dispatcher_t* dispatcher)
    : vfs_(dispatcher), root_(fbl::MakeRefCounted<fs::PseudoDir>()) {}

void SvcfsService::AddService(const char* service_name, fbl::RefPtr<fs::Service> service) {
  root_->AddEntry(service_name, std::move(service));
}

zx_status_t SvcfsService::CreateRootConnection(zx::channel* out) {
  return CreateVnodeConnection(&vfs_, root_, out);
}

fbl::RefPtr<fs::Service> CreateArgumentsService(async_dispatcher_t* dispatcher, zx::vmo vmo,
                                                uint64_t size) {
  ArgumentsData data{std::move(vmo), size};
  return fbl::MakeRefCounted<fs::Service>(
      [dispatcher, data = std::move(data)](zx::channel channel) mutable {
        auto dispatch = reinterpret_cast<fidl_dispatch_t*>(fuchsia_boot_Arguments_dispatch);
        return fidl_bind(dispatcher, channel.release(), dispatch, &data, &kArgumentsOps);
      });
}

fbl::RefPtr<fs::Service> CreateFactoryItemsService(async_dispatcher_t* dispatcher,
                                                   FactoryItemMap map) {
  return fbl::MakeRefCounted<fs::Service>(
      [dispatcher, map = std::move(map)](zx::channel channel) mutable {
        auto dispatch = reinterpret_cast<fidl_dispatch_t*>(fuchsia_boot_FactoryItems_dispatch);
        return fidl_bind(dispatcher, channel.release(), dispatch, &map, &kFactoryItemsOps);
      });
}

fbl::RefPtr<fs::Service> CreateItemsService(async_dispatcher_t* dispatcher, zx::vmo vmo,
                                            ItemMap map) {
  ItemsData data{std::move(vmo), std::move(map)};
  return fbl::MakeRefCounted<fs::Service>(
      [dispatcher, data = std::move(data)](zx::channel channel) mutable {
        auto dispatch = reinterpret_cast<fidl_dispatch_t*>(fuchsia_boot_Items_dispatch);
        return fidl_bind(dispatcher, channel.release(), dispatch, &data, &kItemsOps);
      });
}

fbl::RefPtr<fs::Service> CreateLogService(async_dispatcher_t* dispatcher, const zx::debuglog& log) {
  return fbl::MakeRefCounted<fs::Service>([dispatcher, &log](zx::channel channel) {
    auto dispatch = reinterpret_cast<fidl_dispatch_t*>(fuchsia_boot_Log_dispatch);
    return fidl_bind(dispatcher, channel.release(), dispatch, const_cast<zx::debuglog*>(&log),
                     &kLogOps);
  });
}

fbl::RefPtr<fs::Service> CreateRootJobService(async_dispatcher_t* dispatcher) {
  return fbl::MakeRefCounted<fs::Service>([dispatcher](zx::channel channel) {
    auto dispatch = reinterpret_cast<fidl_dispatch_t*>(fuchsia_boot_RootJob_dispatch);
    return fidl_bind(dispatcher, channel.release(), dispatch, nullptr, &kRootJobOps);
  });
}

fbl::RefPtr<fs::Service> CreateRootResourceService(async_dispatcher_t* dispatcher,
                                                   zx::resource root_resource) {
  return fbl::MakeRefCounted<fs::Service>(
      [dispatcher, resource = std::move(root_resource)](zx::channel channel) mutable {
        auto dispatch = reinterpret_cast<fidl_dispatch_t*>(fuchsia_boot_RootResource_dispatch);
        return fidl_bind(dispatcher, channel.release(), dispatch, &resource, &kRootResourceOps);
      });
}

}  // namespace bootsvc
