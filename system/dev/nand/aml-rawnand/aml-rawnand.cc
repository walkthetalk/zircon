// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-rawnand.h"
#include <assert.h>
#include <fbl/algorithm.h>

namespace amlrawnand {

static constexpr uint32_t NAND_BUSWIDTH_16 = 0x00000002;

// In the case where user_mode == 2 (2 OOB bytes per ECC page),
// the controller adds one of these structs *per* ECC page in
// the info_buf.
struct __attribute__((packed)) AmlInfoFormat {
    uint16_t info_bytes;
    uint8_t zero_bits; /* bit0~5 is valid */
    struct ecc_sta {
        uint8_t eccerr_cnt : 6;
        uint8_t notused : 1;
        uint8_t completed : 1;
    } ecc;
    uint32_t reserved;
};

static_assert(sizeof(struct AmlInfoFormat) == 8,
              "sizeof(struct AmlInfoFormat) must be exactly 8 bytes");

struct NandSetup {
    union {
        uint32_t d32;
        struct {
            unsigned cmd : 22;
            unsigned large_page : 1;
            unsigned no_rb : 1;
            unsigned a2 : 1;
            unsigned reserved25 : 1;
            unsigned page_list : 1;
            unsigned sync_mode : 2;
            unsigned size : 2;
            unsigned active : 1;
        } b;
    } cfg;
    uint16_t id;
    uint16_t max;
};

struct NandCmd {
    uint8_t type;
    uint8_t val;
};

struct ExtInfo {
    uint32_t read_info;
    uint32_t new_type;
    uint32_t page_per_blk;
    uint32_t xlc;
    uint32_t ce_mask;
    uint32_t boot_num;
    uint32_t each_boot_pages;
    uint32_t bbt_occupy_pages;
    uint32_t bbt_start_block;
};

struct NandPage0 {
    NandSetup nand_setup;
    unsigned char page_list[16];
    NandCmd retry_usr[32];
    ExtInfo ext_info;
};

// Controller ECC, OOB, RAND parameters.
struct AmlControllerParams {
    int ecc_strength; // # of ECC bits per ECC page.
    int user_mode;    // OOB bytes every ECC page or per block ?
    int rand_mode;    // Randomize ?
    int bch_mode;
};

AmlControllerParams AmlParams = {
    8, // Overwritten using BCH setting from page0.
    2,
    // The 2 following values are overwritten by page0 contents.
    1,                // rand-mode is 1 for page0.
    AML_ECC_BCH60_1K, // This is the BCH setting for page0.
};

void AmlRawNand::NandctrlSetCfg(uint32_t val) {
    mmio_nandreg_.Write32(val, P_NAND_CFG);
}

void AmlRawNand::NandctrlSetTimingAsync(int bus_tim, int bus_cyc) {
    static constexpr uint32_t lenmask = (static_cast<uint32_t>(1) << 12) - 1;
    uint32_t value = mmio_nandreg_.Read32(P_NAND_CFG);

    value &= ~lenmask;
    value |= (((bus_cyc & 31) | ((bus_tim & 31) << 5) | (0 << 10)) & lenmask);
    mmio_nandreg_.Write32(value, P_NAND_CFG);
}

void AmlRawNand::NandctrlSendCmd(uint32_t cmd) {
    mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

static const char* AmlEccString(uint32_t ecc_mode) {
    const char* s;

    switch (ecc_mode) {
    case AML_ECC_BCH8:
        s = "AML_ECC_BCH8";
        break;
    case AML_ECC_BCH8_1K:
        s = "AML_ECC_BCH8_1K";
        break;
    case AML_ECC_BCH24_1K:
        s = "AML_ECC_BCH24_1K";
        break;
    case AML_ECC_BCH30_1K:
        s = "AML_ECC_BCH30_1K";
        break;
    case AML_ECC_BCH40_1K:
        s = "AML_ECC_BCH40_1K";
        break;
    case AML_ECC_BCH50_1K:
        s = "AML_ECC_BCH50_1K";
        break;
    case AML_ECC_BCH60_1K:
        s = "AML_ECC_BCH60_1K";
        break;
    default:
        s = "BAD ECC Algorithm";
        break;
    }
    return s;
}

static uint32_t AmlGetEccPageSize(uint32_t ecc_mode) {
    uint32_t ecc_page;

    switch (ecc_mode) {
    case AML_ECC_BCH8:
        ecc_page = 512;
        break;
    case AML_ECC_BCH8_1K:
    case AML_ECC_BCH24_1K:
    case AML_ECC_BCH30_1K:
    case AML_ECC_BCH40_1K:
    case AML_ECC_BCH50_1K:
    case AML_ECC_BCH60_1K:
        ecc_page = 1024;
        break;
    default:
        ecc_page = 0;
        break;
    }
    return ecc_page;
}

static int AmlGetEccStrength(uint32_t ecc_mode) {
    int ecc_strength;

    switch (ecc_mode) {
    case AML_ECC_BCH8:
    case AML_ECC_BCH8_1K:
        ecc_strength = 8;
        break;
    case AML_ECC_BCH24_1K:
        ecc_strength = 24;
        break;
    case AML_ECC_BCH30_1K:
        ecc_strength = 30;
        break;
    case AML_ECC_BCH40_1K:
        ecc_strength = 40;
        break;
    case AML_ECC_BCH50_1K:
        ecc_strength = 50;
        break;
    case AML_ECC_BCH60_1K:
        ecc_strength = 60;
        break;
    default:
        ecc_strength = -1;
        break;
    }
    return ecc_strength;
}

void AmlRawNand::AmlCmdIdle(uint32_t time) {
    uint32_t cmd = chip_select_ | AML_CMD_IDLE | (time & 0x3ff);
    mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

zx_status_t AmlRawNand::AmlWaitCmdFinish(unsigned int timeout_ms) {
    zx_status_t ret = ZX_OK;
    uint64_t total_time = 0;

    // Wait until cmd fifo is empty.
    while (true) {
        uint32_t cmd_size = mmio_nandreg_.Read32(P_NAND_CMD);
        uint32_t numcmds = (cmd_size >> 22) & 0x1f;
        if (numcmds == 0)
            break;
        usleep(10);
        total_time += 10;
        if (total_time > (timeout_ms * 1000)) {
            ret = ZX_ERR_TIMED_OUT;
            break;
        }
    }
    if (ret == ZX_ERR_TIMED_OUT)
        zxlogf(ERROR, "wait for empty cmd FIFO time out\n");
    return ret;
}

void AmlRawNand::AmlCmdSeed(uint32_t seed) {
    uint32_t cmd = AML_CMD_SEED | (0xc2 + (seed & 0x7fff));
    mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

void AmlRawNand::AmlCmdN2M(uint32_t ecc_pages, uint32_t ecc_pagesize) {
    uint32_t cmd = CMDRWGEN(AML_CMD_N2M,
                   controller_params_.rand_mode,
                   controller_params_.bch_mode,
                   0,
                   ecc_pagesize,
                   ecc_pages);
    mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

void AmlRawNand::AmlCmdM2N(uint32_t ecc_pages, uint32_t ecc_pagesize) {
    uint32_t cmd = CMDRWGEN(AML_CMD_M2N,
                   controller_params_.rand_mode,
                   controller_params_.bch_mode,
                   0, ecc_pagesize,
                   ecc_pages);
    mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

void AmlRawNand::AmlCmdM2NPage0() {
    // If we ever decide to write to Page0.
}

void AmlRawNand::AmlCmdN2MPage0() {
    // For page0 reads, we must use AML_ECC_BCH60_1K,
    // and rand-mode == 1.
    uint32_t cmd = CMDRWGEN(AML_CMD_N2M,
                                1,                // Force rand_mode.
                                AML_ECC_BCH60_1K, // Force bch_mode.
                                1,                // shortm == 1.
                                384 >> 3,
                                1);
    mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

zx_status_t AmlRawNand::AmlWaitDmaFinish() {
    AmlCmdIdle(0);
    AmlCmdIdle(0);
    // This timeout was 1048 seconds. Make this 1 second, similar
    // to other codepaths where we wait for the cmd fifo to drain.
    return AmlWaitCmdFinish(CMD_FINISH_TIMEOUT_MS);
}

void* AmlRawNand::AmlInfoPtr(int i) {
    auto p = reinterpret_cast<struct AmlInfoFormat*>(info_buf_);
    return &p[i];
}

// In the case where user_mode == 2, info_buf contains one nfc_info_format
// struct per ECC page on completion of a read. This 8 byte structure has
// the 2 OOB bytes and ECC/error status.
zx_status_t AmlRawNand::AmlGetOOBByte(uint8_t* oob_buf) {
    struct AmlInfoFormat* info;
    int count = 0;
    uint32_t ecc_pagesize, ecc_pages;

    ecc_pagesize = AmlGetEccPageSize(controller_params_.bch_mode);
    ecc_pages = writesize_ / ecc_pagesize;
    // user_mode is 2 in our case - 2 bytes of OOB for every ECC page.
    if (controller_params_.user_mode != 2)
        return ZX_ERR_NOT_SUPPORTED;
    for (uint32_t i = 0;
         i < ecc_pages;
         i++) {
        info = reinterpret_cast<struct AmlInfoFormat*>(AmlInfoPtr(i));
        oob_buf[count++] = static_cast<uint8_t>(info->info_bytes & 0xff);
        oob_buf[count++] = static_cast<uint8_t>((info->info_bytes >> 8) & 0xff);
    }
    return ZX_OK;
}

zx_status_t AmlRawNand::AmlSetOOBByte(const uint8_t* oob_buf,
                                      uint32_t ecc_pages)
{
    struct AmlInfoFormat* info;
    int count = 0;

    // user_mode is 2 in our case - 2 bytes of OOB for every ECC page.
    if (controller_params_.user_mode != 2)
        return ZX_ERR_NOT_SUPPORTED;
    for (uint32_t i = 0; i < ecc_pages; i++) {
        info = reinterpret_cast<struct AmlInfoFormat*>(AmlInfoPtr(i));
        info->info_bytes = static_cast<uint16_t>(oob_buf[count] | (oob_buf[count + 1] << 8));
        count += 2;
    }
    return ZX_OK;
}

zx_status_t AmlRawNand::AmlGetECCCorrections(int ecc_pages, uint32_t nand_page,
                                             uint32_t* ecc_corrected) {
    struct AmlInfoFormat* info;
    int bitflips = 0;
    uint8_t zero_bits;

    for (int i = 0; i < ecc_pages; i++) {
        info = reinterpret_cast<struct AmlInfoFormat*>(AmlInfoPtr(i));
        if (info->ecc.eccerr_cnt == AML_ECC_UNCORRECTABLE_CNT) {
            if (!controller_params_.rand_mode) {
                zxlogf(ERROR, "%s: ECC failure (non-randomized)@%u\n", __func__, nand_page);
                stats.failed++;
                return ZX_ERR_IO;
            }
            // Why are we checking for zero_bits here ?
            // To deal with blank NAND pages. A blank page is entirely 0xff.
            // When read with scrambler, the page will be ECC uncorrectable,
            // In theory, if there is a single zero-bit in the page, then that
            // page is not a blank page. But in practice, even fresh NAND chips
            // report a few errors on the read of a page (including blank pages)
            // so we make allowance for a few bitflips. The threshold against
            // which we test the zero-bits is one under which we can correct
            // the bitflips when the page is written to. One option is to set
            // this threshold to be exactly the ECC strength (this is aggressive).
            // TODO(srmohan): What should the correct threshold be ? We could
            // conservatively set this to a small value, or we could have this
            // depend on the quality of the NAND, the wear of the NAND etc.
            zero_bits = info->zero_bits & AML_ECC_UNCORRECTABLE_CNT;
            if (zero_bits >= controller_params_.ecc_strength) {
                zxlogf(ERROR, "%s: ECC failure (randomized)@%u zero_bits=%u\n",
                       __func__, nand_page, zero_bits);
                stats.failed++;
                return ZX_ERR_IO;
            }
            zxlogf(INFO, "%s: Blank Page@%u\n", __func__, nand_page);
            continue;
        }
        stats.ecc_corrected += info->ecc.eccerr_cnt;
        bitflips = fbl::max(static_cast<uint8_t>(bitflips),
                            static_cast<uint8_t>(info->ecc.eccerr_cnt));
    }
    *ecc_corrected = bitflips;
    return ZX_OK;
}

zx_status_t AmlRawNand::AmlCheckECCPages(int ecc_pages) {
    struct AmlInfoFormat* info;

    for (int i = 0; i < ecc_pages; i++) {
        info = reinterpret_cast<struct AmlInfoFormat*>(AmlInfoPtr(i));
        if (info->ecc.completed == 0)
            return ZX_ERR_IO;
    }
    return ZX_OK;
}

zx_status_t AmlRawNand::AmlQueueRB() {
    uint32_t cmd;
    zx_status_t status;

    sync_completion_reset(&req_completion_);
    mmio_nandreg_.SetBits32((1 << 21), P_NAND_CFG);
    AmlCmdIdle(NAND_TWB_TIME_CYCLE);
    cmd = chip_select_ | AML_CMD_CLE | (NAND_CMD_STATUS & 0xff);
    mmio_nandreg_.Write32(cmd, P_NAND_CMD);
    AmlCmdIdle(NAND_TWB_TIME_CYCLE);
    cmd = AML_CMD_RB | AML_CMD_IO6 | (1 << 16) | (0x18 & 0x1f);
    mmio_nandreg_.Write32(cmd, P_NAND_CMD);
    AmlCmdIdle(2);
    status = sync_completion_wait(&req_completion_,
                                  ZX_SEC(1));
    if (status == ZX_ERR_TIMED_OUT) {
        zxlogf(ERROR, "%s: Request timed out, not woken up from irq\n",
               __func__);
    }
    return status;
}

void AmlRawNand::AmlCmdCtrl(int32_t cmd, uint32_t ctrl) {
    if (cmd == NAND_CMD_NONE)
        return;
    if (ctrl & NAND_CLE)
        cmd = chip_select_ | AML_CMD_CLE | (cmd & 0xff);
    else
        cmd = chip_select_ | AML_CMD_ALE | (cmd & 0xff);
    mmio_nandreg_.Write32(cmd, P_NAND_CMD);
}

uint8_t AmlRawNand::AmlReadByte() {
    uint32_t cmd = chip_select_ | AML_CMD_DRD | 0;
    NandctrlSendCmd(cmd);

    AmlCmdIdle(NAND_TWB_TIME_CYCLE);

    AmlCmdIdle(0);
    AmlCmdIdle(0);
    AmlWaitCmdFinish(CMD_FINISH_TIMEOUT_MS);
    // There is no mmio interface to read a byte (?), so get the
    // underlying reg and do this manually.
    auto reg = reinterpret_cast<volatile uint8_t*>(mmio_nandreg_.get());
    return readb(reg + P_NAND_BUF);
}

void AmlRawNand::AmlSetClockRate(uint32_t clk_freq) {
    uint32_t always_on = 0x1 << 24;
    uint32_t clk;

    // For Amlogic type AXG.
    always_on = 0x1 << 28;
    switch (clk_freq) {
    case 24:
        clk = 0x80000201;
        break;
    case 112:
        clk = 0x80000249;
        break;
    case 200:
        clk = 0x80000245;
        break;
    case 250:
        clk = 0x80000244;
        break;
    default:
        clk = 0x80000245;
        break;
    }
    clk |= always_on;
    mmio_clockreg_.Write32(clk, 0);
}

void AmlRawNand::AmlClockInit() {
    uint32_t sys_clk_rate, bus_cycle, bus_timing;

    sys_clk_rate = 200;
    AmlSetClockRate(sys_clk_rate);
    bus_cycle = 6;
    bus_timing = bus_cycle + 1;
    NandctrlSetCfg(0);
    NandctrlSetTimingAsync(bus_timing, (bus_cycle - 1));
    NandctrlSendCmd(1 << 31);
}

void AmlRawNand::AmlAdjustTimings(uint32_t tRC_min, uint32_t tREA_max,
                                  uint32_t RHOH_min) {
    int sys_clk_rate, bus_cycle, bus_timing;
    // NAND timing defaults.
    static constexpr uint32_t TreaMaxDefault = 20;
    static constexpr uint32_t RhohMinDefault = 15;

    if (!tREA_max)
        tREA_max = TreaMaxDefault;
    if (!RHOH_min)
        RHOH_min = RhohMinDefault;
    if (tREA_max > 30)
        sys_clk_rate = 112;
    else if (tREA_max > 16)
        sys_clk_rate = 200;
    else
        sys_clk_rate = 250;
    AmlSetClockRate(sys_clk_rate);
    bus_cycle = 6;
    bus_timing = bus_cycle + 1;
    NandctrlSetCfg(0);
    NandctrlSetTimingAsync(bus_timing, (bus_cycle - 1));
    NandctrlSendCmd(1 << 31);
}

static bool IsPage0NandPage(uint32_t nand_page) {
    // Backup copies of page0 are located every 128 pages,
    // with the last one at 896.
    static constexpr uint32_t AmlPage0Step = 128;
    static constexpr uint32_t AmlPage0MaxAddr = 896;

    return ((nand_page <= AmlPage0MaxAddr) &&
            ((nand_page % AmlPage0Step) == 0));
}

zx_status_t AmlRawNand::RawNandReadPageHwecc(uint32_t nand_page, void* data, size_t data_size,
                                             size_t* data_actual, void* oob, size_t oob_size,
                                             size_t* oob_actual, uint32_t* ecc_correct) {
    zx_status_t status;
    uint32_t ecc_pagesize = 0; // Initialize to silence compiler.
    uint32_t ecc_pages;
    bool page0 = IsPage0NandPage(nand_page);

    if (!page0) {
        ecc_pagesize = AmlGetEccPageSize(controller_params_.bch_mode);
        ecc_pages = writesize_ / ecc_pagesize;
    } else
        ecc_pages = 1;
    // Send the page address into the controller.
    onfi_.OnfiCommand(NAND_CMD_READ0, 0x00, nand_page, static_cast<uint32_t>(chipsize_),
                      chip_delay_, (controller_params_.options & NAND_BUSWIDTH_16));
    mmio_nandreg_.Write32(GENCMDDADDRL(AML_CMD_ADL, data_buf_paddr_), P_NAND_CMD);
    mmio_nandreg_.Write32(GENCMDDADDRH(AML_CMD_ADH, data_buf_paddr_), P_NAND_CMD);
    mmio_nandreg_.Write32(GENCMDIADDRL(AML_CMD_AIL, info_buf_paddr_), P_NAND_CMD);
    mmio_nandreg_.Write32(GENCMDIADDRH(AML_CMD_AIH, info_buf_paddr_), P_NAND_CMD);
    // page0 needs randomization. so force it for page0.
    if (page0 || controller_params_.rand_mode)
        // Only need to set the seed if randomizing is enabled.
        AmlCmdSeed(nand_page);
    if (!page0)
        AmlCmdN2M(ecc_pages, ecc_pagesize);
    else
        AmlCmdN2MPage0();
    status = AmlWaitDmaFinish();
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: AmlWaitDmaFinish failed %d\n",
               __func__, status);
        return status;
    }
    status = AmlQueueRB();
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: AmlQueueRB failed %d\n", __func__, status);
        return ZX_ERR_INTERNAL;
    }
    status = AmlCheckECCPages(ecc_pages);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: AmlCheckECCPages failed %d\n",
               __func__, status);
        return status;
    }
    // Finally copy out the data and oob as needed.
    if (data != nullptr) {
        if (!page0)
            memcpy(data, data_buf_, writesize_);
        else {
            static constexpr uint32_t AmlPage0Len = 384;

            memcpy(data, data_buf_, AmlPage0Len);
        }
    }
    if (oob != nullptr)
        status = AmlGetOOBByte(reinterpret_cast<uint8_t*>(oob));
    status = AmlGetECCCorrections(ecc_pages, nand_page, ecc_correct);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: Uncorrectable ECC error on read\n",
               __func__);
        *ecc_correct = controller_params_.ecc_strength + 1;
    }
    return status;
}

