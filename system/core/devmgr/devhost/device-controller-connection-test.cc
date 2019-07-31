// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device-controller-connection.h"

#include <fbl/auto_lock.h>
#include <fuchsia/io/llcpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/zx/vmo.h>
#include <zxtest/zxtest.h>

#include <thread>

#include "connection-destroyer.h"
#include "zx-device.h"

namespace {

TEST(DeviceControllerConnectionTestCase, Creation) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);

  fbl::RefPtr<zx_device> dev;
  ASSERT_OK(zx_device::Create(&dev));

  zx::channel device_local, device_remote;
  ASSERT_OK(zx::channel::create(0, &device_local, &device_remote));

  std::unique_ptr<devmgr::DeviceControllerConnection> conn;

  ASSERT_NULL(dev->conn.load());
  ASSERT_OK(devmgr::DeviceControllerConnection::Create(dev, std::move(device_remote), &conn));
  ASSERT_NOT_NULL(dev->conn.load());

  ASSERT_OK(devmgr::DeviceControllerConnection::BeginWait(std::move(conn), loop.dispatcher()));
  ASSERT_OK(loop.RunUntilIdle());
}

TEST(DeviceControllerConnectionTestCase, PeerClosedDuringReply) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);

  fbl::RefPtr<zx_device> dev;
  ASSERT_OK(zx_device::Create(&dev));

  zx::channel device_local, device_remote;
  ASSERT_OK(zx::channel::create(0, &device_local, &device_remote));

  class DeviceControllerConnectionTest : public devmgr::DeviceControllerConnection {
   public:
    DeviceControllerConnectionTest(fbl::RefPtr<zx_device> dev, zx::channel rpc,
                                   async_dispatcher_t* dispatcher, zx::channel local)
        : devmgr::DeviceControllerConnection(std::move(dev), std::move(rpc)) {
      dispatcher_ = dispatcher;
      local_ = std::move(local);
    }

    void BindDriver(::fidl::StringView driver_path, ::zx::vmo driver,
                    BindDriverCompleter::Sync completer) override {
      // Pretend that a device closure happened right before we began
      // processing BindDriver.  Close the other half of the channel, so the reply below will fail
      // from ZX_ERR_PEER_CLOSED.
      auto conn = this->dev()->conn.exchange(nullptr);
      devmgr::ConnectionDestroyer::Get()->QueueDeviceControllerConnection(dispatcher_, conn);
      local_.reset();
      completer.Reply(ZX_OK, zx::channel());
    }

   private:
    async_dispatcher_t* dispatcher_;
    zx::channel local_;
  };

  std::unique_ptr<DeviceControllerConnectionTest> conn;
  auto device_local_handle = device_local.get();
  conn = std::make_unique<DeviceControllerConnectionTest>(
      std::move(dev), std::move(device_remote), loop.dispatcher(), std::move(device_local));

  ASSERT_OK(DeviceControllerConnectionTest::BeginWait(std::move(conn), loop.dispatcher()));
  ASSERT_OK(loop.RunUntilIdle());

  // Create a thread to send a BindDriver message.  The thread isn't strictly
  // necessary, but is done out of convenience since the FIDL LLCPP bindings currently don't
  // expose non-zx_channel_call client bindings.
  enum {
    INITIAL,
    VMO_CREATE_FAILED,
    WRONG_CALL_STATUS,
    SUCCESS,
  } thread_status = INITIAL;

  std::thread synchronous_call_thread([owned_channel = device_local_handle, &thread_status]() {
    auto unowned_channel = zx::unowned_channel(owned_channel);
    zx::vmo vmo;
    zx_status_t status = zx::vmo::create(0, 0, &vmo);
    if (status != ZX_OK) {
      thread_status = VMO_CREATE_FAILED;
      return;
    }

    zx_status_t call_status;
    zx::channel test_output;
    status = ::llcpp::fuchsia::device::manager::DeviceController::Call::BindDriver(
        std::move(unowned_channel), ::fidl::StringView(1, ""), std::move(vmo), &call_status,
        &test_output);

    if (status != ZX_ERR_CANCELED) {
      thread_status = WRONG_CALL_STATUS;
      return;
    }
    thread_status = SUCCESS;
    return;
  });

  ASSERT_OK(loop.Run(zx::time::infinite(), true /* run_once */));

  synchronous_call_thread.join();
  ASSERT_EQ(SUCCESS, thread_status);
  ASSERT_FALSE(device_local.is_valid());
}

