/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#include "servo_web.h"

#include "web_utils.h"

namespace {

constexpr char kWebPageTemplate[] =
#include "servo_web_page.inc"
    ;

}  // namespace

void ServoWeb::begin() {
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/settings", HTTP_POST, [this]() { handleSaveSettings(); });
  server_.on("/action", HTTP_POST, [this]() { handleAction(); });
  server_.begin();
  LOGI("[Web] HTTP server started on port 80");
}

void ServoWeb::handle() { server_.handleClient(); }

bool ServoWeb::consumeRequestedSwitchState(bool &switch_on) {
  if (!switch_state_pending_) return false;
  switch_on = requested_switch_state_;
  switch_state_pending_ = false;
  return true;
}

void ServoWeb::handleRoot() {
  logRequest(server_);
  sendPage();
}

void ServoWeb::handleSaveSettings() {
  logRequest(server_);
  String device_name = server_.arg("device_name");
  device_name.trim();
  const int on_angle = server_.arg("on_angle").toInt();
  const int off_angle = server_.arg("off_angle").toInt();
  const int max_speed = server_.arg("max_speed").toInt();

  if (device_name.isEmpty() || device_name.length() > 64 ||
      on_angle < 0 || on_angle > 180 || off_angle < 0 ||
      off_angle > 180 || max_speed < 1 || max_speed > 720) {
    status_message_ =
        "入力内容を確認してください。設定は保存されませんでした。";
    status_is_error_ = true;
    return redirectRoot(server_);
  }

  settings_.device_name = device_name;
  settings_.on_angle = on_angle;
  settings_.off_angle = off_angle;
  settings_.max_speed_dps = max_speed;
  settings_store_.save(settings_);
  status_message_ = "設定を保存しました。";
  status_is_error_ = false;
  LOGI("[Web] Settings saved");
  redirectRoot(server_);
}

void ServoWeb::handleAction() {
  logRequest(server_);
  const String state = server_.arg("state");
  if (state != "on" && state != "off") {
    status_message_ = "操作内容が不正です。";
    status_is_error_ = true;
    return redirectRoot(server_);
  }

  requested_switch_state_ = state == "on";
  switch_state_pending_ = true;
  settings_.switch_on = requested_switch_state_;
  status_message_ =
      String("サーボを") + (requested_switch_state_ ? "ON" : "OFF") +
      "にしました。";
  status_is_error_ = false;
  redirectRoot(server_);
}

void ServoWeb::sendPage() {
  server_.send(200, "text/html", buildPage());
  status_message_ = "";
  status_is_error_ = false;
}

String ServoWeb::buildPage() const {
  String html(kWebPageTemplate);
  html.reserve(html.length() + 256);

  String status_notice;
  if (status_message_.length()) {
    status_notice = "<div class=\"status ";
    status_notice += status_is_error_ ? "error" : "success";
    status_notice += "\">";
    status_notice += status_message_;
    status_notice += "</div>";
  }

  replaceTemplateValue(html, "{{DEVICE_NAME}}",
                       escapeHtml(settings_.device_name.c_str()));
  replaceTemplateValue(html, "{{STATUS_NOTICE}}", status_notice);
  replaceTemplateValue(html, "{{SWITCH_ACTION}}",
                       settings_.switch_on ? "off" : "on");
  replaceTemplateValue(html, "{{SWITCH_CLASS}}",
                       settings_.switch_on ? "on" : "off");
  replaceTemplateValue(html, "{{SWITCH_STATE}}",
                       settings_.switch_on ? "ON" : "OFF");
  replaceTemplateValue(html, "{{ON_ANGLE}}", String(settings_.on_angle));
  replaceTemplateValue(html, "{{OFF_ANGLE}}", String(settings_.off_angle));
  replaceTemplateValue(html, "{{MAX_SPEED}}",
                       String(settings_.max_speed_dps));
  return html;
}
