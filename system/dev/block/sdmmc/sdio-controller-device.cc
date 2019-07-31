// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sdio-controller-device.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/protocol/sdio.h>
#include <ddk/protocol/sdmmc.h>
#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>
#include <hw/sdio.h>
#include <lib/fzl/vmo-mapper.h>
#include <zircon/driver/binding.h>
#include <zircon/process.h>
#include <zircon/threads.h>

namespace {

constexpr uint8_t kCccrVendorAddressMin = 0xf0;

constexpr uint32_t kBcmManufacturerId = 0x02d0;

uint32_t SdioReadTupleBody(const uint8_t* tuple_body, size_t start, size_t numbytes) {
    uint32_t res = 0;

    for (size_t i = start; i < (start + numbytes); i++) {
        res |= tuple_body[i] << ((i - start) * 8);
    }
    return res;
}

inline bool SdioFnIdxValid(uint8_t fn_idx) {
    return (fn_idx < SDIO_MAX_FUNCS);
}

inline bool SdioIsUhsSupported(uint32_t hw_caps) {
    return ((hw_caps & SDIO_CARD_UHS_SDR50) || (hw_caps & SDIO_CARD_UHS_SDR104) ||
            (hw_caps & SDIO_CARD_UHS_DDR50));
}

inline uint8_t GetBits(uint32_t x, uint32_t mask, uint32_t loc) {
    return static_cast<uint8_t>((x & mask) >> loc);
}

inline void UpdateBitsU8(uint8_t* x, uint8_t mask, uint8_t loc, uint8_t val) {
    *x = static_cast<uint8_t>(*x & ~mask);
    *x = static_cast<uint8_t>(*x | ((val << loc) & mask));
}

inline uint8_t GetBitsU8(uint8_t x, uint8_t mask, uint8_t loc) {
    return static_cast<uint8_t>((x & mask) >> loc);
}

}  // namespace

namespace sdmmc {

zx_status_t SdioControllerDevice::Create(zx_device_t* parent, const SdmmcDevice& sdmmc,
                                         fbl::RefPtr<SdioControllerDevice>* out_dev) {
    fbl::AllocChecker ac;
    auto dev = fbl::MakeRefCountedChecked<SdioControllerDevice>(&ac, parent, sdmmc);
    if (!ac.check()) {
        zxlogf(ERROR, "sdmmc: failed to allocate device memory\n");
        return ZX_ERR_NO_MEMORY;
    }

    *out_dev = dev;
    return ZX_OK;
}

zx_status_t SdioControllerDevice::ProbeSdio() {
    fbl::AutoLock lock(&lock_);

    zx_status_t st = SdioReset();

    if ((st = sdmmc().SdmmcGoIdle()) != ZX_OK) {
        zxlogf(ERROR, "sdmmc: SDMMC_GO_IDLE_STATE failed, retcode = %d\n", st);
        return st;
    }

    uint32_t ocr;
    if ((st = sdmmc().SdioSendOpCond(0, &ocr)) != ZX_OK) {
        zxlogf(TRACE, "sdmmc_probe_sdio: SDIO_SEND_OP_COND failed, retcode = %d\n", st);
        return st;
    }
    // Select voltage 3.3 V. Also request for 1.8V. Section 3.2 SDIO spec
    if (ocr & SDIO_SEND_OP_COND_IO_OCR_33V) {
        uint32_t new_ocr = SDIO_SEND_OP_COND_IO_OCR_33V | SDIO_SEND_OP_COND_CMD_S18R;
        if ((st = sdmmc().SdioSendOpCond(new_ocr, &ocr)) != ZX_OK) {
            zxlogf(ERROR, "sdmmc_probe_sdio: SDIO_SEND_OP_COND failed, retcode = %d\n", st);
            return st;
        }
    }
    if (ocr & SDIO_SEND_OP_COND_RESP_MEM_PRESENT) {
        // Combo cards not supported
        zxlogf(ERROR, "sdmmc_probe_sdio: Combo card not supported\n");
        return ZX_ERR_NOT_SUPPORTED;
    }
    sdmmc().SetCurrentVoltage(SDMMC_VOLTAGE_V180);
    hw_info_.num_funcs =
        GetBits(ocr, SDIO_SEND_OP_COND_RESP_NUM_FUNC_MASK, SDIO_SEND_OP_COND_RESP_NUM_FUNC_LOC);
    if ((st = sdmmc().SdSendRelativeAddr(nullptr)) != ZX_OK) {
        zxlogf(ERROR, "sdmcc_probe_sdio: SD_SEND_RELATIVE_ADDR failed, retcode = %d\n", st);
        return st;
    }

    if ((st = sdmmc().MmcSelectCard()) != ZX_OK) {
        zxlogf(ERROR, "sdmmc_probe_sdio: MMC_SELECT_CARD failed, retcode = %d\n", st);
        return st;
    }

    if ((st = ProcessCccr()) != ZX_OK) {
        zxlogf(ERROR, "sdmmc_probe_sdio: Read CCCR failed, retcode = %d\n", st);
        return st;
    }

    // Read CIS to get max block size
    if ((st = ProcessCis(0)) != ZX_OK) {
        zxlogf(ERROR, "sdmmc_probe_sdio: Read CIS failed, retcode = %d\n", st);
        return st;
    }

    if (ocr & SDIO_SEND_OP_COND_RESP_S18A) {
        if ((st = sdmmc().SdSwitchUhsVoltage(ocr)) != ZX_OK) {
            zxlogf(INFO, "Failed to switch voltage to 1.8V\n");
            return st;
        }
    }

    // BCM43458 includes function 0 in its OCR register. This violates the SDIO specification and
    // the assumptions made here. Check the manufacturer ID to account for this quirk.
    if (funcs_[0].hw_info.manufacturer_id != kBcmManufacturerId) {
        hw_info_.num_funcs++;
    }

    // TODO(ravoorir):Re-enable ultra high speed when wifi stack is more stable.
    // if ((st = TrySwitchUhs()) != ZX_OK) {
    //     zxlogf(ERROR, "sdmmc_probe_sdio: Switching to high speed failed, retcode = %d\n", st);
    if ((st = TrySwitchHs()) != ZX_OK) {
        zxlogf(ERROR, "sdmmc_probe_sdio: Switching to high speed failed, retcode = %d\n", st);
        if ((st = SwitchFreq(SDIO_DEFAULT_FREQ)) != ZX_OK) {
            zxlogf(ERROR, "sdmmc_probe_sdio: Switch freq retcode = %d\n", st);
            return st;
        }
    }
    // }

    SdioUpdateBlockSizeLocked(0, 0, true);
    // 0 is the common function. Already initialized
    for (size_t i = 1; i < hw_info_.num_funcs; i++) {
        st = InitFunc(static_cast<uint8_t>(i));
    }

    zxlogf(INFO, "sdmmc_probe_sdio: sdio device initialized successfully\n");
    zxlogf(INFO, "          Manufacturer: 0x%x\n", funcs_[0].hw_info.manufacturer_id);
    zxlogf(INFO, "          Product: 0x%x\n", funcs_[0].hw_info.product_id);
    zxlogf(INFO, "          cccr vsn: 0x%x\n", hw_info_.cccr_vsn);
    zxlogf(INFO, "          SDIO vsn: 0x%x\n", hw_info_.sdio_vsn);
    zxlogf(INFO, "          num funcs: %d\n", hw_info_.num_funcs);
    return ZX_OK;
}

zx_status_t SdioControllerDevice::StartSdioIrqThread() {
    auto thread_func = [](void* ctx) -> int {
        return reinterpret_cast<SdioControllerDevice*>(ctx)->SdioIrqThread();
    };

    int rc = thrd_create_with_name(&irq_thread_, thread_func, this, "sdio-controller-worker");
    return thrd_status_to_zx_status(rc);
}

zx_status_t SdioControllerDevice::AddDevice() {
    fbl::AutoLock lock(&lock_);

    zx_status_t st = StartSdioIrqThread();
    if (st != ZX_OK) {
        return st;
    }

    fbl::AllocChecker ac;
    devices_.reset(new (&ac) fbl::RefPtr<SdioFunctionDevice>[hw_info_.num_funcs - 1],
                   hw_info_.num_funcs - 1);
    if (!ac.check()) {
        zxlogf(ERROR, "sdmmc: failed to allocate device memory\n");
        return ZX_ERR_NO_MEMORY;
    }

    st = DdkAdd("sdmmc-sdio", DEVICE_ADD_NON_BINDABLE);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdmmc: Failed to add sdio device, retcode = %d\n", st);
        return st;
    }

