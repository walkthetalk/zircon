// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_BINDING_INTERNAL_H_
#define ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_BINDING_INTERNAL_H_

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <fbl/array.h>
#include <fbl/macros.h>
#include <stdio.h>

#include "composite-device.h"
#include "coordinator.h"
#include "device.h"

namespace devmgr {

namespace internal {

struct BindProgramContext {
  const fbl::Array<const zx_device_prop_t>* props;
  uint32_t protocol_id;
  size_t binding_size;
  const zx_bind_inst_t* binding;
  const char* name;
  uint32_t autobind;
};

uint32_t LookupBindProperty(BindProgramContext* ctx, uint32_t id);
bool EvaluateBindProgram(BindProgramContext* ctx);

template <typename T>
bool EvaluateBindProgram(const fbl::RefPtr<T>& device, const char* drv_name,
                         const fbl::Array<const zx_bind_inst_t>& bind_program, bool autobind) {
  BindProgramContext ctx;
  ctx.props = &device->props();
  ctx.protocol_id = device->protocol_id();
  ctx.binding = bind_program.get();
  ctx.binding_size = bind_program.size() * sizeof(bind_program[0]);
  ctx.name = drv_name;
  ctx.autobind = autobind ? 1 : 0;
  return EvaluateBindProgram(&ctx);
}

// Represents the number of match chains found by a run of MatchParts()
enum class Match : uint8_t {
  None = 0,
  One,
  Many,
};

// Performs saturating arithmetic on Match values
Match SumMatchCounts(Match m1, Match m2);

// Internal bookkeeping for finding composite device component matches
class ComponentMatchState {
 public:
  ComponentMatchState() = default;
  ComponentMatchState(const ComponentMatchState&) = delete;
  ComponentMatchState& operator=(const ComponentMatchState&) = delete;

  ComponentMatchState(ComponentMatchState&&) = default;
  ComponentMatchState& operator=(ComponentMatchState&&) = default;

  // Create the bookkeeping state for the component matching algorithm.  This
  // preinitializes the state with Match::None,
  static zx_status_t Create(size_t components_count, size_t devices_count,
                            ComponentMatchState* out) {
    ComponentMatchState state;
    state.components_count_ = components_count;
    state.devices_count_ = devices_count;
    // If we wanted to reduce the memory usage here, we could avoid
    // bookkeeping for the perimeter of the array, in which all entries
    // except for the starting point are 0s.
    state.matches_ = std::make_unique<Match[]>(devices_count * components_count);
    *out = std::move(state);
    return ZX_OK;
  }

  Match get(size_t component, size_t ancestor) const {
    return matches_[devices_count_ * component + ancestor];
  }

  void set(size_t component, size_t ancestor, Match value) {
    matches_[devices_count_ * component + ancestor] = value;
  }

