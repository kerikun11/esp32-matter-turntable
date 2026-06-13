/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <mdns.h>

#include "app_log.h"
#include "button.h"
#include "matter_switch.h"
#include "ota_utils.h"
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
String mdns_hostname_;
uint32_t mdns_ipv4_address_ = 0;
unsigned long last_mdns_sync_attempt_ms_ = 0;
esp_err_t last_mdns_error_ = ESP_OK;
volatile bool dhcp_restart_requested_ = false;

static void set_servo_for_switch(bool on, bool move_smoothly) {
  const float angle = on ? settings_.on_angle : settings_.off_angle;
  const float speed = move_smoothly ? settings_.max_speed_dps : 0.0f;
  servo_.setTargetDegree(angle, speed);
}

static void ota_begin() {
  ArduinoOTA.setHostname(settings_.hostname.c_str());
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

static void ip_event_handler(void *, esp_event_base_t event_base,
                             int32_t event_id, void *) {
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    dhcp_restart_requested_ = true;
  }
}

static void register_ipv4_recovery() {
  const esp_err_t err = esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_LOST_IP, ip_event_handler, nullptr);
  if (err != ESP_OK) {
    LOGW("[Net] Failed to register IPv4 recovery: %s", esp_err_to_name(err));
  }
}

static void restart_dhcp_if_requested() {
  if (!dhcp_restart_requested_) return;
  dhcp_restart_requested_ = false;

  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) return;

  const esp_err_t stop_err = esp_netif_dhcpc_stop(netif);
  if (stop_err != ESP_OK &&
      stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
    LOGW("[Net] Failed to stop DHCP client: %s", esp_err_to_name(stop_err));
    return;
  }

  const esp_err_t start_err = esp_netif_dhcpc_start(netif);
  if (start_err == ESP_OK ||
      start_err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
    LOGI("[Net] DHCP client restarted");
  } else {
    LOGW("[Net] Failed to restart DHCP client: %s",
         esp_err_to_name(start_err));
  }
}

static void sync_additional_mdns_hostname(bool force) {
  constexpr unsigned long kRetryIntervalMs = 1000;
  const unsigned long now = millis();
  if (!force && now - last_mdns_sync_attempt_ms_ < kRetryIntervalMs) return;
  last_mdns_sync_attempt_ms_ = now;

  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip_info{};
  if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
      ip_info.ip.addr == 0) {
    return;
  }

  if (!mdns_hostname_.isEmpty() && mdns_hostname_ != settings_.hostname) {
    const esp_err_t err = mdns_delegate_hostname_remove(mdns_hostname_.c_str());
    if (err != ESP_OK) {
      LOGW("[mDNS] Failed to remove %s.local: %s", mdns_hostname_.c_str(),
           esp_err_to_name(err));
      return;
    }
    LOGI("[mDNS] Removed additional hostname: %s.local",
         mdns_hostname_.c_str());
    mdns_hostname_ = "";
    mdns_ipv4_address_ = 0;
  }

  mdns_ip_addr_t address{};
  address.addr.type = ESP_IPADDR_TYPE_V4;
  address.addr.u_addr.ip4 = ip_info.ip;

  if (mdns_hostname_.isEmpty()) {
    const esp_err_t err =
        mdns_delegate_hostname_add(settings_.hostname.c_str(), &address);
    if (err != ESP_OK) {
      if (err != last_mdns_error_) {
        LOGW("[mDNS] Failed to add %s.local: %s", settings_.hostname.c_str(),
             esp_err_to_name(err));
      }
      last_mdns_error_ = err;
      return;
    }
    last_mdns_error_ = ESP_OK;
    if (!mdns_hostname_exists(settings_.hostname.c_str())) {
      char primary_hostname[MDNS_NAME_BUF_LEN] = {};
      const esp_err_t get_err = mdns_hostname_get(primary_hostname);
      LOGW("[mDNS] %s.local was not added (primary: %s)",
           settings_.hostname.c_str(),
           get_err == ESP_OK ? primary_hostname : "unavailable");
      return;
    }
    mdns_hostname_ = settings_.hostname;
    mdns_ipv4_address_ = ip_info.ip.addr;
    LOGI("[mDNS] Added additional hostname: %s.local -> " IPSTR,
         mdns_hostname_.c_str(), IP2STR(&ip_info.ip));
    return;
  }

  if (mdns_ipv4_address_ == ip_info.ip.addr) return;
  const esp_err_t err =
      mdns_delegate_hostname_set_address(mdns_hostname_.c_str(), &address);
  if (err != ESP_OK) {
    LOGW("[mDNS] Failed to update %s.local: %s", mdns_hostname_.c_str(),
         esp_err_to_name(err));
    return;
  }
  mdns_ipv4_address_ = ip_info.ip.addr;
  LOGI("[mDNS] Updated address: %s.local -> " IPSTR, mdns_hostname_.c_str(),
       IP2STR(&ip_info.ip));
}

void setup() {
  Serial.begin(CONFIG_MONITOR_BAUD);

  if (!settings_store_.begin()) {
    LOGE("[Prefs] Failed to open settings");
  }
  settings_ = settings_store_.load();
  matter_.begin(settings_.switch_on);
  if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
    LOGW("[Wi-Fi] Failed to disable power save");
  }
  register_ipv4_recovery();

  ota_begin();
  web_.begin();
  servo_.begin(CONFIG_APP_PIN_SERVO_CTRL, CONFIG_APP_PIN_SERVO_POWER);
  set_servo_for_switch(settings_.switch_on, false);
}

void loop() {
  /* handle */
  yield();
  restart_dhcp_if_requested();
  ArduinoOTA.handle();
  web_.handle();
  if (web_.hostnameUpdated()) {
    ArduinoOTA.setHostname(settings_.hostname.c_str());
    sync_additional_mdns_hostname(true);
    web_.clearHostnameUpdated();
  } else {
    sync_additional_mdns_hostname(false);
  }
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
    if (matter_.isCommissioned()) {
      matter_.decommission();
    } else {
      matter_.openCommissioningWindow();
    }
  }
  if (!matter_.isCommissioned()) {
    static long last_pairing_log_ms_ = 0;
    const long now = millis();
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
