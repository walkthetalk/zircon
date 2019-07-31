// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "mt8167s-display.h"

#include <ddk/binding.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/composite.h>
#include <fbl/auto_call.h>
#include <fbl/vector.h>
#include <fuchsia/sysmem/c/fidl.h>
#include <lib/image-format/image_format.h>
#include <lib/zx/pmt.h>
#include <zircon/pixelformat.h>

#include "common.h"
#include "registers-ovl.h"

namespace mt8167s_display {

namespace {
// List of supported pixel formats
zx_pixel_format_t kSupportedPixelFormats[3] = {ZX_PIXEL_FORMAT_ARGB_8888, ZX_PIXEL_FORMAT_RGB_x888,
                                               ZX_PIXEL_FORMAT_RGB_565};

enum {
  COMPONENT_PDEV,
  COMPONENT_GPIO,
  COMPONENT_SYSMEM,
  COMPONENT_DSI_IMPL,  // DSI is optional
  COMPONENT_POWER,
  COMPONENT_COUNT,
};

constexpr uint64_t kDisplayId = PANEL_DISPLAY_ID;
constexpr uint32_t kLarbMmuEnOffset = 0x0FC0;

constexpr display_setting_t kDisplaySettingIli9881c = {
    .lane_num = 4,
    .bit_rate_max = 0,  // unused
    .clock_factor = 0,  // unused
    .lcd_clock = 270,
    .h_active = 720,
    .v_active = 1280,
    .h_period = 900,   // Vendor provides front porch of 80. calculate period manually
    .v_period = 1340,  // Vendor provides front porch of 40. calculate period manually
    .hsync_width = 20,
    .hsync_bp = 80,
    .hsync_pol = 0,  // unused
    .vsync_width = 4,
    .vsync_bp = 16,
    .vsync_pol = 0,  // unused
};

constexpr display_setting_t kDisplaySettingSt7701s = {
    .lane_num = 2,
    .bit_rate_max = 0,  // unused
    .clock_factor = 0,  // unused
    .lcd_clock = 228,
    .h_active = 480,
    .v_active = 800,
    .h_period = 740,  // Vendor provides front porch of 100. calculate period manually
    .v_period = 848,  // Vendor provides front porch of 20. calculate period manually
    .hsync_width = 60,
    .hsync_bp = 100,
    .hsync_pol = 0,  // unused
    .vsync_width = 8,
    .vsync_bp = 20,
    .vsync_pol = 0,  // unused
};

}  // namespace

void Mt8167sDisplay::CopyDisplaySettings() {
  ZX_DEBUG_ASSERT(init_disp_table_);
  disp_setting_ = *init_disp_table_;
}

void Mt8167sDisplay::PopulateAddedDisplayArgs(added_display_args_t* args) {
  args->display_id = kDisplayId;
  args->edid_present = false;
  args->panel.params.height = height_;
  args->panel.params.width = width_;
  args->panel.params.refresh_rate_e2 = 3000;  // Just guess that it's 30fps
  args->pixel_format_list = kSupportedPixelFormats;
  args->pixel_format_count = countof(kSupportedPixelFormats);
  args->cursor_info_count = 0;
}

// part of ZX_PROTOCOL_DISPLAY_CONTROLLER_IMPL ops
uint32_t Mt8167sDisplay::DisplayControllerImplComputeLinearStride(uint32_t width,
                                                                  zx_pixel_format_t format) {
  return ROUNDUP(width, 32 / ZX_PIXEL_FORMAT_BYTES(format));
}

// part of ZX_PROTOCOL_DISPLAY_CONTROLLER_IMPL ops
void Mt8167sDisplay::DisplayControllerImplSetDisplayControllerInterface(
    const display_controller_interface_protocol_t* intf) {
  fbl::AutoLock lock(&display_lock_);
  dc_intf_ = ddk::DisplayControllerInterfaceProtocolClient(intf);
  added_display_args_t args;
  PopulateAddedDisplayArgs(&args);
  dc_intf_.OnDisplaysChanged(&args, 1, NULL, 0, NULL, 0, NULL);
}

// part of ZX_PROTOCOL_DISPLAY_CONTROLLER_IMPL ops
zx_status_t Mt8167sDisplay::DisplayControllerImplImportVmoImage(image_t* image, zx::vmo vmo,
                                                                size_t offset) {
  auto import_info = std::make_unique<ImageInfo>();
  if (import_info == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }

  fbl::AutoLock lock(&image_lock_);
  if (image->type != IMAGE_TYPE_SIMPLE || image->pixel_format != kSupportedPixelFormats[0]) {
    return ZX_ERR_INVALID_ARGS;
  }

  uint32_t stride = DisplayControllerImplComputeLinearStride(image->width, image->pixel_format);
  unsigned pixel_size = ZX_PIXEL_FORMAT_BYTES(image->pixel_format);
  size_t size =
      ROUNDUP((stride * image->height * pixel_size) + (offset & (PAGE_SIZE - 1)), PAGE_SIZE);
  zx_paddr_t paddr;
  zx_status_t status = bti_.pin(ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS, vmo,
                                offset & ~(PAGE_SIZE - 1), size, &paddr, 1, &import_info->pmt);
  if (status != ZX_OK) {
    DISP_ERROR("Could not pin bit\n");
    return status;
  }
  // Make sure paddr is allocated in the lower 4GB. (ZX-1073)
  ZX_ASSERT((paddr + size) <= UINT32_MAX);
  import_info->paddr = paddr;
  import_info->pitch = stride * pixel_size;
  image->handle = reinterpret_cast<uint64_t>(import_info.get());
  imported_images_.push_back(std::move(import_info));
  return status;
}

// part of ZX_PROTOCOL_DISPLAY_CONTROLLER_IMPL ops
zx_status_t Mt8167sDisplay::DisplayControllerImplImportImage(image_t* image,
                                                             zx_unowned_handle_t handle,
                                                             uint32_t index) {
  auto import_info = std::make_unique<ImageInfo>();
  if (import_info == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }

  fbl::AutoLock lock(&image_lock_);
  if (image->type != IMAGE_TYPE_SIMPLE || image->pixel_format != kSupportedPixelFormats[0]) {
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t status2;
  fuchsia_sysmem_BufferCollectionInfo_2 collection_info;
  zx_status_t status =
      fuchsia_sysmem_BufferCollectionWaitForBuffersAllocated(handle, &status2, &collection_info);
  if (status != ZX_OK) {
    return status;
  }
  if (status2 != ZX_OK) {
    return status2;
  }

  fbl::Vector<zx::vmo> vmos;
  for (uint32_t i = 0; i < collection_info.buffer_count; ++i) {
    vmos.push_back(zx::vmo(collection_info.buffers[i].vmo));
  }

  if (!collection_info.settings.has_image_format_constraints || index >= vmos.size()) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  ZX_DEBUG_ASSERT(collection_info.settings.image_format_constraints.pixel_format.type ==
                  fuchsia_sysmem_PixelFormatType_BGRA32);
  ZX_DEBUG_ASSERT(
      !collection_info.settings.image_format_constraints.pixel_format.has_format_modifier);

  uint32_t minimum_row_bytes;
  if (!ImageFormatMinimumRowBytes(&collection_info.settings.image_format_constraints, image->width,
                                  &minimum_row_bytes)) {
    DISP_ERROR("Invalid image width %d for collection\n", image->width);
    return ZX_ERR_INVALID_ARGS;
  }
  uint64_t offset = collection_info.buffers[index].vmo_usable_start;

  size_t size =
      ROUNDUP((minimum_row_bytes * image->height) + (offset & (PAGE_SIZE - 1)), PAGE_SIZE);
  zx_paddr_t paddr;
  status = bti_.pin(ZX_BTI_PERM_READ | ZX_BTI_CONTIGUOUS, vmos[index], offset & ~(PAGE_SIZE - 1),
                    size, &paddr, 1, &import_info->pmt);
  if (status != ZX_OK) {
    DISP_ERROR("Could not pin bit\n");
    return status;
  }
  // Make sure paddr is allocated in the lower 4GB. (ZX-1073)
  ZX_ASSERT((paddr + size) <= UINT32_MAX);
  import_info->paddr = paddr;
  import_info->pitch = minimum_row_bytes;
  image->handle = reinterpret_cast<uint64_t>(import_info.get());
  imported_images_.push_back(std::move(import_info));
  return status;
}

// part of ZX_PROTOCOL_DISPLAY_CONTROLLER_IMPL ops
void Mt8167sDisplay::DisplayControllerImplReleaseImage(image_t* image) {
  fbl::AutoLock lock(&image_lock_);
  auto info = reinterpret_cast<ImageInfo*>(image->handle);
  imported_images_.erase(*info);
}

// part of ZX_PROTOCOL_DISPLAY_CONTROLLER_IMPL ops
uint32_t Mt8167sDisplay::DisplayControllerImplCheckConfiguration(
    const display_config_t** display_configs, size_t display_count, uint32_t** layer_cfg_results,
    size_t* layer_cfg_result_count) {
  if (display_count != 1) {
    ZX_DEBUG_ASSERT(display_count == 0);
    return CONFIG_DISPLAY_OK;
  }
  ZX_DEBUG_ASSERT(display_configs[0]->display_id == PANEL_DISPLAY_ID);

  fbl::AutoLock lock(&display_lock_);

  bool success = true;
  if (display_configs[0]->layer_count > kMaxLayer) {
    success = false;
  } else {
    for (size_t j = 0; j < display_configs[0]->layer_count; j++) {
      switch (display_configs[0]->layer_list[j]->type) {
        case LAYER_TYPE_PRIMARY: {
          const primary_layer_t& layer = display_configs[0]->layer_list[j]->cfg.primary;
          // TODO(payamm) Add support for 90 and 270 degree rotation (ZX-3252)
          if (layer.transform_mode != FRAME_TRANSFORM_IDENTITY &&
              layer.transform_mode != FRAME_TRANSFORM_REFLECT_X &&
              layer.transform_mode != FRAME_TRANSFORM_REFLECT_Y &&
              layer.transform_mode != FRAME_TRANSFORM_ROT_180) {
            layer_cfg_results[0][j] |= CLIENT_TRANSFORM;
          }
          // TODO(payamm) Add support for scaling (ZX-3228) :
          if (layer.src_frame.width != layer.dest_frame.width ||
              layer.src_frame.height != layer.dest_frame.height) {
            layer_cfg_results[0][j] |= CLIENT_FRAME_SCALE;
          }
          // Ony support ALPHA_HW_MULTIPLY
          if (layer.alpha_mode == ALPHA_PREMULTIPLIED) {
            layer_cfg_results[0][j] |= CLIENT_ALPHA;
            ;
          }
          break;
        }
        case LAYER_TYPE_COLOR: {
          if (j != 0) {
            layer_cfg_results[0][j] |= CLIENT_USE_PRIMARY;
          }
          break;
        }
        default:
          layer_cfg_results[0][j] |= CLIENT_USE_PRIMARY;
          break;
      }
    }
  }

  if (!success) {
    layer_cfg_results[0][0] = CLIENT_MERGE_BASE;
    for (unsigned i = 1; i < display_configs[0]->layer_count; i++) {
      layer_cfg_results[0][i] = CLIENT_MERGE_SRC;
    }
  }
  return CONFIG_DISPLAY_OK;
}

// part of ZX_PROTOCOL_DISPLAY_CONTROLLER_IMPL ops
void Mt8167sDisplay::DisplayControllerImplApplyConfiguration(
    const display_config_t** display_configs, size_t display_count) {
  ZX_DEBUG_ASSERT(display_configs);
  fbl::AutoLock lock(&display_lock_);
  auto* config = display_configs[0];
  if (display_count == 1 && config->layer_count) {
    if (!full_init_done_) {
      zx_status_t status;
      if ((status = DisplaySubsystemInit()) != ZX_OK) {
        DISP_ERROR("Display Hardware Initialization failed! %d\n", status);
        ZX_ASSERT(0);
      }
      full_init_done_ = true;
    }

    // First stop the overlay engine, followed by the DISP RDMA Engine
    syscfg_->MutexReset();
    ovl_->Reset();
    disp_rdma_->Stop();
    for (size_t j = 0; j < config->layer_count; j++) {
      const primary_layer_t& layer = config->layer_list[j]->cfg.primary;
      auto info = reinterpret_cast<ImageInfo*>(layer.image.handle);
      // Build the overlay configuration. For now we only provide format and address.
      OvlConfig cfg;
      cfg.handle = layer.image.handle;
      cfg.paddr = info->paddr;
      cfg.format = layer.image.pixel_format;
      cfg.alpha_mode = layer.alpha_mode;
      cfg.alpha_val = layer.alpha_layer_val;
      cfg.src_frame = layer.src_frame;
      cfg.dest_frame = layer.dest_frame;
      cfg.pitch = info->pitch;
      cfg.transform = layer.transform_mode;
      ovl_->Config(static_cast<uint8_t>(j), cfg);
    }
    // All configurations are done. Re-start the engine
    disp_rdma_->Start();
    ovl_->Start();
    syscfg_->MutexEnable();
  } else {
    if (full_init_done_) {
      syscfg_->MutexReset();
      ovl_->Restart();
      disp_rdma_->Restart();
      syscfg_->MutexEnable();
    }
  }

  // If bootloader does not enable any of the display hardware, no vsync will be generated.
  // This fakes a vsync to let clients know we are ready until we actually initialize hardware
  if (!full_init_done_) {
    if (dc_intf_.is_valid()) {
      if (display_count == 0 || display_configs[0]->layer_count == 0) {
        dc_intf_.OnDisplayVsync(kDisplayId, zx_clock_get_monotonic(), nullptr, 0);
      }
    }
  }
}

// part of ZX_PROTOCOL_DISPLAY_CONTROLLER_IMPL ops
zx_status_t Mt8167sDisplay::DisplayControllerImplAllocateVmo(uint64_t size, zx::vmo* vmo_out) {
  return zx::vmo::create_contiguous(bti_, size, 0, vmo_out);
}

zx_status_t Mt8167sDisplay::DisplayControllerImplGetSysmemConnection(zx::channel connection) {
  zx_status_t status = sysmem_connect(&sysmem_, connection.release());
  if (status != ZX_OK) {
    DISP_ERROR("Could not connect to sysmem\n");
    return status;
  }

  return ZX_OK;
}

zx_status_t Mt8167sDisplay::DisplayControllerImplSetBufferCollectionConstraints(
    const image_t* config, zx_unowned_handle_t collection) {
  fuchsia_sysmem_BufferCollectionConstraints constraints = {};
  constraints.usage.display = fuchsia_sysmem_displayUsageLayer;
  constraints.has_buffer_memory_constraints = true;
  fuchsia_sysmem_BufferMemoryConstraints& buffer_constraints =
      constraints.buffer_memory_constraints;
  buffer_constraints.min_size_bytes = 0;
  buffer_constraints.max_size_bytes = 0xffffffff;
  buffer_constraints.physically_contiguous_required = true;
  buffer_constraints.secure_required = false;
  buffer_constraints.ram_domain_supported = true;
  buffer_constraints.cpu_domain_supported = true;
  buffer_constraints.heap_permitted_count = 1;
  buffer_constraints.heap_permitted[0] = fuchsia_sysmem_HeapType_SYSTEM_RAM;
  constraints.image_format_constraints_count = 1;
  fuchsia_sysmem_ImageFormatConstraints& image_constraints =
      constraints.image_format_constraints[0];
  image_constraints.pixel_format.type = fuchsia_sysmem_PixelFormatType_BGRA32;
  image_constraints.color_spaces_count = 1;
  image_constraints.color_space[0].type = fuchsia_sysmem_ColorSpaceType_SRGB;
  image_constraints.min_coded_width = 0;
  image_constraints.max_coded_width = 0xffffffff;
  image_constraints.min_coded_height = 0;
  image_constraints.max_coded_height = 0xffffffff;
  image_constraints.min_bytes_per_row = 0;
  image_constraints.max_bytes_per_row = 0xffffffff;
  image_constraints.max_coded_width_times_coded_height = 0xffffffff;
  image_constraints.layers = 1;
  image_constraints.coded_width_divisor = 1;
  image_constraints.coded_height_divisor = 1;
  image_constraints.bytes_per_row_divisor = 32;
  image_constraints.start_offset_divisor = 32;
  image_constraints.display_width_divisor = 1;
  image_constraints.display_height_divisor = 1;

  zx_status_t status =
      fuchsia_sysmem_BufferCollectionSetConstraints(collection, true, &constraints);

  if (status != ZX_OK) {
    DISP_ERROR("Failed to set constraints");
    return status;
  }

  return ZX_OK;
}

int Mt8167sDisplay::VSyncThread() {
  zx_status_t status;
  while (1) {
    // clear interrupt source
    ovl_->ClearIrq();
    zx::time timestamp;
    status = vsync_irq_.wait(&timestamp);
    if (status != ZX_OK) {
      DISP_ERROR("VSync Interrupt wait failed\n");
      break;
    }
    fbl::AutoLock lock(&display_lock_);
    if (!ovl_->IsValidIrq()) {
      DISP_SPEW("Spurious Interrupt\n");
      continue;
    }
    uint64_t handles[kMaxLayer];
    size_t handle_count = 0;
    // For all 4 layers supported,obtain the handle for that layer and clear it since
    // we are done applying the new configuration to that layer.
    for (uint8_t i = 0; i < kMaxLayer; i++) {
      if (ovl_->IsLayerActive(i)) {
        handles[handle_count++] = ovl_->GetLayerHandle(i);
        ovl_->ClearLayer(i);
      }
    }

    if (dc_intf_.is_valid()) {
      dc_intf_.OnDisplayVsync(kDisplayId, timestamp.get(), handles, handle_count);
    }
  }
  return ZX_OK;
}

zx_status_t Mt8167sDisplay::ShutdownDisplaySubsytem() {
  // Clear mutex
  syscfg_->MutexClear();

  // Clear Display Subsytem Path
  syscfg_->ClearDefaultPath();

  // Starting disabling from top to bottom
  // (OVL -> Color -> Ccorr -> Aal -> Gamma -> Dither -> RDMA -> DSI)
  syscfg_->PowerDown(MODULE_OVL0);
  syscfg_->PowerDown(MODULE_COLOR0);
  syscfg_->PowerDown(MODULE_CCORR);
  syscfg_->PowerDown(MODULE_AAL);
  syscfg_->PowerDown(MODULE_GAMMA);
  // TODO(payamm): Bootloader does not touch any dither-related regs. I'm feeling adventerous
  syscfg_->PowerDown(MODULE_DITHER);
  syscfg_->PowerDown(MODULE_RDMA0);

  // Finally shutdown DSI host
  dsi_host_->Shutdown(syscfg_);

  return ZX_OK;
}

zx_status_t Mt8167sDisplay::StartupDisplaySubsytem() {
  // Turn top clocks on
  syscfg_->PowerOn(MODULE_SMI);

  // Add default modules to the Mutex system
  syscfg_->MutexSetDefault();

  // Create default path within the display subsystem
  syscfg_->CreateDefaultPath();

  // Enable clock
  syscfg_->PowerOn(MODULE_OVL0);
  syscfg_->PowerOn(MODULE_COLOR0);
  syscfg_->PowerOn(MODULE_CCORR);
  syscfg_->PowerOn(MODULE_AAL);
  syscfg_->PowerOn(MODULE_GAMMA);
  syscfg_->PowerOn(MODULE_DITHER);
  syscfg_->PowerOn(MODULE_RDMA0);

  dsi_host_->PowerOn(syscfg_);

  return ZX_OK;
}

zx_status_t Mt8167sDisplay::CreateAndInitDisplaySubsystems() {
  zx_status_t status;
  fbl::AllocChecker ac;
  // Create and initialize system config object
  syscfg_ = fbl::make_unique_checked<mt8167s_display::MtSysConfig>(&ac);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  status = syscfg_->Init(pdev_device_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not initialize SYS Config object\n");
    return status;
  }

  // Create and initialize DSI Host object
  dsi_host_ = fbl::make_unique_checked<mt8167s_display::MtDsiHost>(&ac, &pdev_, height_, width_,
                                                                   panel_type_);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  status = dsi_host_->Init(&dsiimpl_, &gpio_, &power_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not initialize DSI object\n");
    return status;
  }

  // Create and initialize ovl object
  ovl_ = fbl::make_unique_checked<mt8167s_display::Ovl>(&ac, height_, width_);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  // Initialize ovl object
  status = ovl_->Init(pdev_device_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not initialize OVL object\n");
    return status;
  }

  // Create and initialize color object
  color_ = fbl::make_unique_checked<mt8167s_display::Color>(&ac, height_, width_);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  // Initialize color object
  status = color_->Init(pdev_device_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not initialize Color object\n");
    return status;
  }

  // Create and initialize ccorr object
  ccorr_ = fbl::make_unique_checked<mt8167s_display::Ccorr>(&ac, height_, width_);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  // Initialize ccorr object
  status = ccorr_->Init(pdev_device_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not initialize Ccorr object\n");
    return status;
  }

  // Create and initialize aal object
  aal_ = fbl::make_unique_checked<mt8167s_display::Aal>(&ac, height_, width_);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  // Initialize aal object
  status = aal_->Init(pdev_device_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not initialize Aal object\n");
    return status;
  }

  // Create and initialize gamma object
  gamma_ = fbl::make_unique_checked<mt8167s_display::Gamma>(&ac, height_, width_);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  // Initialize gamma object
  status = gamma_->Init(pdev_device_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not initialize Gamma object\n");
    return status;
  }

  // Create and initialize dither object
  dither_ = fbl::make_unique_checked<mt8167s_display::Dither>(&ac, height_, width_);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  // Initialize dither object
  status = dither_->Init(pdev_device_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not initialize Dither object\n");
    return status;
  }

  // Create and initialize Display RDMA object
  disp_rdma_ = fbl::make_unique_checked<mt8167s_display::DispRdma>(&ac, height_, width_);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  // Initialize disp rdma object
  status = disp_rdma_->Init(pdev_device_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not initialize DISP RDMA object\n");
    return status;
  }
  return ZX_OK;
}

zx_status_t Mt8167sDisplay::DisplaySubsystemInit() {
  zx_status_t status;

  // Select the appropriate display table.
  // TODO(payamm): This should really be done via display ID GPIO pins
  if (board_info_.vid == PDEV_VID_MEDIATEK && board_info_.pid == PDEV_PID_MEDIATEK_8167S_REF) {
    panel_type_ = PANEL_ILI9881C;
    init_disp_table_ = &kDisplaySettingIli9881c;
  } else if (board_info_.vid == PDEV_VID_GOOGLE && board_info_.pid == PDEV_PID_CLEO) {
    panel_type_ = PANEL_ST7701S;
    init_disp_table_ = &kDisplaySettingSt7701s;
  } else {
    DISP_ERROR("Unsupport Hardware Detected\n");
    return ZX_ERR_NOT_SUPPORTED;
  }

  CopyDisplaySettings();

  // Create and Initialize the various display subsystems
  status = CreateAndInitDisplaySubsystems();
  if (status != ZX_OK) {
    return status;
  }

  // First, we need to properly shutdown the display subsystem in order to bring it back up
  // safely.
  ShutdownDisplaySubsytem();

  // Disable MMU Agent --> Treat Agent Transactions as PA (default is VA)
  smi_mmio_->Write32(0, kLarbMmuEnOffset);

  // Let's bring systems back up now
  StartupDisplaySubsytem();

  // TODO(payamm): For now, we set all modules between OVL and RDMA in bypass mode
  // The config function of each of these modules will set it to bypass mode
  color_->Config();
  ccorr_->Config();
  aal_->Config();
  gamma_->Config();
  dither_->Config();

  // Configure the DSI0 interface
  dsi_host_->Config(disp_setting_);

  // TODO(payamm): configuring the display RDMA engine does take into account height and width
  // of the display destination frame. However, it is not clear right now how to program
  // these if various layers have different destination dimensions. For now, we will configure
  // the display rdma to the display's height and width. However, this may need fine-tuning later
  // on.
  disp_rdma_->Config();
  disp_rdma_->Start();

  // Enable Mutex system
  syscfg_->MutexEnable();

  // This will trigger a start of the display subsystem.
  dsi_host_->Start();

  // Map VSync Interrupt
  status = pdev_get_interrupt(&pdev_, 0, 0, vsync_irq_.reset_and_get_address());
  if (status != ZX_OK) {
    DISP_ERROR("Could not map vsync Interruptn");
    return status;
  }

  auto start_thread = [](void* arg) { return static_cast<Mt8167sDisplay*>(arg)->VSyncThread(); };
  status = thrd_create_with_name(&vsync_thread_, start_thread, this, "vsync_thread");
  if (status != ZX_OK) {
    DISP_ERROR("Could not create vsync_thread\n");
    return status;
  }

  return ZX_OK;
}

void Mt8167sDisplay::Shutdown() {
  vsync_irq_.destroy();
  thrd_join(vsync_thread_, nullptr);
}

void Mt8167sDisplay::DdkUnbind() {
  Shutdown();
  DdkRemove();
}
void Mt8167sDisplay::DdkRelease() { delete this; }

zx_status_t Mt8167sDisplay::Bind() {
  composite_protocol_t composite;

  auto status = device_get_protocol(parent_, ZX_PROTOCOL_COMPOSITE, &composite);
  if (status != ZX_OK) {
    DISP_ERROR("Could not get composite protocol\n");
    return status;
  }

  zx_device_t* components[COMPONENT_COUNT];
  size_t actual;
  composite_get_components(&composite, components, COMPONENT_COUNT, &actual);
  if (actual < COMPONENT_DSI_IMPL) {
    DISP_ERROR("could not get components\n");
    return ZX_ERR_NOT_SUPPORTED;
  }

  status = device_get_protocol(components[COMPONENT_PDEV], ZX_PROTOCOL_PDEV, &pdev_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not get parent protocol\n");
    return status;
  }
  pdev_device_ = components[COMPONENT_PDEV];

  // Retrieve optional DSI_IMPL protocol.
  if (actual == COMPONENT_COUNT) {
    dsi_impl_protocol_t dsi;
    status = device_get_protocol(components[COMPONENT_DSI_IMPL], ZX_PROTOCOL_DSI_IMPL, &dsi);
    if (status != ZX_OK) {
      DISP_ERROR("Could not get Display DSI_IMPL protocol\n");
      return status;
    }
    dsiimpl_ = &dsi;
  }

  // Get board info
  status = pdev_get_board_info(&pdev_, &board_info_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not obtain board info\n");
    return status;
  }

  if (board_info_.vid == PDEV_VID_MEDIATEK && board_info_.pid == PDEV_PID_MEDIATEK_8167S_REF) {
    width_ = MTKREF_DISPLAY_WIDTH;
    height_ = MTKREF_DISPLAY_HEIGHT;
    hasDsi_ = true;
  } else if (board_info_.vid == PDEV_VID_GOOGLE && board_info_.pid == PDEV_PID_CLEO) {
    width_ = CLEO_DISPLAY_WIDTH;
    height_ = CLEO_DISPLAY_HEIGHT;
    hasDsi_ = true;
  } else {
    DISP_ERROR("Unsupport Hardware Detected\n");
    return ZX_ERR_NOT_SUPPORTED;
  }

  // Make sure DSI IMPL is valid
  if (hasDsi_) {
    if (!dsiimpl_.is_valid()) {
      DISP_ERROR("DSI Protocol Not implemented\n");
      return ZX_ERR_NO_RESOURCES;
    }
  }

  gpio_protocol_t gpio;
  status = device_get_protocol(components[COMPONENT_GPIO], ZX_PROTOCOL_GPIO, &gpio);
  if (status != ZX_OK) {
    DISP_ERROR("Could not get Display GPIO protocol\n");
    return status;
  }
  gpio_ = &gpio;

  power_protocol_t power;
  status = device_get_protocol(components[COMPONENT_POWER], ZX_PROTOCOL_POWER, &power);
  if (status != ZX_OK) {
    DISP_ERROR("Could not get Display Power protocol\n");
    return status;
  }
  power_ = &power;

  status = device_get_protocol(components[COMPONENT_SYSMEM], ZX_PROTOCOL_SYSMEM, &sysmem_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not get Display SYSMEM protocol\n");
    return status;
  }

  status = pdev_get_bti(&pdev_, 0, bti_.reset_and_get_address());
  if (status != ZX_OK) {
    DISP_ERROR("Could not get BTI handle\n");
    return status;
  }

  mmio_buffer_t mmio;
  status =
      pdev_map_mmio_buffer(&pdev_, MMIO_DISP_SMI_LARB0, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
  if (status != ZX_OK) {
    DISP_ERROR("Could not map SMI LARB0 mmio\n");
    return status;
  }
  fbl::AllocChecker ac;
  smi_mmio_ = fbl::make_unique_checked<ddk::MmioBuffer>(&ac, mmio);
  if (!ac.check()) {
    DISP_ERROR("Could not map SMI LARB0 MMIO\n");
    return ZX_ERR_NO_MEMORY;
  }

  status = DdkAdd("mt8167s-display");
  if (status != ZX_OK) {
    DISP_ERROR("coud not add device\n");
    Shutdown();
    return status;
  }

  return ZX_OK;
}

// main bind function called from dev manager
zx_status_t display_bind(void* ctx, zx_device_t* parent) {
  fbl::AllocChecker ac;
  auto dev = fbl::make_unique_checked<mt8167s_display::Mt8167sDisplay>(&ac, parent);
  if (!ac.check()) {
    DISP_ERROR("no bind\n");
    return ZX_ERR_NO_MEMORY;
  }
  zx_status_t status = dev->Bind();
  if (status == ZX_OK) {
    __UNUSED auto ptr = dev.release();
  }
  return status;
}

static constexpr zx_driver_ops_t display_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = display_bind;
  return ops;
}();

}  // namespace mt8167s_display

// clang-format off
ZIRCON_DRIVER_BEGIN(mt8167s_display, mt8167s_display::display_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_MEDIATEK),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_MEDIATEK_DISPLAY),
ZIRCON_DRIVER_END(mt8167s_display)
