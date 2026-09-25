/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#pragma once

#include <cstdint>

#include "board/app_config.h"
#include "device_common/drivers/button.h"
#include "device_common/drivers/rgb_led.h"
#include "device_common/network/network_health.h"
#include "device_common/system/lockable.h"
#include "drivers/servo_motor.h"
#include "http/servo_web.h"
#include "matter/matter_switch.h"
#include "settings/servo_settings.h"

class TurntableController {
 public:
  TurntableController();

  bool begin();
  void handle();

 private:
  enum class Source {
    kMatter,
    kWeb,
    kButton
  };

  Button btn_{CONFIG_APP_PIN_BUTTON};
  RgbLed led_{CONFIG_APP_PIN_RGB_LED};
  ServoMotor servo_;
  MatterSwitch matter_;
  NetworkHealth network_;
  ServoSettingsStore settings_store_;
  ServoSettings settings_;
  // Guards settings_, which is shared between this class (main app task)
  // and web_ (esp_http_server's worker task).
  Mutex settings_mutex_;
  ServoWeb web_;

  MatterSwitch::Status matter_status_;
  int64_t last_matter_status_ms_ = -1;
  int64_t last_pairing_log_ms_ = 0;

  void applySwitchState(bool on, Source source);
  void moveServoForSwitch(bool on, bool move_smoothly);
  void updateMatterStatus();
  void publishObservedState();
  void updateStatusLed();
  void handleButton();
};
