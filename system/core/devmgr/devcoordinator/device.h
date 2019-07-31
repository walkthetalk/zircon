// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_DEVICE_H_
#define ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_DEVICE_H_

#include <ddk/device.h>
#include <fbl/array.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <fbl/ref_counted.h>
#include <fbl/string.h>
#include <fuchsia/device/manager/c/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/cpp/wait.h>
#include <lib/zx/channel.h>
#include <lib/zx/event.h>

#include <variant>

#include "../shared/async-loop-ref-counted-rpc-handler.h"
#include "composite-device.h"
#include "driver-test-reporter.h"
#include "metadata.h"

namespace devmgr {

class Coordinator;
class Devhost;
struct Devnode;
class SuspendContext;
class SuspendTask;

// clang-format off

// This device is never destroyed
#define DEV_CTX_IMMORTAL           0x01

// This device requires that children are created in a
// new devhost attached to a proxy device
#define DEV_CTX_MUST_ISOLATE       0x02

// This device may be bound multiple times
#define DEV_CTX_MULTI_BIND         0x04

// This device is bound and not eligible for binding
// again until unbound.  Not allowed on MULTI_BIND ctx.
#define DEV_CTX_BOUND              0x08

// Device has been remove()'d
#define DEV_CTX_DEAD               0x10

// This device is a component of a composite device and
// can be part of multiple composite devices.
#define DEV_CTX_ALLOW_MULTI_COMPOSITE    0x20

// Device is a proxy -- its "parent" is the device it's
// a proxy to.
#define DEV_CTX_PROXY              0x40

// Device is not visible in devfs or bindable.
// Devices may be created in this state, but may not
// return to this state once made visible.
#define DEV_CTX_INVISIBLE          0x80

// Signals used on the test event
#define TEST_BIND_DONE_SIGNAL ZX_USER_SIGNAL_0
#define TEST_SUSPEND_DONE_SIGNAL ZX_USER_SIGNAL_1
#define TEST_RESUME_DONE_SIGNAL ZX_USER_SIGNAL_2
#define TEST_REMOVE_DONE_SIGNAL ZX_USER_SIGNAL_3

constexpr zx::duration kDefaultTestTimeout = zx::sec(5);

// clang-format on

struct Device : public fbl::RefCounted<Device>, public AsyncLoopRefCountedRpcHandler<Device> {
  // Node for entry in device child list
  struct Node {
    static fbl::DoublyLinkedListNodeState<Device*>& node_state(Device& obj) { return obj.node_; }
  };

  struct DevhostNode {
    static fbl::DoublyLinkedListNodeState<Device*>& node_state(Device& obj) {
      return obj.devhost_node_;
    }
  };

  struct AllDevicesNode {
    static fbl::DoublyLinkedListNodeState<Device*>& node_state(Device& obj) {
      return obj.all_devices_node_;
    }
  };

  // This iterator provides access to a list of devices that does not provide
  // mechanisms for mutating that list.  With this, a user can get mutable
  // access to a device in the list.  This is achieved by making the linked
  // list iterator opaque. It is not safe to modify the underlying list while
  // this iterator is in use.
  template <typename IterType, typename DeviceType>
  class ChildListIterator {
   public:
    ChildListIterator() : state_(Done{}) {}
    explicit ChildListIterator(DeviceType* device)
        : cur_component_(Composite{}), state_(device->children_.begin()), device_(device) {
      if (device_->parent_) {
        cur_component_ = device->parent_->components().begin();
      }
      SkipInvalidStates();
    }
    ChildListIterator operator++(int) {
      auto other = *this;
      ++*this;
      return other;
    }
    bool operator==(const ChildListIterator& other) const { return state_ == other.state_; }
    bool operator!=(const ChildListIterator& other) const { return !(state_ == other.state_); }

    // The iterator implementation for the child list.  This is the source of truth
    // for what devices are children of the device.
    ChildListIterator& operator++() {
      std::visit(
          [this](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, IterType>) {
              ++arg;
            } else if constexpr (std::is_same_v<T, Composite>) {
                cur_component_++;
            } else if constexpr (std::is_same_v<T, Done>) {
              state_ = Done{};
            }
          },
          state_);
      SkipInvalidStates();
      return *this;
    }

