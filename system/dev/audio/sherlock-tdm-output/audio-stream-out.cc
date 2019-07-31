// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/composite.h>
#include <lib/device-protocol/pdev.h>
#include <soc/aml-t931/t931-gpio.h>

#include "audio-stream-out.h"

namespace audio {
namespace sherlock {

enum {
    COMPONENT_PDEV,
    COMPONENT_FAULT_GPIO,
    COMPONENT_ENABLE_GPIO,
    COMPONENT_I2C_0,
    COMPONENT_I2C_1,
    COMPONENT_I2C_2, // Optional
    COMPONENT_COUNT,
};

// Expects L+R for tweeters + L+R for the 1 Woofer (mixed in HW).
// The user must perform crossover filtering on these channels.
constexpr size_t kNumberOfChannels = 4;
// Calculate ring buffer size for 1 second of 16-bit, 48kHz.
constexpr size_t kRingBufferSize = fbl::round_up<size_t, size_t>(48000 * 2 * kNumberOfChannels,
                                                                 PAGE_SIZE);

SherlockAudioStreamOut::SherlockAudioStreamOut(zx_device_t* parent)
    : SimpleAudioStream(parent, false), pdev_(parent) {
}

zx_status_t SherlockAudioStreamOut::InitPdev() {
    composite_protocol_t composite;

    auto status = device_get_protocol(parent(), ZX_PROTOCOL_COMPOSITE, &composite);
    if (status != ZX_OK) {
        zxlogf(ERROR, "Could not get composite protocol\n");
        return status;
    }

    zx_device_t* components[COMPONENT_COUNT] = {};
    size_t actual;
    composite_get_components(&composite, components, countof(components), &actual);
    if (actual < countof(components) - 1) {
        zxlogf(ERROR, "could not get components\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    pdev_ = components[COMPONENT_PDEV];
    if (!pdev_.is_valid()) {
        return ZX_ERR_NO_RESOURCES;
    }

    status = device_get_metadata(parent(), DEVICE_METADATA_PRIVATE, &codecs_types_,
                                             sizeof(metadata::Codec), &actual);
    if (status != ZX_OK || sizeof(metadata::Codec) != actual) {
        zxlogf(ERROR, "%s device_get_metadata failed %d\n", __FILE__, status);
        return status;
    }

    if (codecs_types_ == metadata::Codec::Tas5720x3) {
        zxlogf(INFO, "audio: using 3 Tas5720 codecs\n");
        fbl::AllocChecker ac;
        codecs_ = fbl::Array(new (&ac) fbl::unique_ptr<Codec>[3], 3);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < 3; ++i) {
            codecs_[i] = Tas5720::Create(components[COMPONENT_I2C_0 + i]);
            if (!codecs_[i]) {
                zxlogf(ERROR, "%s could not get tas5720\n", __func__);
                return ZX_ERR_NO_RESOURCES;
            }
        }
    } else {
        zxlogf(ERROR, "%s invalid or unsupported codec\n", __func__);
        return ZX_ERR_NO_RESOURCES;
    }

    audio_fault_ = components[COMPONENT_FAULT_GPIO];
    audio_en_ = components[COMPONENT_ENABLE_GPIO];

    if (!audio_fault_.is_valid() || !audio_en_.is_valid()) {
        zxlogf(ERROR, "%s failed to allocate gpio\n", __func__);
        return ZX_ERR_NO_RESOURCES;
    }

    status = pdev_.GetBti(0, &bti_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s could not obtain bti - %d\n", __func__, status);
        return status;
    }

    std::optional<ddk::MmioBuffer> mmio;
    status = pdev_.MapMmio(0, &mmio);
    if (status != ZX_OK) {
        return status;
    }
    aml_audio_ = AmlTdmDevice::Create(*std::move(mmio), HIFI_PLL, TDM_OUT_C, FRDDR_A, MCLK_C);
    if (aml_audio_ == nullptr) {
        zxlogf(ERROR, "%s failed to create tdm device\n", __func__);
        return ZX_ERR_NO_MEMORY;
    }

    // Drive strength settings
    status = pdev_.MapMmio(1, &mmio);
    if (status != ZX_OK) {
        return status;
    }
    // Strength 1 for sclk (bit 14, GPIOZ(7)) and lrclk (bit 12, GPIOZ(6)),
    // GPIO offsets are in 4 bytes units.
    mmio->SetBits<uint32_t>((1 << 14) | (1 << 12), 4 * T931_PAD_DS_REG4A);
    status = pdev_.MapMmio(2, &mmio);
    if (status != ZX_OK) {
        return status;
    }
    // Strength 1 for mclk (bit 18,  GPIOAO(9)), GPIO offsets are in 4 bytes units.
    mmio->SetBit<uint32_t>(18, 4 * T931_AO_PAD_DS_A);

    audio_en_.Write(1); // SOC_AUDIO_EN.

    codecs_[0]->Init(0); // Use TDM slot 0.
    codecs_[1]->Init(1); // Use TDM slot 1.
    codecs_[2]->Init(0); // Use TDM slot 0.

    InitBuffer(kRingBufferSize);

    aml_audio_->SetBuffer(pinned_ring_buffer_.region(0).phys_addr,
                          pinned_ring_buffer_.region(0).size);

    // Setup Stereo Left Justified:
    // -lrclk duty = 64 sclk (SetSclkDiv lrdiv=63 below).
    // -No delay from the time the lrclk signal changes state state to the first bit of data on the
    // data lines  (ConfigTdmOutSlot bitoffset=4 below accomplishes this).
    // -3072MHz/64 = 48KHz.

    // 4 bitoffset, 2 slots, 32 bits/slot, 16 bits/sample, enable mix L+R on lane 1.
    aml_audio_->ConfigTdmOutSlot(4, 1, 31, 15, (1 << 1));

    // Lane 0 L channel set to FRDDR slot 0.
    // Lane 0 R channel set to FRDDR slot 1.
    // Lane 1 L channel set to FRDDR slot 2.  Mixed with R, see ConfigTdmOutSlot above.
    // Lane 1 R channel set to FRDDR slot 3.  Mixed with L, see ConfigTdmOutSlot above.
    aml_audio_->ConfigTdmOutSwaps(0x00003210);

    // Tweeters: Lane 0, unmask TDM slots 0 & 1 (L+R FRDDR slots 0 & 1).
    aml_audio_->ConfigTdmOutLane(0, 0x00000003);

    // Woofer: Lane 1, unmask TDM slot 0 & 1 (Woofer FRDDR slots 2 & 3).
    aml_audio_->ConfigTdmOutLane(1, 0x00000003);

    // mclk = T931_HIFI_PLL_RATE/125 = 1536MHz/125 = 12.288MHz.
    aml_audio_->SetMclkDiv(124);

    // Per schematic, mclk uses pad 0 (MCLK_0 instead of MCLK_1).
    aml_audio_->SetMClkPad(MCLK_PAD_0);

    // sclk = 12.288MHz/4 = 3.072MHz, 32L + 32R sclks = 64 sclks.
    aml_audio_->SetSclkDiv(3, 31, 63);

    aml_audio_->Sync();

    return ZX_OK;
}

zx_status_t SherlockAudioStreamOut::Init() {
    zx_status_t status;

    status = InitPdev();
    if (status != ZX_OK) {
        return status;
    }

    status = AddFormats();
    if (status != ZX_OK) {
        return status;
    }

    float gain = codecs_[0]->GetGain();
    float min_gain = codecs_[0]->GetMinGain();
    float max_gain = codecs_[0]->GetMaxGain();
    float gain_step = codecs_[0]->GetGainStep();
    for (size_t i = 1; i < codecs_.size(); ++i) {
        min_gain = fbl::max(min_gain, codecs_[i]->GetMinGain());
        max_gain = fbl::min(max_gain, codecs_[i]->GetMaxGain());
        gain_step = fbl::max(gain_step, codecs_[i]->GetGainStep());
        status = codecs_[i]->SetGain(gain);
        if (status != ZX_OK) {
            return status;
        }
    }
    cur_gain_state_.cur_gain = gain;
    cur_gain_state_.cur_mute = false;
    cur_gain_state_.cur_agc = false;

    cur_gain_state_.min_gain = min_gain;
    cur_gain_state_.max_gain = max_gain;
    cur_gain_state_.gain_step = gain_step;
    cur_gain_state_.can_mute = false;
    cur_gain_state_.can_agc = false;

    snprintf(device_name_, sizeof(device_name_), "sherlock-audio-out");
    snprintf(mfr_name_, sizeof(mfr_name_), "unknown");
    snprintf(prod_name_, sizeof(prod_name_), "sherlock");

    unique_id_ = AUDIO_STREAM_UNIQUE_ID_BUILTIN_SPEAKERS;

    return ZX_OK;
}

zx_status_t SherlockAudioStreamOut::InitPost() {

    notify_timer_ = dispatcher::Timer::Create();
    if (notify_timer_ == nullptr) {
        return ZX_ERR_NO_MEMORY;
    }

    dispatcher::Timer::ProcessHandler thandler(
        [tdm = this](dispatcher::Timer * timer)->zx_status_t {
            OBTAIN_EXECUTION_DOMAIN_TOKEN(t, tdm->domain_);
            return tdm->ProcessRingNotification();
        });

    return notify_timer_->Activate(domain_, std::move(thandler));
}

// Timer handler for sending out position notifications.
zx_status_t SherlockAudioStreamOut::ProcessRingNotification() {
    ZX_ASSERT(us_per_notification_ != 0);

    // TODO(andresoportus): johngro noticed there is some drifting on notifications here,
    // could be improved with maintaining an absolute time and even better computing using
    // rationals, but higher level code should not rely on this anyways (see MTWN-57).
    notify_timer_->Arm(zx_deadline_after(ZX_USEC(us_per_notification_)));

    audio_proto::RingBufPositionNotify resp = {};
    resp.hdr.cmd = AUDIO_RB_POSITION_NOTIFY;

    resp.ring_buffer_pos = aml_audio_->GetRingPosition();
    return NotifyPosition(resp);
}

zx_status_t SherlockAudioStreamOut::ChangeFormat(const audio_proto::StreamSetFmtReq& req) {
    fifo_depth_ = aml_audio_->fifo_depth();
    external_delay_nsec_ = 0;

    // At this time only one format is supported, and hardware is initialized
    // during driver binding, so nothing to do at this time.
    return ZX_OK;
}

void SherlockAudioStreamOut::ShutdownHook() {
    aml_audio_->Shutdown();
    audio_en_.Write(0);
}

zx_status_t SherlockAudioStreamOut::SetGain(const audio_proto::SetGainReq& req) {
    for (size_t i = 0; i < codecs_.size(); ++i) {
        zx_status_t status = codecs_[i]->SetGain(req.gain);
        if (status != ZX_OK) {
            return status;
        }
    }
    cur_gain_state_.cur_gain = req.gain;
    // TODO(andresoportus): More options on volume setting, e.g.:
    // -Allow for ratio between tweeters and woofer gains.
    // -Make use of analog gain options in TAS5720.
    // -Add codecs mute and fade support.
    return ZX_OK;
}

zx_status_t SherlockAudioStreamOut::GetBuffer(const audio_proto::RingBufGetBufferReq& req,
                                              uint32_t* out_num_rb_frames,
                                              zx::vmo* out_buffer) {

    uint32_t rb_frames =
        static_cast<uint32_t>(pinned_ring_buffer_.region(0).size) / frame_size_;

    if (req.min_ring_buffer_frames > rb_frames) {
        return ZX_ERR_OUT_OF_RANGE;
    }
    zx_status_t status;
    constexpr uint32_t rights = ZX_RIGHT_READ | ZX_RIGHT_WRITE | ZX_RIGHT_MAP | ZX_RIGHT_TRANSFER;
    status = ring_buffer_vmo_.duplicate(rights, out_buffer);
    if (status != ZX_OK) {
        return status;
    }

    *out_num_rb_frames = rb_frames;

    aml_audio_->SetBuffer(pinned_ring_buffer_.region(0).phys_addr,
                          rb_frames * frame_size_);

    return ZX_OK;
}

zx_status_t SherlockAudioStreamOut::Start(uint64_t* out_start_time) {

    *out_start_time = aml_audio_->Start();

    uint32_t notifs = LoadNotificationsPerRing();
    if (notifs) {
        us_per_notification_ = static_cast<uint32_t>(
            1000 * pinned_ring_buffer_.region(0).size / (frame_size_ * 48 * notifs));
        notify_timer_->Arm(zx_deadline_after(ZX_USEC(us_per_notification_)));
    } else {
        us_per_notification_ = 0;
    }
    for (size_t i = 0; i < codecs_.size(); ++i) {
        auto status = codecs_[i]->Mute(false);
        if (status != ZX_OK) {
            return status;
        }
    }
    return ZX_OK;
}

zx_status_t SherlockAudioStreamOut::Stop() {
    for (size_t i = 0; i < codecs_.size(); ++i) {
        auto status = codecs_[i]->Mute(true);
        if (status != ZX_OK) {
            return status;
        }
    }
    notify_timer_->Cancel();
    us_per_notification_ = 0;
    aml_audio_->Stop();
    return ZX_OK;
}

zx_status_t SherlockAudioStreamOut::AddFormats() {
    fbl::AllocChecker ac;
    supported_formats_.reserve(1, &ac);
    if (!ac.check()) {
        zxlogf(ERROR, "Out of memory, can not create supported formats list\n");
        return ZX_ERR_NO_MEMORY;
    }

    // Add the range for basic audio support.
    audio_stream_format_range_t range;

    range.min_channels = kNumberOfChannels;
    range.max_channels = kNumberOfChannels;
    range.sample_formats = AUDIO_SAMPLE_FORMAT_16BIT;
    range.min_frames_per_second = 48000;
    range.max_frames_per_second = 48000;
    range.flags = ASF_RANGE_FLAG_FPS_48000_FAMILY;

    supported_formats_.push_back(range);

    return ZX_OK;
}

zx_status_t SherlockAudioStreamOut::InitBuffer(size_t size) {
    zx_status_t status;
    // TODO(ZX-3149): Per johngro's suggestion preallocate contiguous memory (say in
    // platform bus) since we are likely to fail after running for a while and we need to
    // init again (say the devhost is restarted).
    status = zx_vmo_create_contiguous(bti_.get(), size, 0,
                                      ring_buffer_vmo_.reset_and_get_address());
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s failed to allocate ring buffer vmo - %d\n", __func__, status);
        return status;
    }

    status = pinned_ring_buffer_.Pin(ring_buffer_vmo_, bti_, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s failed to pin ring buffer vmo - %d\n", __func__, status);
        return status;
    }
    if (pinned_ring_buffer_.region_count() != 1) {
        zxlogf(ERROR, "%s buffer is not contiguous", __func__);
        return ZX_ERR_NO_MEMORY;
    }

    return ZX_OK;
}

static zx_status_t audio_bind(void* ctx, zx_device_t* device) {
    auto stream =
        audio::SimpleAudioStream::Create<audio::sherlock::SherlockAudioStreamOut>(device);
    if (stream == nullptr) {
        return ZX_ERR_NO_MEMORY;
    }

    return ZX_OK;
}

static constexpr zx_driver_ops_t driver_ops = [](){
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = audio_bind;
    return ops;
}();

} // sherlock
} // audio

// clang-format off
ZIRCON_DRIVER_BEGIN(aml_sherlock_tdm, audio::sherlock::driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_AMLOGIC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_AMLOGIC_T931),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_AMLOGIC_TDM),
ZIRCON_DRIVER_END(aml_sherlock_tdm)
// clang-format on

