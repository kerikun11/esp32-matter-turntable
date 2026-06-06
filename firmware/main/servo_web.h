/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#pragma once

#include <WebServer.h>

#include "servo_settings.h"

class ServoWeb {
 public:
  ServoWeb(ServoSettings &settings, ServoSettingsStore &settings_store)
      : settings_(settings), settings_store_(settings_store) {}

  void begin();
  void handle();
  bool consumeRequestedSwitchState(bool &switch_on);

 private:
  ServoSettings &settings_;
  ServoSettingsStore &settings_store_;
  WebServer server_{80};
  bool switch_state_pending_ = false;
  bool requested_switch_state_ = false;
  String status_message_;
  bool status_is_error_ = false;

  void handleRoot();
  void handleSaveSettings();
  void handleAction();
  void sendPage();
  String buildPage() const;
};
