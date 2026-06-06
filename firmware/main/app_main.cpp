/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <esp_wifi.h>

#include "app_log.h"
#include "button.h"
#include "matter_switch.h"
#include "rgb_led.h"
#include "servo_motor.h"
#include "servo_settings.h"
#include "servo_web.h"

#define CONFIG_APP_PIN_RGB_LED PIN_RGB_LED  //< 8 (defined in pins_arduino.h)
#define CONFIG_APP_PIN_SERVO_CTRL 20
#define CONFIG_APP_PIN_SERVO_POWER 19
#define CONFIG_APP_PIN_BUTTON BOOT_PIN

RgbLed led_(CONFIG_APP_PIN_RGB_LED);
Button button_(CONFIG_APP_PIN_BUTTON);
ServoMotor servo_;
MatterSwitch matter_;
ServoSettingsStore settings_store_;
ServoSettings settings_;
ServoWeb web_(settings_, settings_store_);

static void set_servo_for_switch(bool on, bool move_smoothly) {
  const float angle = on ? settings_.on_angle : settings_.off_angle;
  const float speed = move_smoothly ? settings_.max_speed_dps : 0.0f;
  servo_.setTargetDegree(angle, speed);
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
  ArduinoOTA.setTimeout(10000);  // 10s per chunk x 3 retries = 30s max stall
  ArduinoOTA.onStart([]() {
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(78);  // 78 * 0.25 = 19.5 dBm
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

  if (!settings_store_.begin()) {
    LOGE("[Prefs] Failed to open settings");
  }
  settings_ = settings_store_.load();
  matter_.begin(settings_.switch_on);

  ota_begin();
  web_.begin();
  servo_.begin(CONFIG_APP_PIN_SERVO_CTRL, CONFIG_APP_PIN_SERVO_POWER);
  set_servo_for_switch(settings_.switch_on, false);
}

void loop() {
  /* handle */
  yield();
  ArduinoOTA.handle();
  web_.handle();
  bool web_switch_on = false;
  if (web_.consumeRequestedSwitchState(web_switch_on)) {
    settings_.switch_on = web_switch_on;
    settings_store_.saveSwitchState(web_switch_on);
    matter_.setSwitchState(web_switch_on);
    set_servo_for_switch(web_switch_on, true);
    LOGI("[Web] Switch %s", web_switch_on ? "ON" : "OFF");
  }
  led_.update();
  button_.update();
  servo_.handle();

  /* handle event */
  MatterSwitch::Event event;
  if (matter_.getEvent(event, 0)) {
    led_.blinkOnce(RgbLed::Color::Blue);
    settings_.switch_on = event.switch_state;
    settings_store_.saveSwitchState(event.switch_state);
    LOGI("[Event] Switch %s", event.switch_state ? "ON" : "OFF");
    set_servo_for_switch(event.switch_state, true);
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
