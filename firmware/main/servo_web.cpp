/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#include "servo_web.h"

#include <app/server/Server.h>
#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/task.h>
#include <platform/PlatformManager.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "matter_switch.h"
#include "web_asset_http.h"
#include "web_assets.h"
#include "web_utils.h"

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

const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "電源投入";
    case ESP_RST_SW:
      return "ソフトウェア再起動";
    case ESP_RST_PANIC:
      return "例外（パニック）";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return "ウォッチドッグ";
    case ESP_RST_BROWNOUT:
      return "電圧低下（ブラウンアウト）";
    case ESP_RST_DEEPSLEEP:
      return "ディープスリープ復帰";
    case ESP_RST_EXT:
      return "外部リセット";
    case ESP_RST_USB:
      return "USB";
    default:
      return "不明";
  }
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
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  // Fabric removal synchronously runs Matter cleanup callbacks. The HTTPD
  // default (4096 bytes) is too small for that call chain on ESP32-C6 and
  // trips the stack protector inside newlib formatting code.
  config.stack_size = 8192;
  config.max_uri_handlers = 12;
  // Reclaim the least-recently-used socket instead of making a new client
  // wait for an idle keep-alive connection to hit recv_wait_timeout (5s).
  config.lru_purge_enable = true;
  // Right after boot (Matter operational discovery / CASE re-establishment)
  // the Wi-Fi TX path was observed to stall for more than the default 5s,
  // aborting a page transfer midway with "error in send : 11". Allow
  // longer stalls, still below the web UI's own 20s request timeout.
  config.send_wait_timeout = 15;
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
  logRequest(req);
  // Compression is done at build time: send flash-resident bytes directly.
  const std::string accept = requestHeader(req, "Accept-Encoding");
  const bool gzip = web_asset::quality(accept, "gzip") > 0;
  if (!gzip && web_asset::quality(accept, "identity") == 0) {
    httpd_resp_set_status(req, "406 Not Acceptable");
    return httpd_resp_sendstr(req, "No supported content encoding");
  }
  const char* etag = gzip ? kWebGzipEtag : kWebIdentityEtag;
  const bool unchanged = web_asset::etagMatches(requestHeader(req, "If-None-Match"), etag);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
  // Revalidate the static shell after firmware updates; state is never cached.
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "ETag", etag);
  if (gzip) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  if (unchanged) {
    httpd_resp_set_status(req, "304 Not Modified");
    return httpd_resp_send(req, nullptr, 0);
  }
  return httpd_resp_send(req,
                         reinterpret_cast<const char*>(gzip ? kWebGzip : kWebIdentity),
                         gzip ? sizeof(kWebGzip) : sizeof(kWebIdentity));
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
  logRequest(req);
  const auto fields = parseFormBody(req);
  const auto action = formValue(fields, "action");
  std::string message;
  bool failed = false;
  // Do not hold the settings mutex while taking the stack lock or invoking
  // fabric callbacks (the app task takes them in the opposite order).
  {
    chip::DeviceLayer::StackLock lock;
    auto& server = chip::Server::GetInstance();
    auto& table = server.GetFabricTable();
    auto& window = server.GetCommissioningWindowManager();
    if (action == "commission") {
      if (window.IsCommissioningWindowOpen()) {
        message = "ペアリング受付はすでに開始されています。";
      } else {
        const auto err = window.OpenBasicCommissioningWindow(
            chip::System::Clock::Seconds32(300));
        failed = err != CHIP_NO_ERROR;
        message = failed ? "ペアリング受付を開始できませんでした。"
                         : "ペアリング受付を開始しました（最大5分間）。";
        if (failed) LOGE("[Web] Commissioning failed: %" CHIP_ERROR_FORMAT, err.Format());
      }
    } else if (action == "remove") {
      const auto index_text = formValue(fields, "index");
      int index = 0;
      const auto* fabric = parseInt(index_text, index) && index >= 1 && index <= 254 &&
                                   index_text == std::to_string(index)
                               ? table.FindFabricWithIndex(static_cast<chip::FabricIndex>(index))
                               : nullptr;
      char fabric_id[19] = {}, node_id[19] = {}, vendor_id[7] = {};
      if (fabric) {
        snprintf(fabric_id, sizeof(fabric_id), "0x%016" PRIX64, fabric->GetFabricId());
        snprintf(node_id, sizeof(node_id), "0x%016" PRIX64, fabric->GetNodeId());
        snprintf(vendor_id, sizeof(vendor_id), "0x%04X", static_cast<unsigned>(fabric->GetVendorId()));
      }
      // Reject stale pages if an index has since been reused by another fabric.
      if (!fabric || formValue(fields, "fabric_id") != fabric_id ||
          formValue(fields, "node_id") != node_id ||
          formValue(fields, "vendor_id") != vendor_id) {
        failed = true;
        message = "削除対象が見つからないか、登録情報が変わっています。一覧を確認してください。";
      } else {
        const auto err = table.Delete(static_cast<chip::FabricIndex>(index));
        failed = err != CHIP_NO_ERROR;
        message = failed ? "Fabricを削除できませんでした。"
                         : "Fabric #" + index_text + "を削除しました。Wi-Fi接続は維持されます。";
        if (!failed && table.FabricCount() == 0 && !window.IsCommissioningWindowOpen()) {
          message += "再登録するにはペアリング受付を開始してください。";
        }
        if (failed) LOGE("[Web] Fabric removal failed: %" CHIP_ERROR_FORMAT, err.Format());
      }
    } else {
      failed = true;
      message = "Matterの操作内容が不正です。";
    }
  }
  showStatus(message, failed);
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
  cJSON* info = cJSON_CreateObject();
  if (!info) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
  bool ok = true;
  const auto* app = esp_app_get_description();
  char build_date[40];
  snprintf(build_date, sizeof(build_date), "%s %s", app->date, app->time);
  ok &= addString(info, "project_name", app->project_name);
  ok &= addString(info, "version", app->version);
  ok &= addString(info, "build_date", build_date);
  ok &= addString(info, "idf_version", app->idf_ver);
  ok &= addNumber(info, "uptime_seconds", static_cast<double>(esp_timer_get_time() / 1000000));
  ok &= addString(info, "reset_reason", resetReasonName(esp_reset_reason()));
  ok &= addNumber(info, "free_heap", esp_get_free_heap_size());
  ok &= addNumber(info, "min_free_heap", esp_get_minimum_free_heap_size());
  ok &= addNumber(info, "largest_free_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  wifi_ap_record_t ap{};
  const bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  const std::string ssid(reinterpret_cast<const char*>(ap.ssid),
                         strnlen(reinterpret_cast<const char*>(ap.ssid), sizeof(ap.ssid)));
  ok &= addBool(info, "connected", connected);
  ok &= addString(info, "ssid", ssid.c_str());
  ok &= addNumber(info, "rssi", ap.rssi);
  ok &= addNumber(info, "channel", ap.primary);
  auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  uint8_t mac[6] = {};
  char address[48] = {};
  if (netif && esp_netif_get_mac(netif, mac) == ESP_OK) {
    snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  ok &= addString(info, "mac", address);
  address[0] = '\0';
  esp_netif_ip_info_t ip{};
  if (connected && netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr) {
    snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
  }
  ok &= addString(info, "ipv4", address);
  auto* ipv6 = cJSON_AddArrayToObject(info, "ipv6");
  ok &= ipv6 != nullptr;
  if (connected && netif && ipv6) {
    esp_ip6_addr_t addresses[CONFIG_LWIP_IPV6_NUM_ADDRESSES]{};
    const int count = esp_netif_get_all_preferred_ip6(netif, addresses);
    for (int i = 0; i < count; ++i) {
      snprintf(address, sizeof(address), IPV6STR, IPV62STR(addresses[i]));
      auto* item = cJSON_CreateString(address);
      if (!item || !cJSON_AddItemToArray(ipv6, item)) {
        cJSON_Delete(item);
        ok = false;
      }
    }
  }
  ok &= addString(info, "manual_code", MatterSwitch::kManualCode);
  ok &= addString(info, "qr_payload", MatterSwitch::kQrPayload);
  auto* fabrics = cJSON_AddArrayToObject(info, "fabrics");
  ok &= fabrics != nullptr;
  if (fabrics) {
    chip::DeviceLayer::StackLock lock;
    ok &= addBool(info, "commissioning_open",
                  chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen());
    for (const auto& fabric : chip::Server::GetInstance().GetFabricTable()) {
      auto* item = cJSON_CreateObject();
      if (!item) {
        ok = false;
        break;
      }
      const auto label = fabric.GetFabricLabel();
      const std::string label_text(label.data(), label.size());
      char node_id[19], fabric_id[19], vendor_id[7];
      snprintf(node_id, sizeof(node_id), "0x%016" PRIX64, fabric.GetNodeId());
      snprintf(fabric_id, sizeof(fabric_id), "0x%016" PRIX64, fabric.GetFabricId());
      snprintf(vendor_id, sizeof(vendor_id), "0x%04X", static_cast<unsigned>(fabric.GetVendorId()));
      ok &= addNumber(item, "index", fabric.GetFabricIndex());
      ok &= addString(item, "label", label_text.c_str());
      // Keep 64-bit identifiers as strings to avoid JavaScript precision loss.
      ok &= addString(item, "node_id", node_id);
      ok &= addString(item, "fabric_id", fabric_id);
      ok &= addString(item, "vendor_id", vendor_id);
      if (!cJSON_AddItemToArray(fabrics, item)) {
        cJSON_Delete(item);
        ok = false;
      }
    }
  }
  return sendJson(req, info, ok);
}