// TODO : Right now, the driver uses a buffer for DMA, which
// is not needed. We should initiate DMA to/from pages passed in.
zx_status_t AmlRawNand::RawNandWritePageHwecc(const void* data, size_t data_size,
                                              const void* oob, size_t oob_size, uint32_t nand_page)
{
    zx_status_t status;
    uint32_t ecc_pagesize = 0; // Initialize to silence compiler.
    uint32_t ecc_pages;
    bool page0 = IsPage0NandPage(nand_page);

    if (!page0) {
        ecc_pagesize = AmlGetEccPageSize(controller_params_.bch_mode);
        ecc_pages = writesize_ / ecc_pagesize;
    } else
        ecc_pages = 1;
    if (data != nullptr) {
        memcpy(data_buf_, data, writesize_);
    }
    if (oob != nullptr) {
        AmlSetOOBByte(reinterpret_cast<const uint8_t*>(oob), ecc_pages);
    }

    onfi_.OnfiCommand(NAND_CMD_SEQIN, 0x00, nand_page, static_cast<uint32_t>(chipsize_),
                      chip_delay_, (controller_params_.options & NAND_BUSWIDTH_16));
    mmio_nandreg_.Write32(GENCMDDADDRL(AML_CMD_ADL, data_buf_paddr_), P_NAND_CMD);
    mmio_nandreg_.Write32(GENCMDDADDRH(AML_CMD_ADH, data_buf_paddr_), P_NAND_CMD);
    mmio_nandreg_.Write32(GENCMDIADDRL(AML_CMD_AIL, info_buf_paddr_), P_NAND_CMD);
    mmio_nandreg_.Write32(GENCMDIADDRH(AML_CMD_AIH, info_buf_paddr_), P_NAND_CMD);
    // page0 needs randomization. so force it for page0.
    if (page0 || controller_params_.rand_mode)
        // Only need to set the seed if randomizing is enabled.
        AmlCmdSeed(nand_page);
    if (!page0)
        AmlCmdM2N(ecc_pages, ecc_pagesize);
    else
        AmlCmdM2NPage0();
    status = AmlWaitDmaFinish();
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: error from wait_dma_finish\n",
               __func__);
        return status;
    }
    onfi_.OnfiCommand(NAND_CMD_PAGEPROG, -1, -1, static_cast<uint32_t>(chipsize_),
                      chip_delay_, (controller_params_.options & NAND_BUSWIDTH_16));
    status = onfi_.OnfiWait(AML_WRITE_PAGE_TIMEOUT);

    return status;
}

