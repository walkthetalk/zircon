// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ctype.h>
#include <fbl/string.h>
#include <fbl/vector.h>
#include <fuchsia/boot/c/fidl.h>
#include <launchpad/launchpad.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/fdio.h>
#include <lib/zx/debuglog.h>
#include <lib/zx/job.h>
#include <stdio.h>
#include <zircon/boot/image.h>
#include <zircon/dlfcn.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/status.h>

#include <sstream>
#include <thread>
#include <utility>

#include "bootfs-loader-service.h"
#include "bootfs-service.h"
#include "svcfs-service.h"
#include "util.h"

namespace {

// Wire up stdout so that printf() and friends work.
zx_status_t SetupStdout(const zx::debuglog& log) {
  zx::debuglog dup;
  zx_status_t status = log.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup);
  if (status != ZX_OK) {
    return status;
  }
  fdio_t* logger = nullptr;
  status = fdio_create(dup.release(), &logger);
  if (status != ZX_OK) {
    return status;
  }
  int fd = fdio_bind_to_fd(logger, 1, 0);
  if (fd != 1) {
    return ZX_ERR_BAD_STATE;
  }
  return ZX_OK;
}

// Load the boot arguments from bootfs and environment variables.
zx_status_t LoadBootArgs(const fbl::RefPtr<bootsvc::BootfsService>& bootfs, zx::vmo* out,
                         uint64_t* size) {
  // TODO(teisenbe): Rename this file
  const char* config_path = "/config/devmgr";
  fbl::Vector<char> buf;

  zx::vmo config_vmo;
  uint64_t file_size;
  zx_status_t status = bootfs->Open(config_path, &config_vmo, &file_size);
  if (status == ZX_OK) {
    auto config = std::make_unique<char[]>(file_size);
    status = config_vmo.read(config.get(), 0, file_size);
    if (status != ZX_OK) {
      return status;
    }

    // Parse boot arguments file from bootfs.
    std::string_view str(config.get(), file_size);
    status = bootsvc::ParseBootArgs(std::move(str), &buf);
    if (status != ZX_OK) {
      return status;
    }
  }

  // Add boot arguments from environment variables.
  for (char** e = environ; *e != nullptr; e++) {
    for (const char* x = *e; *x != 0; x++) {
      buf.push_back(*x);
    }
    buf.push_back(0);
  }

  // Copy boot arguments into VMO.
  zx::vmo args_vmo;
  status = zx::vmo::create(buf.size(), 0, &args_vmo);
  if (status != ZX_OK) {
    return status;
  }
  status = args_vmo.write(buf.get(), 0, buf.size());
  if (status != ZX_OK) {
    return status;
  }
  status = args_vmo.replace(ZX_DEFAULT_VMO_RIGHTS & ~ZX_RIGHT_WRITE, &args_vmo);
  if (status != ZX_OK) {
    return status;
  }
  *out = std::move(args_vmo);
  *size = buf.size();
  return ZX_OK;
}

