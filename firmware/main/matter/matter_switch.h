/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2025 Ryotaro Onuki
 */
#pragma once
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/Server.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_attribute.h>
#include <esp_matter_cluster.h>
#include <esp_matter_core.h>
#include <esp_matter_endpoint.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <inttypes.h>
#include <platform/ConfigurationManager.h>
#include <platform/PlatformManager.h>
#include <system/SystemClock.h>

#include "device_common/matter/matter_service.h"

// A single Matter On/Off Plug-in Unit endpoint.
//
// Remote (Matter network) changes arrive through the attribute callback on
// the Matter task and are handed to the app task through a queue. Local
// changes (web UI) are written with attribute::report(), which updates the
// value under the CHIP stack lock and notifies subscribed controllers
// (Google Home / Alexa) without calling the attribute callback back -- so
// the app never receives an echo of its own writes.
class MatterSwitch {
 public:
  struct Event {
    uint64_t timestamp_ms;
    bool switch_state;
  };

  struct Status {
    bool commissioned = false;        // at least one fabric
    bool commissioning_open = false;  // commissioning window is open
  };

  static constexpr const char* kManualCode = "34970112332";
  static constexpr const char* kQrPayload = "MT:Y.K9042C00KA0648G00";
  static constexpr const char* kQrUrl =
      "https://project-chip.github.io/connectedhomeip/"
      "qrcode.html?data=MT:Y.K9042C00KA0648G00";

  bool begin(bool initial_switch_on) {
    if (instance_) {
      ESP_LOGE(kTag, "only one instance is supported");
      return false;
    }
    instance_ = this;

    queue_ = xQueueCreate(1, sizeof(Event));
    if (!queue_) {
      ESP_LOGE(kTag, "xQueueCreate failed");
      return false;
    }

    esp_matter::node::config_t node_cfg{};
    node_ = esp_matter::node::create(&node_cfg, &MatterSwitch::attrCb, nullptr,
                                     this);
    if (!node_) {
      ESP_LOGE(kTag, "node::create failed");
      return false;
    }

    esp_matter::endpoint::on_off_plug_in_unit::config_t cfg{};
    cfg.on_off.on_off = initial_switch_on;
    ep_plugin_ = esp_matter::endpoint::on_off_plug_in_unit::create(node_, &cfg,
                                                                   0, this);
    if (!ep_plugin_) {
      ESP_LOGE(kTag, "plugin::create failed");
      return false;
    }
    endpoint_id_ = esp_matter::endpoint::get_id(ep_plugin_);

    // The OnOff attribute is non-volatile, so its creation above restored
    // the value esp_matter persisted itself. The app's own saved state is
    // the source of truth (it also drives the servo), so override it before
    // the stack starts. No lock or report is needed yet.
    if (auto* attr = onOffAttr()) {
      esp_matter_attr_val_t v = esp_matter_bool(initial_switch_on);
      esp_matter::attribute::set_val(attr, &v, false);
    }

    if (esp_matter::start(nullptr) != ESP_OK) {
      ESP_LOGE(kTag, "esp_matter::start failed");
      return false;
    }

    ESP_LOGI(kTag, "plugin_ep=0x%04x(%s)", endpoint_id_,
             initial_switch_on ? "ON" : "OFF");
    printOnboarding();
    return true;
  }

  bool getEvent(Event& out, TickType_t ticks = portMAX_DELAY) {
    return queue_ && (xQueueReceive(queue_, &out, ticks) == pdTRUE);
  }

  void printOnboarding() const {
    ESP_LOGI(kTag, "Manual: %s", kManualCode);
    ESP_LOGI(kTag, "QR    : %s", kQrUrl);
  }

  // Reads the fabric table and the commissioning window under the CHIP
  // stack lock (both are owned by the Matter task).
  Status getStatus() const {
    const auto status = device_common::matterStatus();
    return {status.commissioned, status.commissioning_open};
  }

  // Updates the OnOff attribute and reports it to subscribed controllers.
  bool setSwitchState(bool on) {
    return ep_plugin_ && device_common::reportOnOff(endpoint_id_, on);
  }

  bool openCommissioningWindow(uint16_t timeout_seconds = 300) {
    if (!device_common::openCommissioningWindow(timeout_seconds)) return false;
    printOnboarding();
    return true;
  }

  void decommission() {
    device_common::factoryReset();
  }

 private:
  static constexpr const char* kTag = "MatterSwitch";

  // Most CHIP APIs assert that this is held when called from any task other
  // than the Matter event loop (the app task and the HTTP server task here).
  using ChipStackLock = chip::DeviceLayer::StackLock;

  static inline MatterSwitch* instance_ = nullptr;

  esp_matter::node_t* node_ = nullptr;
  esp_matter::endpoint_t* ep_plugin_ = nullptr;
  uint16_t endpoint_id_ = 0xFFFF;
  QueueHandle_t queue_ = nullptr;

  esp_matter::attribute_t* onOffAttr() const {
    auto* cluster =
        esp_matter::cluster::get(ep_plugin_, chip::app::Clusters::OnOff::Id);
    if (!cluster) return nullptr;
    return esp_matter::attribute::get(
        cluster, chip::app::Clusters::OnOff::Attributes::OnOff::Id);
  }

  // Runs on the Matter task for every attribute change made through the
  // data model (On/Off/Toggle commands, OnWithTimedOff expiry, scenes, ...).
  static esp_err_t attrCb(esp_matter::attribute::callback_type_t type,
                          uint16_t endpoint_id, uint32_t cluster_id,
                          uint32_t attribute_id, esp_matter_attr_val_t* val,
                          void*) {
    MatterSwitch* self = instance_;
    if (type != esp_matter::attribute::POST_UPDATE || !self || !val ||
        endpoint_id != self->endpoint_id_ ||
        cluster_id != chip::app::Clusters::OnOff::Id ||
        attribute_id != chip::app::Clusters::OnOff::Attributes::OnOff::Id) {
      return ESP_OK;
    }

    Event ev{};
    ev.timestamp_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
    ev.switch_state = val->val.b;
    ESP_LOGI(kTag, "OnOff update ep=0x%04x state=%s", endpoint_id,
             ev.switch_state ? "ON" : "OFF");
    // Each event carries the absolute state, so only the latest one matters.
    // Overwriting (instead of failing on a full queue) guarantees the app
    // never ends up applying a stale state after a burst of commands.
    xQueueOverwrite(self->queue_, &ev);
    return ESP_OK;
  }
};
