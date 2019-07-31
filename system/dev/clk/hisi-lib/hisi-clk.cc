// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <dev/clk/hisi-lib/hisi-clk.h>

#include <hw/reg.h>
#include <stdint.h>
#include <stdlib.h>
#include <threads.h>

#include <zircon/assert.h>
#include <zircon/threads.h>

#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/mmio-buffer.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/clockimpl.h>
#include <lib/device-protocol/platform-device.h>
#include <ddk/protocol/platform/device.h>
#include <lib/device-protocol/pdev.h>

#include <ddktl/protocol/platform/bus.h>

#include <dev/clk/hisi-lib/hisi-gate.h>

// HiSilicon has two different types of clock gates:
//
// + Clock Gates
//   These are enabled and disabled by setting and unsetting bits in the
//   sctrl_mmio register bank. Setting a bit to 1 enables the corresponding
//   clock and 0 disables it.
//
// + Separated Clock Gates
//   These are enabled via one bank of registers and disabled via another.
//   Writing 1 to a clock's enable bit will enable it and writing 1 to its
//   disable bank will disable it.

// These constants only apply to separated clock gates and correspond to the
// offset from the register base that needs to be modified to enable/disable
// the clock.
namespace {
constexpr size_t kSepEnable = 0;
constexpr size_t kSepDisable = 4;
constexpr size_t kSepStatus = 8;
} // namespace

namespace hisi_clock {

zx_status_t HisiClock::Create(const char* name, const Gate gate_list[],
                              const size_t gate_count, zx_device_t* parent) {
    zx_status_t st;

    // Avoiding make_unique becuase HisiClock has a private constructor.
    std::unique_ptr<HisiClock> device(new HisiClock(parent, gate_list, gate_count));

    st = device->Init();
    if (st != ZX_OK) {
        zxlogf(ERROR, "HisiClock::Create: failed to init device, rc = %d\n", st);
        return st;
    }

    st = device->DdkAdd(name);
    if (st != ZX_OK) {
        zxlogf(ERROR, "HisiClock::Create: failed to add device, rc = %d\n", st);
        return st;
    }

    // Devmgr owns memory now.
    __UNUSED auto ptr = device.release();

    return ZX_OK;
}

zx_status_t HisiClock::ToggleSepClkLocked(const Gate& gate, bool enable) {
    const uint32_t val = 1 << gate.Bit();

    if (enable) {
        peri_crg_mmio_->Write32(val, gate.Reg() + kSepEnable);
    } else {
        peri_crg_mmio_->Write32(val, gate.Reg() + kSepDisable);
    }

    peri_crg_mmio_->Read32(gate.Reg() + kSepStatus);

    return ZX_OK;
}

zx_status_t HisiClock::ToggleGateClkLocked(const Gate& gate, bool enable) {
    if (enable) {
        sctrl_mmio_->SetBits32(gate.Bit(), gate.Reg());
    } else {
        sctrl_mmio_->ClearBits32(gate.Bit(), gate.Reg());
    }

    return ZX_OK;
}

zx_status_t HisiClock::Toggle(uint32_t clock, bool enable) {
    fbl::AutoLock<fbl::Mutex> guard(&lock_);

    if (clock >= gate_count_) {
        return ZX_ERR_INVALID_ARGS;
    }

    const Gate& gate = gates_[clock];

    switch (gate.Bank()) {
    case RegisterBank::Sctrl:
        return ToggleGateClkLocked(gate, enable);
    case RegisterBank::Peri:
        return ToggleSepClkLocked(gate, enable);
    }

    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t HisiClock::ClockImplEnable(uint32_t clock) {
    return Toggle(clock, true);
}

zx_status_t HisiClock::ClockImplDisable(uint32_t clock) {
    return Toggle(clock, false);
}

zx_status_t HisiClock::ClockImplIsEnabled(uint32_t id, bool* out_enabled) {
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t HisiClock::ClockImplSetRate(uint32_t id, uint64_t hz) {
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t HisiClock::ClockImplQuerySupportedRate(uint32_t id, uint64_t max_rate, uint64_t* out_best_rate) {
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t HisiClock::ClockImplGetRate(uint32_t id, uint64_t* out_current_rate) {
    return ZX_ERR_NOT_SUPPORTED;
}

void HisiClock::DdkUnbind() {
    DeInit();
    DdkRemove();
}

void HisiClock::DdkRelease() {
    delete this;
}

void HisiClock::DeInit() {
    fbl::AutoLock<fbl::Mutex> guard(&lock_);

    peri_crg_mmio_.reset();
    sctrl_mmio_.reset();
}

zx_status_t HisiClock::RegisterClockProtocol() {
    zx_status_t st;

    ddk::PBusProtocolClient pbus(parent());
    if (!pbus.is_valid()) {
        return ZX_ERR_NO_RESOURCES;
    }

    clock_impl_protocol_t clk_proto = {
        .ops = &clock_impl_protocol_ops_,
        .ctx = this,
    };

    st = pbus.RegisterProtocol(ZX_PROTOCOL_CLOCK_IMPL, &clk_proto, sizeof(clk_proto));
    if (st != ZX_OK) {
        zxlogf(ERROR, "HisiClock::RegisterClockProtocol: pbus_register_protocol"
                      " failed with st = %d\n",
               st);
        return st;
    }

    return ZX_OK;
}

zx_status_t HisiClock::Init() {
    zx_status_t st;

    ddk::PDev pdev(parent());
    if (!pdev.is_valid()) {
        zxlogf(ERROR, "HisiClock::Init: failed to get pdev protocol\n");
        return ZX_ERR_NO_RESOURCES;
    }

    st = pdev.MapMmio(0, &peri_crg_mmio_);
    if (st != ZX_OK) {
        zxlogf(ERROR, "HisiClock::Init: map peri crg mmio failed, st = %d\n", st);
        return st;
    }

    st = pdev.MapMmio(1, &sctrl_mmio_);
    if (st != ZX_OK) {
        zxlogf(ERROR, "HisiClock::Init: map sctrl mmio failed, st = %d\n", st);
        return st;
    }

    st = RegisterClockProtocol();

    return ZX_OK;
}

} // namespace hisi_clock
