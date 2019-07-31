// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#define T931_GPIO_BASE                  0xff634400
#define T931_GPIO_LENGTH                0x400
#define T931_GPIO_A0_BASE               0xff800000
#define T931_GPIO_AO_LENGTH             0x1000
#define T931_GPIO_INTERRUPT_BASE        0xffd00000
#define T931_GPIO_INTERRUPT_LENGTH      0x10000
#define T931_I2C_AOBUS_BASE             (T931_GPIO_A0_BASE + 0x5000)
#define T931_CBUS_BASE                  0xffd00000
#define T931_CBUS_LENGTH                0x100000
#define T931_I2C0_BASE                  (T931_CBUS_BASE + 0x1f000)
#define T931_I2C1_BASE                  (T931_CBUS_BASE + 0x1e000)
#define T931_I2C2_BASE                  (T931_CBUS_BASE + 0x1d000)
#define T931_I2C3_BASE                  (T931_CBUS_BASE + 0x1c000)
#define T931_SPICC0_BASE                (T931_CBUS_BASE + 0x13000)
#define T931_SPICC1_BASE                (T931_CBUS_BASE + 0x15000)

#define T931_PWM_LENGTH                 0x1000 // applies to each PWM bank
#define T931_PWM_AB_BASE                0xffd1b000
#define T931_PWM_PWM_A                  0x0
#define T931_PWM_PWM_B                  0x4
#define T931_PWM_MISC_REG_AB            0x8
#define T931_DS_A_B                     0xc
#define T931_PWM_TIME_AB                0x10
#define T931_PWM_A2                     0x14
#define T931_PWM_B2                     0x18
#define T931_PWM_BLINK_AB               0x1c

#define T931_PWM_CD_BASE                0xffd1a000
#define T931_PWM_PWM_C                  0x0
#define T931_PWM_PWM_D                  0x4
#define T931_PWM_MISC_REG_CD            0x8
#define T931_DS_C_D                     0xc
#define T931_PWM_TIME_CD                0x10
#define T931_PWM_C2                     0x14
#define T931_PWM_D2                     0x18
#define T931_PWM_BLINK_CD               0x1c

#define T931_PWM_EF_BASE                0xffd19000
#define T931_PWM_PWM_E                  0x0
#define T931_PWM_PWM_F                  0x4
#define T931_PWM_MISC_REG_EF            0x8
#define T931_DS_E_F                     0xc
#define T931_PWM_TIME_EF                0x10
#define T931_PWM_E2                     0x14
#define T931_PWM_F2                     0x18
#define T931_PWM_BLINK_EF               0x1c

#define T931_AO_PWM_AB_BASE             0xff807000
#define T931_AO_PWM_CD_BASE             0xff802000
#define T931_AO_PWM_LENGTH              0x1000

#define T931_UART_LENGTH                0x1000 // applies to each UART bank
#define T931_UART_A_BASE                0xffd24000
#define T931_UART_B_BASE                0xffd23000
#define T931_UART_C_BASE                0xffd22000
#define T931_UART_WFIFO                 0x0
#define T931_UART_RFIFO                 0x4
#define T931_UART_CONTROL               0x8
#define T931_UART_STATUS                0xc
#define T931_UART_MISC                  0x10
#define T931_UART_REGS                  0x14

#define T931_DOS_BASE                   0xff620000
#define T931_DOS_LENGTH                 0x10000

#define T931_DMC_BASE                   0xff638000
#define T931_DMC_LENGTH                 0x1000

#define T931_HIU_BASE                   0xff63c000
#define T931_HIU_LENGTH                 0x2000

#define T931_DSI_PHY_BASE               0xff644000
#define T931_DSI_PHY_LENGTH             0x2000

#define T931_AOBUS_BASE                 0xff800000
#define T931_AOBUS_LENGTH               0x100000

#define T931_VPU_BASE                   0xff900000
#define T931_VPU_LENGTH                 0x40000

#define T931_CBUS_BASE                  0xffd00000
#define T931_CBUS_LENGTH                0x100000

#define T931_MIPI_DSI_BASE              0xffd07000
#define T931_MIPI_DSI_LENGTH            0x1000

#define T931_TOP_MIPI_DSI_BASE          0xffd073C0
#define T931_TOP_MIPI_DSI_LENGTH        0x40

#define T931_MSR_CLK_BASE               0xffd18000
#define T931_MSR_CLK_LENGTH             0x1000

