// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "devhost.h"

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <dlfcn.h>
#include <fbl/auto_lock.h>
#include <fbl/function.h>
#include <fs/handler.h>
#include <fuchsia/device/manager/c/fidl.h>
#include <fuchsia/device/manager/llcpp/fidl.h>
#include <fuchsia/io/c/fidl.h>
#include <inttypes.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/receiver.h>
#include <lib/async/cpp/wait.h>
#include <lib/fdio/fdio.h>
#include <lib/fidl/coding.h>
#include <lib/zx/debuglog.h>
#include <lib/zx/resource.h>
#include <lib/zx/vmo.h>
#include <lib/zxio/null.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/dlfcn.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/log.h>

#include <new>
#include <utility>

#include "../shared/async-loop-owned-rpc-handler.h"
#include "../shared/env.h"
#include "../shared/fidl_txn.h"
#include "../shared/log.h"
#include "composite-device.h"
#include "connection-destroyer.h"
#include "device-controller-connection.h"
#include "main.h"
#include "proxy-iostate.h"
#include "scheduler_profile.h"
#include "tracing.h"

zx_status_t zx_driver::Create(fbl::RefPtr<zx_driver>* out_driver) {
  *out_driver = fbl::AdoptRef(new zx_driver());
  return ZX_OK;
}