    for (uint32_t i = 0; i < devices_.size(); i++) {
        if ((st = SdioFunctionDevice::Create(zxdev(), this, &devices_[i])) != ZX_OK) {
            if (!dead_) {
                DdkRemove();
            }

            break;
        }
    }

    for (uint32_t i = 0; i < devices_.size(); i++) {
        if ((st = devices_[i]->AddDevice(funcs_[0].hw_info, i + 1)) != ZX_OK) {
            if (!dead_) {
                DdkRemove();
            }

            break;
        }
    }

    return st;
}

void SdioControllerDevice::DdkUnbind() {
    if (dead_) {
        return;
    }

    for (auto device : devices_) {
        if (device) {
            device->DdkRemove();
        }
    }
    devices_.reset();

    dead_ = true;
    DdkRemove();
}

void SdioControllerDevice::StopSdioIrqThread() {
    dead_ = true;

    if (irq_thread_) {
        sync_completion_signal(&irq_signal_);
        thrd_join(irq_thread_, nullptr);
    }
}

void SdioControllerDevice::DdkRelease() {
    StopSdioIrqThread();
    __UNUSED bool dummy = Release();
}

zx_status_t SdioControllerDevice::SdioGetDevHwInfo(sdio_hw_info_t* out_hw_info) {
    fbl::AutoLock lock(&lock_);

    memcpy(&out_hw_info->dev_hw_info, &hw_info_, sizeof(sdio_device_hw_info_t));
    for (size_t i = 0; i < hw_info_.num_funcs; i++) {
        memcpy(&out_hw_info->funcs_hw_info[i], &funcs_[i].hw_info, sizeof(sdio_func_hw_info_t));
    }
    out_hw_info->host_max_transfer_size =
        static_cast<uint32_t>(sdmmc().host_info().max_transfer_size);
    return ZX_OK;
}

zx_status_t SdioControllerDevice::SdioEnableFn(uint8_t fn_idx) {
    fbl::AutoLock lock(&lock_);
    return SdioEnableFnLocked(fn_idx);
}

zx_status_t SdioControllerDevice::SdioEnableFnLocked(uint8_t fn_idx) {
    uint8_t ioex_reg = 0;
    zx_status_t st = ZX_OK;

    if (!SdioFnIdxValid(fn_idx)) {
        return ZX_ERR_INVALID_ARGS;
    }

    SdioFunction& func = funcs_[fn_idx];
    if (func.enabled) {
        return ZX_OK;
    }
    if ((st = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_IOEx_EN_FUNC_ADDR, 0, &ioex_reg)) !=
        ZX_OK) {
        zxlogf(ERROR, "sdio_enable_function: Error enabling func:%d status:%d\n", fn_idx, st);
        return st;
    }

    ioex_reg = static_cast<uint8_t>(ioex_reg | (1 << fn_idx));
    st = SdioDoRwByteLocked(true, 0, SDIO_CIA_CCCR_IOEx_EN_FUNC_ADDR, ioex_reg, nullptr);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio_enable_function: Error enabling func:%d status:%d\n", fn_idx, st);
        return st;
    }
    // wait for the device to enable the func.
    zx::nanosleep(zx::deadline_after(zx::msec(10)));
    if ((st = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_IOEx_EN_FUNC_ADDR, 0, &ioex_reg)) !=
        ZX_OK) {
        zxlogf(ERROR, "sdio_enable_function: Error enabling func:%d status:%d\n", fn_idx, st);
        return st;
    }

    if (!(ioex_reg & (1 << fn_idx))) {
        st = ZX_ERR_IO;
        zxlogf(ERROR, "sdio_enable_function: Failed to enable func %d\n", fn_idx);
        return st;
    }

    func.enabled = true;
    zxlogf(TRACE, "sdio_enable_function: Func %d is enabled\n", fn_idx);
    return st;
}