// Launch the next process in the boot chain.
// It will receive:
// - stdout wired up via a debuglog handle
// - The boot cmdline arguments, via envp
// - A namespace containing a /boot, serviced by bootsvc
// - A loader that can load libraries from /boot, serviced by bootsvc
// - A handle to the root job
// - A handle to each of the bootdata VMOs the kernel provided
// - A handle to a channel containing the root resource, with type
//   BOOTSVC_ROOT_RESOURCE_CHANNEL_HND
void LaunchNextProcess(fbl::RefPtr<bootsvc::BootfsService> bootfs,
                       fbl::RefPtr<bootsvc::SvcfsService> svcfs,
                       fbl::RefPtr<bootsvc::BootfsLoaderService> loader_svc,
                       const zx::debuglog& log) {
  const char* bootsvc_next = getenv("bootsvc.next");
  if (bootsvc_next == nullptr) {
    bootsvc_next = "bin/devcoordinator";
  }

  // Split the bootsvc.next value into 1 or more arguments using ',' as a
  // delimiter.
  printf("bootsvc: bootsvc.next = %s\n", bootsvc_next);
  fbl::Vector<fbl::String> next_args = bootsvc::SplitString(bootsvc_next, ',');

  // Open the executable we will start next
  zx::vmo program;
  uint64_t file_size;
  const char* next_program = next_args[0].c_str();
  zx_status_t status = bootfs->Open(next_program, &program, &file_size);
  ZX_ASSERT_MSG(status == ZX_OK, "bootsvc: failed to open '%s': %s\n", next_program,
                zx_status_get_string(status));

  // Get the bootfs fuchsia.io.Node service channel that we will hand to the
  // next process in the boot chain.
  zx::channel bootfs_conn;
  status = bootfs->CreateRootConnection(&bootfs_conn);
  ZX_ASSERT_MSG(status == ZX_OK, "bootfs conn creation failed: %s\n", zx_status_get_string(status));

  zx::channel svcfs_conn;
  status = svcfs->CreateRootConnection(&svcfs_conn);
  ZX_ASSERT_MSG(status == ZX_OK, "svcfs conn creation failed: %s\n", zx_status_get_string(status));

  const char* nametable[2] = {};
  uint32_t count = 0;

  launchpad_t* lp;
  launchpad_create(0, next_program, &lp);
  {
    // Use the local loader service backed directly by the primary BOOTFS.
    zx::channel local_loader_conn;
    status = loader_svc->Connect(&local_loader_conn);
    ZX_ASSERT_MSG(status == ZX_OK, "failed to connect to BootfsLoaderService : %s\n",
                  zx_status_get_string(status));
    zx_handle_t old = launchpad_use_loader_service(lp, local_loader_conn.release());
    ZX_ASSERT(old == ZX_HANDLE_INVALID);
  }
  launchpad_load_from_vmo(lp, program.release());
  launchpad_clone(lp, LP_CLONE_DEFAULT_JOB);

  launchpad_add_handle(lp, bootfs_conn.release(), PA_HND(PA_NS_DIR, count));
  nametable[count++] = "/boot";
  launchpad_add_handle(lp, svcfs_conn.release(), PA_HND(PA_NS_DIR, count));
  nametable[count++] = "/bootsvc";

  int argc = static_cast<int>(next_args.size());
  const char* argv[argc];
  for (int i = 0; i < argc; ++i) {
    argv[i] = next_args[i].c_str();
  }
  launchpad_set_args(lp, argc, argv);

  ZX_ASSERT(count <= fbl::count_of(nametable));
  launchpad_set_nametable(lp, count, nametable);

  zx::debuglog log_dup;
  status = log.duplicate(ZX_RIGHT_SAME_RIGHTS, &log_dup);
  if (status != ZX_OK) {
    launchpad_abort(lp, status, "bootsvc: cannot duplicate debuglog handle");
  } else {
    launchpad_add_handle(lp, log_dup.release(), PA_HND(PA_FD, FDIO_FLAG_USE_FOR_STDIO));
  }

  const char* errmsg;
  status = launchpad_go(lp, nullptr, &errmsg);
  if (status != ZX_OK) {
    printf("bootsvc: launchpad %s failed: %s: %s\n", next_program, errmsg,
           zx_status_get_string(status));
  } else {
    printf("bootsvc: Launched %s\n", next_program);
  }
}

}  // namespace