    DeviceType& operator*() const {
      return std::visit(
          [this](auto&& arg) -> DeviceType& {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, IterType>) {
              return *arg;
            } else if constexpr (std::is_same_v<T, Composite>) {
              return *(cur_component_->composite()->device());
            } else {
              __builtin_trap();
            }
          },
          state_);
    }

   private:
    // Advance the iterator to the next valid state or reach the done state.
    // This is used to handle advancement between the different state variants.
    void SkipInvalidStates() {
      bool more = true;
      while (more) {
        more = std::visit(
            [this](auto&& arg) {
              using T = std::decay_t<decltype(arg)>;
              if constexpr (std::is_same_v<T, IterType>) {
                // Check if there are any more children in the list.  If
                // there are, we're in a valid state and can stop.
                // Otherwise, advance to the next variant and check if
                // it's a valid state.
                if (arg != device_->children_.end()) {
                  return false;
                }
                state_ = Composite{};
                return true;
              } else if constexpr (std::is_same_v<T, Composite>) {
                // Check if this device is an internal component device
                // that bound to a composite component.  If it is, and
                // the composite has been constructed, the iterator
                // should yield the composite.
                if (device_->parent_) {
                  if (cur_component_ != device_->parent_->components().end() &&
                      cur_component_->composite()->device() != nullptr) {
                    return false;
                  }
                }
                state_ = Done{};
                return false;
              } else if constexpr (std::is_same_v<T, Done>) {
                return false;
              }
            },
            state_);
      }
    }

