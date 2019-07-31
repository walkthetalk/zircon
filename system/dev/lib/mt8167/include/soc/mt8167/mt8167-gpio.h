// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#define MT8167_GPIO16_EINT16                 16
#define MT8167_GPIO24_EINT24                 24
#define MT8167_GPIO25_EINT25                 25
#define MT8167_GPIO52_SDA1                   52
#define MT8167_GPIO53_SCL1                   53
#define MT8167_GPIO55_DISP_PWM               54
#define MT8167_GPIO55_I2S_DATA_IN            55
#define MT8167_GPIO58_SDA0                   58
#define MT8167_GPIO59_SCL0                   59
#define MT8167_GPIO56_I2S_LRCK               56
#define MT8167_GPIO25_I2S_BCK                57
#define MT8167_GPIO60_SDA2                   60
#define MT8167_GPIO61_SCL2                   61
#define MT8167_GPIO100_CMDAT0                100
#define MT8167_GPIO101_CMDAT1                101
#define MT8167_GPIO102_CMMCLK                102
#define MT8167_GPIO103_CMPCLK                103
#define MT8167_GPIO107_MSDC1_DAT1            107
#define MT8167_GPIO108_MSDC1_DAT2            108

// All functions 0 correspond to GOPIO configuration.
#define MT8167_GPIO_GPIO_FN                  0

#define MT8167_GPIO16_I2S_8CH_BCK_FN         2
#define MT8167_GPIO16_TDM_RX_LRCK_FN         3
#define MT8167_GPIO16_ANT_SEL3_FN            4
#define MT8167_GPIO16_CONN_MCU_TRST_B_FN     5
#define MT8167_GPIO16_NCEBO_FN               6
#define MT8167_GPIO16_DBG_MON_B_10_FN        7

#define MT8167_GPIO25_DPI_D19_FN             1
#define MT8167_GPIO25_DPI_VSYNC_FN           2
#define MT8167_GPIO25_ANT_SEL0_FN            3
#define MT8167_GPIO25_URTS2_FN               4
#define MT8167_GPIO25_PWM_B_FN               5
#define MT8167_GPIO25_I2S2_MCK_FN            6
#define MT8167_GPIO25_DBG_MON_A_1_FN         7

#define MT8167_GPIO54_DISP_PWM_FN            1
#define MT8167_GPIO54_PWM_B_FN               2
#define MT8167_GPIO54_DBG_MON_A_27_FN        7

#define MT8167_GPIO55_I2S0_DI_FN             1
#define MT8167_GPIO55_UCTSO_FN               2
#define MT8167_GPIO55_I2S3_DO_FN             3
#define MT8167_GPIO55_I2S_8CH_DO1_FN         4
#define MT8167_GPIO55_PWM_A_FN               5
#define MT8167_GPIO55_I2S2_BCK_FN            6
#define MT8167_GPIO55_DBG_MON_A_28_FN        7

#define MT8167_GPIO56_I2S0_LRCK_FN           1
#define MT8167_GPIO56_I2S3_LRCK_FN           3
#define MT8167_GPIO56_I2S_8CH_LRCK_FN        4
#define MT8167_GPIO56_PWM_B_FN               5
#define MT8167_GPIO56_I2S2_DI_FN             6
#define MT8167_GPIO56_DBG_MON_A_29_FN        7

#define MT8167_GPIO57_I2S0_BCK_FN            1
#define MT8167_GPIO57_URTSO_FN               2
#define MT8167_GPIO57_I2S3_BCK_FN            3
#define MT8167_GPIO57_I2S_8CH_BCK_FN         4
#define MT8167_GPIO57_PWM_C_FN               5
#define MT8167_GPIO57_I2S_LRCK_FN            6
#define MT8167_GPIO57_DBG_MON_A_30_FN        7

#define MT8167_GPIO100_CMDAT0_FN             1
#define MT8167_GPIO100_CMCSD0_FN             2
#define MT8167_GPIO100_ANT_SEL2_FN           3
#define MT8167_GPIO100_TDM_RX_MCK_FN         5
#define MT8167_GPIO100_DBG_MON_B_21          7

#define MT8167_GPIO101_CMDAT1_FN             1
#define MT8167_GPIO101_CMCSD1_FN             2
#define MT8167_GPIO101_ANT_SEL3_FN           3
#define MT8167_GPIO101_CMFLASH_FN            4
#define MT8167_GPIO101_TDM_RX_BCK_FN         5
#define MT8167_GPIO101_DBG_MON_B_22          7

#define MT8167_GPIO102_CMMCLK_FN             1
#define MT8167_GPIO102_ANT_SEL4_FN           3
#define MT8167_GPIO102_TDM_RX_LRCK_FN        5
#define MT8167_GPIO102_DBG_MON_B_23          7

#define MT8167_GPIO103_CMPCLK_FN             1
#define MT8167_GPIO103_CMCSK_FN              2
#define MT8167_GPIO103_ANT_SEL5_FN           3
#define MT8167_GPIO103_TDM_RX_DI_FN          5
#define MT8167_GPIO103_DBG_MON_B_24          7

