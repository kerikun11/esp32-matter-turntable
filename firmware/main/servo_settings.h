/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#pragma once

#include <Preferences.h>

struct ServoSettings {
  static constexpr const char *kPrefNamespace = "app";
  static constexpr const char *kPrefDeviceName = "device_name";
  static constexpr const char *kPrefOnAngle = "on_angle";
  static constexpr const char *kPrefOffAngle = "off_angle";
  static constexpr const char *kPrefMaxSpeed = "max_speed";
  static constexpr const char *kPrefSwitchOn = "switch_on";

  static constexpr const char *kDeviceNameDefault = "Matter Servo";
  static constexpr int kOnAngleDefault = 180;
  static constexpr int kOffAngleDefault = 0;
  static constexpr int kMaxSpeedDefault = 180;

  String device_name = kDeviceNameDefault;
  int on_angle = kOnAngleDefault;
  int off_angle = kOffAngleDefault;
  int max_speed_dps = kMaxSpeedDefault;
  bool switch_on = true;
};

class ServoSettingsStore {
 public:
  bool begin();
  ServoSettings load();
  void save(const ServoSettings &settings);
  void saveSwitchState(bool on);

 private:
  Preferences prefs_;
};