zx_status_t AmlRawNand::RawNandEraseBlock(uint32_t nand_page) {
    zx_status_t status;

    // nandblock has to be erasesize_ aligned.
    if (nand_page % erasesize_pages_) {
        zxlogf(ERROR, "%s: NAND block %u must be a erasesize_pages (%u) multiple\n",
               __func__, nand_page, erasesize_pages_);
        return ZX_ERR_INVALID_ARGS;
    }
    onfi_.OnfiCommand(NAND_CMD_ERASE1, -1, nand_page,
                      static_cast<uint32_t>(chipsize_), chip_delay_,
                      (controller_params_.options & NAND_BUSWIDTH_16));
    onfi_.OnfiCommand(NAND_CMD_ERASE2, -1, -1,
                      static_cast<uint32_t>(chipsize_), chip_delay_,
                      (controller_params_.options & NAND_BUSWIDTH_16));
    status = onfi_.OnfiWait(AML_ERASE_BLOCK_TIMEOUT);
    return status;
}

zx_status_t AmlRawNand::AmlGetFlashType() {
    uint8_t nand_maf_id, nand_dev_id;
    uint8_t id_data[8];
    struct nand_chip_table* nand_chip;

    onfi_.OnfiCommand(NAND_CMD_RESET, -1, -1,
                      static_cast<uint32_t>(chipsize_), chip_delay_,
                      (controller_params_.options & NAND_BUSWIDTH_16));
    onfi_.OnfiCommand(NAND_CMD_READID, 0x00, -1,
                      static_cast<uint32_t>(chipsize_), chip_delay_,
                      (controller_params_.options & NAND_BUSWIDTH_16));
    // Read manufacturer and device IDs.
    nand_maf_id = AmlReadByte();
    nand_dev_id = AmlReadByte();
    // Read again.
    onfi_.OnfiCommand(NAND_CMD_READID, 0x00, -1,
                      static_cast<uint32_t>(chipsize_), chip_delay_,
                      (controller_params_.options & NAND_BUSWIDTH_16));
    // Read entire ID string.
    for (uint32_t i = 0; i < sizeof(id_data); i++)
        id_data[i] = AmlReadByte();
    if (id_data[0] != nand_maf_id || id_data[1] != nand_dev_id) {
        zxlogf(ERROR, "second ID read did not match %02x,%02x against %02x,%02x\n",
               nand_maf_id, nand_dev_id, id_data[0], id_data[1]);
    }

    zxlogf(INFO, "%s: manufacturer_id = %x, device_ide = %x\n",
           __func__, nand_maf_id, nand_dev_id);
    nand_chip = onfi_.FindNandChipTable(nand_maf_id, nand_dev_id);
    if (nand_chip == nullptr) {
        zxlogf(ERROR, "%s: Cound not find matching NAND chip. NAND chip unsupported."
                      " This is FATAL\n",
               __func__);
        return ZX_ERR_UNAVAILABLE;
    }
    if (nand_chip->extended_id_nand) {
        // Initialize pagesize, eraseblk size, oobsize_ and
        // buswidth from extended parameters queried just now.
        uint8_t extid = id_data[3];

        writesize_ = 1024 << (extid & 0x03);
        extid = static_cast<uint8_t>(extid >> 2);
        // Calc oobsize_.
        oobsize_ = (8 << (extid & 0x01)) * (writesize_ >> 9);
        extid = static_cast<uint8_t>(extid >> 2);
        // Calc blocksize. Blocksize is multiples of 64KiB.
        erasesize_ = (64 * 1024) << (extid & 0x03);
        extid = static_cast<uint8_t>(extid >> 2);
        // Get buswidth information.
        bus_width_ = (extid & 0x01) ? NAND_BUSWIDTH_16 : 0;
    } else {
        // Initialize pagesize, eraseblk size, oobsize_ and
        // buswidth from values in table.
        writesize_ = nand_chip->page_size;
        oobsize_ = nand_chip->oobsize;
        erasesize_ = nand_chip->erase_block_size;
        bus_width_ = nand_chip->bus_width;
    }
    erasesize_pages_ = erasesize_ / writesize_;
    chipsize_ = nand_chip->chipsize;
    page_shift_ = ffs(writesize_) - 1;

    // We found a matching device in our database, use it to
    // initialize. Adjust timings and set various parameters.
    AmlAdjustTimings(nand_chip->timings.tRC_min,
                     nand_chip->timings.tREA_max,
                     nand_chip->timings.RHOH_min);
    // chip_delay is used OnfiCommand(), after sending down some commands
    // to the NAND chip.
    chip_delay_ = nand_chip->chip_delay_us;
    zxlogf(INFO, "NAND %s %s: chip size = %lu(GB), page size = %u, oob size = %u\n"
           "eraseblock size = %u, chip delay (us) = %u\n",
           nand_chip->manufacturer_name, nand_chip->device_name,
           chipsize_, writesize_, oobsize_, erasesize_,
           chip_delay_);
    return ZX_OK;
}

