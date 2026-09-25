/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#pragma once

#include <sdkconfig.h>

/* Pin Assign */
#if CONFIG_IDF_TARGET_ESP32C6

#define CONFIG_APP_PIN_RGB_LED 8  //< onboard addressable RGB LED
#define CONFIG_APP_PIN_BUTTON 9   //< BOOT button
#define CONFIG_APP_PIN_SERVO_CTRL 20
#define CONFIG_APP_PIN_SERVO_POWER 19

#else
#error "unsupported target"
#endif