    using Composite =
        fbl::DoublyLinkedList<CompositeDeviceComponent*, CompositeDeviceComponent::DeviceNode>::iterator;
    struct Done {
      bool operator==(Done) const { return true; }
    };
    Composite cur_component_;
    std::variant<IterType, Composite, Done> state_;
    DeviceType* device_;
  };

  // This class exists to allow consumers of the Device class to write
  //   for (auto& child : dev->children())
  // and get mutable access to the children without getting mutable access to
  // the list.
  template <typename DeviceType, typename IterType>
  class ChildListIteratorFactory {
   public:
    explicit ChildListIteratorFactory(DeviceType* device) : device_(device) {}

    IterType begin() const { return IterType(device_); }
    IterType end() const { return IterType(); }

    bool is_empty() const { return begin() == end(); }

   private:
    DeviceType* device_;
  };

  Device(Coordinator* coord, fbl::String name, fbl::String libname, fbl::String args,
         fbl::RefPtr<Device> parent, uint32_t protocol_id, zx::channel client_remote);
  ~Device();

  // Create a new device with the given parameters.  This sets up its
  // relationship with its parent and devhost and adds its RPC channel to the
  // coordinator's async loop.  This does not add the device to the
  // coordinator's devices_ list, or trigger publishing
  static zx_status_t Create(Coordinator* coordinator, const fbl::RefPtr<Device>& parent,
                            fbl::String name, fbl::String driver_path, fbl::String args,
                            uint32_t protocol_id, fbl::Array<zx_device_prop_t> props,
                            zx::channel rpc, bool invisible, zx::channel client_remote,
                            fbl::RefPtr<Device>* device);
  static zx_status_t CreateComposite(Coordinator* coordinator, Devhost* devhost,
                                     const CompositeDevice& composite, zx::channel rpc,
                                     fbl::RefPtr<Device>* device);
  zx_status_t CreateProxy();

  static void HandleRpc(fbl::RefPtr<Device>&& dev, async_dispatcher_t* dispatcher,
                        async::WaitBase* wait, zx_status_t status,
                        const zx_packet_signal_t* signal);

  // We do not want to expose the list itself for mutation, even if the
  // children are allowed to be mutated.  We manage this by making the
  // iterator opaque.
  using NonConstChildListIterator =
      ChildListIterator<fbl::DoublyLinkedList<Device*, Node>::iterator, Device>;
  using ConstChildListIterator =
      ChildListIterator<fbl::DoublyLinkedList<Device*, Node>::const_iterator, const Device>;
  using NonConstChildListIteratorFactory =
      ChildListIteratorFactory<Device, NonConstChildListIterator>;
  using ConstChildListIteratorFactory =
      ChildListIteratorFactory<const Device, ConstChildListIterator>;
  NonConstChildListIteratorFactory children() { return NonConstChildListIteratorFactory(this); }
  ConstChildListIteratorFactory children() const { return ConstChildListIteratorFactory(this); }

  // Signal that this device is ready for bind to happen.  This should happen
  // either immediately after the device is created, if it's created visible,
  // or after it becomes visible.
  zx_status_t SignalReadyForBind(zx::duration delay = zx::sec(0));

  using SuspendCompletion = fit::function<void(zx_status_t)>;
  // Issue a Suspend request to this device.  When the response comes in, the
  // given completion will be invoked.
  zx_status_t SendSuspend(uint32_t flags, SuspendCompletion completion);

  // Break the relationship between this device object and its parent
  void DetachFromParent();

  // Sets the properties of this device.  Returns an error if the properties
  // array contains more than one property from the BIND_TOPO_* range.
  zx_status_t SetProps(fbl::Array<const zx_device_prop_t> props);
  const fbl::Array<const zx_device_prop_t>& props() const { return props_; }
  const zx_device_prop_t* topo_prop() const { return topo_prop_; }

  const fbl::RefPtr<Device>& parent() { return parent_; }
  fbl::RefPtr<const Device> parent() const { return parent_; }

  const fbl::RefPtr<Device>& proxy() { return proxy_; }
  fbl::RefPtr<const Device> proxy() const { return proxy_; }

  uint32_t protocol_id() const { return protocol_id_; }

  bool is_bindable() const {
    return !(flags & (DEV_CTX_BOUND | DEV_CTX_INVISIBLE)) && (state_ != Device::State::kDead);
  }

  bool is_composite_bindable() const {
    if (flags & (DEV_CTX_DEAD | DEV_CTX_INVISIBLE)) {
      return false;
    }
    if ((flags & DEV_CTX_BOUND) && !(flags & DEV_CTX_ALLOW_MULTI_COMPOSITE)) {
      return false;
    }
    return true;
  }

  void push_component(CompositeDeviceComponent* component) { components_.push_back(component); }
  bool is_components_empty() { return components_.is_empty(); }

  fbl::DoublyLinkedList<CompositeDeviceComponent*, CompositeDeviceComponent::DeviceNode>&
  components() {
    return components_;
  }
  // If the device was created as a composite, this returns its description.
  CompositeDevice* composite() const {
    auto val = std::get_if<CompositeDevice*>(&composite_);
    return val ? *val : nullptr;
  }
  void set_composite(CompositeDevice* composite) {
    ZX_ASSERT(std::holds_alternative<UnassociatedWithComposite>(composite_));
    composite_ = composite;
  }
  void disassociate_from_composite() { composite_ = UnassociatedWithComposite{}; }

  void set_host(Devhost* host);
  Devhost* host() const { return host_; }
  uint64_t local_id() const { return local_id_; }

  const fbl::DoublyLinkedList<fbl::unique_ptr<Metadata>, Metadata::Node>& metadata() const {
    return metadata_;
  }
  void AddMetadata(fbl::unique_ptr<Metadata> md) { metadata_.push_front(std::move(md)); }

  // Creates a new suspend task if necessary and returns a reference to it.
  // If one is already in-progress, a reference to it is returned instead
  fbl::RefPtr<SuspendTask> RequestSuspendTask(uint32_t suspend_flags);
  // Run the completion for the outstanding suspend, if any.  This method is
  // only exposed currently because RemoveDevice is on Coordinator instead of
  // Device.
  void CompleteSuspend(zx_status_t status);

  zx_status_t DriverCompatibiltyTest();

  zx::channel take_client_remote() { return std::move(client_remote_); }

  const fbl::String& name() const { return name_; }
  const fbl::String& libname() const { return libname_; }
  const fbl::String& args() const { return args_; }

  Coordinator* coordinator;
  uint32_t flags = 0;

  // The backoff between each driver retry. This grows exponentially.
  zx::duration backoff = zx::msec(250);
  // The number of retries left for the driver.
  uint32_t retries = 4;
  Devnode* self = nullptr;
  Devnode* link = nullptr;

  // TODO(teisenbe): We probably want more states.
  enum class State {
    kActive,
    kSuspending,  // The devhost is in the process of suspending the device.
    kSuspended,
    kDead,        // The device has been remove()'d
  };

  void set_state(Device::State state) { state_ = state; }
  State state() const { return state_; }

  enum class TestStateMachine {
    kTestNotStarted = 1,
    kTestUnbindSent,
    kTestBindSent,
    kTestBindDone,
    kTestSuspendSent,
    kTestSuspendDone,
    kTestResumeSent,
    kTestResumeDone,
    kTestDone,
  };

  TestStateMachine test_state() {
    fbl::AutoLock<fbl::Mutex> lock(&test_state_lock_);
    return test_state_;
  }

  void set_test_state(TestStateMachine new_state) {
    fbl::AutoLock<fbl::Mutex> lock(&test_state_lock_);
    test_state_ = new_state;
  }
  void set_test_time(zx::duration& test_time) { test_time_ = test_time; }
  void set_test_reply_required(bool required) { test_reply_required_ = required; }
  zx::duration& test_time() { return test_time_; }
  const char* GetTestDriverName();
  zx::event& test_event() { return test_event_; }

  // This is public for testing purposes.
  std::unique_ptr<DriverTestReporter> test_reporter;

 private:
  zx_status_t HandleRead();
  int RunCompatibilityTests();

  void HandleTestOutput(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                        const zx_packet_signal_t* signal);

  const fbl::String name_;
  const fbl::String libname_;
  const fbl::String args_;

  fbl::RefPtr<Device> parent_;
  const uint32_t protocol_id_;

  fbl::RefPtr<Device> proxy_;

  fbl::Array<const zx_device_prop_t> props_;
  // If the device has a topological property in |props|, this points to it.
  const zx_device_prop_t* topo_prop_ = nullptr;

  async::TaskClosure publish_task_;

  // listnode for this device in its parent's list-of-children
  fbl::DoublyLinkedListNodeState<Device*> node_;

  // List of all child devices of this device, except for composite devices.
  // Composite devices are excluded because their multiple-parent nature
  // precludes using the same intrusive nodes as single-parent devices.
  fbl::DoublyLinkedList<Device*, Node> children_;

  // Metadata entries associated to this device.
  fbl::DoublyLinkedList<fbl::unique_ptr<Metadata>, Metadata::Node> metadata_;

  // listnode for this device in the all devices list
  fbl::DoublyLinkedListNodeState<Device*> all_devices_node_;

  // listnode for this device in its devhost's list-of-devices
  fbl::DoublyLinkedListNodeState<Device*> devhost_node_;

  // list of all components that this device bound to.
  fbl::DoublyLinkedList<CompositeDeviceComponent*, CompositeDeviceComponent::DeviceNode>
      components_;

  // - If this device is part of a composite device, this is inhabited by
  //   CompositeDeviceComponent* and it points to the component that matched it.
  //   Note that this is only set on the device that matched the component, not
  //   the "component device" added by the component driver.
  // - If this device is a composite device, this is inhabited by
  //   CompositeDevice* and it points to the composite that describes it.
  // - Otherwise, it is inhabited by UnassociatedWithComposite
  struct UnassociatedWithComposite {};
  std::variant<UnassociatedWithComposite, CompositeDevice*> composite_;

  Devhost* host_ = nullptr;
  // The id of this device from the perspective of the devhost.  This can be
  // used to communicate with the devhost about this device.
  uint64_t local_id_ = 0;

  // The current state of the device
  State state_ = State::kActive;

  // If a suspend is in-progress, this task represents it.
  fbl::RefPtr<SuspendTask> active_suspend_;
  // If a suspend is in-progress, this completion will be invoked when it is
  // completed.  It will likely mark |active_suspend_| as completed and clear
  // it.
  SuspendCompletion suspend_completion_;

  // For attaching as an open connection to the proxy device,
  // or once the device becomes visible.
  zx::channel client_remote_;

  // For compatibility tests.
  fbl::Mutex test_state_lock_;
  TestStateMachine test_state_ __TA_GUARDED(test_state_lock_) = TestStateMachine::kTestNotStarted;
  zx::event test_event_;
  zx::duration test_time_;
  fuchsia_device_manager_CompatibilityTestStatus test_status_;
  bool test_reply_required_ = false;

  // The driver sends output from run_unit_tests over this channel.
  zx::channel test_output_;

  // Async waiter that drives the consumption of test_output_. It is triggered when the channel is
  // closed by the driver, signalling the end of the tests. We don't print log messages until the
  // entire test is finished to avoid interleaving output from multiple drivers.
  async::WaitMethod<Device, &Device::HandleTestOutput> test_wait_{this};
};

}  // namespace devmgr

#endif  // ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_DEVICE_H_