int AmlRawNand::IrqThread() {
    zxlogf(INFO, "aml_raw_nand_irq_thread start\n");

    while (1) {
        zx::time timestamp;
        if (irq_.wait(&timestamp) != ZX_OK) {
            zxlogf(ERROR, "%s: IRQ wait failed\n", __FILE__);
            return thrd_error;
        }

        // Wakeup blocked requester on
        // sync_completion_wait(&req_completion_, ZX_TIME_INFINITE);
        sync_completion_signal(&req_completion_);
    }

    return 0;
}

zx_status_t AmlRawNand::RawNandGetNandInfo(fuchsia_hardware_nand_Info* nand_info) {
    uint64_t capacity;
    zx_status_t status = ZX_OK;

    nand_info->page_size = writesize_;
    nand_info->pages_per_block = erasesize_pages_;
    capacity = chipsize_ * (1024 * 1024);
    capacity /= erasesize_;
    nand_info->num_blocks = static_cast<uint32_t>(capacity);
    nand_info->ecc_bits = controller_params_.ecc_strength;

    nand_info->nand_class = fuchsia_hardware_nand_Class_PARTMAP;
    memset(&nand_info->partition_guid, 0, sizeof(nand_info->partition_guid));

    if (controller_params_.user_mode == 2)
        nand_info->oob_size =
            (writesize_ /
             AmlGetEccPageSize(controller_params_.bch_mode)) *
            2;
    else
        status = ZX_ERR_NOT_SUPPORTED;
    return status;
}

