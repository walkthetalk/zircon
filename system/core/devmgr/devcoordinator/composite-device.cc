// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "composite-device.h"

#include <zircon/status.h>

#include <utility>

#include "../shared/log.h"
#include "binding-internal.h"
#include "coordinator.h"
#include "fidl.h"

namespace devmgr {

// CompositeDevice methods

CompositeDevice::CompositeDevice(fbl::String name, fbl::Array<const zx_device_prop_t> properties,
                                 uint32_t components_count, uint32_t coresident_device_index)
    : name_(std::move(name)),
      properties_(std::move(properties)),
      components_count_(components_count),
      coresident_device_index_(coresident_device_index) {}

CompositeDevice::~CompositeDevice() = default;

zx_status_t CompositeDevice::Create(const fbl::StringPiece& name,
                                    const zx_device_prop_t* props_data, size_t props_count,
                                    const fuchsia_device_manager_DeviceComponent* components,
                                    size_t components_count, uint32_t coresident_device_index,
                                    std::unique_ptr<CompositeDevice>* out) {
  if (components_count > UINT32_MAX) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::String name_obj(name);
  fbl::Array<zx_device_prop_t> properties(new zx_device_prop_t[props_count], props_count);
  memcpy(properties.get(), props_data, props_count * sizeof(props_data[0]));

  auto dev = std::make_unique<CompositeDevice>(std::move(name), std::move(properties),
                                               components_count, coresident_device_index);
  for (uint32_t i = 0; i < components_count; ++i) {
    const auto& fidl_component = components[i];
    size_t parts_count = fidl_component.parts_count;
    fbl::Array<ComponentPartDescriptor> parts(new ComponentPartDescriptor[parts_count],
                                              parts_count);
    for (size_t j = 0; j < parts_count; ++j) {
      const auto& fidl_part = fidl_component.parts[j];
      size_t program_count = fidl_part.match_program_count;
      fbl::Array<zx_bind_inst_t> match_program(new zx_bind_inst_t[program_count], program_count);
      static_assert(sizeof(zx_bind_inst_t) == sizeof(fidl_part.match_program[0]));
      memcpy(match_program.get(), fidl_part.match_program, sizeof(zx_bind_inst_t) * program_count);
      parts[j] = {std::move(match_program)};
    }

    auto component = std::make_unique<CompositeDeviceComponent>(dev.get(), i, std::move(parts));
    dev->unbound_.push_back(std::move(component));
  }
  *out = std::move(dev);
  return ZX_OK;
}

bool CompositeDevice::TryMatchComponents(const fbl::RefPtr<Device>& dev, size_t* index_out) {
  for (auto itr = bound_.begin(); itr != bound_.end(); ++itr) {
    if (itr->TryMatch(dev)) {
      log(ERROR, "devcoordinator: ambiguous composite bind! composite='%s', dev1='%s', dev2='%s'\n",
          name_.data(), itr->bound_device()->name().data(), dev->name().data());
      return false;
    }
  }
  for (auto itr = unbound_.begin(); itr != unbound_.end(); ++itr) {
    if (itr->TryMatch(dev)) {
      log(SPEW, "devcoordinator: found match for composite='%s', dev='%s'\n", name_.data(),
          dev->name().data());
      *index_out = itr->index();
      return true;
    }
  }
  log(SPEW, "devcoordinator: no match for composite='%s', dev='%s'\n", name_.data(),
      dev->name().data());
  return false;
}

zx_status_t CompositeDevice::BindComponent(size_t index, const fbl::RefPtr<Device>& dev) {
  // Find the component we're binding
  CompositeDeviceComponent* component = nullptr;
  for (auto& unbound_component : unbound_) {
    if (unbound_component.index() == index) {
      component = &unbound_component;
      break;
    }
  }
  ZX_ASSERT_MSG(component != nullptr, "Attempted to bind component that wasn't unbound!\n");

  zx_status_t status = component->Bind(dev);
  if (status != ZX_OK) {
    return status;
  }
  bound_.push_back(unbound_.erase(*component));
  return ZX_OK;
}

zx_status_t CompositeDevice::TryAssemble() {
  ZX_ASSERT(device_ == nullptr);
  if (!unbound_.is_empty()) {
    return ZX_ERR_SHOULD_WAIT;
  }

  Devhost* devhost = nullptr;
  for (auto& component : bound_) {
    // Find the devhost to put everything in (if we don't find one, nullptr
    // means "a new devhost").
    if (component.index() == coresident_device_index_) {
      devhost = component.bound_device()->host();
    }
    // Make sure the component driver has created its device
    if (component.component_device() == nullptr) {
      return ZX_ERR_SHOULD_WAIT;
    }
  }

  Coordinator* coordinator = nullptr;
  uint64_t component_local_ids[fuchsia_device_manager_COMPONENTS_MAX] = {};

  // Create all of the proxies for the component devices, in the same process
  for (auto& component : bound_) {
    const auto& component_dev = component.component_device();
    auto bound_dev = component.bound_device();
    coordinator = component_dev->coordinator;

    // If the device we're bound to is proxied, we care about its proxy
    // rather than it, since that's the side that we communicate with.
    if (bound_dev->proxy()) {
      bound_dev = bound_dev->proxy();
    }

    // Check if we need to use the proxy.  If not, share a reference straight
    // to the target device rather than the instance of the component device
    // that bound to it.
    if (bound_dev->host() == devhost) {
      component_local_ids[component.index()] = bound_dev->local_id();
      continue;
    }

    // We need to create it.  Double check that we haven't ended up in a state
    // where the proxies would need to be in different processes.
    if (devhost != nullptr && component_dev->proxy() != nullptr &&
        component_dev->proxy()->host() != nullptr && component_dev->proxy()->host() != devhost) {
      log(ERROR, "devcoordinator: cannot create composite, proxies in different processes\n");
      return ZX_ERR_BAD_STATE;
    }

    zx_status_t status = coordinator->PrepareProxy(component_dev, devhost);
    if (status != ZX_OK) {
      return status;
    }
    // If we hadn't picked a devhost, use the one that was created just now.
    if (devhost == nullptr) {
      devhost = component_dev->proxy()->host();
      ZX_ASSERT(devhost != nullptr);
    }
    // Stash the local ID after the proxy has been created
    component_local_ids[component.index()] = component_dev->proxy()->local_id();
  }

  zx::channel rpc_local, rpc_remote;
  zx_status_t status = zx::channel::create(0, &rpc_local, &rpc_remote);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<Device> new_device;
  status = Device::CreateComposite(coordinator, devhost, *this, std::move(rpc_local), &new_device);
  if (status != ZX_OK) {
    return status;
  }
  coordinator->devices().push_back(new_device);

  // Create the composite device in the devhost
  status = dh_send_create_composite_device(devhost, new_device.get(), *this, component_local_ids,
                                           std::move(rpc_remote));
  if (status != ZX_OK) {
    log(ERROR, "devcoordinator: create composite device request failed: %s\n",
        zx_status_get_string(status));
    return status;
  }

  device_ = std::move(new_device);
  device_->set_composite(this);

  status = device_->SignalReadyForBind();
  if (status != ZX_OK) {
    return status;
  }

  return ZX_OK;
}

void CompositeDevice::UnbindComponent(CompositeDeviceComponent* component) {
  // If the composite was fully instantiated, diassociate from it.  It will be
  // reinstantiated when this component is re-bound.
  if (device_ != nullptr) {
    Remove();
  }
  ZX_ASSERT(device_ == nullptr);
  ZX_ASSERT(component->composite() == this);
  unbound_.push_back(bound_.erase(*component));
}

void CompositeDevice::Remove() {
  device_->disassociate_from_composite();
  device_ = nullptr;
}

// CompositeDeviceComponent methods

CompositeDeviceComponent::CompositeDeviceComponent(CompositeDevice* composite, uint32_t index,
                                                   fbl::Array<const ComponentPartDescriptor> parts)
    : composite_(composite), index_(index), parts_(std::move(parts)) {}

CompositeDeviceComponent::~CompositeDeviceComponent() = default;

bool CompositeDeviceComponent::TryMatch(const fbl::RefPtr<Device>& dev) {
  if (parts_.size() > UINT32_MAX) {
    return false;
  }
  auto match =
      ::devmgr::internal::MatchParts(dev, parts_.get(), static_cast<uint32_t>(parts_.size()));
  if (match != ::devmgr::internal::Match::One) {
    return false;
  }
  return true;
}

zx_status_t CompositeDeviceComponent::Bind(const fbl::RefPtr<Device>& dev) {
  ZX_ASSERT(bound_device_ == nullptr);

  zx_status_t status = dev->coordinator->BindDriverToDevice(
      dev, dev->coordinator->component_driver(), true /* autobind */);
  if (status != ZX_OK) {
    return status;
  }

  bound_device_ = dev;
  dev->push_component(this);

  return ZX_OK;
}

void CompositeDeviceComponent::Unbind() {
  ZX_ASSERT(bound_device_ != nullptr);
  composite_->UnbindComponent(this);
  // Drop our reference to the device added by the component driver
  component_device_ = nullptr;
  bound_device_->disassociate_from_composite();
  bound_device_ = nullptr;
}

}  // namespace devmgr