zx_status_t SdioControllerDevice::SdioDisableFn(uint8_t fn_idx) {
    uint8_t ioex_reg = 0;
    zx_status_t st = ZX_OK;

    if (!SdioFnIdxValid(fn_idx)) {
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::AutoLock lock(&lock_);

    SdioFunction* func = &funcs_[fn_idx];
    if (!func->enabled) {
        zxlogf(ERROR, "sdio_disable_function: Func %d is not enabled\n", fn_idx);
        return ZX_ERR_IO;
    }

    if ((st = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_IOEx_EN_FUNC_ADDR, 0, &ioex_reg)) !=
        ZX_OK) {
        zxlogf(ERROR, "sdio_disable_function: Error reading IOEx reg. func: %d status: %d\n",
               fn_idx, st);
        return st;
    }

    ioex_reg = static_cast<uint8_t>(ioex_reg & ~(1 << fn_idx));
    st = SdioDoRwByteLocked(true, 0, SDIO_CIA_CCCR_IOEx_EN_FUNC_ADDR, ioex_reg, nullptr);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio_disable_function: Error writing IOEx reg. func: %d status:%d\n", fn_idx,
               st);
        return st;
    }

    func->enabled = false;
    zxlogf(TRACE, "sdio_disable_function: Function %d is disabled\n", fn_idx);
    return st;
}

zx_status_t SdioControllerDevice::SdioEnableFnIntr(uint8_t fn_idx) {
    zx_status_t st = ZX_OK;

    if (!SdioFnIdxValid(fn_idx)) {
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::AutoLock lock(&lock_);

    SdioFunction* func = &funcs_[fn_idx];
    if (func->intr_enabled) {
        return ZX_OK;
    }

    uint8_t intr_byte;
    st = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_IEN_INTR_EN_ADDR, 0, &intr_byte);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio_enable_interrupt: Failed to enable interrupt for fn: %d status: %d\n",
               fn_idx, st);
        return st;
    }

    // Enable fn intr
    intr_byte = static_cast<uint8_t>(intr_byte | 1 << fn_idx);
    // Enable master intr
    intr_byte = static_cast<uint8_t>(intr_byte | 1);

    st = SdioDoRwByteLocked(true, 0, SDIO_CIA_CCCR_IEN_INTR_EN_ADDR, intr_byte, nullptr);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio_enable_interrupt: Failed to enable interrupt for fn: %d status: %d\n",
               fn_idx, st);
        return st;
    }

    func->intr_enabled = true;
    zxlogf(TRACE, "sdio_enable_interrupt: Interrupt enabled for fn %d\n", fn_idx);
    return ZX_OK;
}

zx_status_t SdioControllerDevice::SdioDisableFnIntr(uint8_t fn_idx) {
    zx_status_t st = ZX_OK;

    if (!SdioFnIdxValid(fn_idx)) {
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::AutoLock lock(&lock_);

    SdioFunction* func = &funcs_[fn_idx];
    if (!func->intr_enabled) {
        zxlogf(ERROR, "sdio_disable_interrupt: Interrupt is not enabled for %d\n", fn_idx);
        return ZX_ERR_BAD_STATE;
    }

    uint8_t intr_byte;
    st = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_IEN_INTR_EN_ADDR, 0, &intr_byte);
    if (st != ZX_OK) {
        zxlogf(ERROR,
               "sdio_disable_interrupt: Failed reading intr enable reg. func: %d status: %d\n",
               fn_idx, st);
        return st;
    }

    intr_byte = static_cast<uint8_t>(intr_byte & ~(1 << fn_idx));
    if (!(intr_byte & SDIO_ALL_INTR_ENABLED_MASK)) {
        // disable master as well
        intr_byte = 0;
    }

    st = SdioDoRwByteLocked(true, 0, SDIO_CIA_CCCR_IEN_INTR_EN_ADDR, intr_byte, nullptr);
    if (st != ZX_OK) {
        zxlogf(ERROR,
               "sdio_disable_interrupt: Error writing to intr enable reg. func: %d status: %d\n",
               fn_idx, st);
        return st;
    }

    func->intr_enabled = false;
    zxlogf(TRACE, "sdio_enable_interrupt: Interrupt disabled for fn %d\n", fn_idx);
    return ZX_OK;
}

zx_status_t SdioControllerDevice::SdioUpdateBlockSize(uint8_t fn_idx, uint16_t blk_sz, bool deflt) {
    fbl::AutoLock lock(&lock_);
    return SdioUpdateBlockSizeLocked(fn_idx, blk_sz, deflt);
}

zx_status_t SdioControllerDevice::SdioUpdateBlockSizeLocked(uint8_t fn_idx, uint16_t blk_sz,
                                                            bool deflt) {
    SdioFunction* func = &funcs_[fn_idx];
    if (deflt) {
        blk_sz = static_cast<uint16_t>(func->hw_info.max_blk_size);
    }

    if (blk_sz > func->hw_info.max_blk_size) {
        return ZX_ERR_INVALID_ARGS;
    }

    if (func->cur_blk_size == blk_sz) {
        return ZX_OK;
    }

    zx_status_t st =
        WriteData16(0, SDIO_CIA_FBR_BASE_ADDR(fn_idx) + SDIO_CIA_FBR_BLK_SIZE_ADDR, blk_sz);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio_modify_block_size: Error setting blk size.fn: %d blk_sz: %d ret: %d\n",
               fn_idx, blk_sz, st);
        return st;
    }

    func->cur_blk_size = blk_sz;
    return st;
}

