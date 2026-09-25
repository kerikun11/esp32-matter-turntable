/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#include "http/servo_web.h"

#include <cJSON.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "device_common/http/device_http.h"
#include "device_common/http/web_asset_http.h"
#include "device_common/http/web_utils.h"
#include "matter/matter_switch.h"
#include "web_assets.h"

namespace {

constexpr int64_t kActionTimeoutUs = 5000000;
constexpr int64_t kRebootDelayUs = 500000;

std::string requestHeader(httpd_req_t* req, const char* name) {
  const size_t length = httpd_req_get_hdr_value_len(req, name);
  if (length == 0) return {};
  std::string value(length + 1, '\0');
  if (httpd_req_get_hdr_value_str(req, name, value.data(), value.size()) != ESP_OK) return {};
  value.resize(length);
  return value;
}

// Parses a decimal integer that must fill the whole string.
bool parseInt(const std::string& text, int& value) {
  if (text.empty() || text.size() > 6) return false;
  char* end = nullptr;
  const long parsed = strtol(text.c_str(), &end, 10);
  if (*end != '\0') return false;
  value = static_cast<int>(parsed);
  return true;
}

bool addString(cJSON* object, const char* key, const char* value) {
  return cJSON_AddStringToObject(object, key, value) != nullptr;
}
bool addNumber(cJSON* object, const char* key, double value) {
  return cJSON_AddNumberToObject(object, key, value) != nullptr;
}
bool addBool(cJSON* object, const char* key, bool value) {
  return cJSON_AddBoolToObject(object, key, value) != nullptr;
}

esp_err_t sendJson(httpd_req_t* req, cJSON* root, bool ok) {
  char* json = ok ? cJSON_PrintUnformatted(root) : nullptr;
  cJSON_Delete(root);
  if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const esp_err_t result = httpd_resp_sendstr(req, json);
  cJSON_free(json);
  return result;
}

}  // namespace

void ServoWeb::begin() {
  auto config = device_common::httpServerConfig();
  if (httpd_start(&server_, &config) != ESP_OK) {
    LOGE("[Web] httpd_start failed");
    return;
  }

  const httpd_uri_t uris[] = {
      {.uri = "/", .method = HTTP_GET, .handler = &trampoline<&ServoWeb::handleRoot>, .user_ctx = this},
      {.uri = "/state", .method = HTTP_GET, .handler = &trampoline<&ServoWeb::handleState>, .user_ctx = this},
      {.uri = "/device-info", .method = HTTP_GET, .handler = &trampoline<&ServoWeb::handleDeviceInfo>, .user_ctx = this},
      {.uri = "/settings", .method = HTTP_POST, .handler = &trampoline<&ServoWeb::handleSaveSettings>, .user_ctx = this},
      {.uri = "/action", .method = HTTP_POST, .handler = &trampoline<&ServoWeb::handleAction>, .user_ctx = this},
      {.uri = "/move", .method = HTTP_POST, .handler = &trampoline<&ServoWeb::handleMove>, .user_ctx = this},
      {.uri = "/matter", .method = HTTP_POST, .handler = &trampoline<&ServoWeb::handleMatter>, .user_ctx = this},
      {.uri = "/reboot", .method = HTTP_POST, .handler = &trampoline<&ServoWeb::handleReboot>, .user_ctx = this},
  };
  for (const auto& uri : uris) httpd_register_uri_handler(server_, &uri);

  LOGI("[Web] HTTP server started on port 80");
}

void ServoWeb::setObservedState(const ObservedState& state) {
  Lock lock(mutex_);
  observed_ = state;
}

bool ServoWeb::consumeHostnameUpdated() {
  Lock lock(mutex_);
  const bool updated = hostname_updated_;
  hostname_updated_ = false;
  return updated;
}

bool ServoWeb::consumeRequestedSwitchState(bool& switch_on) {
  Lock lock(mutex_);
  int value = 0;
  if (!requested_switch_state_.consume(value)) return false;
  switch_on = value != 0;
  return true;
}

