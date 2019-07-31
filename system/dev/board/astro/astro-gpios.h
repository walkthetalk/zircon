// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <soc/aml-s905d2/s905d2-gpio.h>

namespace astro {

#define GPIO_BACKLIGHT_ENABLE   S905D2_GPIOA(10)
#define GPIO_LCD_RESET          S905D2_GPIOH(6)
#define GPIO_PANEL_DETECT       S905D2_GPIOH(5)
#define GPIO_TOUCH_INTERRUPT    S905D2_GPIOZ(4)
#define GPIO_TOUCH_RESET        S905D2_GPIOZ(9)
#define GPIO_LIGHT_INTERRUPT    S905D2_GPIOAO(5)
#define GPIO_AUDIO_SOC_FAULT_L  S905D2_GPIOA(4)
#define GPIO_SOC_AUDIO_EN       S905D2_GPIOA(5)
#define GPIO_VOLUME_UP          S905D2_GPIOZ(5)
#define GPIO_VOLUME_DOWN        S905D2_GPIOZ(6)
#define GPIO_VOLUME_BOTH        S905D2_GPIOAO(10)
#define GPIO_MIC_PRIVACY        S905D2_GPIOZ(2)

} // namespace astro