void AmlRawNand::AmlSetEncryption() {
    mmio_nandreg_.SetBits32((1 << 17), P_NAND_CFG);
}

zx_status_t AmlRawNand::AmlReadPage0(void* data, size_t data_size,
                                     void* oob, size_t oob_size,
                                     uint32_t nand_page, uint32_t* ecc_correct,
                                     int retries) {
    zx_status_t status;

    retries++;
    do {
        status = RawNandReadPageHwecc(nand_page, data, data_size, nullptr,
                                      oob, oob_size, nullptr, ecc_correct);
    } while (status != ZX_OK && --retries > 0);
    if (status != ZX_OK)
        zxlogf(ERROR, "%s: Read error\n", __func__);
    return status;
}

zx_status_t AmlRawNand::AmlNandInitFromPage0() {
    zx_status_t status;
    NandPage0* page0;
    uint32_t ecc_correct;

    fbl::unique_ptr<char[]> buffer(new char[writesize_]);
    char* data = buffer.get();
    // There are 8 copies of page0 spaced apart by 128 pages
    // starting at Page 0. Read the first we can.
    for (uint32_t i = 0; i < 7; i++) {
        status = AmlReadPage0(data, writesize_, nullptr, 0, i * 128,
                              &ecc_correct, 3);
        if (status == ZX_OK)
            break;
    }
    if (status != ZX_OK) {
        // Could not read any of the page0 copies. This is a fatal error.
        zxlogf(ERROR, "%s: Page0 Read (all copies) failed\n", __func__);
        return status;
    }

    page0 = reinterpret_cast<NandPage0*>(data);
    controller_params_.rand_mode =
        (page0->nand_setup.cfg.d32 >> 19) & 0x1;
    controller_params_.bch_mode =
        (page0->nand_setup.cfg.d32 >> 14) & 0x7;

    controller_params_.ecc_strength =
        AmlGetEccStrength(controller_params_.bch_mode);
    if (controller_params_.ecc_strength < 0) {
        zxlogf(INFO, "%s: BAD ECC strength computed from BCH Mode\n", __func__);
        return ZX_ERR_BAD_STATE;
    }

    zxlogf(INFO, "%s: NAND BCH Mode is %s\n", __func__,
           AmlEccString(controller_params_.bch_mode));
    return ZX_OK;
}