bool ServoWeb::consumeRequestedAngle(int& angle) {
  Lock lock(mutex_);
  return requested_angle_.consume(angle);
}

bool ServoWeb::consumeRebootRequested() {
  Lock lock(mutex_);
  if (!reboot_requested_ || esp_timer_get_time() < reboot_after_us_) return false;
  reboot_requested_ = false;
  return true;
}

void ServoWeb::completeAction() {
  Lock lock(mutex_);
  action_in_progress_ = false;
}

void ServoWeb::showStatus(const std::string& message, bool is_error) {
  Lock lock(mutex_);
  status_message_ = message;
  status_is_error_ = is_error;
}

void ServoWeb::requestReboot() {
  Lock lock(mutex_);
  reboot_requested_ = true;
  reboot_after_us_ = esp_timer_get_time() + kRebootDelayUs;
}

esp_err_t ServoWeb::handleRoot(httpd_req_t* req) {
  return device_common::sendWebPage(req, {kWebIdentity, sizeof(kWebIdentity), kWebIdentityEtag,
                                          kWebGzip, sizeof(kWebGzip), kWebGzipEtag});
}

esp_err_t ServoWeb::handleState(httpd_req_t* req) {
  // Polled periodically by the page; not logged to keep the console quiet.
  return sendState(req);
}

esp_err_t ServoWeb::handleSaveSettings(httpd_req_t* req) {
  logRequest(req);
  const auto fields = parseFormBody(req);
  const std::string device_name = trim(formValue(fields, "device_name"));
  const std::string hostname = trim(formValue(fields, "hostname"));
  int on_angle = -1, off_angle = -1, max_speed = 0;
  const bool numbers_ok = parseInt(formValue(fields, "on_angle"), on_angle) &&
                          parseInt(formValue(fields, "off_angle"), off_angle) &&
                          parseInt(formValue(fields, "max_speed"), max_speed);
  if (device_name.empty() || device_name.size() > ServoSettings::kDeviceNameMaxBytes) {
    showStatus("デバイス名は1〜64バイトで入力してください。設定は保存されませんでした。", true);
    return respondMutation(req);
  }
  if (!ServoSettings::isValidHostname(hostname)) {
    showStatus("ホスト名は英数字とハイフン（先頭・末尾以外）で63文字以内にしてください。設定は保存されませんでした。", true);
    return respondMutation(req);
  }
  if (!numbers_ok ||
      on_angle < ServoSettings::kAngleMin || on_angle > ServoSettings::kAngleMax ||
      off_angle < ServoSettings::kAngleMin || off_angle > ServoSettings::kAngleMax ||
      max_speed < ServoSettings::kMaxSpeedMin || max_speed > ServoSettings::kMaxSpeedMax) {
    showStatus("角度・速度の入力内容を確認してください。設定は保存されませんでした。", true);
    return respondMutation(req);
  }

  ServoSettings snapshot;
  {
    Lock lock(mutex_);
    hostname_updated_ = settings_.hostname != hostname;
    settings_.device_name = device_name;
    settings_.hostname = hostname;
    settings_.on_angle = on_angle;
    settings_.off_angle = off_angle;
    settings_.max_speed_dps = max_speed;
    snapshot = settings_;
  }
  settings_store_.save(snapshot);
  LOGI("[Web] Settings saved");
  showStatus("設定を保存しました。角度は次回のON/OFF操作から反映されます。");
  return respondMutation(req);
}

esp_err_t ServoWeb::handleAction(httpd_req_t* req) {
  logRequest(req);
  const auto fields = parseFormBody(req);
  const std::string state = formValue(fields, "state");
  if (state != "on" && state != "off") {
    LOGW("[Web] action rejected: state must be on/off, got '%s'", state.c_str());
    showStatus("操作内容が不正です。", true);
    return respondMutation(req);
  }
  return runAction(req, &ServoWeb::requested_switch_state_, state == "on" ? 1 : 0);
}