zx_status_t SdioControllerDevice::SdioGetBlockSize(uint8_t fn_idx, uint16_t* out_cur_blk_size) {
    fbl::AutoLock lock(&lock_);

    zx_status_t st = ReadData16(0, SDIO_CIA_FBR_BASE_ADDR(fn_idx) + SDIO_CIA_FBR_BLK_SIZE_ADDR,
                                out_cur_blk_size);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio_get_cur_block_size: Failed to get block size for fn: %d ret: %d\n",
               fn_idx, st);
    }
    return st;
}

zx_status_t SdioControllerDevice::SdioDoRwTxn(uint8_t fn_idx, sdio_rw_txn_t* txn) {
    if (!SdioFnIdxValid(fn_idx)) {
        return ZX_ERR_INVALID_ARGS;
    }

    zx_status_t st = ZX_OK;
    uint32_t addr = txn->addr;
    uint32_t data_size = txn->data_size;
    bool use_dma = txn->use_dma;

    fbl::AutoLock lock(&lock_);

    // Single byte reads at some addresses are stuck when using io_rw_extended.
    // Use io_rw_direct whenever possible.
    if (!use_dma && data_size == 1) {
        return SdioDoRwByteLocked(txn->write, fn_idx, addr,
                                  *reinterpret_cast<uint8_t*>(txn->virt_buffer),
                                  reinterpret_cast<uint8_t*>(txn->virt_buffer));
    }

    if ((data_size % 4) != 0) {
        // TODO(ravoorir): This is definitely needed for PIO mode. Astro has
        // a hardware bug about not supporting DMA. We end up doing non-dma
        // transfers on astro.For now restrict the size for dma requests as well.
        zxlogf(ERROR, "sdio_rw_data: data size is not a multiple of 4\n");
        return ZX_ERR_NOT_SUPPORTED;
    }
    bool dma_supported = sdmmc().UseDma();
    uint8_t* buf = use_dma ? nullptr : reinterpret_cast<uint8_t*>(txn->virt_buffer);
    zx_handle_t dma_vmo = use_dma ? txn->dma_vmo : ZX_HANDLE_INVALID;
    uint64_t buf_offset = txn->buf_offset;
    fzl::VmoMapper mapper;

    if (txn->use_dma && !dma_supported) {
        // host does not support dma
        st = mapper.Map(*zx::unowned_vmo(txn->dma_vmo), txn->buf_offset, data_size,
                        ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
        if (st != ZX_OK) {
            zxlogf(TRACE, "sdio_rw_data: vmo map error %d\n", st);
            return ZX_ERR_IO;
        }
        buf = reinterpret_cast<uint8_t*>(mapper.start());
        use_dma = false;
        dma_vmo = ZX_HANDLE_INVALID;
        buf_offset = 0;  // set it to 0 since we mapped starting from offset.
    }

    bool mbs = hw_info_.caps & SDIO_CARD_MULTI_BLOCK;
    uint32_t func_blk_size = funcs_[fn_idx].cur_blk_size;
    uint32_t rem_blocks = (func_blk_size == 0) ? 0 : (data_size / func_blk_size);
    uint32_t data_processed = 0;
    while (rem_blocks > 0) {
        uint32_t num_blocks = 1;
        if (mbs) {
            uint32_t max_host_blocks;
            max_host_blocks = static_cast<uint32_t>(
                use_dma ? (sdmmc().host_info().max_transfer_size / func_blk_size)
                        : (sdmmc().host_info().max_transfer_size_non_dma / func_blk_size));
            // multiblock is supported, determine max number of blocks per cmd
            num_blocks =
                fbl::min(fbl::min(SDIO_IO_RW_EXTD_MAX_BLKS_PER_CMD, max_host_blocks), rem_blocks);
        }
        st = sdmmc().SdioIoRwExtended(hw_info_.caps, txn->write, fn_idx, addr, txn->incr,
                                      num_blocks, func_blk_size, use_dma, buf, dma_vmo,
                                      buf_offset + data_processed);
        if (st != ZX_OK) {
            zxlogf(ERROR, "sdio_rw_data: Error %sing data.func: %d status: %d\n",
                   txn->write ? "writ" : "read", fn_idx, st);
            return st;
        }
        rem_blocks -= num_blocks;
        data_processed += num_blocks * func_blk_size;
        if (txn->incr) {
            addr += num_blocks * func_blk_size;
        }
    }

    if (data_processed < data_size) {
        // process remaining data.
        st = sdmmc().SdioIoRwExtended(hw_info_.caps, txn->write, fn_idx, addr, txn->incr, 1,
                                      data_size - data_processed, use_dma, buf, dma_vmo,
                                      buf_offset + data_processed);
    }

    return st;
}

zx_status_t SdioControllerDevice::SdioDoRwByte(bool write, uint8_t fn_idx, uint32_t addr,
                                               uint8_t write_byte, uint8_t* out_read_byte) {
    fbl::AutoLock lock(&lock_);
    return SdioDoRwByteLocked(write, fn_idx, addr, write_byte, out_read_byte);
}

zx_status_t SdioControllerDevice::SdioDoRwByteLocked(bool write, uint8_t fn_idx, uint32_t addr,
                                                     uint8_t write_byte, uint8_t* out_read_byte) {
    if (!SdioFnIdxValid(fn_idx)) {
        return ZX_ERR_INVALID_ARGS;
    }

    out_read_byte = write ? nullptr : out_read_byte;
    write_byte = write ? write_byte : 0;
    return sdmmc().SdioIoRwDirect(write, fn_idx, addr, write_byte, out_read_byte);
}

zx_status_t SdioControllerDevice::SdioGetInBandIntr(uint8_t fn_idx, zx::interrupt* out_irq) {
    if (!SdioFnIdxValid(fn_idx) || fn_idx == 0) {
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::AutoLock lock(&lock_);

    if (sdio_irqs_[fn_idx].is_valid()) {
        return ZX_ERR_ALREADY_EXISTS;
    }

    zx_status_t st =
        zx::interrupt::create(zx::resource(), 0, ZX_INTERRUPT_VIRTUAL, &sdio_irqs_[fn_idx]);
    if (st != ZX_OK) {
        return st;
    }

    if ((st = sdio_irqs_[fn_idx].duplicate(ZX_RIGHT_SAME_RIGHTS, out_irq)) != ZX_OK) {
        return st;
    }

    return sdmmc().host().RegisterInBandInterrupt(this, &in_band_interrupt_protocol_ops_);
}

void SdioControllerDevice::InBandInterruptCallback() {
    sync_completion_signal(&irq_signal_);
}

int SdioControllerDevice::SdioIrqThread() {
    for (;;) {
        sync_completion_wait(&irq_signal_, ZX_TIME_INFINITE);
        sync_completion_reset(&irq_signal_);

        if (dead_) {
            return thrd_success;
        }

        uint8_t intr_byte;
        zx_status_t st = SdioDoRwByte(false, 0, SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, 0, &intr_byte);
        if (st != ZX_OK) {
            zxlogf(ERROR, "sdio_irq: Failed reading intr pending reg. status: %d\n", st);
            return thrd_error;
        }

        for (uint8_t i = 1; SdioFnIdxValid(i); i++) {
            if ((intr_byte & (1 << i)) && sdio_irqs_[i].is_valid()) {
                sdio_irqs_[i].trigger(0, zx::clock::get_monotonic());
            }
        }
    }

    return thrd_success;
}

zx_status_t SdioControllerDevice::SdioIoAbort(uint8_t fn_idx) {
    if (!SdioFnIdxValid(fn_idx) || fn_idx == 0) {
        return ZX_ERR_INVALID_ARGS;
    }

    return SdioDoRwByte(true, 0, SDIO_CIA_CCCR_ASx_ABORT_SEL_CR_ADDR, fn_idx, nullptr);
}

zx_status_t SdioControllerDevice::SdioIntrPending(uint8_t fn_idx, bool* out_pending) {
    if (!SdioFnIdxValid(fn_idx) || fn_idx == 0) {
        return ZX_ERR_INVALID_ARGS;
    }

    uint8_t intr_byte;
    zx_status_t st = SdioDoRwByte(false, 0, SDIO_CIA_CCCR_INTx_INTR_PEN_ADDR, 0, &intr_byte);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio_intr_pending: Failed reading intr pending reg. status: %d\n", st);
        return st;
    }

    *out_pending = intr_byte & (1 << fn_idx);
    return ZX_OK;
}

zx_status_t SdioControllerDevice::SdioDoVendorControlRwByte(bool write, uint8_t addr,
                                                            uint8_t write_byte,
                                                            uint8_t* out_read_byte) {
    // The vendor area of the CCCR is 0xf0 - 0xff.
    if (addr < kCccrVendorAddressMin) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return SdioDoRwByte(write, 0, addr, write_byte, out_read_byte);
}

zx_status_t SdioControllerDevice::SdioReset() {
    zx_status_t st = ZX_OK;
    uint8_t abort_byte;

    st = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_ASx_ABORT_SEL_CR_ADDR, 0, &abort_byte);
    if (st != ZX_OK) {
        abort_byte = SDIO_CIA_CCCR_ASx_ABORT_SOFT_RESET;
    } else {
        abort_byte |= SDIO_CIA_CCCR_ASx_ABORT_SOFT_RESET;
    }
    return SdioDoRwByteLocked(true, 0, SDIO_CIA_CCCR_ASx_ABORT_SEL_CR_ADDR, abort_byte, nullptr);
}