namespace devmgr {

uint32_t log_flags = LOG_ERROR | LOG_INFO;

static fbl::DoublyLinkedList<fbl::RefPtr<zx_driver>> dh_drivers;

// Access the devhost's async event loop
async::Loop* DevhostAsyncLoop() {
  static async::Loop loop(&kAsyncLoopConfigAttachToThread);
  return &loop;
}

static zx_status_t SetupRootDevcoordinatorConnection(zx::channel ch) {
  auto conn = std::make_unique<DevhostControllerConnection>();
  if (conn == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }

  conn->set_channel(std::move(ch));
  return DevhostControllerConnection::BeginWait(std::move(conn), DevhostAsyncLoop()->dispatcher());
}

const char* mkdevpath(const fbl::RefPtr<zx_device_t>& dev, char* path, size_t max) {
  if (dev == nullptr) {
    return "";
  }
  if (max < 1) {
    return "<invalid>";
  }
  char* end = path + max;
  char sep = 0;

  fbl::RefPtr<zx_device> itr_dev(dev);
  while (itr_dev) {
    *(--end) = sep;

    size_t len = strlen(itr_dev->name);
    if (len > (size_t)(end - path)) {
      break;
    }
    end -= len;
    memcpy(end, itr_dev->name, len);
    sep = '/';
    itr_dev = itr_dev->parent;
  }
  return end;
}

static uint32_t logflagval(char* flag) {
  if (!strcmp(flag, "error")) {
    return DDK_LOG_ERROR;
  }
  if (!strcmp(flag, "warn")) {
    return DDK_LOG_WARN;
  }
  if (!strcmp(flag, "info")) {
    return DDK_LOG_INFO;
  }
  if (!strcmp(flag, "trace")) {
    return DDK_LOG_TRACE;
  }
  if (!strcmp(flag, "spew")) {
    return DDK_LOG_SPEW;
  }
  if (!strcmp(flag, "debug1")) {
    return DDK_LOG_DEBUG1;
  }
  if (!strcmp(flag, "debug2")) {
    return DDK_LOG_DEBUG2;
  }
  if (!strcmp(flag, "debug3")) {
    return DDK_LOG_DEBUG3;
  }
  if (!strcmp(flag, "debug4")) {
    return DDK_LOG_DEBUG4;
  }
  return static_cast<uint32_t>(strtoul(flag, nullptr, 0));
}

static void logflag(char* flag, uint32_t* flags) {
  if (*flag == '+') {
    *flags |= logflagval(flag + 1);
  } else if (*flag == '-') {
    *flags &= ~logflagval(flag + 1);
  }
}

zx_status_t dh_find_driver(fbl::StringPiece libname, zx::vmo vmo, fbl::RefPtr<zx_driver_t>* out) {
  // check for already-loaded driver first
  for (auto& drv : dh_drivers) {
    if (!libname.compare(drv.libname())) {
      *out = fbl::RefPtr(&drv);
      return drv.status();
    }
  }

  fbl::RefPtr<zx_driver> new_driver;
  zx_status_t status = zx_driver::Create(&new_driver);
  if (status != ZX_OK) {
    return status;
  }
  new_driver->set_libname(libname);

  // Let the |dh_drivers| list and our out parameter each have a refcount.
  dh_drivers.push_back(new_driver);
  *out = new_driver;

  const char* c_libname = new_driver->libname().c_str();

  void* dl = dlopen_vmo(vmo.get(), RTLD_NOW);
  if (dl == nullptr) {
    log(ERROR, "devhost: cannot load '%s': %s\n", c_libname, dlerror());
    new_driver->set_status(ZX_ERR_IO);
    return new_driver->status();
  }

  auto dn = static_cast<const zircon_driver_note_t*>(dlsym(dl, "__zircon_driver_note__"));
  if (dn == nullptr) {
    log(ERROR, "devhost: driver '%s' missing __zircon_driver_note__ symbol\n", c_libname);
    new_driver->set_status(ZX_ERR_IO);
    return new_driver->status();
  }
  auto ops = static_cast<const zx_driver_ops_t**>(dlsym(dl, "__zircon_driver_ops__"));
  auto dr = static_cast<zx_driver_rec_t*>(dlsym(dl, "__zircon_driver_rec__"));
  if (dr == nullptr) {
    log(ERROR, "devhost: driver '%s' missing __zircon_driver_rec__ symbol\n", c_libname);
    new_driver->set_status(ZX_ERR_IO);
    return new_driver->status();
  }
  // TODO(kulakowski) Eventually just check __zircon_driver_ops__,
  // when bind programs are standalone.
  if (ops == nullptr) {
    ops = &dr->ops;
  }
  if (!(*ops)) {
    log(ERROR, "devhost: driver '%s' has nullptr ops\n", c_libname);
    new_driver->set_status(ZX_ERR_INVALID_ARGS);
    return new_driver->status();
  }
  if ((*ops)->version != DRIVER_OPS_VERSION) {
    log(ERROR,
        "devhost: driver '%s' has bad driver ops version %" PRIx64 ", expecting %" PRIx64 "\n",
        c_libname, (*ops)->version, DRIVER_OPS_VERSION);
    new_driver->set_status(ZX_ERR_INVALID_ARGS);
    return new_driver->status();
  }

  new_driver->set_driver_rec(dr);
  new_driver->set_name(dn->payload.name);
  new_driver->set_ops(*ops);
  dr->driver = new_driver.get();

  // check for dprintf log level flags
  char tmp[128];
  snprintf(tmp, sizeof(tmp), "driver.%s.log", new_driver->name());
  char* log = getenv(tmp);
  if (log) {
    while (log) {
      char* sep = strchr(log, ',');
      if (sep) {
        *sep = 0;
        logflag(log, &dr->log_flags);
        *sep = ',';
        log = sep + 1;
      } else {
        logflag(log, &dr->log_flags);
        break;
      }
    }
    log(INFO, "devhost: driver '%s': log flags set to: 0x%x\n", new_driver->name(), dr->log_flags);
  }

  if (new_driver->has_init_op()) {
    new_driver->set_status(new_driver->InitOp());
    if (new_driver->status() != ZX_OK) {
      log(ERROR, "devhost: driver '%s' failed in init: %d\n", c_libname, new_driver->status());
    }
  } else {
    new_driver->set_status(ZX_OK);
  }

  return new_driver->status();
}

void DevhostControllerConnection::CreateDevice(zx::channel rpc, ::fidl::StringView driver_path_view,
                                               ::zx::vmo driver_vmo, ::zx::handle parent_proxy,
                                               ::fidl::StringView proxy_args,
                                               uint64_t local_device_id,
                                               CreateDeviceCompleter::Sync completer) {
  fbl::StringPiece driver_path(driver_path_view.data(), driver_path_view.size());
  // This does not operate under the devhost api lock,
  // since the newly created device is not visible to
  // any API surface until a driver is bound to it.
  // (which can only happen via another message on this thread)
  log(RPC_IN, "devhost: create device drv='%.*s' args='%.*s'\n",
      static_cast<int>(driver_path.size()), driver_path.data(), static_cast<int>(proxy_args.size()),
      proxy_args.data());

  // named driver -- ask it to create the device
  fbl::RefPtr<zx_driver_t> drv;
  zx_status_t r = dh_find_driver(driver_path, std::move(driver_vmo), &drv);
  if (r != ZX_OK) {
    log(ERROR, "devhost: driver load failed: %d\n", r);
    return;
  }
  if (!drv->has_create_op()) {
    log(ERROR, "devhost: driver create() not supported\n");
    return;
  }

  // Create a dummy parent device for use in this call to Create
  fbl::RefPtr<zx_device> parent;
  if ((r = zx_device::Create(&parent)) != ZX_OK) {
    log(ERROR, "devhost: device create() failed: %d\n", r);
    return;
  }
  // magic cookie for device create handshake
  char dummy_name[sizeof(parent->name)] = "device_create dummy";
  memcpy(&parent->name, &dummy_name, sizeof(parent->name));

  CreationContext creation_context = {
      .parent = std::move(parent),
      .child = nullptr,
      .rpc = zx::unowned_channel(rpc),
  };

  r = drv->CreateOp(&creation_context, creation_context.parent, "proxy", proxy_args.data(),
                    parent_proxy.release());

  // Suppress a warning about dummy device being in a bad state.  The
  // message is spurious in this case, since the dummy parent never
  // actually begins its device lifecycle.  This flag is ordinarily
  // set by device_remove().
  creation_context.parent->flags |= DEV_FLAG_VERY_DEAD;

  if (r != ZX_OK) {
    log(ERROR, "devhost: driver create() failed: %d\n", r);
    return;
  }

  auto new_device = std::move(creation_context.child);
  if (new_device == nullptr) {
    log(ERROR, "devhost: driver create() failed to create a device!");
    return;
  }

  new_device->set_local_id(local_device_id);
  std::unique_ptr<DeviceControllerConnection> newconn;
  r = DeviceControllerConnection::Create(std::move(new_device), std::move(rpc), &newconn);
  if (r != ZX_OK) {
    return;
  }

  // TODO: inform devcoord

  log(RPC_IN, "devhost: creating '%.*s' conn=%p\n", static_cast<int>(driver_path.size()),
      driver_path.data(), newconn.get());
  if ((r = DeviceControllerConnection::BeginWait(std::move(newconn),
                                                 DevhostAsyncLoop()->dispatcher())) != ZX_OK) {
    return;
  }
}

void DevhostControllerConnection::CreateCompositeDevice(
    zx::channel rpc, ::fidl::VectorView<uint64_t> components, ::fidl::StringView name,
    uint64_t local_device_id, CreateCompositeDeviceCompleter::Sync completer) {
  log(RPC_IN, "devhost: create composite device %.*s'\n", static_cast<int>(name.size()),
      name.data());

  // Convert the component IDs into zx_device references
  CompositeComponents components_list(new fbl::RefPtr<zx_device>[components.count()],
                                      components.count());
  {
    // Acquire the API lock so that we don't have to worry about concurrent
    // device removes
    ApiAutoLock lock;

    for (size_t i = 0; i < components.count(); ++i) {
      uint64_t local_id = components.data()[i];
      fbl::RefPtr<zx_device_t> dev = zx_device::GetDeviceFromLocalId(local_id);
      if (dev == nullptr || (dev->flags & DEV_FLAG_DEAD)) {
        completer.Reply(ZX_ERR_NOT_FOUND);
        return;
      }
      components_list[i] = std::move(dev);
    }
  }

  fbl::RefPtr<zx_device_t> dev;
  zx_status_t status = zx_device::Create(&dev);
  if (status != ZX_OK) {
    completer.Reply(status);
    return;
  }
  static_assert(fuchsia_device_manager_DEVICE_NAME_MAX + 1 >= sizeof(dev->name));
  memcpy(dev->name, name.data(), name.size());
  dev->name[name.size()] = 0;
  dev->set_local_id(local_device_id);

  std::unique_ptr<DeviceControllerConnection> newconn;
  status = DeviceControllerConnection::Create(dev, std::move(rpc), &newconn);
  if (status != ZX_OK) {
    completer.Reply(status);
    return;
  }

  status = InitializeCompositeDevice(dev, std::move(components_list));
  if (status != ZX_OK) {
    completer.Reply(status);
    return;
  }

  log(RPC_IN, "devhost: creating new composite conn=%p\n", newconn.get());
  if ((status = DeviceControllerConnection::BeginWait(std::move(newconn),
                                                      DevhostAsyncLoop()->dispatcher())) != ZX_OK) {
    completer.Reply(status);
    return;
  }
  completer.Reply(ZX_OK);
}

void DevhostControllerConnection::CreateDeviceStub(zx::channel rpc, uint32_t protocol_id,
                                                   uint64_t local_device_id,
                                                   CreateDeviceStubCompleter::Sync completer) {
  log(RPC_IN, "devhost: create device stub\n");

  fbl::RefPtr<zx_device_t> dev;
  zx_status_t r = zx_device::Create(&dev);
  // TODO: dev->ops and other lifecycle bits
  // no name means a dummy proxy device
  if (r != ZX_OK) {
    return;
  }
  strcpy(dev->name, "proxy");
  dev->protocol_id = protocol_id;
  dev->ops = &device_default_ops;
  dev->set_local_id(local_device_id);

  std::unique_ptr<DeviceControllerConnection> newconn;
  r = DeviceControllerConnection::Create(dev, std::move(rpc), &newconn);
  if (r != ZX_OK) {
    return;
  }

  log(RPC_IN, "devhost: creating new stub conn=%p\n", newconn.get());
  if ((r = DeviceControllerConnection::BeginWait(std::move(newconn),
                                                 DevhostAsyncLoop()->dispatcher())) != ZX_OK) {
    return;
  }
}

zx_status_t DevhostControllerConnection::HandleRead() {
  zx::unowned_channel conn = channel();
  uint8_t msg[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t hin[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t msize = sizeof(msg);
  uint32_t hcount = fbl::count_of(hin);
  zx_status_t status = conn->read(0, msg, hin, msize, hcount, &msize, &hcount);
  if (status != ZX_OK) {
    return status;
  }

  fidl_msg_t fidl_msg = {
      .bytes = msg,
      .handles = hin,
      .num_bytes = msize,
      .num_handles = hcount,
  };

  if (fidl_msg.num_bytes < sizeof(fidl_message_header_t)) {
    zx_handle_close_many(fidl_msg.handles, fidl_msg.num_handles);
    return ZX_ERR_IO;
  }

  auto hdr = static_cast<fidl_message_header_t*>(fidl_msg.bytes);
  DevmgrFidlTxn txn(std::move(conn), hdr->txid);
  fuchsia::device::manager::DevhostController::Dispatch(this, &fidl_msg, &txn);
  return txn.Status();
}

// handles devcoordinator rpc

void DevhostControllerConnection::HandleRpc(fbl::unique_ptr<DevhostControllerConnection> conn,
                                            async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                            zx_status_t status, const zx_packet_signal_t* signal) {
  if (status != ZX_OK) {
    log(ERROR, "devhost: devcoord conn wait error: %d\n", status);
    return;
  }
  if (signal->observed & ZX_CHANNEL_READABLE) {
    status = conn->HandleRead();
    if (status != ZX_OK) {
      log(ERROR, "devhost: devmgr rpc unhandleable ios=%p r=%s. fatal.\n", conn.get(),
          zx_status_get_string(status));
      abort();
    }
    BeginWait(std::move(conn), dispatcher);
    return;
  }
  if (signal->observed & ZX_CHANNEL_PEER_CLOSED) {
    log(ERROR, "devhost: devmgr disconnected! fatal. (conn=%p)\n", conn.get());
    abort();
  }
  log(ERROR, "devhost: no work? %08x\n", signal->observed);
  BeginWait(std::move(conn), dispatcher);
}

// handles remoteio rpc
void DevfsConnection::HandleRpc(fbl::unique_ptr<DevfsConnection> conn,
                                async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                zx_status_t status, const zx_packet_signal_t* signal) {
  if (status != ZX_OK) {
    log(ERROR, "devhost: devfs conn wait error: %d\n", status);
    return;
  }

  if (signal->observed & ZX_CHANNEL_READABLE) {
    status = fs::ReadMessage(wait->object(), [&conn](fidl_msg_t* msg, fs::FidlConnection* txn) {
      return devhost_fidl_handler(msg, txn->Txn(), conn.get());
    });
    if (status == ZX_OK) {
      BeginWait(std::move(conn), dispatcher);
      return;
    }
  } else if (signal->observed & ZX_CHANNEL_PEER_CLOSED) {
    fs::CloseMessage([&conn](fidl_msg_t* msg, fs::FidlConnection* txn) {
      return devhost_fidl_handler(msg, txn->Txn(), conn.get());
    });
  } else {
    printf("dh_handle_fidl_rpc: invalid signals %x\n", signal->observed);
    abort();
  }

  // We arrive here if devhost_fidl_handler was a clean close (ERR_DISPATCHER_DONE),
  // or close-due-to-error (non-ZX_OK), or if the channel was closed
  // out from under us.  In all cases, we are done with this connection, so we
  // will destroy it by letting it leave scope.
  log(TRACE, "devhost: destroying devfs conn %p\n", conn.get());
}

static void proxy_ios_destroy(const fbl::RefPtr<zx_device_t>& dev) {
  fbl::AutoLock guard(&dev->proxy_ios_lock);

  if (dev->proxy_ios) {
    dev->proxy_ios->CancelLocked(DevhostAsyncLoop()->dispatcher());
  }
}

#define LOGBUF_MAX (ZX_LOG_RECORD_MAX - sizeof(zx_log_record_t))

static zx::debuglog devhost_log_handle;

static ssize_t devhost_log_write_internal(uint32_t flags, const void* void_data, size_t len) {
  struct Context {
    Context() = default;

    uint32_t next = 0;
    zx::unowned_debuglog handle;
    char data[LOGBUF_MAX] = {};
  };
  static thread_local fbl::unique_ptr<Context> ctx;

  if (ctx == nullptr) {
    ctx = std::make_unique<Context>();
    if (ctx == nullptr) {
      return len;
    }
    ctx->handle = zx::unowned_debuglog(devhost_log_handle);
  }

  const char* data = static_cast<const char*>(void_data);
  size_t r = len;

  auto flush_context = [&]() {
    ctx->handle->write(flags, ctx->data, ctx->next);
    ctx->next = 0;
  };

  while (len-- > 0) {
    char c = *data++;
    if (c == '\n') {
      if (ctx->next) {
        flush_context();
      }
      continue;
    }
    if (c < ' ') {
      continue;
    }
    ctx->data[ctx->next++] = c;
    if (ctx->next == LOGBUF_MAX) {
      flush_context();
    }
  }
  return r;
}

}  // namespace devmgr

__EXPORT void driver_printf(uint32_t flags, const char* fmt, ...) {
  char buffer[512];
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buffer, sizeof(buffer), fmt, ap);
  va_end(ap);

  if (r > (int)sizeof(buffer)) {
    r = sizeof(buffer);
  }

  devmgr::devhost_log_write_internal(flags, buffer, r);
}

namespace devmgr {

static zx_status_t devhost_log_write(zxio_t* io, const void* buffer, size_t capacity,
                                     size_t* out_actual) {
  devhost_log_write_internal(0, buffer, capacity);
  *out_actual = capacity;
  return ZX_OK;
}

static zx_status_t devhost_log_isatty(zxio_t* io, bool* tty) {
  // Claim to be a TTY to get line buffering
  *tty = true;
  return ZX_OK;
}

static constexpr zxio_ops_t devhost_log_ops = []() {
  zxio_ops_t ops = zxio_default_ops;
  ops.write = devhost_log_write;
  ops.isatty = devhost_log_isatty;
  return ops;
}();

static void devhost_io_init() {
  if (zx::debuglog::create(zx::resource(), 0, &devhost_log_handle) < 0) {
    return;
  }
  fdio_t* io = nullptr;
  zxio_storage_t* storage = nullptr;
  if ((io = fdio_zxio_create(&storage)) == nullptr) {
    return;
  }
  zxio_init(&storage->io, &devhost_log_ops);
  close(1);
  fdio_bind_to_fd(io, 1, 0);
  dup2(1, 2);
}

// Send message to devcoordinator asking to add child device to
// parent device.  Called under devhost api lock.
zx_status_t devhost_add(const fbl::RefPtr<zx_device_t>& parent,
                        const fbl::RefPtr<zx_device_t>& child, const char* proxy_args,
                        const zx_device_prop_t* props, uint32_t prop_count,
                        zx::channel client_remote) {
  char buffer[512];
  const char* path = mkdevpath(parent, buffer, sizeof(buffer));
  log(RPC_OUT, "devhost[%s] add '%s'\n", path, child->name);

  bool add_invisible = child->flags & DEV_FLAG_INVISIBLE;
  fuchsia_device_manager_AddDeviceConfig add_device_config = 0;

  // TODO(ravoorir): Support this per device configuration. Uncomment the below when
  // we can configure this per device.
  /*if (child->flags & DEV_FLAG_ALLOW_MULTI_COMPOSITE) {
    add_device_config |= fuchsia_device_manager_AddDeviceConfig_ALLOW_MULTI_COMPOSITE;
  }*/

  add_device_config |= fuchsia_device_manager_AddDeviceConfig_ALLOW_MULTI_COMPOSITE;
  zx_status_t status;
  zx::channel hrpc, hsend;
  if ((status = zx::channel::create(0, &hrpc, &hsend)) != ZX_OK) {
    return status;
  }

  std::unique_ptr<DeviceControllerConnection> conn;
  status = DeviceControllerConnection::Create(child, std::move(hrpc), &conn);
  if (status != ZX_OK) {
    return status;
  }

  const zx::channel& rpc = *parent->rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  size_t proxy_args_len = proxy_args ? strlen(proxy_args) : 0;
  zx_status_t call_status;
  static_assert(sizeof(zx_device_prop_t) == sizeof(uint64_t));
  uint64_t device_id = 0;
  if (add_invisible) {
    status = fuchsia_device_manager_CoordinatorAddDeviceInvisible(
        rpc.get(), hsend.release(), reinterpret_cast<const uint64_t*>(props), prop_count,
        child->name, strlen(child->name), child->protocol_id, child->driver->libname().data(),
        child->driver->libname().size(), proxy_args, proxy_args_len, client_remote.release(),
        &call_status, &device_id);
  } else {
    status = fuchsia_device_manager_CoordinatorAddDevice(
        rpc.get(), hsend.release(), reinterpret_cast<const uint64_t*>(props), prop_count,
        child->name, strlen(child->name), child->protocol_id, child->driver->libname().data(),
        child->driver->libname().size(), proxy_args, proxy_args_len, add_device_config,
        client_remote.release(), &call_status, &device_id);
  }
  if (status != ZX_OK) {
    log(ERROR, "devhost[%s] add '%s': rpc sending failed: %d\n", path, child->name, status);
    return status;
  } else if (call_status != ZX_OK) {
    log(ERROR, "devhost[%s] add '%s': rpc failed: %d\n", path, child->name, call_status);
    return call_status;
  }

  child->set_local_id(device_id);

  status = DeviceControllerConnection::BeginWait(std::move(conn), DevhostAsyncLoop()->dispatcher());
  if (status != ZX_OK) {
    return status;
  }
  return ZX_OK;
}

static void log_rpc(const fbl::RefPtr<zx_device_t>& dev, const char* opname) {
  char buffer[512];
  const char* path = mkdevpath(dev, buffer, sizeof(buffer));
  log(RPC_OUT, "devhost[%s] %s'\n", path, opname);
}

static void log_rpc_result(const char* opname, zx_status_t status, zx_status_t call_status) {
  if (status != ZX_OK) {
    log(ERROR, "devhost: rpc:%s sending failed: %d\n", opname, status);
  } else if (call_status != ZX_OK) {
    log(ERROR, "devhost: rpc:%s failed: %d\n", opname, call_status);
  }
}

void devhost_make_visible(const fbl::RefPtr<zx_device_t>& dev) {
  const zx::channel& rpc = *dev->rpc;
  if (!rpc.is_valid()) {
    return;
  }

  // TODO(teisenbe): Handle failures here...
  log_rpc(dev, "make-visible");
  zx_status_t call_status;
  zx_status_t status = fuchsia_device_manager_CoordinatorMakeVisible(rpc.get(), &call_status);
  log_rpc_result("make-visible", status, call_status);
}

// Send message to devcoordinator informing it that this device
// is being removed.  Called under devhost api lock.
zx_status_t devhost_remove(const fbl::RefPtr<zx_device_t>& dev) {
  DeviceControllerConnection* conn = dev->conn.load();
  if (conn == nullptr) {
    log(ERROR, "removing device %p, conn is nullptr\n", dev.get());
    return ZX_ERR_INTERNAL;
  }

  // This must be done before the RemoveDevice message is sent to
  // devcoordinator, since devcoordinator will close the channel in response.
  // The async loop may see the channel close before it sees the queued
  // shutdown packet, so it needs to check if dev->conn has been nulled to
  // handle that gracefully.
  dev->conn.store(nullptr);

  log(DEVLC, "removing device %p, conn %p\n", dev.get(), conn);

  const zx::channel& rpc = *dev->rpc;
  ZX_ASSERT(rpc.is_valid());
  // TODO(teisenbe): Handle failures here...
  log_rpc(dev, "remove-device");
  zx_status_t call_status;
  zx_status_t status = fuchsia_device_manager_CoordinatorRemoveDevice(rpc.get(), &call_status);
  log_rpc_result("remove-device", status, call_status);

  // Forget our local ID, to release the reference stored by the local ID map
  dev->set_local_id(0);

  // Forget about our rpc channel since after the port_queue below it may be
  // closed.
  dev->rpc = zx::unowned_channel();

  // queue an event to destroy the connection
  ConnectionDestroyer::Get()->QueueDeviceControllerConnection(DevhostAsyncLoop()->dispatcher(),
                                                              conn);

  // shut down our proxy rpc channel if it exists
  proxy_ios_destroy(dev);

  return ZX_OK;
}

zx_status_t devhost_get_topo_path(const fbl::RefPtr<zx_device_t>& dev, char* path, size_t max,
                                  size_t* actual) {
  fbl::RefPtr<zx_device_t> remote_dev = dev;
  if (dev->flags & DEV_FLAG_INSTANCE) {
    // Instances cannot be opened a second time. If dev represents an instance, return the path
    // to its parent, prefixed with an '@'.
    if (max < 1) {
      return ZX_ERR_BUFFER_TOO_SMALL;
    }
    path[0] = '@';
    path++;
    max--;
    remote_dev = dev->parent;
  }

  const zx::channel& rpc = *remote_dev->rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }

  log_rpc(remote_dev, "get-topo-path");
  zx_status_t call_status;
  zx_status_t status = fuchsia_device_manager_CoordinatorGetTopologicalPath(rpc.get(), &call_status,
                                                                            path, max - 1, actual);
  log_rpc_result("get-topo-path", status, call_status);
  if (status != ZX_OK) {
    return status;
  }
  if (call_status != ZX_OK) {
    return status;
  }

  path[*actual] = 0;
  *actual += 1;

  // Account for the prefixed '@' we may have added above.
  if (dev->flags & DEV_FLAG_INSTANCE) {
    *actual += 1;
  }
  return ZX_OK;
}

zx_status_t devhost_device_bind(const fbl::RefPtr<zx_device_t>& dev, const char* drv_libname) {
  const zx::channel& rpc = *dev->rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  log_rpc(dev, "bind-device");
  zx_status_t call_status;
  zx_status_t status = fuchsia_device_manager_CoordinatorBindDevice(
      rpc.get(), drv_libname, strlen(drv_libname), &call_status);
  log_rpc_result("bind-device", status, call_status);
  if (status != ZX_OK) {
    return status;
  }
  return call_status;
}

zx_status_t devhost_device_run_compatibility_tests(const fbl::RefPtr<zx_device_t>& dev,
                                                   int64_t hook_wait_time) {
    const zx::channel& rpc = *dev->rpc;
    if (!rpc.is_valid()) {
        return ZX_ERR_IO_REFUSED;
    }
    log_rpc(dev, "run-compatibility-test");
    zx_status_t call_status;
    zx_status_t status = fuchsia_device_manager_CoordinatorRunCompatibilityTests(
                          rpc.get(), hook_wait_time, &call_status);
    log_rpc_result("run-compatibility-test", status, call_status);
    if (status != ZX_OK) {
        return status;
    }
    return call_status;
}

zx_status_t devhost_load_firmware(const fbl::RefPtr<zx_device_t>& dev, const char* path,
                                  zx_handle_t* vmo, size_t* size) {
  if ((vmo == nullptr) || (size == nullptr)) {
    return ZX_ERR_INVALID_ARGS;
  }

  const zx::channel& rpc = *dev->rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  log_rpc(dev, "load-firmware");
  zx_status_t call_status;
  zx_status_t status = fuchsia_device_manager_CoordinatorLoadFirmware(rpc.get(), path, strlen(path),
                                                                      &call_status, vmo, size);
  log_rpc_result("load-firmware", status, call_status);
  if (status != ZX_OK) {
    return status;
  }
  if (call_status == ZX_OK && *vmo == ZX_HANDLE_INVALID) {
    return ZX_ERR_INTERNAL;
  }
  return call_status;
}

zx_status_t devhost_get_metadata(const fbl::RefPtr<zx_device_t>& dev, uint32_t type, void* buf,
                                 size_t buflen, size_t* actual) {
  if (!buf) {
    return ZX_ERR_INVALID_ARGS;
  }

  const zx::channel& rpc = *dev->rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  uint8_t data[fuchsia_device_manager_METADATA_MAX];
  size_t length = 0;
  log_rpc(dev, "get-metadata");
  zx_status_t call_status;
  zx_status_t status = fuchsia_device_manager_CoordinatorGetMetadata(rpc.get(), type, &call_status,
                                                                     data, sizeof(data), &length);
  if (status != ZX_OK) {
    log(ERROR, "devhost: rpc:get-metadata sending failed: %d\n", status);
    return status;
  }
  if (call_status != ZX_OK) {
    if (call_status != ZX_ERR_NOT_FOUND) {
      log(ERROR, "devhost: rpc:get-metadata failed: %d\n", call_status);
    }
    return call_status;
  }

  memcpy(buf, data, length);
  if (actual != nullptr) {
    *actual = length;
  }
  return ZX_OK;
}

zx_status_t devhost_get_metadata_size(const fbl::RefPtr<zx_device_t>& dev, uint32_t type,
                                      size_t* out_length) {
  const zx::channel& rpc = *dev->rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  log_rpc(dev, "get-metadata");
  zx_status_t call_status;
  zx_status_t status =
      fuchsia_device_manager_CoordinatorGetMetadataSize(rpc.get(), type, &call_status, out_length);
  if (status != ZX_OK) {
    log(ERROR, "devhost: rpc:get-metadata sending failed: %d\n", status);
    return status;
  }
  if (call_status != ZX_OK) {
    if (call_status != ZX_ERR_NOT_FOUND) {
      log(ERROR, "devhost: rpc:get-metadata failed: %d\n", call_status);
    }
    return call_status;
  }
  return ZX_OK;
}

zx_status_t devhost_add_metadata(const fbl::RefPtr<zx_device_t>& dev, uint32_t type,
                                 const void* data, size_t length) {
  if (!data && length) {
    return ZX_ERR_INVALID_ARGS;
  }
  const zx::channel& rpc = *dev->rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  log_rpc(dev, "add-metadata");
  zx_status_t call_status;
  zx_status_t status = fuchsia_device_manager_CoordinatorAddMetadata(
      rpc.get(), type, reinterpret_cast<const uint8_t*>(data), length, &call_status);
  log_rpc_result("add-metadata", status, call_status);
  if (status != ZX_OK) {
    return status;
  }
  return call_status;
}

zx_status_t devhost_publish_metadata(const fbl::RefPtr<zx_device_t>& dev, const char* path,
                                     uint32_t type, const void* data, size_t length) {
  if (!path || (!data && length)) {
    return ZX_ERR_INVALID_ARGS;
  }
  const zx::channel& rpc = *dev->rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  log_rpc(dev, "publish-metadata");
  zx_status_t call_status;
  zx_status_t status = fuchsia_device_manager_CoordinatorPublishMetadata(
      rpc.get(), path, strlen(path), type, reinterpret_cast<const uint8_t*>(data), length,
      &call_status);
  log_rpc_result("publish-metadata", status, call_status);
  if (status != ZX_OK) {
    return status;
  }
  return call_status;
}

zx_status_t devhost_device_add_composite(const fbl::RefPtr<zx_device_t>& dev, const char* name,
                                         const zx_device_prop_t* props, size_t props_count,
                                         const device_component_t* components,
                                         size_t components_count,
                                         uint32_t coresident_device_index) {
  if ((props == nullptr && props_count > 0) || components == nullptr || name == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (components_count > fuchsia_device_manager_COMPONENTS_MAX) {
    return ZX_ERR_INVALID_ARGS;
  }
  const zx::channel& rpc = *dev->rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }

  // Ideally we could perform the entire serialization with a single
  // allocation, but for now we allocate this (potentially large) array on
  // the heap.  The array is extra-large because of the use of FIDL array
  // types instead of vector types, to get around the SimpleLayout
  // restrictions.
  std::unique_ptr<fuchsia_device_manager_DeviceComponent[]> fidl_components(
      new fuchsia_device_manager_DeviceComponent[fuchsia_device_manager_COMPONENTS_MAX]());
  for (size_t i = 0; i < components_count; ++i) {
    auto& component = fidl_components[i];
    component.parts_count = components[i].parts_count;
    if (component.parts_count > fuchsia_device_manager_DEVICE_COMPONENT_PARTS_MAX) {
      return ZX_ERR_INVALID_ARGS;
    }
    for (size_t j = 0; j < component.parts_count; ++j) {
      auto& part = fidl_components[i].parts[j];
      part.match_program_count = components[i].parts[j].instruction_count;
      if (part.match_program_count >
          fuchsia_device_manager_DEVICE_COMPONENT_PART_INSTRUCTIONS_MAX) {
        return ZX_ERR_INVALID_ARGS;
      }

      static_assert(sizeof(components[i].parts[j].match_program[0]) ==
                    sizeof(part.match_program[0]));
      memcpy(part.match_program, components[i].parts[j].match_program,
             sizeof(part.match_program[0]) * part.match_program_count);
    }
  }

  log_rpc(dev, "create-composite");
  zx_status_t call_status;
  static_assert(sizeof(props[0]) == sizeof(uint64_t));
  zx_status_t status = fuchsia_device_manager_CoordinatorAddCompositeDevice(
      rpc.get(), name, strlen(name), reinterpret_cast<const uint64_t*>(props), props_count,
      fidl_components.get(), static_cast<uint32_t>(components_count), coresident_device_index,
      &call_status);
  log_rpc_result("create-composite", status, call_status);
  if (status != ZX_OK) {
    return status;
  }
  return call_status;
}

zx_handle_t root_resource_handle;

zx_status_t devhost_start_connection(fbl::unique_ptr<DevfsConnection> conn, zx::channel h) {
  conn->set_channel(std::move(h));
  return DevfsConnection::BeginWait(std::move(conn), DevhostAsyncLoop()->dispatcher());
}

int device_host_main(int argc, char** argv) {
  devhost_io_init();

  log(TRACE, "devhost: main()\n");

  zx::channel root_conn_channel(zx_take_startup_handle(PA_HND(PA_USER0, 0)));
  if (!root_conn_channel.is_valid()) {
    log(ERROR, "devhost: rpc handle invalid\n");
    return -1;
  }

  root_resource_handle = zx_take_startup_handle(PA_HND(PA_RESOURCE, 0));
  if (root_resource_handle == ZX_HANDLE_INVALID) {
    log(TRACE, "devhost: no root resource handle!\n");
  }

  zx_status_t r;

  if (getenv_bool("driver.tracing.enable", true)) {
    r = devhost_start_trace_provider();
    if (r != ZX_OK) {
      log(INFO, "devhost: error registering as trace provider: %d\n", r);
      // This is not a fatal error.
    }
  }

  r = devhost_connect_scheduler_profile_provider();
  if (r != ZX_OK) {
    log(INFO, "devhost: error connecting to profile provider: %d\n", r);
    return -1;
  }

  if ((r = SetupRootDevcoordinatorConnection(std::move(root_conn_channel))) != ZX_OK) {
    log(ERROR, "devhost: could not watch rpc channel: %d\n", r);
    return -1;
  }

  r = DevhostAsyncLoop()->Run(zx::time::infinite(), false /* once */);
  log(ERROR, "devhost: async loop finished: %d\n", r);

  return 0;
}

}  // namespace devmgr

__EXPORT int devmgr_device_host_main(int argc, char** argv) {
  return devmgr::device_host_main(argc, argv);
}
