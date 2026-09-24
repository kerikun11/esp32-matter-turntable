/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2026 Ryotaro Onuki
 */
#pragma once

#include <Arduino.h>
#include <esp_event.h>
#include <esp_netif.h>

// Owns everything needed to keep the Wi-Fi STA's IPv4 connectivity and its
// additional mDNS hostname alias healthy, independently of Matter (which
// mostly relies on IPv6 link-local and is not affected by IPv4 issues).
class NetworkHealth {
 public:
  // Disables Wi-Fi power save, registers for IPv4-loss recovery, and
  // publishes the initial mDNS hostname alias.
  void begin(const String &hostname);

  // Call once per loop iteration.
  void handle();

  // Call whenever the desired hostname changes (e.g. saved from the web UI)
  // to republish the mDNS alias immediately instead of waiting for the next
  // periodic sync.
  void setHostname(const String &hostname);

 private:
  static void ipEventHandler_(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);
  void onLostIp_();
  static void restartDhcpClient_(esp_netif_t *netif);
  void ensureIpv4Address_();
  void syncMdnsHostname_(bool force);
  void logDiagnostics_();

  String hostname_;

  volatile bool dhcp_restart_requested_ = false;
  unsigned long last_ipv4_watchdog_attempt_ms_ = 0;
  unsigned long last_diag_log_ms_ = 0;

  String mdns_hostname_;
  uint32_t mdns_ipv4_address_ = 0;
  unsigned long last_mdns_sync_attempt_ms_ = 0;
  esp_err_t last_mdns_error_ = ESP_OK;
};