zx_status_t SdioControllerDevice::ProcessCccr() {
    uint8_t cccr_vsn, sdio_vsn, vsn_info, bus_speed, card_caps, uhs_caps, drv_strength;

    // version info
    zx_status_t status =
        SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_CCCR_SDIO_VER_ADDR, 0, &vsn_info);
    if (status != ZX_OK) {
        zxlogf(ERROR, "sdio_process_cccr: Error reading CCCR reg: %d\n", status);
        return status;
    }
    cccr_vsn = GetBits(vsn_info, SDIO_CIA_CCCR_CCCR_VER_MASK, SDIO_CIA_CCCR_CCCR_VER_LOC);
    sdio_vsn = GetBits(vsn_info, SDIO_CIA_CCCR_SDIO_VER_MASK, SDIO_CIA_CCCR_SDIO_VER_LOC);
    if ((cccr_vsn < SDIO_CCCR_FORMAT_VER_3) || (sdio_vsn < SDIO_SDIO_VER_3)) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    hw_info_.cccr_vsn = cccr_vsn;
    hw_info_.sdio_vsn = sdio_vsn;

    // card capabilities
    status = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_CARD_CAPS_ADDR, 0, &card_caps);
    if (status != ZX_OK) {
        zxlogf(ERROR, "sdio_process_cccr: Error reading CAPS reg: %d\n", status);
        return status;
    }
    hw_info_.caps = 0;
    if (card_caps & SDIO_CIA_CCCR_CARD_CAP_SMB) {
        hw_info_.caps |= SDIO_CARD_MULTI_BLOCK;
    }
    if (card_caps & SDIO_CIA_CCCR_CARD_CAP_LSC) {
        hw_info_.caps |= SDIO_CARD_LOW_SPEED;
    }
    if (card_caps & SDIO_CIA_CCCR_CARD_CAP_4BLS) {
        hw_info_.caps |= SDIO_CARD_FOUR_BIT_BUS;
    }

    // speed
    status = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_BUS_SPEED_SEL_ADDR, 0, &bus_speed);
    if (status != ZX_OK) {
        zxlogf(ERROR, "sdio_process_cccr: Error reading SPEED reg: %d\n", status);
        return status;
    }
    if (bus_speed & SDIO_CIA_CCCR_BUS_SPEED_SEL_SHS) {
        hw_info_.caps |= SDIO_CARD_HIGH_SPEED;
    }

    // Is UHS supported?
    status = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_UHS_SUPPORT_ADDR, 0, &uhs_caps);
    if (status != ZX_OK) {
        zxlogf(ERROR, "sdio_process_cccr: Error reading SPEED reg: %d\n", status);
        return status;
    }
    if (uhs_caps & SDIO_CIA_CCCR_UHS_SDR50) {
        hw_info_.caps |= SDIO_CARD_UHS_SDR50;
    }
    if (uhs_caps & SDIO_CIA_CCCR_UHS_SDR104) {
        hw_info_.caps |= SDIO_CARD_UHS_SDR104;
    }
    if (uhs_caps & SDIO_CIA_CCCR_UHS_DDR50) {
        hw_info_.caps |= SDIO_CARD_UHS_DDR50;
    }

    // drv_strength
    status = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_DRV_STRENGTH_ADDR, 0, &drv_strength);
    if (status != ZX_OK) {
        zxlogf(ERROR, "sdio_process_cccr: Error reading SPEED reg: %d\n", status);
        return status;
    }
    if (drv_strength & SDIO_CIA_CCCR_DRV_STRENGTH_SDTA) {
        hw_info_.caps |= SDIO_CARD_TYPE_A;
    }
    if (drv_strength & SDIO_CIA_CCCR_DRV_STRENGTH_SDTB) {
        hw_info_.caps |= SDIO_CARD_TYPE_B;
    }
    if (drv_strength & SDIO_CIA_CCCR_DRV_STRENGTH_SDTD) {
        hw_info_.caps |= SDIO_CARD_TYPE_D;
    }
    return status;
}