int main(int argc, char** argv) {
  // NOTE: This will be the only source of zx::debuglog in the system.
  // Eventually, we will receive this through a startup handle from userboot.
  zx::debuglog log;
  zx_status_t status = zx::debuglog::create(zx::resource(), 0, &log);
  ZX_ASSERT(status == ZX_OK);

  status = SetupStdout(log);
  ZX_ASSERT(status == ZX_OK);

  printf("bootsvc: Starting...\n");

  // Close the loader-service channel so the service can go away.
  // We won't use it any more (no dlopen calls in this process).
  zx_handle_close(dl_set_loader_service(ZX_HANDLE_INVALID));

  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);

  zx::vmo bootfs_vmo(zx_take_startup_handle(PA_HND(PA_VMO_BOOTFS, 0)));
  ZX_ASSERT(bootfs_vmo.is_valid());

  // Set up the bootfs service
  printf("bootsvc: Creating bootfs service...\n");
  fbl::RefPtr<bootsvc::BootfsService> bootfs_svc;
  status = bootsvc::BootfsService::Create(loop.dispatcher(), &bootfs_svc);
  ZX_ASSERT_MSG(status == ZX_OK, "BootfsService creation failed: %s\n",
                zx_status_get_string(status));
  status = bootfs_svc->AddBootfs(std::move(bootfs_vmo));
  ZX_ASSERT_MSG(status == ZX_OK, "bootfs add failed: %s\n", zx_status_get_string(status));

  // Process the ZBI boot image
  printf("bootsvc: Retrieving boot image...\n");
  zx::vmo image_vmo;
  bootsvc::ItemMap item_map;
  bootsvc::FactoryItemMap factory_item_map;
  status = bootsvc::RetrieveBootImage(&image_vmo, &item_map, &factory_item_map);
  ZX_ASSERT_MSG(status == ZX_OK, "Retrieving boot image failed: %s\n",
                zx_status_get_string(status));

  // Load boot arguments into VMO
  printf("bootsvc: Loading boot arguments...\n");
  zx::vmo args_vmo;
  uint64_t args_size = 0;
  status = LoadBootArgs(bootfs_svc, &args_vmo, &args_size);
  ZX_ASSERT_MSG(status == ZX_OK, "Loading boot arguments failed: %s\n",
                zx_status_get_string(status));

  // Take the root resource
  printf("bootsvc: Taking root resource handle...\n");
  zx::resource root_resource_handle(zx_take_startup_handle(PA_HND(PA_RESOURCE, 0)));
  ZX_ASSERT_MSG(root_resource_handle.is_valid(), "Invalid root resource handle\n");

  // Set up the svcfs service
  printf("bootsvc: Creating svcfs service...\n");
  fbl::RefPtr<bootsvc::SvcfsService> svcfs_svc = bootsvc::SvcfsService::Create(loop.dispatcher());
  svcfs_svc->AddService(
      fuchsia_boot_Arguments_Name,
      bootsvc::CreateArgumentsService(loop.dispatcher(), std::move(args_vmo), args_size));
  svcfs_svc->AddService(
      fuchsia_boot_Items_Name,
      bootsvc::CreateItemsService(loop.dispatcher(), std::move(image_vmo), std::move(item_map)));
  svcfs_svc->AddService(
      fuchsia_boot_FactoryItems_Name,
      bootsvc::CreateFactoryItemsService(loop.dispatcher(), std::move(factory_item_map)));
  svcfs_svc->AddService(fuchsia_boot_Log_Name, bootsvc::CreateLogService(loop.dispatcher(), log));
  zx::job::default_job()->set_property(ZX_PROP_NAME, "root", 4);
  svcfs_svc->AddService(fuchsia_boot_RootJob_Name,
                        bootsvc::CreateRootJobService(loop.dispatcher()));
  svcfs_svc->AddService(
      fuchsia_boot_RootResource_Name,
      bootsvc::CreateRootResourceService(loop.dispatcher(), std::move(root_resource_handle)));

  // Consume certain VMO types from the startup handle table
  printf("bootsvc: Loading kernel VMOs...\n");
  bootfs_svc->PublishStartupVmos(PA_VMO_VDSO, "PA_VMO_VDSO");
  bootfs_svc->PublishStartupVmos(PA_VMO_KERNEL_FILE, "PA_VMO_KERNEL_FILE");

  // Creating the loader service
  printf("bootsvc: Creating loader service...\n");
  fbl::RefPtr<bootsvc::BootfsLoaderService> loader_svc;
  status = bootsvc::BootfsLoaderService::Create(bootfs_svc, loop.dispatcher(), &loader_svc);
  ZX_ASSERT_MSG(status == ZX_OK, "BootfsLoaderService creation failed: %s\n",
                zx_status_get_string(status));

  // Launch the next process in the chain.  This must be in a thread, since
  // it may issue requests to the loader, which runs in the async loop that
  // starts running after this.
  printf("bootsvc: Launching next process...\n");
  std::thread(LaunchNextProcess, bootfs_svc, svcfs_svc, loader_svc, std::cref(log)).detach();

  // Begin serving the bootfs fileystem and loader
  loop.Run();
  return 0;
}
