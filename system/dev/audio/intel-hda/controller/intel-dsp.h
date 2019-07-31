// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_AUDIO_INTEL_HDA_CONTROLLER_INTEL_DSP_H_
#define ZIRCON_SYSTEM_DEV_AUDIO_INTEL_HDA_CONTROLLER_INTEL_DSP_H_

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/protocol/intelhda/codec.h>
#include <fbl/mutex.h>
#include <intel-hda/codec-utils/codec-driver-base.h>
#include <intel-hda/utils/intel-audio-dsp-ipc.h>
#include <intel-hda/utils/intel-hda-registers.h>
#include <intel-hda/utils/nhlt.h>
#include <intel-hda/utils/utils.h>
#include <lib/fzl/vmo-mapper.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <threads.h>

#include "debug-logging.h"
#include "intel-dsp-ipc.h"
#include "intel-dsp-stream.h"
#include "intel-dsp-topology.h"
#include "intel-hda-stream.h"

namespace audio {
namespace intel_hda {

class IntelHDAController;

class IntelDsp : public codecs::IntelHDACodecDriverBase {
 public:
  IntelDsp(IntelHDAController* controller, hda_pp_registers_t* pp_regs);
  ~IntelDsp();

  zx_status_t Init(zx_device_t* dsp_dev);

  const char* log_prefix() const { return log_prefix_; }

  // Interrupt handler.
  void ProcessIrq();

  // Mailbox constants
  static constexpr size_t MAILBOX_SIZE = 0x1000;

  // IPC helper methods
  void SendIpcMessage(const IpcMessage& message) {
    // HIPCIE must be programmed before setting HIPCI.BUSY
    REG_WR(&regs()->hipcie, message.extension);
    REG_WR(&regs()->hipci, message.primary | ADSP_REG_HIPCI_BUSY);
  }
  void IpcMailboxWrite(const void* data, size_t size) { mailbox_out_.Write(data, size); }
  void IpcMailboxRead(void* data, size_t size) { mailbox_in_.Read(data, size); }

  zx_status_t StartPipeline(const DspPipeline& pipeline);
  zx_status_t PausePipeline(const DspPipeline& pipeline);

  void DeviceShutdown();
  zx_status_t Suspend(uint32_t flags) override final;

  // ZX_PROTOCOL_IHDA_CODEC Interface
  zx_status_t CodecGetDispatcherChannel(zx_handle_t* remote_endpoint_out);

 private:
  // Accessor for our mapped registers
  adsp_registers_t* regs() const;
  adsp_fw_registers_t* fw_regs() const;

  zx_status_t SetupDspDevice();
  zx_status_t ParseNhlt();

  int InitThread();

  zx_status_t Boot();
  zx_status_t StripFirmware(const zx::vmo& fw, void* out, size_t* size_inout);
  zx_status_t LoadFirmware();

  zx_status_t GetI2SBlob(uint8_t bus_id, uint8_t direction, const AudioDataFormat& format,
                         const void** out_blob, size_t* out_size);

  zx_status_t GetModulesInfo();
  zx_status_t CreateHostDmaModule(uint8_t instance_id, uint8_t pipeline_id, const CopierCfg& cfg);
  zx_status_t CreateI2SModule(uint8_t instance_id, uint8_t pipeline_id, uint8_t i2s_instance_id,
                              uint8_t direction, const CopierCfg& cfg);
  zx_status_t CreateMixinModule(uint8_t instance_id, uint8_t pipeline_id, const BaseModuleCfg& cfg);
  zx_status_t CreateMixoutModule(uint8_t instance_id, uint8_t pipeline_id,
                                 const BaseModuleCfg& cfg);
  zx_status_t SetupPipelines();
  zx_status_t RunPipeline(uint8_t pipeline_id);

  bool IsCoreEnabled(uint8_t core_mask);

  zx_status_t ResetCore(uint8_t core_mask);
  zx_status_t UnResetCore(uint8_t core_mask);
  zx_status_t PowerDownCore(uint8_t core_mask);
  zx_status_t PowerUpCore(uint8_t core_mask);
  void RunCore(uint8_t core_mask);

  void EnableInterrupts();