// Verify we do not abort when an expected PEER_CLOSED comes in.
TEST(DeviceControllerConnectionTestCase, PeerClosed) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);

  fbl::RefPtr<zx_device> dev;
  ASSERT_OK(zx_device::Create(&dev));

  zx::channel device_local, device_remote;
  ASSERT_OK(zx::channel::create(0, &device_local, &device_remote));

  std::unique_ptr<devmgr::DeviceControllerConnection> conn;
  ASSERT_OK(devmgr::DeviceControllerConnection::Create(dev, std::move(device_remote), &conn));

  ASSERT_OK(devmgr::DeviceControllerConnection::BeginWait(std::move(conn), loop.dispatcher()));
  ASSERT_OK(loop.RunUntilIdle());

  // Perform the device shutdown protocol, since otherwise the devhost code
  // will assert, since it is unable to handle unexpected connection closures.
  auto dev_conn = dev->conn.exchange(nullptr);
  devmgr::ConnectionDestroyer::Get()->QueueDeviceControllerConnection(loop.dispatcher(), dev_conn);
  device_local.reset();

  ASSERT_OK(loop.RunUntilIdle());
}

TEST(DeviceControllerConnectionTestCase, UnbindHook) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);

  fbl::RefPtr<zx_device> dev;
  ASSERT_OK(zx_device::Create(&dev));

  zx::channel device_local, device_remote;
  ASSERT_OK(zx::channel::create(0, &device_local, &device_remote));

  class DeviceControllerConnectionTest : public devmgr::DeviceControllerConnection {
   public:
    DeviceControllerConnectionTest(fbl::RefPtr<zx_device> dev, zx::channel rpc)
        : devmgr::DeviceControllerConnection(std::move(dev), std::move(rpc)) {}

    void Unbind(UnbindCompleter::Sync completer) {
      fbl::RefPtr<zx_device> dev = this->dev();
      // Set dev->flags so that we can check that the unbind hook is called in
      // the test.
      dev->flags = DEV_FLAG_DEAD;
    }
  };

  std::unique_ptr<DeviceControllerConnectionTest> conn;
  conn = std::make_unique<DeviceControllerConnectionTest>(std::move(dev), std::move(device_remote));
  fbl::RefPtr<zx_device> my_dev = conn->dev();
  ASSERT_OK(DeviceControllerConnectionTest::BeginWait(std::move(conn), loop.dispatcher()));
  ASSERT_OK(loop.RunUntilIdle());

  // Create a thread to send the Unbind message.  The thread isn't strictly
  // necessary, but is done out of convenience since the FIDL C bindings don't
  // expose non-zx_channel_call client bindings.
  enum {
    INITIAL,
    WRITE_FAILED,
    SUCCESS,
  } thread_status = INITIAL;
  std::thread synchronous_call_thread([channel = device_local.get(), &thread_status]() {
    auto unowned_channel = zx::unowned_channel(channel);
    zx_status_t status = ::llcpp::fuchsia::device::manager::DeviceController::Call::Unbind(
        std::move(unowned_channel));
    if (status != ZX_OK) {
      thread_status = WRITE_FAILED;
      return;
    }
    thread_status = SUCCESS;
  });

  ASSERT_OK(loop.Run(zx::time::infinite(), true /* run_once */));

  synchronous_call_thread.join();
  ASSERT_EQ(SUCCESS, thread_status);
  ASSERT_EQ(my_dev->flags, DEV_FLAG_DEAD);
}

}  // namespace