zx_status_t AmlRawNand::AmlRawNandAllocBufs() {
    // The iobuffers MUST be uncachable. Making these cachable, with
    // cache flush/invalidate at the right places in the code does not
    // work. We see data corruptions caused by speculative cache prefetching
    // done by ARM. Note also that these corruptions are not easily reproducible.
    zx_status_t status = data_buffer_.Init(bti_.get(), writesize_,
                                           IO_BUFFER_UNCACHED | IO_BUFFER_RW | IO_BUFFER_CONTIG);
    if (status != ZX_OK) {
        zxlogf(ERROR,
               "raw_nand_test_allocbufs: io_buffer_init(data_buffer_) failed\n");
        return status;
    }
    ZX_DEBUG_ASSERT(writesize_ > 0);
    status = info_buffer_.Init(bti_.get(), writesize_,
                               IO_BUFFER_UNCACHED | IO_BUFFER_RW | IO_BUFFER_CONTIG);
    if (status != ZX_OK) {
        zxlogf(ERROR,
               "raw_nand_test_allocbufs: io_buffer_init(info_buffer_) failed\n");
        return status;
    }
    data_buf_ = data_buffer_.virt();
    info_buf_ = info_buffer_.virt();
    data_buf_paddr_ = data_buffer_.phys();
    info_buf_paddr_ = info_buffer_.phys();
    return ZX_OK;
}

