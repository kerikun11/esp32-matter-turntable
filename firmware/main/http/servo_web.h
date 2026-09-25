/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#pragma once

#include <esp_http_server.h>

#include <string>

#include "device_common/system/lockable.h"
#include "settings/servo_settings.h"

// Owns the on-device settings UI's HTTP server. esp_http_server runs its
// handlers on its own worker task, so all member state below, plus
// `settings` (shared with TurntableController), is guarded by `mutex`,
// which the caller owns and shares with TurntableController.
//
// Requests that move the servo or change the switch are not executed here:
// they are queued for the app task (TurntableController::handle()), and the
// handler waits until the app task has committed them, so the JSON response
// always reflects the resulting state.
class ServoWeb {
 public:
  struct ObservedState {
    bool switch_on = false;
    bool moving = false;
    bool powered = false;
    int angle = 0;
    int target_angle = 0;
    bool commissioned = false;
    bool commissioning_open = false;
  };

  ServoWeb(ServoSettings& settings, Mutex& settings_mutex,
           ServoSettingsStore& settings_store)
      : settings_(settings),
        mutex_(settings_mutex),
        settings_store_(settings_store) {}

  void begin();
  httpd_handle_t rawHandle() const { return server_; }

  // Called from the app task.
  void setObservedState(const ObservedState& state);
  bool consumeHostnameUpdated();
  bool consumeRequestedSwitchState(bool& switch_on);
  bool consumeRequestedAngle(int& angle);
  bool consumeRebootRequested();
  void completeAction();
  void showStatus(const std::string& message, bool is_error = false);

 private:
  struct PendingValue {
    bool pending = false;
    int value = 0;

    void request(int requested_value) {
      value = requested_value;
      pending = true;
    }

    bool consume(int& requested_value) {
      if (!pending) return false;
      requested_value = value;
      pending = false;
      return true;
    }
  };

  ServoSettings& settings_;
  Mutex& mutex_;
  ServoSettingsStore& settings_store_;
  httpd_handle_t server_ = nullptr;

  ObservedState observed_;
  bool action_in_progress_ = false;
  bool hostname_updated_ = false;
  PendingValue requested_switch_state_;
  PendingValue requested_angle_;
  bool reboot_requested_ = false;
  int64_t reboot_after_us_ = 0;
  std::string status_message_;
  bool status_is_error_ = false;

  esp_err_t handleRoot(httpd_req_t* req);
  esp_err_t handleState(httpd_req_t* req);
  esp_err_t handleDeviceInfo(httpd_req_t* req);
  esp_err_t handleSaveSettings(httpd_req_t* req);
  esp_err_t handleAction(httpd_req_t* req);
  esp_err_t handleMove(httpd_req_t* req);
  esp_err_t handleMatter(httpd_req_t* req);
  esp_err_t handleReboot(httpd_req_t* req);

  // Queues a request for the app task and waits until it is committed.
  esp_err_t runAction(httpd_req_t* req, PendingValue ServoWeb::* slot,
                      int value);
  void requestReboot();
  esp_err_t respondMutation(httpd_req_t* req);
  esp_err_t sendState(httpd_req_t* req);

  template <esp_err_t (ServoWeb::*Handler)(httpd_req_t*)>
  static esp_err_t trampoline(httpd_req_t* req) {
    return (static_cast<ServoWeb*>(req->user_ctx)->*Handler)(req);
  }
};
