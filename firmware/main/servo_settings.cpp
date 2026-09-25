/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#include "servo_settings.h"

#include <algorithm>

#include "app_log.h"

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
  return prefs_.begin(ServoSettings::kPrefNamespace);
}

ServoSettings ServoSettingsStore::load() {
  ServoSettings settings;
  settings.device_name = prefs_.getString(ServoSettings::kPrefDeviceName,
                                          ServoSettings::kDeviceNameDefault);
  settings.hostname = prefs_.getString(ServoSettings::kPrefHostname,
                                       ServoSettings::kHostnameDefault);
  settings.on_angle = prefs_.getInt(ServoSettings::kPrefOnAngle,
                                    ServoSettings::kOnAngleDefault);
  settings.off_angle = prefs_.getInt(ServoSettings::kPrefOffAngle,
                                     ServoSettings::kOffAngleDefault);
  settings.max_speed_dps = prefs_.getInt(ServoSettings::kPrefMaxSpeed,
                                         ServoSettings::kMaxSpeedDefault);
  settings.switch_on = prefs_.getBool(ServoSettings::kPrefSwitchOn, true);

  // Sanitize values that older firmware accepted without validation, so a
  // corrupted or out-of-range value can never drive the servo or mDNS.
  if (settings.device_name.empty() ||
      settings.device_name.size() > ServoSettings::kDeviceNameMaxBytes) {
    settings.device_name = ServoSettings::kDeviceNameDefault;
  }
  if (!ServoSettings::isValidHostname(settings.hostname)) {
    LOGW("[Prefs] invalid hostname '%s'; using default", settings.hostname.c_str());
    settings.hostname = ServoSettings::kHostnameDefault;
  }
  settings.on_angle = clampInt(settings.on_angle, ServoSettings::kAngleMin, ServoSettings::kAngleMax);
  settings.off_angle = clampInt(settings.off_angle, ServoSettings::kAngleMin, ServoSettings::kAngleMax);
  settings.max_speed_dps = clampInt(settings.max_speed_dps, ServoSettings::kMaxSpeedMin, ServoSettings::kMaxSpeedMax);

  LOGI("[Prefs] device name: %s", settings.device_name.c_str());
  LOGI("[Prefs] hostname: %s", settings.hostname.c_str());
  LOGI("[Prefs] ON angle: %d", settings.on_angle);
  LOGI("[Prefs] OFF angle: %d", settings.off_angle);
  LOGI("[Prefs] max speed: %d deg/s", settings.max_speed_dps);
  LOGI("[Prefs] switch: %s", settings.switch_on ? "ON" : "OFF");
  return settings;
}

void ServoSettingsStore::save(const ServoSettings& settings) {
  prefs_.putString(ServoSettings::kPrefDeviceName, settings.device_name);
  prefs_.putString(ServoSettings::kPrefHostname, settings.hostname);
  prefs_.putInt(ServoSettings::kPrefOnAngle, settings.on_angle);
  prefs_.putInt(ServoSettings::kPrefOffAngle, settings.off_angle);
  prefs_.putInt(ServoSettings::kPrefMaxSpeed, settings.max_speed_dps);
}

void ServoSettingsStore::saveSwitchState(bool on) {
  prefs_.putBool(ServoSettings::kPrefSwitchOn, on);
  LOGI("[Prefs] Saved switch: %s", on ? "ON" : "OFF");
}