zx_status_t AmlRawNand::AmlNandInit() {
    zx_status_t status;

    // Do nand scan to get manufacturer and other info.
    status = AmlGetFlashType();
    if (status != ZX_OK)
        return status;
    controller_params_.ecc_strength = AmlParams.ecc_strength;
    controller_params_.user_mode = AmlParams.user_mode;
    controller_params_.rand_mode = AmlParams.rand_mode;
    static constexpr auto NAND_USE_BOUNCE_BUFFER = 0x1;
    controller_params_.options = NAND_USE_BOUNCE_BUFFER;
    controller_params_.bch_mode = AmlParams.bch_mode;

    // Note on OOB byte settings.
    // The default config for OOB is 2 bytes per OOB page. This is the
    // settings we use. So nothing to be done for OOB. If we ever need
    // to switch to 16 bytes of OOB per NAND page, we need to set the
    // right bits in the CFG register.
    status = AmlRawNandAllocBufs();
    if (status != ZX_OK)
        return status;

    // Read one of the copies of page0, and use that to initialize
    // ECC algorithm and rand-mode.
    status = AmlNandInitFromPage0();

    static constexpr uint32_t chipsel[2] = {NAND_CE0, NAND_CE1};
    // Force chip_select to 0.
    chip_select_ = chipsel[0];

    return status;
}