#define T931_MALI_BASE                  0xffe40000
#define T931_MALI_LENGTH                0x40000

#define T931_TEMP_SENSOR_BASE           0xff634000
#define T931_TEMP_SENSOR_LENGTH         0x1000

// MIPI CSI & Adapter
#define T931_CSI_PHY0_BASE              0xff650000
#define T931_CSI_PHY0_LENGTH            0x2000
#define T931_APHY_BASE                  0xff63c300
#define T931_APHY_LENGTH                0x100
#define T931_CSI_HOST0_BASE             0xff654000
#define T931_CSI_HOST0_LENGTH           0x100
#define T931_MIPI_ADAPTER_BASE          0xff650000
#define T931_MIPI_ADAPTER_LENGTH        0x6000

// Power domain
#define T931_POWER_DOMAIN_BASE          0xff800000
#define T931_POWER_DOMAIN_LENGTH        0x1000

// Memory Power Domain
#define T931_MEMORY_PD_BASE             0xff63c000
#define T931_MEMORY_PD_LENGTH           0x1000

// Reset
#define T931_RESET_BASE                 0xffd01000
#define T931_RESET_LENGTH               0x100

// USB.
#define T931_USB0_BASE                  0xff500000
#define T931_USB0_LENGTH                0x100000

#define T931_USB1_BASE                  0xff400000
#define T931_USB1_LENGTH                0x40000

#define T931_USBCTRL_BASE               0xffe09000
#define T931_USBCTRL_LENGTH             0x2000

#define T931_USBPHY20_BASE              0xff636000
#define T931_USBPHY20_LENGTH            0x2000

#define T931_USBPHY21_BASE              0xff63a000
#define T931_USBPHY21_LENGTH            0x2000

// ISP
#define T931_ISP_BASE                   0xff140000
#define T931_ISP_LENGTH                 0x00040000

// GDC
#define T931_GDC_BASE                   0xFF950000
#define T931_GDC_LENGTH                 0x100

// eMMC/SD register blocks
#define T931_SD_EMMC_A_BASE             0xffE03000
#define T931_SD_EMMC_A_LENGTH           0x2000
#define T931_SD_EMMC_B_BASE             0xffE05000
#define T931_SD_EMMC_B_LENGTH           0x2000
#define T931_SD_EMMC_C_BASE             0xffE07000
#define T931_SD_EMMC_C_LENGTH           0x2000

// Reset register offsets
#define T931_RESET0_REGISTER          0x04
#define T931_RESET1_REGISTER          0x08
#define T931_RESET2_REGISTER          0x0c
#define T931_RESET3_REGISTER          0x10
#define T931_RESET4_REGISTER          0x14
#define T931_RESET6_REGISTER          0x1c
#define T931_RESET7_REGISTER          0x20
#define T931_RESET0_MASK              0x40
#define T931_RESET1_MASK              0x44
#define T931_RESET2_MASK              0x48
#define T931_RESET3_MASK              0x4c
#define T931_RESET4_MASK              0x50
#define T931_RESET6_MASK              0x58
#define T931_RESET7_MASK              0x5c
#define T931_RESET0_LEVEL             0x80
#define T931_RESET1_LEVEL             0x84
#define T931_RESET2_LEVEL             0x88
#define T931_RESET3_LEVEL             0x8c
#define T931_RESET4_LEVEL             0x90
#define T931_RESET6_LEVEL             0x98
#define T931_RESET7_LEVEL             0x9c

