// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_DISPLAY_DISPLAY_IMAGE_H_
#define ZIRCON_SYSTEM_DEV_DISPLAY_DISPLAY_IMAGE_H_

#include <ddk/protocol/display/controller.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <lib/zx/vmo.h>
#include <zircon/listnode.h>

#include <atomic>

#include "fence.h"
#include "fuchsia/hardware/display/c/fidl.h"
#include "id-map.h"

namespace display {

class Controller;

typedef struct image_node {
  list_node_t link;
  fbl::RefPtr<class Image> self;
} image_node_t;

class Image : public fbl::RefCounted<Image>, public IdMappable<fbl::RefPtr<Image>> {
 public:
  Image(Controller* controller, const image_t& info, zx::vmo vmo, uint32_t stride_px);
  ~Image();

  image_t& info() { return info_; }
  uint32_t stride_px() const { return stride_px_; }

  // Marks the image as in use.
  bool Acquire();
  // Marks the image as not in use. Should only be called before PrepareFences.
  void DiscardAcquire();
  // Called to set this image's fences and prepare the image to be displayed.
  void PrepareFences(fbl::RefPtr<FenceReference>&& wait, fbl::RefPtr<FenceReference>&& signal);
  // Called to immediately retire the image if StartPresent hasn't been called yet.
  void EarlyRetire();
  // Called when the image is passed to the display hardware.
  void StartPresent();
  // Called when another image is presented after this one.
  void StartRetire();
  // Called on vsync after StartRetire has been called.
  void OnRetire();

  // Called on all waiting images when any fence fires.
  void OnFenceReady(FenceReference* fence);

  // Called to reset fences when client releases the image. Releasing fences
  // is independent of the rest of the image lifecycle.
  void ResetFences();

  bool IsReady() const { return wait_fence_ == nullptr; }

  bool HasSameConfig(const image_t& config) const {
    for (uint32_t i = 0; i < countof(info_.planes); i++) {
      if (info_.planes[i].bytes_per_row != config.planes[i].bytes_per_row ||
          info_.planes[i].byte_offset != config.planes[i].byte_offset) {
        return false;
      }
    }

    return info_.width == config.width && info_.height == config.height &&
           info_.pixel_format == config.pixel_format && info_.type == config.type;
  }
  bool HasSameConfig(const Image& other) const { return HasSameConfig(other.info_); }

  const zx::vmo& vmo() { return vmo_; }

  void set_z_index(uint32_t z_index) { z_index_ = z_index; }
  uint32_t z_index() const { return z_index_; }

  // The node alternates between a client's waiting image list and the controller's
  // presented image list. The presented image list is protected with the controller mutex,
  // and the waiting list is only accessed on the loop and thus is not generally
  // protected. However, transfers between the lists are protected by the controller mutex.
  image_node_t node = {
      .link = LIST_INITIAL_CLEARED_VALUE,
      .self = nullptr,
  };

 private:
  image_t info_;
  uint32_t stride_px_;
  Controller* const controller_;

  // z_index is set/read by controller.cpp under its lock
  uint32_t z_index_;

  // Only ever accessed on loop thread, so no synchronization
  fbl::RefPtr<FenceReference> wait_fence_ = nullptr;
  // signal_fence_ is only accessed on the loop. armed_signal_fence_ is accessed
  // under the controller mutex. See comment in ::OnRetire for more details.
  fbl::RefPtr<FenceReference> signal_fence_ = nullptr;
  fbl::RefPtr<FenceReference> armed_signal_fence_ = nullptr;

  // Flag which indicates that the image is currently in some display configuration.
  std::atomic_bool in_use_ = {};
  // Flag indicating that the image is being managed by the display hardware. Only
  // accessed under the controller mutex.
  bool presenting_ = false;
  // Flag indicating that the image has started the process of retiring and will be free after
  // the next vsync. This is distinct from presenting_ due to multiplexing the display between
  // multiple clients. Only accessed under the controller mutex.
  bool retiring_ = false;

  const zx::vmo vmo_;
};

}  // namespace display

#endif  // ZIRCON_SYSTEM_DEV_DISPLAY_DISPLAY_IMAGE_H_
