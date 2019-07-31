// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

// Peripheral Memory
#define MT8167_XO_BASE                                      0x10000000
#define MT8167_XO_SIZE                                      0x2000

#define MT8167_GPIO_BASE                                    0x10005000
#define MT8167_GPIO_SIZE                                    0x700

#define MT8167_IOCFG_BASE                                   0x10005900
#define MT8167_IOCFG_SIZE                                   0x700

#define MT8167_SCPSYS_BASE                                  0x10006000
#define MT8167_SCPSYS_SIZE                                  0x1000

#define MT8167_EINT_BASE                                    0x1000B000
#define MT8167_EINT_SIZE                                    0x800

#define MT8167_SOC_BASE                                     0x10200000
#define MT8167_SOC_SIZE                                     0x1D00

#define MT8167_PLL_BASE                                     0x10018000
#define MT8167_PLL_SIZE                                     0x800

#define MT8167_I2C0_BASE                                    0x11009000
#define MT8167_I2C0_SIZE                                    0x8c

#define MT8167_I2C1_BASE                                    0x1100a000
#define MT8167_I2C1_SIZE                                    0x8c

#define MT8167_I2C2_BASE                                    0x1100b000
#define MT8167_I2C2_SIZE                                    0x8c

#define MT8167_USB0_BASE                                    0x11100000
#define MT8167_USB0_LENGTH                                  0x1000
#define MT8167_USB1_BASE                                    0x11190000
#define MT8167_USB1_LENGTH                                  0x1000

#define MT8167_USBPHY_BASE                                  0x11110800
#define MT8167_USBPHY_LENGTH                                0x800

#define MT8167_MSDC0_BASE                                   0x11120000
#define MT8167_MSDC0_SIZE                                   0x22c

#define MT8167_MSDC1_BASE                                   0x11130000
#define MT8167_MSDC1_SIZE                                   0x22c

#define MT8167_AUDIO_BASE                                   0x11140000
#define MT8167_AUDIO_SIZE                                   0x1000

#define MT8167_MSDC2_BASE                                   0x11170000
#define MT8167_MSDC2_SIZE                                   0x22c

#define MT8167_MFG_BASE                                     0x13000000
#define MT8167_MFG_SIZE                                     0x80000

#define MT8167_MFG_TOP_CONFIG_BASE                          0x13ffe000
#define MT8167_MFG_TOP_CONFIG_SIZE                          0x1000

#define MT8167_AP_MIXED_SYS_BASE                            0x10018000
#define MT8167_AP_MIXED_SYS_SIZE                            0x714

#define MT8167_THERMAL_BASE                                 0x1100d000
#define MT8167_THERMAL_SIZE                                 0x510

#define MT8167_FUSE_BASE                                    0x10009000
#define MT8167_FUSE_SIZE                                    0x1000

#define MT8167_PMIC_WRAP_BASE                               0x1000f000
#define MT8167_PMIC_WRAP_SIZE                               0x1000

#define MT8167_INFRACFG_BASE                                0x10001000
#define MT8167_INFRACFG_SIZE                                0x1000

// Display Subsystem
#define MT8167_MSYS_CFG_BASE                                0x14000000
#define MT8167_MSYS_CFG_SIZE                                0x1000
#define MT8167_DISP_OVL_BASE                                0x14007000
#define MT8167_DISP_OVL_SIZE                                0x1000
#define MT8167_DISP_RDMA_BASE                               0x14009000
#define MT8167_DISP_RDMA_SIZE                               0x1000
#define MT8167_DISP_COLOR_BASE                              0x1400C000
#define MT8167_DISP_COLOR_SIZE                              0x1000
#define MT8167_DISP_CCORR_BASE                              0x1400D000
#define MT8167_DISP_CCORR_SIZE                              0x1000
#define MT8167_DISP_AAL_BASE                                0x1400E000
#define MT8167_DISP_AAL_SIZE                                0x1000
#define MT8167_DISP_GAMMA_BASE                              0x1400F000
#define MT8167_DISP_GAMMA_SIZE                              0x1000
#define MT8167_DITHER_BASE                                  0x14010000
#define MT8167_DITHER_SIZE                                  0x1000
#define MT8167_DISP_DSI_BASE                                0x14012000
#define MT8167_DISP_DSI_SIZE                                0x1000
#define MT8167_DISP_MUTEX_BASE                              0x14015000
#define MT8167_DISP_MUTEX_SIZE                              0x1000
#define MT8167_DISP_SMI_LARB0_BASE                          0x14016000
#define MT8167_DISP_SMI_LARB0_SIZE                          0x1000
#define MT8167_MIPI_TX_BASE                                 0x14018000
#define MT8167_MIPI_TX_SIZE                                 0x1000
#define MT8167_LVDS_BASE                                    0x1401A000
#define MT8167_LVDS_SIZE                                    0x1000

// SOC Interrupt polarity registers start
#define MT8167_SOC_INT_POL                                  0x620

// MT8167s IRQ Table
#define MT8167_IRQ_USB_MCU                                  104
#define MT8167_IRQ_DISP_PWM                                 105
#define MT8167_IRQ_PWM                                      108
#define MT8167_IRQ_PTP_THERM                                109
#define MT8167_IRQ_MSDC0                                    110
#define MT8167_IRQ_MSDC1                                    111
#define MT8167_IRQ_I2C0                                     112
#define MT8167_IRQ_I2C1                                     113
#define MT8167_IRQ_I2C2                                     114
#define MT8167_IRQ_UART0                                    116
#define MT8167_IRQ_UART1                                    117
#define MT8167_IRQ_MSDC2                                    141
#define MT8167_IRQ_HDMI_SIFM                                142
#define MT8167_IRQ_ETHER_NIC_WRAP                           143
#define MT8167_IRQ_ARM_EINT                                 166
#define MT8167_IRQ_DISP_OVL0                                192
#define MT8167_IRQ_DISP_RDMA0                               194
#define MT8167_IRQ_DISP_RDMA1                               195
#define MT8167_IRQ_DISP_COLOR                               197
#define MT8167_IRQ_DISP_DSI0                                203
#define MT8167_IRQ_RGX                                      217
#define MT8167_IRQ_WDT                                      230
#define MT8167_IRQ_I2C3                                     241
#define MT8167_IRQ_USB_MCU_P1                               242
#define MT8167_IRQ_UART2                                    243

#define MT8167_I2C_CNT                                      3
#define MT8167_GPIO_EINT_MAX                                131

#define MT8167_GPIO_MT7668_PMU_EN                           2
#define MT8167_GPIO_TOUCH_INT                               8
#define MT8167_GPIO_TOUCH_RST                               9
#define MT8167_GPIO_MIC_PRIVACY                             23
#define MT8167_GPIO_KP_ROW0                                 40
#define MT8167_GPIO_KP_ROW1                                 41
#define MT8167_GPIO_KP_COL0                                 42
#define MT8167_GPIO_VOLUME_UP                               42
#define MT8167_GPIO_KP_COL1                                 43
#define MT8167_GPIO_LCD_RST                                 66
#define MT8167_GPIO_MSDC0_RST                               114
#define MT8167_CLEO_GPIO_HUB_PWR_EN                         13
#define MT8167_CLEO_GPIO_LCM_EN                             22