 private:
  std::unique_ptr<Match[]> matches_;
  size_t components_count_ = 0;
  size_t devices_count_ = 0;
};

// Return a list containing the device and all of its ancestors.  The 0th entry is the |device|
// itself, the 1st is its parent, etc.  Composite devices have no ancestors for the
// purpose of this function.
template <typename T>
void MakeDeviceList(const fbl::RefPtr<T>& device, fbl::Array<fbl::RefPtr<T>>* out) {
  size_t device_count = 0;
  fbl::RefPtr<T> dev = device;
  while (dev) {
    ++device_count;
    dev = dev->parent();
  }

  fbl::Array<fbl::RefPtr<T>> devices(new fbl::RefPtr<T>[device_count], device_count);
  size_t i = 0;
  for (dev = device; dev != nullptr; ++i, dev = dev->parent()) {
    devices[i] = dev;
  }
  ZX_DEBUG_ASSERT(i == device_count);
  *out = std::move(devices);
}

DECLARE_HAS_MEMBER_FN_WITH_SIGNATURE(has_props, props,
                                     const fbl::Array<const zx_device_prop_t>& (C::*)() const);
DECLARE_HAS_MEMBER_FN_WITH_SIGNATURE(has_topo_prop, topo_prop,
                                     const zx_device_prop_t* (C::*)() const);
DECLARE_HAS_MEMBER_FN_WITH_SIGNATURE(has_parent, parent, const fbl::RefPtr<T>& (C::*)());
DECLARE_HAS_MEMBER_FN_WITH_SIGNATURE(has_protocol_id, protocol_id, uint32_t (C::*)() const);

// Evaluates whether |device| and its ancestors match the sequence of binding
// programs described in |parts|.
//
// We consider a match to be found if the following hold:
// 1) For every part p_i, there is a device d that matches the bind program in
//    that part (we'll refer to this as a part/device pair (p_i, d)).
// 2) In (p_0, d), d must be the root device.
// 3) In (p_(N-1), d), d must be the leaf device.
// 4) If we have pairs (p_i, d) and (p_j, e), and i < j, then d is an ancestor
//    of e.  That is, the devices must match in the same sequence as the parts.
// 5) For every ancestor of the leaf device that has a BIND_TOPO_* property,
//    there exists a part that matches matches it.
// 6) There is a unique pairing that satisfies properties 1-5.
//
// The high-level idea of the rules above is that we want an unambiguous
// matching of the parts to the devices that is allowed to skip over ancestors
// that do not have topological properties.  We do not allow skipping over
// devices with topological properties, since the intent of this mechanism is to
// allow the description of devices that correspond to particular pieces of
// hardware.
//
// If all of these properties hold, MatchParts() returns Match::One.  If all of
// the properties except for property 6 hold, it returns Match::Many.
// Otherwise, it returns Match::None.
template <typename T>
Match MatchParts(const fbl::RefPtr<T>& device, const ComponentPartDescriptor* parts,
                 uint32_t parts_count) {
  static_assert(has_props<T>::value && has_topo_prop<T>::value && has_parent<T>::value &&
                has_protocol_id<T>::value);

  if (parts_count == 0) {
    return Match::None;
  }

  auto& first_part = parts[0];
  auto& last_part = parts[parts_count - 1];
  // The last part must match this device exactly
  bool match =
      EvaluateBindProgram(device, "composite_binder", last_part.match_program, true /* autobind */);
  if (!match) {
    return Match::None;
  }

  fbl::Array<fbl::RefPtr<T>> device_list;
  MakeDeviceList(device, &device_list);

  // If we have fewer device nodes than parts, we can't possibly match
  if (device_list.size() < parts_count) {
    return Match::None;
  }

  // Special-case for a single part: can only happen if one device
  if (parts_count == 1) {
    if (device_list.size() != 1) {
      return Match::None;
    }
    return Match::One;
  }

  // The first part must match the final ancestor
  match = EvaluateBindProgram(device_list[device_list.size() - 1], "composite_binder",
                              first_part.match_program, true /* autobind */);
  if (!match) {
    return Match::None;
  }

  // We now need to find if there exists a unique chain from parts[1] to
  // parts[parts_count - 2] such that each bind program has a match, and every
  // ancestor that has a BIND_TOPO property has a match.

  // If we have only two parts, we need to see if there are any unmatched
  // topological nodes.
  if (parts_count == 2) {
    // We've matched on the first and last device already, so check everything in-between
    for (size_t i = 1; i < device_list.size() - 1; ++i) {
      if (device_list[i]->topo_prop() != nullptr) {
        return Match::None;
      }
    }
    return Match::One;
  }

  ZX_DEBUG_ASSERT(device_list.size() >= 2 && parts_count >= 2);

  ComponentMatchState state;
  // For the matching state, we're focused on all of the devices between the
  // leaf device and the root device.
  zx_status_t status = ComponentMatchState::Create(parts_count, device_list.size(), &state);
  if (status != ZX_OK) {
    return Match::None;
  }
  // Record that we have a single match for the leaf.
  state.set(parts_count - 1, 0, Match::One);

  // We need to find a match for each intermediate part.  We'll move from the
  // closest to the leaf to the furthest.
  for (size_t part_idx = parts_count - 2; part_idx >= 1; --part_idx) {
    const ComponentPartDescriptor& part = parts[part_idx];

    // The number of matches we have so far is the sum of the number of
    // matches from the last iteration (i.e. of the chain of components from
    // part_idx+1 to to the end of the parts list) that did not make use of
    // this device or any of its ancestors.
    Match match_count = Match::None;

    // We iterate from the leaf device to the final ancestor.
    for (size_t device_idx = 1; device_idx < device_list.size() - 1; ++device_idx) {
      match_count = SumMatchCounts(match_count, state.get(part_idx + 1, device_idx - 1));

      // If there were no matches yet, this chain can't exist.
      if (match_count == Match::None) {
        continue;
      }

      match = EvaluateBindProgram(device_list[device_idx], "composite_binder", part.match_program,
                                  true /* autobind */);
      if (match) {
        // Propagate the current match_count.  Any chain that got here
        // is being extended by this latest match, so the number of
        // matching chains is unchanged.
        state.set(part_idx, device_idx, match_count);
      }

      // Move on to the next component, since we cannot cross a
      // topological property without matching against it.
      if (device_list[device_idx]->topo_prop() != nullptr) {
        break;
      }
    }
  }

  // Any chains we have found will be in the state with part_idx=1.  We need
  // to find how many of those chains have no devices with topological
  // properties between the last matching device in the chain and the root
  // device.
  Match match_count = Match::None;
  for (size_t i = device_list.size() - 2; i >= parts_count - 2; --i) {
    match_count = SumMatchCounts(match_count, state.get(1, i));
    if (device_list[i]->topo_prop() != nullptr) {
      break;
    }
  }
  return match_count;
}

}  // namespace internal

}  // namespace devmgr

#endif  // ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_BINDING_INTERNAL_H_