esp_err_t ServoWeb::handleMove(httpd_req_t* req) {
  logRequest(req);
  const auto fields = parseFormBody(req);
  int angle = -1;
  if (!parseInt(formValue(fields, "angle"), angle) ||
      angle < ServoSettings::kAngleMin || angle > ServoSettings::kAngleMax) {
    showStatus("角度は0〜180度で指定してください。", true);
    return respondMutation(req);
  }
  return runAction(req, &ServoWeb::requested_angle_, angle);
}

esp_err_t ServoWeb::runAction(httpd_req_t* req, PendingValue ServoWeb::* slot,
                              int value) {
  {
    Lock lock(mutex_);
    if (action_in_progress_) {
      httpd_resp_set_status(req, "503 Service Unavailable");
      return httpd_resp_sendstr(req, "Previous action is still running");
    }
    action_in_progress_ = true;
    (this->*slot).request(value);
  }
  // Do not respond until the app task has committed the request and
  // published the resulting state. Never hold the mutex while sleeping.
  const int64_t deadline = esp_timer_get_time() + kActionTimeoutUs;
  while (true) {
    {
      Lock lock(mutex_);
      if (!action_in_progress_) break;
      if (esp_timer_get_time() >= deadline) {
        // Leave the request queued (the app task still applies it) but let
        // later requests through instead of rejecting them forever.
        action_in_progress_ = false;
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Action result is not available yet");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return respondMutation(req);
}

esp_err_t ServoWeb::handleMatter(httpd_req_t* req) {
  const auto result = device_common::handleMatterAction(req);
  showStatus(result.message, result.failed);
  return respondMutation(req);
}

esp_err_t ServoWeb::handleReboot(httpd_req_t* req) {
  logRequest(req);
  requestReboot();
  showStatus("再起動しています。");
  return respondMutation(req);
}

esp_err_t ServoWeb::respondMutation(httpd_req_t* req) {
  if (requestHeader(req, "Accept").find("application/json") != std::string::npos) {
    return sendState(req);
  }
  redirectRoot(req);
  return ESP_OK;
}

esp_err_t ServoWeb::sendState(httpd_req_t* req) {
  cJSON* state = cJSON_CreateObject();
  if (!state) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
  std::string message;
  bool ok = true;
  {
    Lock lock(mutex_);
    message = status_message_;
    ok &= addBool(state, "switch", observed_.switch_on);
    ok &= addBool(state, "moving", observed_.moving);
    ok &= addBool(state, "powered", observed_.powered);
    ok &= addNumber(state, "angle", observed_.angle);
    ok &= addNumber(state, "target_angle", observed_.target_angle);
    ok &= addBool(state, "commissioned", observed_.commissioned);
    ok &= addBool(state, "commissioning_open", observed_.commissioning_open);
    ok &= addString(state, "device_name", settings_.device_name.c_str());
    ok &= addString(state, "hostname", settings_.hostname.c_str());
    ok &= addNumber(state, "on_angle", settings_.on_angle);
    ok &= addNumber(state, "off_angle", settings_.off_angle);
    ok &= addNumber(state, "max_speed", settings_.max_speed_dps);
    ok &= addString(state, "message", message.c_str());
    ok &= addBool(state, "error", status_is_error_);
    ok &= addBool(state, "reboot", reboot_requested_);
  }
  const esp_err_t result = sendJson(req, state, ok);
  if (result == ESP_OK && !message.empty()) {
    // A status message is delivered once, to the response that follows it.
    Lock lock(mutex_);
    if (status_message_ == message) {
      status_message_.clear();
      status_is_error_ = false;
    }
  }
  return result;
}

// Read Matter under its stack lock, independently of the settings mutex.
esp_err_t ServoWeb::handleDeviceInfo(httpd_req_t* req) {
  return device_common::sendDeviceInfo(req, MatterSwitch::kManualCode, MatterSwitch::kQrPayload);
}