zx_status_t SdioControllerDevice::ProcessCis(uint8_t fn_idx) {
    zx_status_t st = ZX_OK;

    if (fn_idx >= SDIO_MAX_FUNCS) {
        return ZX_ERR_INVALID_ARGS;
    }
    uint32_t cis_ptr = 0;
    for (size_t i = 0; i < SDIO_CIS_ADDRESS_SIZE; i++) {
        uint8_t addr;
        st = SdioDoRwByteLocked(
            false, 0,
            static_cast<uint32_t>(SDIO_CIA_FBR_BASE_ADDR(fn_idx) + SDIO_CIA_FBR_CIS_ADDR + i), 0,
            &addr);
        if (st != ZX_OK) {
            zxlogf(ERROR, "sdio: Error reading CIS of CCCR reg: %d\n", st);
            return st;
        }
        cis_ptr |= addr << (i * 8);
    }
    if (!cis_ptr) {
        zxlogf(ERROR, "sdio: CIS address is invalid\n");
        return ZX_ERR_IO;
    }

    while (true) {
        uint8_t tuple_code, tuple_link;
        SdioFuncTuple cur_tup;
        st = SdioDoRwByteLocked(false, 0, cis_ptr + SDIO_CIS_TPL_FRMT_TCODE_OFF, 0, &tuple_code);
        if (st != ZX_OK) {
            zxlogf(ERROR, "sdio: Error reading tuple code for fn %d\n", fn_idx);
            break;
        }
        // Ignore null tuples
        if (tuple_code == SDIO_CIS_TPL_CODE_NULL) {
            cis_ptr++;
            continue;
        }
        if (tuple_code == SDIO_CIS_TPL_CODE_END) {
            break;
        }
        st = SdioDoRwByteLocked(false, 0, cis_ptr + SDIO_CIS_TPL_FRMT_TLINK_OFF, 0, &tuple_link);
        if (st != ZX_OK) {
            zxlogf(ERROR, "sdio: Error reading tuple size for fn %d\n", fn_idx);
            break;
        }
        if (tuple_link == SDIO_CIS_TPL_LINK_END) {
            break;
        }

        cur_tup.tuple_code = tuple_code;
        cur_tup.tuple_body_size = tuple_link;

        cis_ptr += SDIO_CIS_TPL_FRMT_TBODY_OFF;
        for (size_t i = 0; i < tuple_link; i++, cis_ptr++) {
            st = SdioDoRwByteLocked(false, 0, cis_ptr, 0, &cur_tup.tuple_body[i]);
            if (st != ZX_OK) {
                zxlogf(ERROR, "sdio: Error reading tuple body for fn %d\n", fn_idx);
                return st;
            }
        }
        ParseFnTuple(fn_idx, cur_tup);
    }
    return st;
}

zx_status_t SdioControllerDevice::ParseFnTuple(uint8_t fn_idx, const SdioFuncTuple& tup) {
    zx_status_t st = ZX_OK;
    switch (tup.tuple_code) {
    case SDIO_CIS_TPL_CODE_MANFID:
        st = ParseMfidTuple(fn_idx, tup);
        break;
    case SDIO_CIS_TPL_CODE_FUNCE:
        st = ParseFuncExtTuple(fn_idx, tup);
        break;
    default:
        break;
    }
    return st;
}

zx_status_t SdioControllerDevice::ParseFuncExtTuple(uint8_t fn_idx, const SdioFuncTuple& tup) {
    SdioFunction* func = &funcs_[fn_idx];
    if (fn_idx == 0) {
        if (tup.tuple_body_size < SDIO_CIS_TPL_FUNC0_FUNCE_MIN_BDY_SZ) {
            return ZX_ERR_IO;
        }
        func->hw_info.max_blk_size =
            SdioReadTupleBody(tup.tuple_body, SDIO_CIS_TPL_FUNCE_FUNC0_MAX_BLK_SIZE_LOC, 2);
        func->hw_info.max_blk_size = static_cast<uint32_t>(
            fbl::min<uint64_t>(sdmmc().host_info().max_transfer_size, func->hw_info.max_blk_size));
        uint8_t speed_val = GetBitsU8(tup.tuple_body[3], SDIO_CIS_TPL_FUNCE_MAX_TRAN_SPEED_VAL_MASK,
                                      SDIO_CIS_TPL_FUNCE_MAX_TRAN_SPEED_VAL_LOC);
        uint8_t speed_unit =
            GetBitsU8(tup.tuple_body[3], SDIO_CIS_TPL_FUNCE_MAX_TRAN_SPEED_UNIT_MASK,
                      SDIO_CIS_TPL_FUNCE_MAX_TRAN_SPEED_UNIT_LOC);
        func->hw_info.max_tran_speed = sdio_cis_tpl_funce_tran_speed_val[speed_val] *
                                       sdio_cis_tpl_funce_tran_speed_unit[speed_unit];
        return ZX_OK;
    }

    if (tup.tuple_body_size < SDIO_CIS_TPL_FUNCx_FUNCE_MIN_BDY_SZ) {
        zxlogf(ERROR, "sdio_parse_func_ext: Invalid body size: %d for func_ext tuple\n",
               tup.tuple_body_size);
        return ZX_ERR_IO;
    }
    func->hw_info.max_blk_size =
        SdioReadTupleBody(tup.tuple_body, SDIO_CIS_TPL_FUNCE_FUNCx_MAX_BLK_SIZE_LOC, 2);
    return ZX_OK;
}

