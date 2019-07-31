// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_DISPLAY_DUMMY_DUMMY_DISPLAY_H_
#define ZIRCON_SYSTEM_DEV_DISPLAY_DUMMY_DUMMY_DISPLAY_H_

#include <ddk/debug.h>
#include <ddk/driver.h>
#include <ddk/protocol/sysmem.h>
#include <ddktl/device.h>
#include <ddktl/protocol/display/controller.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <fbl/unique_ptr.h>
#include <unistd.h>
#include <zircon/compiler.h>
#include <zircon/pixelformat.h>
#include <zircon/thread_annotations.h>

#include <atomic>

namespace dummy_display {

class DummyDisplay;

using DeviceType = ddk::Device<DummyDisplay, ddk::Unbindable>;

class DummyDisplay : public DeviceType,
                     public ddk::DisplayControllerImplProtocol<DummyDisplay, ddk::base_protocol> {
 public:
  DummyDisplay(zx_device_t* parent) : DeviceType(parent) {}

  // This function is called from the c-bind function upon driver matching
  zx_status_t Bind();

  // Required functions needed to implement Display Controller Protocol
  void DisplayControllerImplSetDisplayControllerInterface(
      const display_controller_interface_protocol_t* intf);
  zx_status_t DisplayControllerImplImportVmoImage(image_t* image, zx::vmo vmo, size_t offset);
  zx_status_t DisplayControllerImplImportImage(image_t* image, zx_unowned_handle_t handle,
                                               uint32_t index);
  void DisplayControllerImplReleaseImage(image_t* image);
  uint32_t DisplayControllerImplCheckConfiguration(const display_config_t** display_configs,
                                                   size_t display_count,
                                                   uint32_t** layer_cfg_results,
                                                   size_t* layer_cfg_result_count);
  void DisplayControllerImplApplyConfiguration(const display_config_t** display_config,
                                               size_t display_count);
  uint32_t DisplayControllerImplComputeLinearStride(uint32_t width, zx_pixel_format_t format);
  zx_status_t DisplayControllerImplAllocateVmo(uint64_t size, zx::vmo* vmo_out);
  zx_status_t DisplayControllerImplGetSysmemConnection(zx::channel connection);
  zx_status_t DisplayControllerImplSetBufferCollectionConstraints(const image_t* config,
                                                                  uint32_t collection);
  zx_status_t DisplayControllerImplGetSingleBufferFramebuffer(zx::vmo* out_vmo,
                                                              uint32_t* out_stride) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  // Required functions for DeviceType
  void DdkUnbind();
  void DdkRelease();

 private:
  zx_status_t SetupDisplayInterface();
  int VSyncThread();
  void PopulateAddedDisplayArgs(added_display_args_t* args);

  sysmem_protocol_t sysmem_ = {};

  std::atomic_bool vsync_shutdown_flag_ = false;

  // Thread handles
  thrd_t vsync_thread_;

  // Locks used by the display driver
  fbl::Mutex display_lock_;  // general display state (i.e. display_id)

  uint64_t current_image_ TA_GUARDED(display_lock_);
  bool current_image_valid_ TA_GUARDED(display_lock_);

  // Display controller related data
  ddk::DisplayControllerInterfaceProtocolClient dc_intf_ TA_GUARDED(display_lock_);
};

}  // namespace dummy_display

#endif  // ZIRCON_SYSTEM_DEV_DISPLAY_DUMMY_DUMMY_DISPLAY_H_
