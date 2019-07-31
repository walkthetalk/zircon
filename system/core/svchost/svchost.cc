// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <crashsvc/crashsvc.h>
#include <fbl/algorithm.h>
#include <fbl/string_printf.h>
#include <fs/remote-dir.h>
#include <fuchsia/boot/c/fidl.h>
#include <fuchsia/device/manager/c/fidl.h>
#include <fuchsia/fshost/c/fidl.h>
#include <fuchsia/net/llcpp/fidl.h>
#include <fuchsia/paver/c/fidl.h>
#include <fuchsia/virtualconsole/c/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/kernel-debug/kernel-debug.h>
#include <lib/kernel-mexec/kernel-mexec.h>
#include <lib/logger/provider.h>
#include <lib/process-launcher/launcher.h>
#include <lib/profile/profile.h>
#include <lib/svc/outgoing.h>
#include <lib/zx/job.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/status.h>

#include "sysmem.h"

// An instance of a zx_service_provider_t.
//
// Includes the |ctx| pointer for the zx_service_provider_t.
typedef struct zx_service_provider_instance {
  // The service provider for which this structure is an instance.
  const zx_service_provider_t* provider;

  // The |ctx| pointer returned by the provider's |init| function, if any.
  void* ctx;
} zx_service_provider_instance_t;

static zx_status_t provider_init(zx_service_provider_instance_t* instance) {
  if (instance->provider->ops->init) {
    zx_status_t status = instance->provider->ops->init(&instance->ctx);
    if (status != ZX_OK)
      return status;
  }
  return ZX_OK;
}

static zx_status_t provider_publish(zx_service_provider_instance_t* instance,
                                    async_dispatcher_t* dispatcher,
                                    const fbl::RefPtr<fs::PseudoDir>& dir) {
  const zx_service_provider_t* provider = instance->provider;

  if (!provider->services || !provider->ops->connect)
    return ZX_ERR_INVALID_ARGS;

  for (size_t i = 0; provider->services[i]; ++i) {
    const char* service_name = provider->services[i];
    zx_status_t status = dir->AddEntry(
        service_name,
        fbl::MakeRefCounted<fs::Service>([instance, dispatcher, service_name](zx::channel request) {
          return instance->provider->ops->connect(instance->ctx, dispatcher, service_name,
                                                  request.release());
        }));
    if (status != ZX_OK) {
      for (size_t j = 0; j < i; ++j)
        dir->RemoveEntry(provider->services[j]);
      return status;
    }
  }

  return ZX_OK;
}

static void provider_release(zx_service_provider_instance_t* instance) {
  if (instance->provider->ops->release)
    instance->provider->ops->release(instance->ctx);
  instance->ctx = nullptr;
}