zx_status_t SdioControllerDevice::ParseMfidTuple(uint8_t fn_idx, const SdioFuncTuple& tup) {
    if (tup.tuple_body_size < SDIO_CIS_TPL_MANFID_MIN_BDY_SZ) {
        return ZX_ERR_IO;
    }
    SdioFunction* func = &funcs_[fn_idx];
    func->hw_info.manufacturer_id = SdioReadTupleBody(tup.tuple_body, 0, 2);
    func->hw_info.product_id = SdioReadTupleBody(tup.tuple_body, 2, 2);
    return ZX_OK;
}

zx_status_t SdioControllerDevice::ProcessFbr(uint8_t fn_idx) {
    zx_status_t st = ZX_OK;
    uint8_t fbr, fn_intf_code;

    SdioFunction* func = &funcs_[fn_idx];
    if ((st = SdioDoRwByteLocked(false, 0,
                                 SDIO_CIA_FBR_BASE_ADDR(fn_idx) + SDIO_CIA_FBR_STD_IF_CODE_ADDR, 0,
                                 &fbr)) != ZX_OK) {
        zxlogf(ERROR, "sdio: Error reading intf code: %d\n", st);
        return st;
    }
    fn_intf_code = GetBitsU8(fbr, SDIO_CIA_FBR_STD_IF_CODE_MASK, SDIO_CIA_FBR_STD_IF_CODE_LOC);
    if (fn_intf_code == SDIO_CIA_FBR_STD_IF_CODE_MASK) {
        // fn_code > 0Eh
        if ((st = SdioDoRwByteLocked(
                 false, 0, SDIO_CIA_FBR_BASE_ADDR(fn_idx) + SDIO_CIA_FBR_STD_IF_CODE_EXT_ADDR, 0,
                 &fn_intf_code)) != ZX_OK) {
            zxlogf(ERROR, "sdio: Error while reading the extended intf code %d\n", st);
            return st;
        }
    }
    func->hw_info.fn_intf_code = fn_intf_code;
    return ZX_OK;
}

zx_status_t SdioControllerDevice::InitFunc(uint8_t fn_idx) {
    zx_status_t st = ZX_OK;

    if ((st = ProcessFbr(fn_idx)) != ZX_OK) {
        return st;
    }

    if ((st = ProcessCis(fn_idx)) != ZX_OK) {
        return st;
    }

    // Enable all func for now. Should move to wifi driver ?
    if ((st = SdioEnableFnLocked(fn_idx)) != ZX_OK) {
        return st;
    }

    // Set default block size
    if ((st = SdioUpdateBlockSizeLocked(fn_idx, 0, true)) != ZX_OK) {
        return st;
    }

    return st;
}

zx_status_t SdioControllerDevice::SwitchFreq(uint32_t new_freq) {
    zx_status_t st;
    if ((st = sdmmc().host().SetBusFreq(new_freq)) != ZX_OK) {
        zxlogf(ERROR, "sdio: Error while switching host bus frequency, retcode = %d\n", st);
        return st;
    }
    return ZX_OK;
}

zx_status_t SdioControllerDevice::TrySwitchHs() {
    if (!(hw_info_.caps & SDIO_CARD_HIGH_SPEED)) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_status_t st = ZX_OK;
    uint8_t speed = 0;

    if (!(hw_info_.caps & SDIO_CARD_HIGH_SPEED)) {
        zxlogf(ERROR, "sdio: High speed not supported, retcode = %d\n", st);
        return ZX_ERR_NOT_SUPPORTED;
    }
    st = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_BUS_SPEED_SEL_ADDR, 0, &speed);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio: Error while reading CCCR reg, retcode = %d\n", st);
        return st;
    }
    UpdateBitsU8(&speed, SDIO_CIA_CCCR_BUS_SPEED_BSS_MASK, SDIO_CIA_CCCR_BUS_SPEED_BSS_LOC,
                 SDIO_BUS_SPEED_EN_HS);
    st = SdioDoRwByteLocked(true, 0, SDIO_CIA_CCCR_BUS_SPEED_SEL_ADDR, speed, nullptr);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio: Error while writing to CCCR reg, retcode = %d\n", st);
        return st;
    }
    // Switch the host timing
    if ((st = sdmmc().host().SetTiming(SDMMC_TIMING_HS)) != ZX_OK) {
        zxlogf(ERROR, "sdio: failed to switch to hs timing on host : %d\n", st);
        return st;
    }

    if ((st = SwitchFreq(SDIO_HS_MAX_FREQ)) != ZX_OK) {
        zxlogf(ERROR, "sdio: failed to switch to hs timing on host : %d\n", st);
        return st;
    }

    if ((st = SwitchBusWidth(SDIO_BW_4BIT)) != ZX_OK) {
        zxlogf(ERROR, "sdmmc_probe_sdio: Swtiching to 4-bit bus width failed, retcode = %d\n", st);
        return st;
    }
    return ZX_OK;
}

