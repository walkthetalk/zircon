// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <ddk/driver.h>
#include <fbl/auto_call.h>
#include <fuchsia/device/manager/c/fidl.h>
#include <fuchsia/driver/test/c/fidl.h>
#include <lib/fidl/coding.h>

#include "../shared/fidl_txn.h"
#include "../shared/log.h"
#include "coordinator.h"
#include "devfs.h"
#include "fidl.h"
#include "suspend-task.h"

namespace devmgr {

Device::Device(Coordinator* coord, fbl::String name, fbl::String libname, fbl::String args,
               fbl::RefPtr<Device> parent, uint32_t protocol_id, zx::channel client_remote)
    : coordinator(coord),
      name_(std::move(name)),
      libname_(std::move(libname)),
      args_(std::move(args)),
      parent_(std::move(parent)),
      protocol_id_(protocol_id),
      publish_task_([this] { coordinator->HandleNewDevice(fbl::WrapRefPtr(this)); }),
      client_remote_(std::move(client_remote)) {
  test_reporter = std::make_unique<DriverTestReporter>(name_);
}

Device::~Device() {
  // Ideally we'd assert here that immortal devices are never destroyed, but
  // they're destroyed when the Coordinator object is cleaned up in tests.
  // We can probably get rid of the IMMORTAL flag, since if the Coordinator is
  // holding a reference we shouldn't be able to hit that check, in which case
  // the flag is only used to modify the proxy library loading behavior.

  log(DEVLC, "devcoordinator: destroy dev %p name='%s'\n", this, name_.data());

  devfs_unpublish(this);

  // Drop our reference to our devhost if we still have it
  set_host(nullptr);

  fbl::unique_ptr<Metadata> md;
  while ((md = metadata_.pop_front()) != nullptr) {
    if (md->has_path) {
      // return to published_metadata list
      coordinator->AppendPublishedMetadata(std::move(md));
    } else {
      // metadata was attached directly to this device, so we release it now
    }
  }

  // TODO: cancel any pending rpc responses
  // TODO: Have dtor assert that DEV_CTX_IMMORTAL set on flags
}

zx_status_t Device::Create(Coordinator* coordinator, const fbl::RefPtr<Device>& parent,
                           fbl::String name, fbl::String driver_path, fbl::String args,
                           uint32_t protocol_id, fbl::Array<zx_device_prop_t> props,
                           zx::channel rpc, bool invisible, zx::channel client_remote,
                           fbl::RefPtr<Device>* device) {
  fbl::RefPtr<Device> real_parent;
  // If our parent is a proxy, for the purpose of devfs, we need to work with
  // *its* parent which is the device that it is proxying.
  if (parent->flags & DEV_CTX_PROXY) {
    real_parent = parent->parent();
  } else {
    real_parent = parent;
  }

  auto dev = fbl::MakeRefCounted<Device>(coordinator, std::move(name), std::move(driver_path),
                                         std::move(args), real_parent, protocol_id,
                                         std::move(client_remote));
  if (!dev) {
    return ZX_ERR_NO_MEMORY;
  }
  zx_status_t status = dev->SetProps(std::move(props));
  if (status != ZX_OK) {
    return status;
  }

  dev->set_channel(std::move(rpc));

  // If we have bus device args we are, by definition, a bus device.
  if (dev->args_.size() > 0) {
    dev->flags |= DEV_CTX_MUST_ISOLATE;
  }

  // We exist within our parent's device host
  dev->set_host(parent->host());

  // We must mark the device as invisible before publishing so
  // that we don't send "device added" notifications.
  if (invisible) {
    dev->flags |= DEV_CTX_INVISIBLE;
  }

  if ((status = devfs_publish(real_parent, dev)) < 0) {
    return status;
  }

  if ((status = Device::BeginWait(dev, coordinator->dispatcher())) != ZX_OK) {
    return status;
  }

  if (dev->host_) {
    // TODO host == nullptr should be impossible
    dev->host_->devices().push_back(dev.get());
  }
  real_parent->children_.push_back(dev.get());
  log(DEVLC, "devcoord: dev %p name='%s' (child)\n", real_parent.get(), real_parent->name().data());

  *device = std::move(dev);
  return ZX_OK;
}

zx_status_t Device::CreateComposite(Coordinator* coordinator, Devhost* devhost,
                                    const CompositeDevice& composite, zx::channel rpc,
                                    fbl::RefPtr<Device>* device) {
  const auto& composite_props = composite.properties();
  fbl::Array<zx_device_prop_t> props(new zx_device_prop_t[composite_props.size()],
                                     composite_props.size());
  memcpy(props.get(), composite_props.get(), props.size() * sizeof(props[0]));

  auto dev =
      fbl::MakeRefCounted<Device>(coordinator, composite.name(), fbl::String(), fbl::String(),
                                  nullptr, ZX_PROTOCOL_COMPOSITE, zx::channel());
  if (!dev) {
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t status = dev->SetProps(std::move(props));
  if (status != ZX_OK) {
    return status;
  }

  dev->set_channel(std::move(rpc));
  // We exist within our parent's device host
  dev->set_host(devhost);

  // TODO: Record composite membership

  // TODO(teisenbe): Figure out how to manifest in devfs?  For now just hang it off of
  // the root device?
  if ((status = devfs_publish(coordinator->root_device(), dev)) < 0) {
    return status;
  }

  if ((status = Device::BeginWait(dev, coordinator->dispatcher())) != ZX_OK) {
    return status;
  }

  dev->host_->AddRef();
  dev->host_->devices().push_back(dev.get());

  log(DEVLC, "devcoordinator: composite dev created %p name='%s'\n", dev.get(), dev->name().data());

  *device = std::move(dev);
  return ZX_OK;
}

zx_status_t Device::CreateProxy() {
  ZX_ASSERT(proxy_ == nullptr);

  fbl::String driver_path = libname_;
  // non-immortal devices, use foo.proxy.so for
  // their proxy devices instead of foo.so
  if (!(this->flags & DEV_CTX_IMMORTAL)) {
    const char* begin = driver_path.data();
    const char* end = strstr(begin, ".so");
    fbl::StringPiece prefix(begin, end == nullptr ? driver_path.size() : end - begin);
    fbl::AllocChecker ac;
    driver_path = fbl::String::Concat({prefix, ".proxy.so"}, &ac);
    if (!ac.check()) {
      return ZX_ERR_NO_MEMORY;
    }
  }

  auto dev =
      fbl::MakeRefCounted<Device>(this->coordinator, name_, std::move(driver_path), fbl::String(),
                                  fbl::WrapRefPtr(this), protocol_id_, zx::channel());
  if (dev == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }

  dev->flags = DEV_CTX_PROXY;
  proxy_ = std::move(dev);
  log(DEVLC, "devcoord: dev %p name='%s' (proxy)\n", this, name_.data());
  return ZX_OK;
}

void Device::DetachFromParent() {
  if (this->flags & DEV_CTX_PROXY) {
    parent_->proxy_ = nullptr;
  } else {
    parent_->children_.erase(*this);
  }
  parent_ = nullptr;
}

zx_status_t Device::SignalReadyForBind(zx::duration delay) {
  return publish_task_.PostDelayed(this->coordinator->dispatcher(), delay);
}

fbl::RefPtr<SuspendTask> Device::RequestSuspendTask(uint32_t suspend_flags) {
  if (active_suspend_) {
    // We don't support different types of suspends concurrently, and
    // shouldn't be able to reach this state.
    ZX_ASSERT(suspend_flags == active_suspend_->suspend_flags());
  } else {
    active_suspend_ = SuspendTask::Create(fbl::WrapRefPtr(this), suspend_flags);
  }
  return active_suspend_;
}

zx_status_t Device::SendSuspend(uint32_t flags, SuspendCompletion completion) {
  if (suspend_completion_) {
    // We already have a pending suspend
    return ZX_ERR_UNAVAILABLE;
  }
  log(DEVLC, "devcoordinator: suspend dev %p name='%s'\n", this, name_.data());
  zx_status_t status = dh_send_suspend(this, flags);
  if (status != ZX_OK) {
    return status;
  }
  state_ = Device::State::kSuspending;
  suspend_completion_ = std::move(completion);
  return ZX_OK;
}

void Device::CompleteSuspend(zx_status_t status) {
  if (status == ZX_OK) {
    // If a device is being removed, any existing suspend task will be forcibly completed,
    // in which case we should not update the state.
    if (state_ != Device::State::kDead) {
      state_ = Device::State::kSuspended;
    }
  } else {
    state_ = Device::State::kActive;
  }

  active_suspend_ = nullptr;
  SuspendCompletion completion(std::move(suspend_completion_));
  if (completion) {
    completion(status);
  }
}

// Handle inbound messages from devhost to devices
void Device::HandleRpc(fbl::RefPtr<Device>&& dev, async_dispatcher_t* dispatcher,
                       async::WaitBase* wait, zx_status_t status,
                       const zx_packet_signal_t* signal) {
  if (status != ZX_OK) {
    log(ERROR, "devcoordinator: Device::HandleRpc aborting, saw status %d\n", status);
    return;
  }

  if (signal->observed & ZX_CHANNEL_READABLE) {
    zx_status_t r;
    if ((r = dev->HandleRead()) < 0) {
      if (r != ZX_ERR_STOP) {
        log(ERROR, "devcoordinator: device %p name='%s' rpc status: %d\n", dev.get(),
            dev->name().data(), r);
      }
      // If this device isn't already dead (removed), remove it. RemoveDevice() may
      // have been called by the RPC handler, in particular for the RemoveDevice RPC.
      if (dev->state() != Device::State::kDead) {
        dev->coordinator->RemoveDevice(dev, true);
      }
      // Do not start waiting again on this device's channel again
      return;
    }
    Device::BeginWait(std::move(dev), dispatcher);
    return;
  }
  if (signal->observed & ZX_CHANNEL_PEER_CLOSED) {
    log(ERROR, "devcoordinator: device %p name='%s' disconnected!\n", dev.get(),
        dev->name().data());
    dev->coordinator->RemoveDevice(dev, true);
    // Do not start waiting again on this device's channel again
    return;
  }
  log(ERROR, "devcoordinator: no work? %08x\n", signal->observed);
  Device::BeginWait(std::move(dev), dispatcher);
}

static zx_status_t fidl_LogMessage(void* ctx, const char* msg, size_t size) {
  auto dev = static_cast<Device*>(ctx);
  dev->test_reporter->LogMessage(msg, size);
  return ZX_OK;
}

static zx_status_t fidl_LogTestCase(void* ctx, const char* name, size_t name_size,
                                    const fuchsia_driver_test_TestCaseResult* result) {
  auto dev = static_cast<Device*>(ctx);
  dev->test_reporter->LogTestCase(name, name_size, result);
  return ZX_OK;
}

static const fuchsia_driver_test_Logger_ops_t kTestOps = {
    .LogMessage = fidl_LogMessage,
    .LogTestCase = fidl_LogTestCase,
};

void Device::HandleTestOutput(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                              zx_status_t status, const zx_packet_signal_t* signal) {
  if (status != ZX_OK) {
    log(ERROR, "devcoordinator: dev '%s' test output error: %d\n", name_.data(), status);
    return;
  }
  if (!(signal->observed & ZX_CHANNEL_PEER_CLOSED)) {
    log(ERROR, "devcoordinator: dev '%s' test output unexpected signal: %d\n", name_.data(),
        signal->observed);
    return;
  }

  test_reporter->TestStart();

  // Now that the driver has closed the channel, read all of the messages.
  // TODO(ZX-4374): Handle the case where the channel fills up before we begin reading.
  while (true) {
    uint8_t msg_bytes[ZX_CHANNEL_MAX_MSG_BYTES];
    zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
    uint32_t msize = sizeof(msg_bytes);
    uint32_t hcount = fbl::count_of(handles);

    zx_status_t r = test_output_.read(0, &msg_bytes, handles, msize, hcount, &msize, &hcount);
    if (r == ZX_ERR_PEER_CLOSED) {
      test_reporter->TestFinished();
      break;
    } else if (r != ZX_OK) {
      log(ERROR, "devcoordinator: dev '%s' failed to read test output: %d\n", name_.data(), r);
      break;
    }

    fidl_msg_t fidl_msg = {
        .bytes = msg_bytes,
        .handles = handles,
        .num_bytes = msize,
        .num_handles = hcount,
    };

    if (fidl_msg.num_bytes < sizeof(fidl_message_header_t)) {
      zx_handle_close_many(fidl_msg.handles, fidl_msg.num_handles);
      log(ERROR, "devcoordinator: dev '%s' bad test output fidl message header: \n", name_.data());
      break;
    }

    auto header = static_cast<fidl_message_header_t*>(fidl_msg.bytes);
    FidlTxn txn(test_output_, header->txid);
    r = fuchsia_driver_test_Logger_dispatch(this, txn.fidl_txn(), &fidl_msg, &kTestOps);
    if (r != ZX_OK) {
      log(ERROR, "devcoordinator: dev '%s' failed to dispatch test output: %d\n", name_.data(), r);
      break;
    }
  }
}

static zx_status_t fidl_AddDevice(void* ctx, zx_handle_t raw_rpc, const uint64_t* props_data,
                                  size_t props_count, const char* name_data, size_t name_size,
                                  uint32_t protocol_id, const char* driver_path_data,
                                  size_t driver_path_size, const char* args_data, size_t args_size,
                                  fuchsia_device_manager_AddDeviceConfig device_add_config,
                                  zx_handle_t raw_client_remote, fidl_txn_t* txn);
static zx_status_t fidl_AddDeviceInvisible(void* ctx, zx_handle_t raw_rpc,
                                           const uint64_t* props_data, size_t props_count,
                                           const char* name_data, size_t name_size,
                                           uint32_t protocol_id, const char* driver_path_data,
                                           size_t driver_path_size, const char* args_data,
                                           size_t args_size, zx_handle_t raw_client_remote,
                                           fidl_txn_t* txn);
static zx_status_t fidl_RemoveDevice(void* ctx, fidl_txn_t* txn);
static zx_status_t fidl_MakeVisible(void* ctx, fidl_txn_t* txn);
static zx_status_t fidl_RunCompatibilityTests(void* ctx, int64_t hook_wait_time, fidl_txn_t* txn);
static zx_status_t fidl_BindDevice(void* ctx, const char* driver_path_data, size_t driver_path_size,
                                   fidl_txn_t* txn);
static zx_status_t fidl_GetTopologicalPath(void* ctx, fidl_txn_t* txn);
static zx_status_t fidl_LoadFirmware(void* ctx, const char* fw_path_data, size_t fw_path_size,
                                     fidl_txn_t* txn);
static zx_status_t fidl_GetMetadata(void* ctx, uint32_t key, fidl_txn_t* txn);
static zx_status_t fidl_GetMetadataSize(void* ctx, uint32_t key, fidl_txn_t* txn);
static zx_status_t fidl_AddMetadata(void* ctx, uint32_t key, const uint8_t* data_data,
                                    size_t data_count, fidl_txn_t* txn);
static zx_status_t fidl_PublishMetadata(void* ctx, const char* device_path_data,
                                        size_t device_path_size, uint32_t key,
                                        const uint8_t* data_data, size_t data_count,
                                        fidl_txn_t* txn);
static zx_status_t fidl_AddCompositeDevice(
    void* ctx, const char* name_data, size_t name_size, const uint64_t* props_data,
    size_t props_count, const fuchsia_device_manager_DeviceComponent components[16],
    uint32_t components_count, uint32_t coresident_device_index, fidl_txn_t* txn);

static const fuchsia_device_manager_Coordinator_ops_t fidl_ops = {
    .AddDevice = fidl_AddDevice,
    .AddDeviceInvisible = fidl_AddDeviceInvisible,
    .RemoveDevice = fidl_RemoveDevice,
    .MakeVisible = fidl_MakeVisible,
    .BindDevice = fidl_BindDevice,
    .GetTopologicalPath = fidl_GetTopologicalPath,
    .LoadFirmware = fidl_LoadFirmware,
    .GetMetadata = fidl_GetMetadata,
    .GetMetadataSize = fidl_GetMetadataSize,
    .AddMetadata = fidl_AddMetadata,
    .PublishMetadata = fidl_PublishMetadata,
    .AddCompositeDevice = fidl_AddCompositeDevice,

    .DirectoryWatch = fidl_DirectoryWatch,
    .RunCompatibilityTests = fidl_RunCompatibilityTests,
};

zx_status_t Device::HandleRead() {
  uint8_t msg[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t hin[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t msize = sizeof(msg);
  uint32_t hcount = fbl::count_of(hin);

  if (state_ == Device::State::kDead) {
    log(ERROR, "devcoordinator: dev %p already dead (in read)\n", this);
    return ZX_ERR_INTERNAL;
  }

  zx_status_t r;
  if ((r = channel()->read(0, &msg, hin, msize, hcount, &msize, &hcount)) != ZX_OK) {
    return r;
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
  // Check if we're receiving a Coordinator request
  {
    FidlTxn txn(*channel(), hdr->txid);
    r = fuchsia_device_manager_Coordinator_try_dispatch(this, txn.fidl_txn(), &fidl_msg, &fidl_ops);
    if (r != ZX_ERR_NOT_SUPPORTED) {
      return r;
    }
  }

  // TODO: Check txid on the message
  // This is an if statement because, depending on the state of the ordinal
  // migration, GenOrdinal and Ordinal may be the same value.  See FIDL-524.
  uint64_t ordinal = hdr->ordinal;
  if (ordinal == fuchsia_device_manager_DeviceControllerBindDriverOrdinal ||
      ordinal == fuchsia_device_manager_DeviceControllerBindDriverGenOrdinal) {
    const char* err_msg = nullptr;
    r = fidl_decode_msg(&fuchsia_device_manager_DeviceControllerBindDriverResponseTable, &fidl_msg,
                        &err_msg);
    if (r != ZX_OK) {
      log(ERROR, "devcoordinator: rpc: bind-driver '%s' received malformed reply: %s\n",
          name_.data(), err_msg);
      return ZX_ERR_IO;
    }
    auto resp = reinterpret_cast<fuchsia_device_manager_DeviceControllerBindDriverResponse*>(
        fidl_msg.bytes);
    if (resp->status != ZX_OK) {
      // TODO: try next driver, clear BOUND flag
      log(ERROR, "devcoordinator: rpc: bind-driver '%s' status %d\n", name_.data(), resp->status);
    } else {
      Device* real_parent;
      if (flags & DEV_CTX_PROXY) {
        real_parent = this->parent().get();
      } else {
        real_parent = this;
      }

      for (auto& child : real_parent->children()) {
        char bootarg[256] = {0};
        const char* drivername =
            this->coordinator->LibnameToDriver(child.libname().data())->name.data();
        snprintf(bootarg, sizeof(bootarg), "driver.%s.compatibility-tests-enable", drivername);

        if (this->coordinator->boot_args().GetBool(bootarg, false) &&
            (real_parent->test_state() == Device::TestStateMachine::kTestNotStarted)) {
          snprintf(bootarg, sizeof(bootarg), "driver.%s.compatibility-tests-wait-time", drivername);
          const char* test_timeout = coordinator->boot_args().Get(bootarg);
          zx::duration test_time =
              (test_timeout != nullptr ? zx::msec(atoi(test_timeout)) : kDefaultTestTimeout);
          real_parent->set_test_time(test_time);
          real_parent->DriverCompatibiltyTest();
          break;
        } else if (real_parent->test_state() == Device::TestStateMachine::kTestBindSent) {
          real_parent->test_event().signal(0, TEST_BIND_DONE_SIGNAL);
          break;
        }
      }
    }
    if (resp->test_output) {
      log(ERROR, "devcoordinator: rpc: bind-driver '%s' set test channel\n", name_.data());
      test_output_ = zx::channel(resp->test_output);
      test_wait_.set_object(test_output_.get());
      test_wait_.set_trigger(ZX_CHANNEL_PEER_CLOSED);
      zx_status_t status = test_wait_.Begin(coordinator->dispatcher());
      if (status != ZX_OK) {
        log(ERROR, "devcoordinator: rpc: bind-driver '%s' failed to start test output wait: %d\n",
            name_.data(), status);
        return status;
      }
    }
  } else if (ordinal == fuchsia_device_manager_DeviceControllerSuspendOrdinal ||
             ordinal == fuchsia_device_manager_DeviceControllerSuspendGenOrdinal) {
    const char* err_msg = nullptr;
    r = fidl_decode_msg(&fuchsia_device_manager_DeviceControllerSuspendResponseTable, &fidl_msg,
                        &err_msg);
    if (r != ZX_OK) {
      log(ERROR, "devcoordinator: rpc: suspend '%s' received malformed reply: %s\n", name_.data(),
          err_msg);
      return ZX_ERR_IO;
    }
    auto resp =
        reinterpret_cast<fuchsia_device_manager_DeviceControllerSuspendResponse*>(fidl_msg.bytes);
    if (resp->status != ZX_OK) {
      log(ERROR, "devcoordinator: rpc: suspend '%s' status %d\n", name_.data(), resp->status);
    }

    if (!suspend_completion_) {
      log(ERROR, "devcoordinator: rpc: unexpected suspend reply for '%s' status %d\n", name_.data(),
          resp->status);
      return ZX_ERR_IO;
    }
    log(DEVLC, "devcoordinator: suspended dev %p name='%s'\n", this, name_.data());
    CompleteSuspend(resp->status);
  } else {
    log(ERROR, "devcoordinator: rpc: dev '%s' received wrong unexpected reply %16lx\n",
        name_.data(), hdr->ordinal);
    zx_handle_close_many(fidl_msg.handles, fidl_msg.num_handles);
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

zx_status_t Device::SetProps(fbl::Array<const zx_device_prop_t> props) {
  // This function should only be called once
  ZX_DEBUG_ASSERT(props_.get() == nullptr);

  props_ = std::move(props);
  topo_prop_ = nullptr;

  for (const auto prop : props_) {
    if (prop.id >= BIND_TOPO_START && prop.id <= BIND_TOPO_END) {
      if (topo_prop_ != nullptr) {
        return ZX_ERR_INVALID_ARGS;
      }
      topo_prop_ = &prop;
    }
  }
  return ZX_OK;
}

void Device::set_host(Devhost* host) {
  if (host_) {
    this->coordinator->ReleaseDevhost(host_);
  }
  host_ = host;
  local_id_ = 0;
  if (host_) {
    host_->AddRef();
    local_id_ = host_->new_device_id();
  }
}

const char* Device::GetTestDriverName() {
  for (auto& child : children()) {
    return this->coordinator->LibnameToDriver(child.libname().data())->name.data();
  }
  return nullptr;
}

zx_status_t Device::DriverCompatibiltyTest() {
  thrd_t t;
  if (test_state() != TestStateMachine::kTestNotStarted) {
    return ZX_ERR_ALREADY_EXISTS;
  }
  auto cb = [](void* arg) -> int {
    auto dev = fbl::WrapRefPtr(reinterpret_cast<Device*>(arg));
    return dev->RunCompatibilityTests();
  };
  int ret = thrd_create_with_name(&t, cb, this, "compatibility-tests-thread");
  if (ret != thrd_success) {
    log(ERROR,
        "Driver Compatibility test failed for %s: "
        "Thread creation failed\n",
        GetTestDriverName());
    if (test_reply_required_) {
      dh_send_complete_compatibility_tests(
          this, fuchsia_device_manager_CompatibilityTestStatus_ERR_INTERNAL);
    }
    return ZX_ERR_NO_RESOURCES;
  }
  thrd_detach(t);
  return ZX_OK;
}

int Device::RunCompatibilityTests() {
  const char* test_driver_name = GetTestDriverName();
  log(INFO, "%s: Running ddk compatibility test for driver %s \n", __func__, test_driver_name);
  auto cleanup = fbl::MakeAutoCall([this]() {
    if (test_reply_required_) {
      dh_send_complete_compatibility_tests(this, test_status_);
    }
    test_event().reset();
    set_test_state(Device::TestStateMachine::kTestDone);
    set_test_reply_required(false);
  });
  // Device should be bound for test to work
  if (!(flags & DEV_CTX_BOUND) || children().is_empty()) {
    log(ERROR,
        "devcoordinator: Driver Compatibility test failed for %s: "
        "Parent Device not bound\n",
        test_driver_name);
    test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_BIND_NO_DDKADD;
    return -1;
  }
  zx_status_t status = zx::event::create(0, &test_event());
  if (status != ZX_OK) {
    log(ERROR,
        "devcoordinator: Driver Compatibility test failed for %s: "
        "Event creation failed : %d\n",
        test_driver_name, status);
    test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_INTERNAL;
    return -1;
  }

  // Issue unbind on all its children.
  for (auto itr = children().begin(); itr != children().end();) {
    auto& child = *itr;
    itr++;
    this->set_test_state(Device::TestStateMachine::kTestUnbindSent);
    status = dh_send_unbind(&child);
    if (status != ZX_OK) {
      // TODO(ravoorir): How do we return to clean state here? Forcefully
      // remove all the children?
      log(ERROR,
          "devcoordinator: Driver Compatibility test failed for %s: "
          "Sending unbind to %s failed\n",
          test_driver_name, child.name().data());
      test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_INTERNAL;
      return -1;
    }
  }

  zx_signals_t observed = 0;
  // Now wait for the device to be removed.
  status =
      test_event().wait_one(TEST_REMOVE_DONE_SIGNAL, zx::deadline_after(test_time()), &observed);
  if (status != ZX_OK) {
    if (status == ZX_ERR_TIMED_OUT) {
      // The Remove did not complete.
      log(ERROR,
          "devcoordinator: Driver Compatibility test failed for %s: "
          "Timed out waiting for device to be removed. Check if device_remove was "
          "called in the unbind routine of the driver: %d\n",
          test_driver_name, status);
      test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_UNBIND_TIMEOUT;
    } else {
      log(ERROR,
          "devcoordinator: Driver Compatibility test failed for %s: "
          "Error waiting for device to be removed.\n",
          test_driver_name);
      test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_INTERNAL;
    }
    return -1;
  }
  this->set_test_state(Device::TestStateMachine::kTestBindSent);
  this->coordinator->HandleNewDevice(fbl::WrapRefPtr(this));
  observed = 0;
  status = test_event().wait_one(TEST_BIND_DONE_SIGNAL, zx::deadline_after(test_time()), &observed);
  if (status != ZX_OK) {
    if (status == ZX_ERR_TIMED_OUT) {
      // The Bind did not complete.
      log(ERROR,
          "devcoordinator: Driver Compatibility test failed for %s: "
          "Timed out waiting for driver to be bound. Check if Bind routine "
          "of the driver is doing blocking I/O: %d\n",
          test_driver_name, status);
      test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_BIND_TIMEOUT;
    } else {
      log(ERROR,
          "devcoordinator: Driver Compatibility test failed for %s: "
          "Error waiting for driver to be bound: %d\n",
          test_driver_name, status);
      test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_INTERNAL;
    }
    return -1;
  }
  this->set_test_state(Device::TestStateMachine::kTestBindDone);
  if (this->children().is_empty()) {
    log(ERROR,
        "devcoordinator: Driver Compatibility test failed for %s: "
        "Driver Bind routine did not add a child. Check if Bind routine "
        "Called DdkAdd() at the end.\n",
        test_driver_name);
    test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_BIND_NO_DDKADD;
    return -1;
  }
  log(ERROR, "devcoordinator: Driver Compatibility test succeeded for %s\n", test_driver_name);
  // TODO(ravoorir): Test Suspend and Resume hooks
  test_status_ = fuchsia_device_manager_CompatibilityTestStatus_OK;
  return 0;
}

// Handlers for the messages from devices
static zx_status_t fidl_AddDevice(void* ctx, zx_handle_t raw_rpc, const uint64_t* props_data,
                                  size_t props_count, const char* name_data, size_t name_size,
                                  uint32_t protocol_id, const char* driver_path_data,
                                  size_t driver_path_size, const char* args_data, size_t args_size,
                                  fuchsia_device_manager_AddDeviceConfig device_add_config,
                                  zx_handle_t raw_client_remote, fidl_txn_t* txn) {
  auto parent = fbl::WrapRefPtr(static_cast<Device*>(ctx));
  zx::channel rpc(raw_rpc);
  fbl::StringPiece name(name_data, name_size);
  fbl::StringPiece driver_path(driver_path_data, driver_path_size);
  fbl::StringPiece args(args_data, args_size);
  zx::channel client_remote(raw_client_remote);

  fbl::RefPtr<Device> device;
  zx_status_t status = parent->coordinator->AddDevice(
      parent, std::move(rpc), props_data, props_count, name, protocol_id, driver_path, args, false,
      std::move(client_remote), &device);
  if (device != nullptr &&
      (device_add_config & fuchsia_device_manager_AddDeviceConfig_ALLOW_MULTI_COMPOSITE)) {
      device->flags |= DEV_CTX_ALLOW_MULTI_COMPOSITE;
  }
  uint64_t local_id = device != nullptr ? device->local_id() : 0;
  return fuchsia_device_manager_CoordinatorAddDevice_reply(txn, status, local_id);
}

static zx_status_t fidl_AddDeviceInvisible(void* ctx, zx_handle_t raw_rpc,
                                           const uint64_t* props_data, size_t props_count,
                                           const char* name_data, size_t name_size,
                                           uint32_t protocol_id, const char* driver_path_data,
                                           size_t driver_path_size, const char* args_data,
                                           size_t args_size, zx_handle_t raw_client_remote,
                                           fidl_txn_t* txn) {
  auto parent = fbl::WrapRefPtr(static_cast<Device*>(ctx));
  zx::channel rpc(raw_rpc);
  fbl::StringPiece name(name_data, name_size);
  fbl::StringPiece driver_path(driver_path_data, driver_path_size);
  fbl::StringPiece args(args_data, args_size);
  zx::channel client_remote(raw_client_remote);

  fbl::RefPtr<Device> device;
  zx_status_t status = parent->coordinator->AddDevice(
      parent, std::move(rpc), props_data, props_count, name, protocol_id, driver_path, args, true,
      std::move(client_remote), &device);
  
  uint64_t local_id = device != nullptr ? device->local_id() : 0;
  return fuchsia_device_manager_CoordinatorAddDeviceInvisible_reply(txn, status, local_id);
}

static zx_status_t fidl_RemoveDevice(void* ctx, fidl_txn_t* txn) {
  auto dev = fbl::WrapRefPtr(static_cast<Device*>(ctx));
  if (dev->state() == Device::State::kSuspending) {
    log(ERROR, "devcoordinator: rpc: remove-device '%s' forbidden when device is suspending\n",
        dev->name().data());
    return fuchsia_device_manager_CoordinatorRemoveDevice_reply(txn, ZX_ERR_BAD_STATE);
  }

  log(RPC_IN, "devcoordinator: rpc: remove-device '%s'\n", dev->name().data());
  // TODO(teisenbe): RemoveDevice and the reply func can return errors.  We should probably
  // act on it, but the existing code being migrated does not.
  dev->coordinator->RemoveDevice(dev, false);
  fuchsia_device_manager_CoordinatorRemoveDevice_reply(txn, ZX_OK);

  // Return STOP to signal we are done with this channel
  return ZX_ERR_STOP;
}

static zx_status_t fidl_MakeVisible(void* ctx, fidl_txn_t* txn) {
  auto dev = fbl::WrapRefPtr(static_cast<Device*>(ctx));
  if (dev->coordinator->InSuspend()) {
    log(ERROR, "devcoordinator: rpc: make-visible '%s' forbidden in suspend\n", dev->name().data());
    return fuchsia_device_manager_CoordinatorMakeVisible_reply(txn, ZX_ERR_BAD_STATE);
  }
  log(RPC_IN, "devcoordinator: rpc: make-visible '%s'\n", dev->name().data());
  // TODO(teisenbe): MakeVisibile can return errors.  We should probably
  // act on it, but the existing code being migrated does not.
  dev->coordinator->MakeVisible(dev);
  return fuchsia_device_manager_CoordinatorMakeVisible_reply(txn, ZX_OK);
}

static zx_status_t fidl_BindDevice(void* ctx, const char* driver_path_data, size_t driver_path_size,
                                   fidl_txn_t* txn) {
  auto dev = fbl::WrapRefPtr(static_cast<Device*>(ctx));
  fbl::StringPiece driver_path(driver_path_data, driver_path_size);
  if (dev->coordinator->InSuspend()) {
    log(ERROR, "devcoordinator: rpc: bind-device '%s' forbidden in suspend\n", dev->name().data());
    return fuchsia_device_manager_CoordinatorBindDevice_reply(txn, ZX_ERR_BAD_STATE);
  }
  // Made this log at ERROR instead of RPC_IN to help debug DNO-492; we should
  // take it back down when done with that bug.
  log(ERROR, "devcoordinator: rpc: bind-device '%s'\n", dev->name().data());
  zx_status_t status = dev->coordinator->BindDevice(dev, driver_path, false /* new device */);
  return fuchsia_device_manager_CoordinatorBindDevice_reply(txn, status);
}

static zx_status_t fidl_RunCompatibilityTests(void* ctx, int64_t hook_wait_time, fidl_txn_t* txn) {
  auto dev = fbl::WrapRefPtr(static_cast<Device*>(ctx));
  fbl::RefPtr<Device>& real_parent = dev;
  zx_status_t status = ZX_OK;
  if (dev->flags & DEV_CTX_PROXY) {
    real_parent = dev->parent();
  }
  zx::duration test_time = zx::nsec(hook_wait_time);
  real_parent->set_test_time(test_time);
  real_parent->set_test_reply_required(true);
  status = real_parent->DriverCompatibiltyTest();
  return fuchsia_device_manager_CoordinatorRunCompatibilityTests_reply(txn, status);
}

static zx_status_t fidl_GetTopologicalPath(void* ctx, fidl_txn_t* txn) {
  char path[fuchsia_device_manager_DEVICE_PATH_MAX + 1];

  auto dev = fbl::WrapRefPtr(static_cast<Device*>(ctx));
  zx_status_t status;
  if ((status = dev->coordinator->GetTopologicalPath(dev, path, sizeof(path))) != ZX_OK) {
    return fuchsia_device_manager_CoordinatorGetTopologicalPath_reply(txn, status, nullptr, 0);
  }
  return fuchsia_device_manager_CoordinatorGetTopologicalPath_reply(txn, ZX_OK, path, strlen(path));
}

static zx_status_t fidl_LoadFirmware(void* ctx, const char* fw_path_data, size_t fw_path_size,
                                     fidl_txn_t* txn) {
  auto dev = fbl::WrapRefPtr(static_cast<Device*>(ctx));

  char fw_path[fuchsia_device_manager_DEVICE_PATH_MAX + 1];
  memcpy(fw_path, fw_path_data, fw_path_size);
  fw_path[fw_path_size] = 0;

  zx::vmo vmo;
  uint64_t size = 0;
  zx_status_t status;
  if ((status = dev->coordinator->LoadFirmware(dev, fw_path, &vmo, &size)) != ZX_OK) {
    return fuchsia_device_manager_CoordinatorLoadFirmware_reply(txn, status, ZX_HANDLE_INVALID, 0);
  }

  return fuchsia_device_manager_CoordinatorLoadFirmware_reply(txn, ZX_OK, vmo.release(), size);
}

static zx_status_t fidl_GetMetadata(void* ctx, uint32_t key, fidl_txn_t* txn) {
  auto dev = fbl::WrapRefPtr(static_cast<Device*>(ctx));

  uint8_t data[fuchsia_device_manager_METADATA_MAX];
  size_t actual = 0;
  zx_status_t status = dev->coordinator->GetMetadata(dev, key, data, sizeof(data), &actual);
  if (status != ZX_OK) {
    return fuchsia_device_manager_CoordinatorGetMetadata_reply(txn, status, nullptr, 0);
  }
  return fuchsia_device_manager_CoordinatorGetMetadata_reply(txn, status, data, actual);
}

static zx_status_t fidl_GetMetadataSize(void* ctx, uint32_t key, fidl_txn_t* txn) {
  auto dev = fbl::WrapRefPtr(static_cast<Device*>(ctx));
  size_t size;
  zx_status_t status = dev->coordinator->GetMetadataSize(dev, key, &size);
  if (status != ZX_OK) {
    return fuchsia_device_manager_CoordinatorGetMetadataSize_reply(txn, status, 0);
  }
  return fuchsia_device_manager_CoordinatorGetMetadataSize_reply(txn, status, size);
}

static zx_status_t fidl_AddMetadata(void* ctx, uint32_t key, const uint8_t* data_data,
                                    size_t data_count, fidl_txn_t* txn) {
  static_assert(fuchsia_device_manager_METADATA_MAX <= UINT32_MAX);

  auto dev = fbl::WrapRefPtr(static_cast<Device*>(ctx));
  zx_status_t status =
      dev->coordinator->AddMetadata(dev, key, data_data, static_cast<uint32_t>(data_count));
  return fuchsia_device_manager_CoordinatorAddMetadata_reply(txn, status);
}

static zx_status_t fidl_PublishMetadata(void* ctx, const char* device_path_data,
                                        size_t device_path_size, uint32_t key,
                                        const uint8_t* data_data, size_t data_count,
                                        fidl_txn_t* txn) {
  auto dev = fbl::WrapRefPtr(static_cast<Device*>(ctx));

  char path[fuchsia_device_manager_DEVICE_PATH_MAX + 1];
  memcpy(path, device_path_data, device_path_size);
  path[device_path_size] = 0;

  zx_status_t status = dev->coordinator->PublishMetadata(dev, path, key, data_data,
                                                         static_cast<uint32_t>(data_count));
  return fuchsia_device_manager_CoordinatorPublishMetadata_reply(txn, status);
}

static zx_status_t fidl_AddCompositeDevice(
    void* ctx, const char* name_data, size_t name_size, const uint64_t* props_data,
    size_t props_count, const fuchsia_device_manager_DeviceComponent components[16],
    uint32_t components_count, uint32_t coresident_device_index, fidl_txn_t* txn) {
  auto dev = fbl::WrapRefPtr(static_cast<Device*>(ctx));

  fbl::StringPiece name(name_data, name_size);
  auto props = reinterpret_cast<const zx_device_prop_t*>(props_data);

  zx_status_t status = dev->coordinator->AddCompositeDevice(
      dev, name, props, props_count, components, components_count, coresident_device_index);
  return fuchsia_device_manager_CoordinatorAddCompositeDevice_reply(txn, status);
}

}  // namespace devmgr
