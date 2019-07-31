// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_MISC_GOLDFISH_PIPE_DEVICE_H_
#define ZIRCON_SYSTEM_DEV_MISC_GOLDFISH_PIPE_DEVICE_H_

#include <ddk/device.h>
#include <ddk/io-buffer.h>
#include <ddktl/device.h>
#include <ddktl/protocol/acpi.h>
#include <ddktl/protocol/goldfish/pipe.h>
#include <fbl/mutex.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/bti.h>
#include <lib/zx/event.h>
#include <lib/zx/interrupt.h>
#include <threads.h>
#include <zircon/thread_annotations.h>
#include <zircon/types.h>

#include <map>
#include <memory>
#include <optional>

namespace goldfish {

class PipeDevice;
using DeviceType = ddk::Device<PipeDevice, ddk::Unbindable, ddk::Openable>;

class PipeDevice
    : public DeviceType,
      public ddk::GoldfishPipeProtocol<PipeDevice, ddk::base_protocol> {
public:
    static zx_status_t Create(void* ctx, zx_device_t* parent);

    explicit PipeDevice(zx_device_t* parent);
    ~PipeDevice();

    zx_status_t Bind();

    // Device protocol implementation.
    zx_status_t DdkOpen(zx_device_t** dev_out, uint32_t flags);
    void DdkUnbind();
    void DdkRelease();
    zx_status_t GoldfishPipeCreate(const goldfish_pipe_signal_value_t* cb_value,
                                   int32_t* out_id, zx::vmo* out_vmo);
    void GoldfishPipeDestroy(int32_t id);
    void GoldfishPipeOpen(int32_t id);
    void GoldfishPipeExec(int32_t id);
    zx_status_t GoldfishPipeGetBti(zx::bti* out_bti);
    zx_status_t GoldfishPipeConnectSysmem(zx::channel connection);
    zx_status_t GoldfishPipeRegisterSysmemHeap(uint64_t heap,
                                               zx::channel connection);

    int IrqHandler();

private:
    struct Pipe {
        Pipe(zx_paddr_t paddr, zx::pmt pmt,
             const goldfish_pipe_signal_value_t* cb_value)
            : paddr(paddr), pmt(std::move(pmt)), cb_value(*cb_value) {}

        const zx_paddr_t paddr;
        const zx::pmt pmt;
        const goldfish_pipe_signal_value_t cb_value;
    };

    ddk::AcpiProtocolClient acpi_;
    zx::interrupt irq_;
    zx::bti bti_;
    ddk::IoBuffer io_buffer_;
    thrd_t irq_thread_{};
    int32_t next_pipe_id_ = 1;

    fbl::Mutex mmio_lock_;
    std::optional<ddk::MmioBuffer> mmio_ TA_GUARDED(mmio_lock_);

    fbl::Mutex pipes_lock_;
    // TODO(TC-383): This should be std::unordered_map.
    using PipeMap = std::map<int32_t, std::unique_ptr<Pipe>>;
    PipeMap pipes_ TA_GUARDED(pipes_lock_);

    DISALLOW_COPY_ASSIGN_AND_MOVE(PipeDevice);
};

} // namespace goldfish

#endif // ZIRCON_SYSTEM_DEV_MISC_GOLDFISH_PIPE_DEVICE_H_