  zx_status_t GetMmio(zx_handle_t* out_vmo, size_t* out_size);
  void Enable();
  void Disable();
  void IrqEnable();
  void IrqDisable();

  // Thunks for interacting with clients and codec drivers.
  zx_status_t ProcessClientRequest(dispatcher::Channel* channel, bool is_driver_channel);
  void ProcessClientDeactivate(const dispatcher::Channel* channel);
  zx_status_t ProcessRequestStream(dispatcher::Channel* channel,
                                   const ihda_proto::RequestStreamReq& req);
  zx_status_t ProcessReleaseStream(dispatcher::Channel* channel,
                                   const ihda_proto::ReleaseStreamReq& req);
  zx_status_t ProcessSetStreamFmt(dispatcher::Channel* channel,
                                  const ihda_proto::SetStreamFmtReq& req);

  zx_status_t CreateAndStartStreams();

  // Debug
  void DumpRegs();
  void DumpNhlt(const nhlt_table_t* table, size_t length);
  void DumpFirmwareConfig(const TLVHeader* config, size_t length);
  void DumpHardwareConfig(const TLVHeader* config, size_t length);
  void DumpModulesInfo(const ModuleEntry* info, uint32_t count);
  void DumpPipelineListInfo(const PipelineListInfo* info);
  void DumpPipelineProps(const PipelineProps* props);

  enum class State : uint8_t {
    START,
    INITIALIZING,  // init thread running
    OPERATING,
    SHUT_DOWN,
    ERROR = 0xFF,
  };
  State state_ = State::START;

  // Pointer to our owner.
  IntelHDAController* controller_ = nullptr;

  // Pipe processintg registers
  hda_pp_registers_t* pp_regs_ = nullptr;

  // IPC
  IntelDspIpc ipc_{this};

  // IPC Mailboxes
  class Mailbox {
   public:
    void Initialize(void* base, size_t size) {
      base_ = base;
      size_ = size;
    }

    size_t size() const { return size_; }

    void Write(const void* data, size_t size) {
      // It is the caller's responsibility to ensure size fits in the mailbox.
      ZX_DEBUG_ASSERT(size <= size_);
      memcpy(base_, data, size);
    }
    void Read(void* data, size_t size) {
      // It is the caller's responsibility to ensure size fits in the mailbox.
      ZX_DEBUG_ASSERT(size <= size_);
      memcpy(data, base_, size);
    }

   private:
    void* base_;
    size_t size_;
  };
  Mailbox mailbox_in_;
  Mailbox mailbox_out_;

  // NHLT buffer.
  uint8_t nhlt_buf_[PAGE_SIZE];

  // I2S config
  struct I2SConfig {
    I2SConfig(uint8_t bid, uint8_t dir, const formats_config_t* f)
        : valid(true), bus_id(bid), direction(dir), formats(f) {}
    I2SConfig() = default;

    bool valid = false;
    uint8_t bus_id = 0;
    uint8_t direction = 0;

    const formats_config_t* formats = nullptr;
  };
  static constexpr size_t I2S_CONFIG_MAX = 8;
  I2SConfig i2s_configs_[I2S_CONFIG_MAX];

  // Module IDs
  enum Module {
    COPIER,
    MIXIN,
    MIXOUT,
    MODULE_COUNT,
  };
  static constexpr uint16_t MODULE_ID_INVALID = 0xFFFF;
  uint16_t module_ids_[to_underlying(Module::MODULE_COUNT)];

  // Init thread
  thrd_t init_thread_;

  // Log prefix storage
  char log_prefix_[LOG_PREFIX_STORAGE] = {0};

  // PCI registers
  fzl::VmoMapper mapped_regs_;

  // Driver connection state
  fbl::Mutex codec_driver_channel_lock_;
  fbl::RefPtr<dispatcher::Channel> codec_driver_channel_ TA_GUARDED(codec_driver_channel_lock_);

  // Active DMA streams
  fbl::Mutex active_streams_lock_;
  IntelHDAStream::Tree active_streams_ TA_GUARDED(active_streams_lock_);
};

}  // namespace intel_hda
}  // namespace audio

#endif  // ZIRCON_SYSTEM_DEV_AUDIO_INTEL_HDA_CONTROLLER_INTEL_DSP_H_
