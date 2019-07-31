// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <string.h>

#include <ddk/debug.h>

#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>

#include <fbl/unique_ptr.h>

#include <zircon/compiler.h>

#include <librtc.h>

namespace {

zx_status_t set_utc_offset(const fuchsia_hardware_rtc_Time* rtc) {
    uint64_t rtc_nanoseconds = seconds_since_epoch(rtc) * 1000000000;;
    int64_t offset = rtc_nanoseconds - zx_clock_get_monotonic();
    // Please do not use get_root_resource() in new code. See ZX-1467.
    return zx_clock_adjust(get_root_resource(), ZX_CLOCK_UTC, offset);
}

static zx_status_t fidl_Get(void* ctx, fidl_txn_t* txn);
static zx_status_t fidl_Set(void* ctx, const fuchsia_hardware_rtc_Time* rtc, fidl_txn_t* txn);

class FallbackRtc;
using RtcDevice = ddk::Device<FallbackRtc, ddk::Messageable>;

// The fallback RTC driver is a fake driver which avoids to special case
// in the upper layers on boards which don't have an RTC chip (and battery).
// it assumes that an external entity will set it to a approximately correct
// time based on other sources, most likely the roughtime service which
// runs at every boot.
class FallbackRtc : public RtcDevice,
                    public ddk::EmptyProtocol<ZX_PROTOCOL_RTC> {
  public:
    FallbackRtc(zx_device_t* parent)
        : RtcDevice(parent), rtc_last_({}) {
        // We don't rely on the default value to be correct to any approximation
        // but for debugging purposes is best to return a known value.
        rtc_last_.year = 2018;
        rtc_last_.month = 1;
        rtc_last_.day = 1;
    }

    zx_status_t Bind() {
        return DdkAdd("fallback-rtc");
    }

    void DdkRelease() {
        delete this;
    }

    zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
        return fuchsia_hardware_rtc_Device_dispatch(this, txn, msg, &fidl_ops_);
    }

  private:
      zx_status_t Get(fuchsia_hardware_rtc_Time& rtc) {

          // TODO(cpu): Advance the clock. This is not strictly necessary at the
          // moment because this driver basically serves as a rendezvous between
          // a Internet time server and the rest of the system.

          rtc = rtc_last_;
          return ZX_OK;
      }

      zx_status_t Set(const fuchsia_hardware_rtc_Time& rtc) {
          if (rtc_is_invalid(&rtc)) {
              return ZX_ERR_OUT_OF_RANGE;
          }

          rtc_last_ = rtc;

          auto status = set_utc_offset(&rtc_last_);
          if (status != ZX_OK) {
              zxlogf(ERROR, "The RTC driver was unable to set the UTC clock!\n");
          }

          return ZX_OK;
      }

    friend zx_status_t fidl_Get(void*, fidl_txn_t*);
    friend zx_status_t fidl_Set(void*, const fuchsia_hardware_rtc_Time*, fidl_txn_t*);

    const fuchsia_hardware_rtc_Device_ops_t fidl_ops_ = {
        .Get = fidl_Get,
        .Set = fidl_Set,
    };

    fuchsia_hardware_rtc_Time rtc_last_;
};

zx_status_t fidl_Get(void* ctx, fidl_txn_t* txn) {
    auto dev = static_cast<FallbackRtc*>(ctx);
    fuchsia_hardware_rtc_Time rtc;
    dev->Get(rtc);
    return fuchsia_hardware_rtc_DeviceGet_reply(txn, &rtc);
}

zx_status_t fidl_Set(void* ctx, const fuchsia_hardware_rtc_Time* rtc, fidl_txn_t* txn) {
    auto dev = static_cast<FallbackRtc*>(ctx);
    auto status = dev->Set(*rtc);
    return fuchsia_hardware_rtc_DeviceSet_reply(txn, status);
}

}  // namespace

extern "C" zx_status_t fallback_rtc_bind(void* ctx, zx_device_t* parent) {
    auto dev = std::make_unique<FallbackRtc>(parent);
    auto status = dev->Bind();
    if (status == ZX_OK) {
        // devmgr is now in charge of the device, until DdkRelease().
        __UNUSED auto ptr = dev.release();
    }
    return status;
}
