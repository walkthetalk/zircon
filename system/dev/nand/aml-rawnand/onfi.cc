// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "onfi.h"

#include <string.h>
#include <unistd.h>

#include <ddk/debug.h>

// Database of settings for the NAND flash devices we support.
// Note on chip_delay: chip_delay is the delay after we enqueue certain ONFI
// commands (RESET, READSTART). The value of 30us was experimentally picked for
// the Samsung NAND, and 20us for the Toshiba NAND. It turns out that a value
// of 25us works better for the Micron NAND (25us reduces the number of ECC
// errors significantly).
// TODO(ZX-2696): Determine the value of chip delay more scientifically.
static struct nand_chip_table nand_chip_table[] = {
    {0x2C, 0xDC, "Micron", "MT29F4G08ABAEA", {20, 16, 15}, 25, true, 512, 0, 0, 0, 0},
    {0xEC, 0xDC, "Samsung", "K9F4G08U0F", {25, 20, 15}, 30, true, 512, 0, 0, 0, 0},
    /* TODO: This works. but doublecheck Toshiba nand_timings from datasheet */
    {0x98, 0xDC, "Toshiba", "TC58NVG2S0F", {25, 20, /* 15 */ 25}, 25, true, 512, 0, 0, 0, 0},
};

#define NAND_CHIP_TABLE_SIZE \
    (sizeof(nand_chip_table) / sizeof(struct nand_chip_table))

struct nand_chip_table* Onfi::FindNandChipTable(uint8_t manuf_id,
                                                uint8_t device_id) {
    for (uint32_t i = 0; i < NAND_CHIP_TABLE_SIZE; i++)
        if (manuf_id == nand_chip_table[i].manufacturer_id &&
            device_id == nand_chip_table[i].device_id)
            return &nand_chip_table[i];
    return NULL;
}

void Onfi::Init(fbl::Function<void(int32_t cmd, uint32_t ctrl)> cmd_ctrl,
                fbl::Function<uint8_t()> read_byte) {
    cmd_ctrl_ = std::move(cmd_ctrl);
    read_byte_ = std::move(read_byte);
}

zx_status_t Onfi::OnfiWait(uint32_t timeout_ms) {
    uint64_t total_time = 0;
    uint8_t cmd_status;

    cmd_ctrl_(NAND_CMD_STATUS, NAND_CTRL_CLE | NAND_CTRL_CHANGE);
    cmd_ctrl_(NAND_CMD_NONE, NAND_NCE | NAND_CTRL_CHANGE);
    while (!((cmd_status = read_byte_()) & NAND_STATUS_READY)) {
        usleep(10);
        total_time += 10;
        if (total_time > (timeout_ms * 1000)) {
            break;
        }
    }
    if (!(cmd_status & NAND_STATUS_READY)) {
        zxlogf(ERROR, "nand command wait timed out\n");
        return ZX_ERR_TIMED_OUT;
    }
    if (cmd_status & NAND_STATUS_FAIL) {
        zxlogf(ERROR, "%s: nand command returns error\n", __func__);
        return ZX_ERR_IO;
    }
    return ZX_OK;
}

void Onfi::OnfiCommand(uint32_t command, int32_t column, int32_t page_addr,
                       uint32_t capacity_mb, uint32_t chip_delay_us,
                       int buswidth_16) {
    cmd_ctrl_(command, NAND_NCE | NAND_CLE | NAND_CTRL_CHANGE);
    if (column != -1 || page_addr != -1) {
        uint32_t ctrl = NAND_CTRL_CHANGE | NAND_NCE | NAND_ALE;

        if (column != -1) {
            /* 16 bit buswidth ? */
            if (buswidth_16)
                column >>= 1;
            cmd_ctrl_(column, ctrl);
            ctrl &= ~NAND_CTRL_CHANGE;
            cmd_ctrl_(column >> 8, ctrl);
        }
        if (page_addr != -1) {
            cmd_ctrl_(page_addr, ctrl);
            cmd_ctrl_(page_addr >> 8,
                              NAND_NCE | NAND_ALE);
            /* one more address cycle for devices > 128M */
            if (capacity_mb > 128)
                cmd_ctrl_(page_addr >> 16, NAND_NCE | NAND_ALE);
        }
    }
    cmd_ctrl_(NAND_CMD_NONE, NAND_NCE | NAND_CTRL_CHANGE);

    if (command == NAND_CMD_ERASE1 || command == NAND_CMD_ERASE2 ||
        command == NAND_CMD_SEQIN || command == NAND_CMD_PAGEPROG)
        return;
    if (command == NAND_CMD_RESET) {
        usleep(chip_delay_us);
        cmd_ctrl_(NAND_CMD_STATUS, NAND_NCE | NAND_CLE | NAND_CTRL_CHANGE);
        cmd_ctrl_(NAND_CMD_NONE, NAND_NCE | NAND_CTRL_CHANGE);
        /* We have to busy loop until ready */
        while (!(read_byte_() & NAND_STATUS_READY))
            ;
        return;
    }
    if (command == NAND_CMD_READ0) {
        cmd_ctrl_(NAND_CMD_READSTART, NAND_NCE | NAND_CLE | NAND_CTRL_CHANGE);
        cmd_ctrl_(NAND_CMD_NONE, NAND_NCE | NAND_CTRL_CHANGE);
    }
    usleep(chip_delay_us);
}
