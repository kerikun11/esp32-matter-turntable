/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#include "turntable_controller.h"

#include <esp_system.h>
#include <esp_timer.h>

#include <cmath>

#include "app_log.h"
#include "ota_service.h"

namespace {

constexpr int64_t kMatterStatusIntervalMs = 500;
constexpr int64_t kPairingLogIntervalMs = 10000;

int64_t nowMs() { return esp_timer_get_time() / 1000; }

}  // namespace

TurntableController::TurntableController()
    : web_(settings_, settings_mutex_, settings_store_) {}

void TurntableController::begin() {
  led_.setBackground(RgbLed::Color::kGreen);

  if (!settings_store_.begin()) {
    LOGE("[Prefs] Failed to open settings");
  }
  settings_ = settings_store_.load();

  if (!servo_.begin(CONFIG_APP_PIN_SERVO_CTRL, CONFIG_APP_PIN_SERVO_POWER)) {
    LOGE("[Servo] Failed to initialize");
  }
  moveServoForSwitch(settings_.switch_on, false);

  if (!matter_.begin(settings_.switch_on)) {
    LOGE("[Matter] Failed to start");
  }
  network_.begin(settings_.hostname);

  web_.begin();
  registerOtaHandlers(web_.rawHandle());
  updateMatterStatus();
  publishObservedState();
}

void TurntableController::handle() {
  btn_.update();
  led_.update();
  servo_.handle();
  network_.handle();

  if (web_.consumeRebootRequested()) {
    servo_.free();
    esp_restart();
  }

  // Locked for the rest of this function: settings_ is shared with web_'s
  // HTTP worker task.
  Lock lock(settings_mutex_);
  if (web_.consumeHostnameUpdated()) {
    network_.setHostname(settings_.hostname);
  }

  bool action_done = false;
  bool web_switch_on = false;
  if (web_.consumeRequestedSwitchState(web_switch_on)) {
    applySwitchState(web_switch_on, Source::kWeb);
    web_.showStatus(std::string("ターンテーブルを") +
                    (web_switch_on ? "ON" : "OFF") + "にしました。");
    action_done = true;
  }
  int web_angle = 0;
  if (web_.consumeRequestedAngle(web_angle)) {
    servo_.setTargetDegree(web_angle, settings_.max_speed_dps);
    LOGI("[Web] Test move to %d deg", web_angle);
    web_.showStatus("試し動作：" + std::to_string(web_angle) +
                    "度に移動します。ON/OFFの状態は変わりません。");
    action_done = true;
  }

  MatterSwitch::Event event;
  if (matter_.getEvent(event, 0)) {
    led_.blinkOnce(RgbLed::Color::kBlue);
    applySwitchState(event.switch_state, Source::kMatter);
  }

  handleButton();
  updateMatterStatus();
  // Publish after applying every request so the response to the web
  // request that is waiting on completeAction() shows the committed state.
  publishObservedState();
  if (action_done) web_.completeAction();
  updateStatusLed();
}

void TurntableController::applySwitchState(bool on, Source source) {
  static constexpr const char* kSourceNames[] = {"Matter", "Web", "Button"};
  LOGI("[Switch] %s (%s)", on ? "ON" : "OFF",
       kSourceNames[static_cast<int>(source)]);
  if (settings_.switch_on != on) {
    settings_.switch_on = on;
    settings_store_.saveSwitchState(on);
  }
  // Matter already holds the new value when the change came from Matter.
  if (source != Source::kMatter) matter_.setSwitchState(on);
  // Always re-assert the position, even if the state did not change, so a
  // repeated command also corrects a manually rotated turntable.
  moveServoForSwitch(on, true);
}

void TurntableController::moveServoForSwitch(bool on, bool move_smoothly) {
  const float angle = on ? settings_.on_angle : settings_.off_angle;
  const float speed = move_smoothly ? settings_.max_speed_dps : 0.0f;
  servo_.setTargetDegree(angle, speed);
}

void TurntableController::updateMatterStatus() {
  const int64_t now = nowMs();
  if (last_matter_status_ms_ >= 0 &&
      now - last_matter_status_ms_ < kMatterStatusIntervalMs) {
    return;
  }
  last_matter_status_ms_ = now;
  matter_status_ = matter_.getStatus();
}

void TurntableController::publishObservedState() {
  ServoWeb::ObservedState state;
  state.switch_on = settings_.switch_on;
  state.moving = servo_.isBusy();
  state.powered = servo_.getPowered();
  state.angle = static_cast<int>(std::lround(servo_.getDegree()));
  state.target_angle = static_cast<int>(std::lround(servo_.getTargetDegree()));
  state.commissioned = matter_status_.commissioned;
  state.commissioning_open = matter_status_.commissioning_open;
  web_.setObservedState(state);
}

void TurntableController::updateStatusLed() {
  if (!matter_status_.commissioned || matter_status_.commissioning_open) {
    led_.setBackground(RgbLed::Color::kMagenta);
  } else if (!network_.hasIpv4()) {
    led_.setBackground(RgbLed::Color::kRed);
  } else {
    led_.setBackground(RgbLed::Color::kWhite);
  }
}

void TurntableController::handleButton() {
  /* Short press: toggle */
  if (btn_.pressed()) {
    led_.blinkOnce(RgbLed::Color::kCyan);
    applySwitchState(!settings_.switch_on, Source::kButton);
  }

  /* Long press: Matter decommission / commissioning window */
  if (btn_.longHoldStarted()) led_.blinkOnce(RgbLed::Color::kMagenta);
  if (btn_.longPressed()) {
    if (matter_status_.commissioned) {
      servo_.free();
      matter_.decommission();
    } else {
      matter_.openCommissioningWindow();
    }
  }

  if (!matter_status_.commissioned) {
    const int64_t now = nowMs();
    if (now - last_pairing_log_ms_ > kPairingLogIntervalMs) {
      last_pairing_log_ms_ = now;
      matter_.printOnboarding();
    }
  }
}