static zx_status_t provider_load(zx_service_provider_instance_t* instance,
                                 async_dispatcher_t* dispatcher,
                                 const fbl::RefPtr<fs::PseudoDir>& dir) {
  if (instance->provider->version != SERVICE_PROVIDER_VERSION) {
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t status = provider_init(instance);
  if (status != ZX_OK) {
    return status;
  }

  status = provider_publish(instance, dispatcher, dir);
  if (status != ZX_OK) {
    provider_release(instance);
    return status;
  }

  return ZX_OK;
}

static zx_handle_t root_job;
static zx_handle_t root_resource;

// We shouldn't need to access these non-Zircon services from svchost, but
// currently some tests assume they can reach these services from the test
// environment. Instead, we should make the test environment hermetic and
// remove the dependencies on these services.
static constexpr const char* deprecated_services[] = {
    "fuchsia.amber.Control", "fuchsia.cobalt.LoggerFactory",
    "fuchsia.devicesettings.DeviceSettingsManager", "fuchsia.logger.Log", "fuchsia.logger.LogSink",
    // Interface to resolve shell commands.
    "fuchsia.process.Resolver", ::llcpp::fuchsia::net::SocketProvider::Name_,
    ::llcpp::fuchsia::net::NameLookup::Name_,
    // Legacy interface for netstack, defined in //garnet
    "fuchsia.netstack.Netstack",
    // New interface for netstack (WIP), defined in //zircon
    "fuchsia.net.stack.Stack", "fuchsia.sys.Environment", "fuchsia.sys.Launcher",
    "fuchsia.wlan.service.Wlan",
    // We should host the tracing.provider service ourselves instead of
    // routing the request to appmgr.
    "fuchsia.tracing.provider.Registry",
    // TODO(PT-88): This entry is temporary, until PT-88 is resolved.
    "fuchsia.tracing.controller.Controller",
    // For amberctl over serial shell.
    "fuchsia.pkg.PackageResolver", "fuchsia.pkg.RepositoryManager", "fuchsia.pkg.rewrite.Engine",
    nullptr,
    // DO NOT ADD MORE ENTRIES TO THIS LIST.
    // Tests should not be accessing services from the environment. Instead,
    // they should run in containers that have their own service instances.
};

// List of services which are re-routed to the fshost service provider handle.
static constexpr const char* fshost_services[] = {
    fuchsia_fshost_Filesystems_Name,
    fuchsia_fshost_Registry_Name,
    nullptr,
};

// Forward these Zircon services to miscsvc.
static constexpr const char* miscsvc_services[] = {
    fuchsia_paver_Paver_Name,
    nullptr,
};

// List of services which are re-routed to bootsvc.
static constexpr const char* bootsvc_services[] = {
    fuchsia_boot_FactoryItems_Name,
    fuchsia_boot_Items_Name,
    fuchsia_boot_Log_Name,
    fuchsia_boot_RootResource_Name,
    nullptr,
};

// List of services which are re-routed to devmgr.
static constexpr const char* devmgr_services[] = {
    fuchsia_device_manager_Administrator_Name,
    fuchsia_device_manager_DebugDumper_Name,
    nullptr,
};

// The ServiceProxy is a Vnode which, if opened, connects to a service.
// However, if treated like a directory, the service proxy will attempt to
// relay the underlying request to the connected service channel.
class ServiceProxy : public fs::Service {
 public:
  ServiceProxy(zx::unowned_channel svc, fbl::StringPiece svc_name)
      : Service([this](zx::channel request) {
          return fdio_service_connect_at(svc_->get(), svc_name_.data(), request.release());
        }),
        svc_(std::move(svc)),
        svc_name_(svc_name) {}

  // This proxy may be a directory. Attempt to connect to the requested object,
  // and return a RemoteDir representing the connection.
  //
  // If the underlying service does not speak the directory protocol, then attempting
  // to connect to the service will close the connection. This is expected.
  zx_status_t Lookup(fbl::RefPtr<Vnode>* out, fbl::StringPiece name) final {
    fbl::String path(fbl::StringPrintf("%s/%.*s", svc_name_.data(), static_cast<int>(name.length()),
                                       name.data()));
    zx::channel client, server;
    zx_status_t status = zx::channel::create(0, &client, &server);
    if (status != ZX_OK) {
      return status;
    }
    status = fdio_service_connect_at(svc_->get(), path.data(), server.release());
    if (status != ZX_OK) {
      return status;
    }

    *out = fbl::MakeRefCounted<fs::RemoteDir>(std::move(client));
    return ZX_OK;
  }

 private:
  zx::unowned_channel svc_;
  fbl::StringPiece svc_name_;
};

void publish_service(const fbl::RefPtr<fs::PseudoDir>& dir, const char* name,
                     zx::unowned_channel svc) {
  dir->AddEntry(name, fbl::MakeRefCounted<ServiceProxy>(std::move(svc), name));
}

void publish_services(const fbl::RefPtr<fs::PseudoDir>& dir, const char* const* names,
                      zx::unowned_channel svc) {
  for (size_t i = 0; names[i] != nullptr; ++i) {
    const char* service_name = names[i];
    publish_service(dir, service_name, zx::unowned_channel(svc->get()));
  }
}

void publish_remote_service(const fbl::RefPtr<fs::PseudoDir>& dir, const char* name,
                            zx::unowned_channel forwarding_channel) {
  dir->AddEntry(
      name, fbl::MakeRefCounted<fs::Service>([name, forwarding_channel = std::move(
                                                        forwarding_channel)](zx::channel request) {
        return fdio_service_connect_at(forwarding_channel->get(), name, request.release());
      }));
}

// TODO(edcoyne): remove this and make virtcon talk virtual filesystems too.
void publish_proxy_service(const fbl::RefPtr<fs::PseudoDir>& dir, const char* name,
                           zx::unowned_channel forwarding_channel) {
  dir->AddEntry(
      name, fbl::MakeRefCounted<fs::Service>(
                [name, forwarding_channel = std::move(forwarding_channel)](zx::channel request) {
                  const auto request_handle = request.release();
                  return forwarding_channel->write(0, name, static_cast<uint32_t>(strlen(name)),
                                                   &request_handle, 1);
                }));
}

int main(int argc, char** argv) {
  bool require_system = false;
  if (argc > 1) {
    require_system = strcmp(argv[1], "--require-system") == 0 ? true : false;
  }
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  async_dispatcher_t* dispatcher = loop.dispatcher();
  svc::Outgoing outgoing(dispatcher);

  zx::channel appmgr_svc = zx::channel(zx_take_startup_handle(PA_HND(PA_USER0, 0)));
  root_job = zx_take_startup_handle(PA_HND(PA_USER0, 1));
  root_resource = zx_take_startup_handle(PA_HND(PA_USER0, 2));
  zx::channel devmgr_proxy_channel = zx::channel(zx_take_startup_handle(PA_HND(PA_USER0, 3)));
  zx::channel fshost_svc = zx::channel(zx_take_startup_handle(PA_HND(PA_USER0, 4)));
  zx::channel virtcon_proxy_channel = zx::channel(zx_take_startup_handle(PA_HND(PA_USER0, 5)));
  zx::channel miscsvc_svc = zx::channel(zx_take_startup_handle(PA_HND(PA_USER0, 6)));
  zx::channel bootsvc_svc = zx::channel(zx_take_startup_handle(PA_HND(PA_USER0, 7)));

  zx_status_t status = outgoing.ServeFromStartupInfo();
  if (status != ZX_OK) {
    fprintf(stderr, "svchost: error: Failed to serve outgoing directory: %d (%s).\n", status,
            zx_status_get_string(status));
    return 1;
  }

  zx_handle_t profile_root_job_copy;
  status = zx_handle_duplicate(root_job, ZX_RIGHT_SAME_RIGHTS, &profile_root_job_copy);
  if (status != ZX_OK) {
    fprintf(stderr, "svchost: failed to duplicate root job: %d (%s).\n", status,
            zx_status_get_string(status));
    return 1;
  }

  KernelMexecContext mexec_context = {
      .root_resource = root_resource,
      .devmgr_channel = zx::unowned_channel(devmgr_proxy_channel),
  };

  zx_service_provider_instance_t service_providers[] = {
      {.provider = launcher_get_service_provider(), .ctx = nullptr},
      {.provider = sysmem2_get_service_provider(), .ctx = nullptr},
      {.provider = kernel_debug_get_service_provider(),
       .ctx = reinterpret_cast<void*>(static_cast<uintptr_t>(root_resource))},
      {.provider = kernel_mexec_get_service_provider(),
       .ctx = reinterpret_cast<void*>(&mexec_context)},
      {.provider = profile_get_service_provider(),
       .ctx = reinterpret_cast<void*>(static_cast<uintptr_t>(profile_root_job_copy))},
  };

  for (size_t i = 0; i < fbl::count_of(service_providers); ++i) {
    status = provider_load(&service_providers[i], dispatcher, outgoing.svc_dir());
    if (status != ZX_OK) {
      fprintf(stderr, "svchost: error: Failed to load service provider %zu: %d (%s).\n", i, status,
              zx_status_get_string(status));
      return 1;
    }
  }

  // if full system is not required drop simple logger service.
  zx_service_provider_instance_t logger_service{.provider = logger_get_service_provider(),
                                                .ctx = nullptr};
  if (!require_system) {
    status = provider_load(&logger_service, dispatcher, outgoing.svc_dir());
    if (status != ZX_OK) {
      fprintf(stderr, "svchost: error: Failed to publish logger: %d (%s).\n", status,
              zx_status_get_string(status));
      return 1;
    }
  }

  publish_services(outgoing.svc_dir(), deprecated_services, zx::unowned_channel(appmgr_svc));
  publish_services(outgoing.svc_dir(), fshost_services, zx::unowned_channel(fshost_svc));
  publish_services(outgoing.svc_dir(), miscsvc_services, zx::unowned_channel(miscsvc_svc));
  publish_services(outgoing.svc_dir(), bootsvc_services, zx::unowned_channel(bootsvc_svc));
  publish_services(outgoing.svc_dir(), devmgr_services, zx::unowned_channel(devmgr_proxy_channel));

  if (virtcon_proxy_channel.is_valid()) {
    publish_proxy_service(outgoing.svc_dir(), fuchsia_virtualconsole_SessionManager_Name,
                          zx::unowned_channel(virtcon_proxy_channel));
  }

  thrd_t thread;
  status = start_crashsvc(zx::job(root_job), require_system ? appmgr_svc.get() : ZX_HANDLE_INVALID,
                          &thread);
  if (status != ZX_OK) {
    // The system can still function without crashsvc, log the error but
    // keep going.
    fprintf(stderr, "svchost: error: Failed to start crashsvc: %d (%s).\n", status,
            zx_status_get_string(status));
  } else {
    thrd_detach(thread);
  }

  status = loop.Run();

  for (size_t i = 0; i < fbl::count_of(service_providers); ++i) {
    provider_release(&service_providers[i]);
  }

  return status;
}
