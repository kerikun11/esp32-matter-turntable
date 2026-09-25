/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#pragma once

#include <string>

#include "preferences.h"

struct ServoSettings {
  // Same namespace/keys/types as the former Arduino Preferences storage, so
  // settings survive the update from the arduino-esp32 based firmware.
  static constexpr const char* kPrefNamespace = "app";
  static constexpr const char* kPrefDeviceName = "device_name";
  static constexpr const char* kPrefHostname = "hostname";
  static constexpr const char* kPrefOnAngle = "on_angle";
  static constexpr const char* kPrefOffAngle = "off_angle";
  static constexpr const char* kPrefMaxSpeed = "max_speed";
  static constexpr const char* kPrefSwitchOn = "switch_on";

  static constexpr const char* kDeviceNameDefault = "Matter Turntable";
  static constexpr const char* kHostnameDefault = "esp32-matter-turntable";
  static constexpr int kOnAngleDefault = 180;
  static constexpr int kOffAngleDefault = 0;
  static constexpr int kMaxSpeedDefault = 180;

  static constexpr int kAngleMin = 0;
  static constexpr int kAngleMax = 180;
  static constexpr int kMaxSpeedMin = 1;
  static constexpr int kMaxSpeedMax = 720;
  static constexpr size_t kDeviceNameMaxBytes = 64;
  static constexpr size_t kHostnameMaxLength = 63;

  std::string device_name = kDeviceNameDefault;
  std::string hostname = kHostnameDefault;
  int on_angle = kOnAngleDefault;
  int off_angle = kOffAngleDefault;
  int max_speed_dps = kMaxSpeedDefault;
  bool switch_on = true;

  // A DNS label usable as "<hostname>.local": 1-63 chars of [A-Za-z0-9-],
  // not starting or ending with '-' (mDNS names are case-insensitive).
  static bool isValidHostname(const std::string& hostname);
};

class ServoSettingsStore {
 public:
  bool begin();
  ServoSettings load();
  void save(const ServoSettings& settings);
  void saveSwitchState(bool on);

 private:
  Preferences prefs_;
};
