/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#include "servo_settings.h"

#include "app_log.h"

bool ServoSettingsStore::begin() {
  return prefs_.begin(ServoSettings::kPrefNamespace);
}

ServoSettings ServoSettingsStore::load() {
  ServoSettings settings;
  settings.on_angle =
      prefs_.getInt(ServoSettings::kPrefOnAngle,
                    ServoSettings::kOnAngleDefault);
  settings.off_angle =
      prefs_.getInt(ServoSettings::kPrefOffAngle,
                    ServoSettings::kOffAngleDefault);
  settings.max_speed_dps =
      prefs_.getInt(ServoSettings::kPrefMaxSpeed,
                    ServoSettings::kMaxSpeedDefault);
  settings.switch_on = prefs_.getBool(ServoSettings::kPrefSwitchOn, true);

  LOGI("[Prefs] ON angle: %d", settings.on_angle);
  LOGI("[Prefs] OFF angle: %d", settings.off_angle);
  LOGI("[Prefs] max speed: %d deg/s", settings.max_speed_dps);
  LOGI("[Prefs] switch: %s", settings.switch_on ? "ON" : "OFF");
  return settings;
}

void ServoSettingsStore::save(const ServoSettings &settings) {
  prefs_.putInt(ServoSettings::kPrefOnAngle, settings.on_angle);
  prefs_.putInt(ServoSettings::kPrefOffAngle, settings.off_angle);
  prefs_.putInt(ServoSettings::kPrefMaxSpeed, settings.max_speed_dps);
}

void ServoSettingsStore::saveSwitchState(bool on) {
  prefs_.putBool(ServoSettings::kPrefSwitchOn, on);
  LOGI("[Prefs] Saved switch: %s", on ? "ON" : "OFF");
}
