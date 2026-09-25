/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#include "settings/servo_settings.h"

#include <algorithm>

#include "device_common/system/app_log.h"

namespace {

int clampInt(int value, int lo, int hi) { return std::min(std::max(value, lo), hi); }

}  // namespace

bool ServoSettings::isValidHostname(const std::string& hostname) {
  if (hostname.empty() || hostname.size() > kHostnameMaxLength) return false;
  if (hostname.front() == '-' || hostname.back() == '-') return false;
  return std::all_of(hostname.begin(), hostname.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-';
  });
}

bool ServoSettingsStore::begin() {
  return storage_.open(ServoSettings::kPrefNamespace);
}

ServoSettings ServoSettingsStore::load() {
  ServoSettings settings;
  settings.device_name = storage_.readString(ServoSettings::kPrefDeviceName,
                                             ServoSettings::kDeviceNameDefault);
  settings.hostname = storage_.readString(ServoSettings::kPrefHostname,
                                          ServoSettings::kHostnameDefault);
  settings.on_angle = storage_.readInt32(ServoSettings::kPrefOnAngle,
                                         ServoSettings::kOnAngleDefault);
  settings.off_angle = storage_.readInt32(ServoSettings::kPrefOffAngle,
                                          ServoSettings::kOffAngleDefault);
  settings.max_speed_dps = storage_.readInt32(ServoSettings::kPrefMaxSpeed,
                                              ServoSettings::kMaxSpeedDefault);
  settings.switch_on = storage_.readBool(ServoSettings::kPrefSwitchOn, true);

  // Sanitize values that older firmware accepted without validation, so a
  // corrupted or out-of-range value can never drive the servo or mDNS.
  if (settings.device_name.empty() ||
      settings.device_name.size() > ServoSettings::kDeviceNameMaxBytes) {
    settings.device_name = ServoSettings::kDeviceNameDefault;
  }
  if (!ServoSettings::isValidHostname(settings.hostname)) {
    LOGW("[NVS] invalid hostname '%s'; using default", settings.hostname.c_str());
    settings.hostname = ServoSettings::kHostnameDefault;
  }
  settings.on_angle = clampInt(settings.on_angle, ServoSettings::kAngleMin, ServoSettings::kAngleMax);
  settings.off_angle = clampInt(settings.off_angle, ServoSettings::kAngleMin, ServoSettings::kAngleMax);
  settings.max_speed_dps = clampInt(settings.max_speed_dps, ServoSettings::kMaxSpeedMin, ServoSettings::kMaxSpeedMax);

  LOGI("[NVS] device name: %s", settings.device_name.c_str());
  LOGI("[NVS] hostname: %s", settings.hostname.c_str());
  LOGI("[NVS] ON angle: %d", settings.on_angle);
  LOGI("[NVS] OFF angle: %d", settings.off_angle);
  LOGI("[NVS] max speed: %d deg/s", settings.max_speed_dps);
  LOGI("[NVS] switch: %s", settings.switch_on ? "ON" : "OFF");
  return settings;
}

void ServoSettingsStore::save(const ServoSettings& settings) {
  storage_.writeString(ServoSettings::kPrefDeviceName, settings.device_name);
  storage_.writeString(ServoSettings::kPrefHostname, settings.hostname);
  storage_.writeInt32(ServoSettings::kPrefOnAngle, settings.on_angle);
  storage_.writeInt32(ServoSettings::kPrefOffAngle, settings.off_angle);
  storage_.writeInt32(ServoSettings::kPrefMaxSpeed, settings.max_speed_dps);
}

void ServoSettingsStore::saveSwitchState(bool on) {
  storage_.writeBool(ServoSettings::kPrefSwitchOn, on);
  LOGI("[NVS] Saved switch: %s", on ? "ON" : "OFF");
}
