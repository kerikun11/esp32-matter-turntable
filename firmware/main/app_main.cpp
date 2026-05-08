/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Preferences.h>

#include "app_log.h"
#include "button.h"
#include "matter_switch.h"
#include "rgb_led.h"
#include "servo_motor.h"

#define CONFIG_APP_PIN_RGB_LED PIN_RGB_LED  //< 8 (defined in pins_arduino.h)
#define CONFIG_APP_PIN_SERVO_CTRL 20
#define CONFIG_APP_PIN_SERVO_POWER 19
#define CONFIG_APP_PIN_BUTTON BOOT_PIN

RgbLed led_(CONFIG_APP_PIN_RGB_LED);
Button button_(CONFIG_APP_PIN_BUTTON);
ServoMotor servo_;
MatterSwitch matter_;

static constexpr const char *kPrefsNamespace = "app";
static constexpr const char *kPrefsSwitchOnKey = "switch_on";

static bool load_switch_state() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    LOGI("[Prefs] Failed to open, using default ON");
    return true;
  }
  bool on = prefs.getBool(kPrefsSwitchOnKey, true);
  prefs.end();
  LOGI("[Prefs] Restored switch: %s", on ? "ON" : "OFF");
  return on;
}

static void save_switch_state(bool on) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    LOGI("[Prefs] Failed to save switch: %s", on ? "ON" : "OFF");
    return;
  }
  prefs.putBool(kPrefsSwitchOnKey, on);
  prefs.end();
  LOGI("[Prefs] Saved switch: %s", on ? "ON" : "OFF");
}

static void set_servo_for_switch(bool on, float speed_dps) {
  servo_.setTargetDegree(on ? 180.0f : 0.0f, speed_dps);
}

static const char *ota_error_name(ota_error_t error) {
  switch (error) {
    case OTA_AUTH_ERROR:
      return "auth";
    case OTA_BEGIN_ERROR:
      return "begin";
    case OTA_CONNECT_ERROR:
      return "connect";
    case OTA_RECEIVE_ERROR:
      return "receive";
    case OTA_END_ERROR:
      return "end";
    default:
      return "unknown";
  }
}

static void ota_begin() {
  ArduinoOTA.setMdnsEnabled(false);  // to avoid Matter mDNS conflict
  ArduinoOTA.setTimeout(10000);
  ArduinoOTA.onStart([]() {
    auto cmd = ArduinoOTA.getCommand();
    servo_.free();
    LOGI("[OTA] Start updating %s",
         cmd == U_FLASH ? "sketch"
                        : (cmd == U_SPIFFS ? "filesystem" : "unknown"));
  });
  ArduinoOTA.onEnd([]() { LOGI("[OTA] End"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    LOGI("[OTA] Progress: %u%% (%d/%d)", 100 * progress / total, progress,
         total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    LOGI("[OTA] Error: %s (%d)", ota_error_name(error), error);
  });
  ArduinoOTA.begin();
}

void setup() {
  Serial.begin(CONFIG_MONITOR_BAUD);

  bool switch_on = load_switch_state();
  matter_.begin(switch_on);

  ota_begin();
  servo_.begin(CONFIG_APP_PIN_SERVO_CTRL, CONFIG_APP_PIN_SERVO_POWER);
  set_servo_for_switch(switch_on, 0.0f);
}

void loop() {
  /* handle */
  yield();
  ArduinoOTA.handle();
  led_.update();
  button_.update();
  servo_.handle();

  /* handle event */
  MatterSwitch::Event event;
  if (matter_.getEvent(event, 0)) {
    led_.blinkOnce(RgbLed::Color::Blue);
    save_switch_state(event.switch_state);
    LOGI("[Event] Switch %s", event.switch_state ? "ON" : "OFF");
    set_servo_for_switch(event.switch_state, 180.0f);
  }

  /* Matter Decommission */
  if (button_.longHoldStarted()) led_.blinkOnce(RgbLed::Color::Magenta);
  if (button_.longPressed()) {
    matter_.decommission();
  }
  if (!matter_.isCommissioned()) {
    static long last_pairing_log_ms_ = 0;
    long now = millis();
    if (now - last_pairing_log_ms_ > 10000) {
      last_pairing_log_ms_ = now;
      matter_.printOnboarding();
    }
  }

  /* LED Status */
  if (!matter_.isCommissioned()) {
    led_.setBackground(RgbLed::Color::Magenta);
  } else if (!matter_.isConnected()) {
    led_.setBackground(RgbLed::Color::Red);
  } else {
    led_.setBackground(RgbLed::Color::White);
  }
}