// IRQs
#define T931_VIU1_VSYNC_IRQ             35
#define T931_USB_IDDIG_IRQ              48
#define T931_I2C0_IRQ                   53
#define T931_DEMUX_IRQ                  55
#define T931_UART_A_IRQ                 58
#define T931_USB0_IRQ                   62
#define T931_USB1_IRQ                   63
#define T931_PARSER_IRQ                 64
#define T931_TS_PLL_IRQ                 67
#define T931_I2C3_IRQ                   71
#define T931_DOS_MBOX_0_IRQ             75
#define T931_DOS_MBOX_1_IRQ             76
#define T931_DOS_MBOX_2_IRQ             77
#define T931_GPIO_IRQ_0                 96
#define T931_GPIO_IRQ_1                 97
#define T931_GPIO_IRQ_2                 98
#define T931_GPIO_IRQ_3                 99
#define T931_GPIO_IRQ_4                 100
#define T931_GPIO_IRQ_5                 101
#define T931_GPIO_IRQ_6                 102
#define T931_GPIO_IRQ_7                 103
#define T931_UART_B_IRQ                 107
#define T931_SPICC0_IRQ                 113
#define T931_RDMA_DONE                  121
#define T931_SPICC1_IRQ                 122
#define T931_UART_C_IRQ                 125
#define T931_MALI_ISP_IRQ               174
#define T931_MALI_GDC_IRQ               176
#define T931_MALI_IRQ_GP                192
#define T931_MALI_IRQ_GPMMU             193
#define T931_MALI_IRQ_PP                194
#define T931_MIPI_ADAPTER_IRQ           211
#define T931_SD_EMMC_A_IRQ              221
#define T931_SD_EMMC_B_IRQ              222
#define T931_SD_EMMC_C_IRQ              223
#define T931_I2C_AO_0_IRQ               227
#define T931_I2C1_IRQ                   246
#define T931_I2C2_IRQ                   247

// Alternate Functions for EMMC
#define T931_EMMC_D0                    T931_GPIOBOOT(0)
#define T931_EMMC_D0_FN                 1
#define T931_EMMC_D1                    T931_GPIOBOOT(1)
#define T931_EMMC_D1_FN                 1
#define T931_EMMC_D2                    T931_GPIOBOOT(2)
#define T931_EMMC_D2_FN                 1
#define T931_EMMC_D3                    T931_GPIOBOOT(3)
#define T931_EMMC_D3_FN                 1
#define T931_EMMC_D4                    T931_GPIOBOOT(4)
#define T931_EMMC_D4_FN                 1
#define T931_EMMC_D5                    T931_GPIOBOOT(5)
#define T931_EMMC_D5_FN                 1
#define T931_EMMC_D6                    T931_GPIOBOOT(6)
#define T931_EMMC_D6_FN                 1
#define T931_EMMC_D7                    T931_GPIOBOOT(7)
#define T931_EMMC_D7_FN                 1
#define T931_EMMC_CLK                   T931_GPIOBOOT(8)
#define T931_EMMC_CLK_FN                1
#define T931_EMMC_RST                   T931_GPIOBOOT(9)
#define T931_EMMC_RST_FN                1
#define T931_EMMC_CMD                   T931_GPIOBOOT(10)
#define T931_EMMC_CMD_FN                1
#define T931_EMMC_DS                    T931_GPIOBOOT(15)
#define T931_EMMC_DS_FN                 1

// Alternate Functions for SDIO
#define T931_SDIO_D0                    T931_GPIOX(0)
#define T931_SDIO_D0_FN                 1
#define T931_SDIO_D1                    T931_GPIOX(1)
#define T931_SDIO_D1_FN                 1
#define T931_SDIO_D2                    T931_GPIOX(2)
#define T931_SDIO_D2_FN                 1
#define T931_SDIO_D3                    T931_GPIOX(3)
#define T931_SDIO_D3_FN                 1
#define T931_SDIO_CLK                   T931_GPIOX(4)
#define T931_SDIO_CLK_FN                1
#define T931_SDIO_CMD                   T931_GPIOX(5)
#define T931_SDIO_CMD_FN                1
#define T931_WIFI_REG_ON                T931_GPIOX(6)
#define T931_WIFI_REG_ON_FN             0
#define T931_WIFI_HOST_WAKE             T931_GPIOX(7)
#define T931_WIFI_HOST_WAKE_FN          0
#define T931_WIFI_LPO_CLK               T931_GPIOX(16)
#define T931_WIFI_LPO_CLK_FN            1 // PWM_E

// Alternate Functions for UART
#define T931_UART_A_TX                  T931_GPIOX(12)
#define T931_UART_A_TX_FN               1
#define T931_UART_A_RX                  T931_GPIOX(13)
#define T931_UART_A_RX_FN               1
#define T931_UART_A_CTS                 T931_GPIOX(14)
#define T931_UART_A_CTS_FN              1
#define T931_UART_A_RTS                 T931_GPIOX(15)
#define T931_UART_A_RTS_FN              1

#define T931_EE_PDM_BASE                (0xff640000)
#define T931_EE_PDM_LENGTH              (0x2000)

#define T931_EE_AUDIO_BASE              (0xff642000)
#define T931_EE_AUDIO_LENGTH            (0x1000)

#define T931_HIFI_PLL_RATE              1536000000
