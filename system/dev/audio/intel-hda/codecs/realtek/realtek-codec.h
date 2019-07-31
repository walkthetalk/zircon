// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_AUDIO_INTEL_HDA_CODECS_REALTEK_REALTEK_CODEC_H_
#define ZIRCON_SYSTEM_DEV_AUDIO_INTEL_HDA_CODECS_REALTEK_REALTEK_CODEC_H_

#include <ddk/device.h>
#include <fbl/ref_ptr.h>
#include <intel-hda/codec-utils/codec-driver-base.h>

#include "utils.h"

namespace audio {
namespace intel_hda {
namespace codecs {

class RealtekCodec : public IntelHDACodecDriverBase {
 public:
  static zx_status_t Create(void* ctx, zx_device_t* parent);

  zx_status_t Init(zx_device_t* codec_dev);
  zx_status_t Start() override;
  zx_status_t ProcessSolicitedResponse(const CodecResponse& resp) override;

 protected:
  void PrintDebugPrefix() const override;

  zx_status_t SetupCommon();
  zx_status_t SetupAcer12();
  zx_status_t SetupIntelNUC();
  zx_status_t RunCommandList(const CommandListEntry* cmds, size_t cmd_count);
  zx_status_t CreateAndStartStreams(const StreamProperties* streams, size_t stream_cnt);

 private:
  friend class fbl::RefPtr<RealtekCodec>;
  RealtekCodec() {}
  virtual ~RealtekCodec() {}

  bool waiting_for_impl_id_ = true;
};

}  // namespace codecs
}  // namespace intel_hda
}  // namespace audio

#endif  // ZIRCON_SYSTEM_DEV_AUDIO_INTEL_HDA_CODECS_REALTEK_REALTEK_CODEC_H_
