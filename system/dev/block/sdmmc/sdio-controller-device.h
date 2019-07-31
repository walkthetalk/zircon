// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <atomic>

#include <ddk/protocol/sdio.h>
#include <ddktl/device.h>
#include <fbl/algorithm.h>
#include <fbl/array.h>
#include <fbl/mutex.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <lib/sync/completion.h>
#include <zircon/thread_annotations.h>

#include "sdio-function-device.h"
#include "sdmmc-device.h"

namespace sdmmc {

class SdioControllerDevice;
using SdioControllerDeviceType = ddk::Device<SdioControllerDevice, ddk::Unbindable>;

class SdioControllerDevice : public SdioControllerDeviceType,
                             public fbl::RefCounted<SdioControllerDevice>,
                             public ddk::InBandInterruptProtocol<SdioControllerDevice> {
public:
    // SDIO cards support one common function and up to seven I/O functions. This struct is used to
    // keep track of each function's state as they can be configured independently.
    // Visible for testing.
    struct SdioFunction {
        sdio_func_hw_info_t hw_info;
        uint16_t cur_blk_size;
        bool enabled;
        bool intr_enabled;
    };

    SdioControllerDevice(zx_device_t* parent, const SdmmcDevice& sdmmc)
        : SdioControllerDeviceType(parent), sdmmc_(sdmmc) {
        for (size_t i = 0; i < fbl::count_of(funcs_); i++) {
            funcs_[i] = {};
        }
    }
    virtual ~SdioControllerDevice() = default;

    static zx_status_t Create(zx_device_t* parent, const SdmmcDevice& sdmmc,
                              fbl::RefPtr<SdioControllerDevice>* out_dev);

    void DdkUnbind();
    void DdkRelease();

    zx_status_t ProbeSdio();
    zx_status_t AddDevice();

    zx_status_t SdioGetDevHwInfo(sdio_hw_info_t* out_hw_info);
    zx_status_t SdioEnableFn(uint8_t fn_idx) TA_EXCL(lock_);
    zx_status_t SdioDisableFn(uint8_t fn_idx) TA_EXCL(lock_);
    zx_status_t SdioEnableFnIntr(uint8_t fn_idx) TA_EXCL(lock_);
    zx_status_t SdioDisableFnIntr(uint8_t fn_idx) TA_EXCL(lock_);
    zx_status_t SdioUpdateBlockSize(uint8_t fn_idx, uint16_t blk_sz, bool deflt) TA_EXCL(lock_);
    zx_status_t SdioGetBlockSize(uint8_t fn_idx, uint16_t* out_cur_blk_size);
    zx_status_t SdioDoRwTxn(uint8_t fn_idx, sdio_rw_txn_t* txn) TA_EXCL(lock_);
    zx_status_t SdioDoRwByte(bool write, uint8_t fn_idx, uint32_t addr, uint8_t write_byte,
                             uint8_t* out_read_byte) TA_EXCL(lock_);
    zx_status_t SdioGetInBandIntr(uint8_t fn_idx, zx::interrupt* out_irq) TA_EXCL(lock_);
    zx_status_t SdioIoAbort(uint8_t fn_idx) TA_EXCL(lock_);
    zx_status_t SdioIntrPending(uint8_t fn_idx, bool* out_pending);
    zx_status_t SdioDoVendorControlRwByte(bool write, uint8_t addr, uint8_t write_byte,
                                          uint8_t* out_read_byte);

    void InBandInterruptCallback();

    // Visible for testing.

    // Reads the card common control registers (CCCR) to enumerate the card's capabilities.
    zx_status_t ProcessCccr() TA_REQ(lock_);
    // Reads the card information structure (CIS) for the given function to get the manufacturer
    // identification and function extensions tuples.
    zx_status_t ProcessCis(uint8_t fn_idx) TA_REQ(lock_);
    // Reads the I/O function code and saves it in the given function's struct.
    zx_status_t ProcessFbr(uint8_t fn_idx) TA_REQ(lock_);

    zx_status_t StartSdioIrqThread();
    void StopSdioIrqThread();

protected:
    virtual SdmmcDevice& sdmmc() { return sdmmc_; }

    virtual zx_status_t SdioDoRwByteLocked(bool write, uint8_t fn_idx, uint32_t addr,
                                           uint8_t write_byte, uint8_t* out_read_byte)
        TA_REQ(lock_);

    fbl::Mutex lock_;
    SdioFunction funcs_[SDIO_MAX_FUNCS] TA_GUARDED(lock_);
    sdio_device_hw_info_t hw_info_ TA_GUARDED(lock_);

private:
    struct SdioFuncTuple {
        uint8_t tuple_code;
        uint8_t tuple_body_size;
        uint8_t tuple_body[UINT8_MAX];
    };

    zx_status_t SdioReset() TA_REQ(lock_);
    // Parses a tuple read from the CIS.
    zx_status_t ParseFnTuple(uint8_t fn_idx, const SdioFuncTuple& tup) TA_REQ(lock_);
    // Parses the manufacturer ID tuple and saves it in the given function's struct.
    zx_status_t ParseMfidTuple(uint8_t fn_idx, const SdioFuncTuple& tup) TA_REQ(lock_);
    // Parses the function extensions tuple and saves it in the given function's struct.
    zx_status_t ParseFuncExtTuple(uint8_t fn_idx, const SdioFuncTuple& tup) TA_REQ(lock_);
    // Popluates the given function's struct by calling the methods above. Also enables the
    // function and sets its default block size.
    zx_status_t InitFunc(uint8_t fn_idx) TA_REQ(lock_);

    zx_status_t SwitchFreq(uint32_t new_freq) TA_REQ(lock_);
    zx_status_t TrySwitchHs() TA_REQ(lock_);
    zx_status_t TrySwitchUhs() TA_REQ(lock_);
    zx_status_t Enable4BitBus() TA_REQ(lock_);
    zx_status_t SwitchBusWidth(uint32_t bw) TA_REQ(lock_);

    zx_status_t ReadData16(uint8_t fn_idx, uint32_t addr, uint16_t* word) TA_REQ(lock_);
    zx_status_t WriteData16(uint8_t fn_idx, uint32_t addr, uint16_t word) TA_REQ(lock_);

    zx_status_t SdioEnableFnLocked(uint8_t fn_idx) TA_REQ(lock_);
    zx_status_t SdioUpdateBlockSizeLocked(uint8_t fn_idx, uint16_t blk_sz, bool deflt)
        TA_REQ(lock_);

    int SdioIrqThread();

    thrd_t irq_thread_ = 0;
    sync_completion_t irq_signal_;

    SdmmcDevice sdmmc_ TA_GUARDED(lock_);
    std::atomic<bool> dead_ = false;
    fbl::Array<fbl::RefPtr<SdioFunctionDevice>> devices_;
    zx::interrupt sdio_irqs_[SDIO_MAX_FUNCS];
};

}  // namespace sdmmc