zx_status_t SdioControllerDevice::TrySwitchUhs() {
    if (!SdioIsUhsSupported(hw_info_.caps)) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_status_t st = ZX_OK;
    if ((st = SwitchBusWidth(SDIO_BW_4BIT)) != ZX_OK) {
        zxlogf(ERROR, "sdmmc_probe_sdio: Swtiching to 4-bit bus width failed, retcode = %d\n", st);
        return st;
    }

    uint8_t speed = 0;
    uint32_t hw_caps = hw_info_.caps;

    uint32_t new_freq = SDIO_DEFAULT_FREQ;
    uint8_t select_speed = SDIO_BUS_SPEED_SDR50;
    sdmmc_timing_t timing = SDMMC_TIMING_SDR50;

    st = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_BUS_SPEED_SEL_ADDR, 0, &speed);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio: Error while reading CCCR reg, retcode = %d\n", st);
        return st;
    }

    if (hw_caps & SDIO_CARD_UHS_SDR104) {
        select_speed = SDIO_BUS_SPEED_SDR104;
        timing = SDMMC_TIMING_SDR104;
        new_freq = SDIO_UHS_SDR104_MAX_FREQ;
    } else if (hw_caps & SDIO_CARD_UHS_SDR50) {
        select_speed = SDIO_BUS_SPEED_SDR50;
        timing = SDMMC_TIMING_SDR50;
        new_freq = SDIO_UHS_SDR50_MAX_FREQ;
    } else if (hw_caps & SDIO_CARD_UHS_DDR50) {
        select_speed = SDIO_BUS_SPEED_DDR50;
        timing = SDMMC_TIMING_DDR50;
        new_freq = SDIO_UHS_DDR50_MAX_FREQ;
    } else {
        return ZX_ERR_NOT_SUPPORTED;
    }

    UpdateBitsU8(&speed, SDIO_CIA_CCCR_BUS_SPEED_BSS_MASK, SDIO_CIA_CCCR_BUS_SPEED_BSS_LOC,
                 select_speed);

    st = SdioDoRwByteLocked(true, 0, SDIO_CIA_CCCR_BUS_SPEED_SEL_ADDR, speed, nullptr);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio: Error while writing to CCCR reg, retcode = %d\n", st);
        return st;
    }
    // Switch the host timing
    if ((st = sdmmc().host().SetTiming(timing)) != ZX_OK) {
        zxlogf(ERROR, "sdio: failed to switch to hs timing on host : %d\n", st);
        return st;
    }

    if ((st = SwitchFreq(new_freq)) != ZX_OK) {
        zxlogf(ERROR, "sdio: failed to switch to hs timing on host : %d\n", st);
        return st;
    }

    if ((hw_caps & SDIO_CARD_UHS_SDR104) || (hw_caps & SDIO_CARD_UHS_SDR50)) {
        st = sdmmc().host().PerformTuning(SD_SEND_TUNING_BLOCK);
        if (st != ZX_OK) {
            zxlogf(ERROR, "mmc: tuning failed %d\n", st);
            return st;
        }
    }
    return ZX_OK;
}

zx_status_t SdioControllerDevice::Enable4BitBus() {
    zx_status_t st = ZX_OK;
    if ((hw_info_.caps & SDIO_CARD_LOW_SPEED) && !(hw_info_.caps & SDIO_CARD_FOUR_BIT_BUS)) {
        zxlogf(ERROR, "sdio: Switching to 4-bit bus unsupported\n");
        return ZX_ERR_NOT_SUPPORTED;
    }
    uint8_t bus_ctrl_reg;
    if ((st = SdioDoRwByteLocked(false, 0, SDIO_CIA_CCCR_BUS_INTF_CTRL_ADDR, 0, &bus_ctrl_reg)) !=
        ZX_OK) {
        zxlogf(INFO, "sdio: Error reading the current bus width\n");
        return st;
    }
    UpdateBitsU8(&bus_ctrl_reg, SDIO_CIA_CCCR_INTF_CTRL_BW_MASK, SDIO_CIA_CCCR_INTF_CTRL_BW_LOC,
                 SDIO_BW_4BIT);
    if ((st = SdioDoRwByteLocked(true, 0, SDIO_CIA_CCCR_BUS_INTF_CTRL_ADDR, bus_ctrl_reg,
                                 nullptr)) != ZX_OK) {
        zxlogf(ERROR, "sdio: Error while switching the bus width\n");
        return st;
    }
    if ((st = sdmmc().host().SetBusWidth(SDMMC_BUS_WIDTH_FOUR)) != ZX_OK) {
        zxlogf(ERROR, "sdio: failed to switch the host bus width to %d, retcode = %d\n",
               SDMMC_BUS_WIDTH_FOUR, st);
        return ZX_ERR_INTERNAL;
    }

    return ZX_OK;
}

zx_status_t SdioControllerDevice::SwitchBusWidth(uint32_t bw) {
    zx_status_t st = ZX_OK;
    if (bw != SDIO_BW_1BIT && bw != SDIO_BW_4BIT) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (bw == SDIO_BW_4BIT) {
        if ((st = Enable4BitBus()) != ZX_OK) {
            return st;
        }
    }
    return ZX_OK;
}

zx_status_t SdioControllerDevice::ReadData16(uint8_t fn_idx, uint32_t addr, uint16_t* word) {
    uint8_t byte1 = 0, byte2 = 0;
    zx_status_t st = SdioDoRwByteLocked(false, 0, addr, 0, &byte1);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio_read_data16: Error reading from addr:0x%x, retcode: %d\n", addr, st);
        return st;
    }

    st = SdioDoRwByteLocked(false, 0, addr + 1, 0, &byte2);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio_read_data16: Error reading from addr:0x%x, retcode: %d\n", addr + 1,
               st);
        return st;
    }

    *word = static_cast<uint16_t>(byte2 << 8 | byte1);
    return ZX_OK;
}

zx_status_t SdioControllerDevice::WriteData16(uint8_t fn_idx, uint32_t addr, uint16_t word) {
    zx_status_t st = SdioDoRwByteLocked(true, 0, addr, static_cast<uint8_t>(word & 0xff), nullptr);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio_write_data16: Error writing to addr:0x%x, retcode: %d\n", addr, st);
        return st;
    }

    st = SdioDoRwByteLocked(true, 0, addr + 1, static_cast<uint8_t>((word >> 8) & 0xff), nullptr);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdio_write_data16: Error writing to addr:0x%x, retcode: %d\n", addr + 1, st);
        return st;
    }

    return ZX_OK;
}

}  // namespace sdmmc