void AmlRawNand::DdkRelease() {
    // This should result in the dtors of all members to be
    // called (so the MmioBuffers, bti, irq handle should get
    // cleaned up).
    delete this;
}

void AmlRawNand::CleanUpIrq() {
    irq_.destroy();
    thrd_join(irq_thread_, nullptr);
}

void AmlRawNand::DdkUnbind() {
    CleanUpIrq();
    DdkRemove();
}

zx_status_t AmlRawNand::Init() {
    onfi_.Init([this](int32_t cmd, uint32_t ctrl)->void { AmlCmdCtrl(cmd, ctrl); },
               [this]()->uint8_t { return AmlReadByte(); });
    auto cb = [](void* arg) -> int { return reinterpret_cast<AmlRawNand*>(arg)->IrqThread(); };
    if (thrd_create_with_name(&irq_thread_, cb,
                              this, "aml_raw_nand_irq_thread") != thrd_success) {
        zxlogf(ERROR, "%s: Failed to create IRQ thread\n", __FILE__);
        return ZX_ERR_INTERNAL;
    }

    // Do the rest of the init here, instead of up top in the irq
    // thread, because the init needs for irq's to work.
    AmlClockInit();
    zx_status_t status = AmlNandInit();
    if (status != ZX_OK) {
        zxlogf(ERROR, "aml_raw_nand: AmlNandInit() failed - This is FATAL\n");
        CleanUpIrq();
    }
    return status;
}

zx_status_t AmlRawNand::Bind() {
    zx_status_t status = DdkAdd("aml-raw_nand");
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DdkAdd failed\n", __FILE__);
        CleanUpIrq();
    }
    return status;
}

zx_status_t AmlRawNand::Create(void* ctx, zx_device_t* parent) {
    zx_status_t status;

    ddk::PDev pdev(parent);
    if (!pdev.is_valid()) {
        zxlogf(ERROR, "%s: ZX_PROTOCOL_PDEV not available\n", __FILE__);
        return ZX_ERR_NO_RESOURCES;
    }

    zx::bti bti;
    if ((status = pdev.GetBti(0, &bti)) != ZX_OK) {
        zxlogf(ERROR, "%s: pdev_get_bti failed\n", __FILE__);
        return status;
    }

    static constexpr uint32_t NandRegWindow = 0;
    static constexpr uint32_t ClockRegWindow = 1;
    std::optional<ddk::MmioBuffer> mmio_nandreg;
    status = pdev.MapMmio(NandRegWindow, &mmio_nandreg);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: pdev.MapMmio nandreg failed\n", __FILE__);
        return status;
    }

    std::optional<ddk::MmioBuffer> mmio_clockreg;
    status = pdev.MapMmio(ClockRegWindow, &mmio_clockreg);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: pdev.MapMmio clockreg failed\n", __FILE__);
        return status;
    }

    zx::interrupt irq;
    if ((status = pdev.GetInterrupt(0, &irq)) != ZX_OK) {
        zxlogf(ERROR, "%s: Failed to map interrupt\n", __FILE__);
        return status;
    }
    fbl::AllocChecker ac;
    fbl::unique_ptr<AmlRawNand> device(new (&ac) AmlRawNand(parent,
                                                            *std::move(mmio_nandreg),
                                                            *std::move(mmio_clockreg),
                                                            std::move(bti),
                                                            std::move(irq)));

    if (!ac.check()) {
        zxlogf(ERROR, "%s: AmlRawNand alloc failed\n", __FILE__);
        return ZX_ERR_NO_MEMORY;
    }

    if ((status = device->Init()) != ZX_OK) {
        return status;
    }

    if ((status = device->Bind()) != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the device.
    __UNUSED auto* dummy = device.release();
    return ZX_OK;
}

static constexpr zx_driver_ops_t amlrawnand_driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = AmlRawNand::Create;
    return ops;
}();

} // namespace amlrawnand

// The formatter does not play nice with these macros.
// clang-format off
ZIRCON_DRIVER_BEGIN(aml_rawnand, amlrawnand::amlrawnand_driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_AMLOGIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_AMLOGIC_RAW_NAND),
ZIRCON_DRIVER_END(aml_rawnand)
// clang-format on
