/**
 * SPDX-License-Identifier: LGPL-2.1
 * @copyright 2026 Ryotaro Onuki
 */
#include "network_health.h"

#include <esp_wifi.h>
#include <mdns.h>

#include "app_log.h"

namespace {
constexpr const char *kStaNetifKey = "WIFI_STA_DEF";
constexpr unsigned long kIpv4WatchdogIntervalMs = 15000;
constexpr unsigned long kMdnsSyncIntervalMs = 1000;
constexpr unsigned long kDiagLogIntervalMs = 5 * 60 * 1000UL;
}  // namespace

void NetworkHealth::begin(const String &hostname) {
  hostname_ = hostname;

  if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
    LOGW("[Net] Failed to disable Wi-Fi power save");
  }

  const esp_err_t err = esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_LOST_IP, &NetworkHealth::ipEventHandler_, this);
  if (err != ESP_OK) {
    LOGW("[Net] Failed to register IPv4 recovery: %s", esp_err_to_name(err));
  }

  syncMdnsHostname_(true);
}

void NetworkHealth::handle() {
  if (dhcp_restart_requested_) {
    dhcp_restart_requested_ = false;
    if (esp_netif_t *netif = esp_netif_get_handle_from_ifkey(kStaNetifKey)) {
      restartDhcpClient_(netif);
    }
  }

  ensureIpv4Address_();
  syncMdnsHostname_(false);
  logDiagnostics_();
}

void NetworkHealth::setHostname(const String &hostname) {
  hostname_ = hostname;
  syncMdnsHostname_(true);
}

void NetworkHealth::ipEventHandler_(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *) {
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    static_cast<NetworkHealth *>(arg)->onLostIp_();
  }
}

void NetworkHealth::onLostIp_() { dhcp_restart_requested_ = true; }

void NetworkHealth::restartDhcpClient_(esp_netif_t *netif) {
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

// Fallback for the case where a single DHCP restart (triggered by
// IP_EVENT_STA_LOST_IP) fails to obtain a new lease and no further lost-IP
// event ever fires again: periodically check whether the STA interface is up
// but still has no IPv4 address, and keep retrying DHCP until it succeeds.
// Without this, a one-shot failed renewal can leave the device reachable
// over Matter (IPv6 link-local) forever while the IPv4-only web server stays
// unreachable until a manual power cycle.
void NetworkHealth::ensureIpv4Address_() {
  const unsigned long now = millis();
  if (now - last_ipv4_watchdog_attempt_ms_ < kIpv4WatchdogIntervalMs) return;

  esp_netif_t *netif = esp_netif_get_handle_from_ifkey(kStaNetifKey);
  if (!netif || !esp_netif_is_netif_up(netif)) return;

  esp_netif_ip_info_t ip_info{};
  if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    return;  // already has an IPv4 address
  }

  last_ipv4_watchdog_attempt_ms_ = now;
  LOGW("[Net] Wi-Fi is up but IPv4 address is missing; retrying DHCP");
  restartDhcpClient_(netif);
}

void NetworkHealth::syncMdnsHostname_(bool force) {
  const unsigned long now = millis();
  if (!force && now - last_mdns_sync_attempt_ms_ < kMdnsSyncIntervalMs) return;
  last_mdns_sync_attempt_ms_ = now;

  esp_netif_t *netif = esp_netif_get_handle_from_ifkey(kStaNetifKey);
  esp_netif_ip_info_t ip_info{};
  if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
      ip_info.ip.addr == 0) {
    return;
  }

  if (!mdns_hostname_.isEmpty() && mdns_hostname_ != hostname_) {
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
    const esp_err_t err = mdns_delegate_hostname_add(hostname_.c_str(), &address);
    if (err != ESP_OK) {
      if (err != last_mdns_error_) {
        LOGW("[mDNS] Failed to add %s.local: %s", hostname_.c_str(),
             esp_err_to_name(err));
      }
      last_mdns_error_ = err;
      return;
    }
    last_mdns_error_ = ESP_OK;
    if (!mdns_hostname_exists(hostname_.c_str())) {
      char primary_hostname[MDNS_NAME_BUF_LEN] = {};
      const esp_err_t get_err = mdns_hostname_get(primary_hostname);
      LOGW("[mDNS] %s.local was not added (primary: %s)", hostname_.c_str(),
           get_err == ESP_OK ? primary_hostname : "unavailable");
      return;
    }
    mdns_hostname_ = hostname_;
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

void NetworkHealth::logDiagnostics_() {
  const unsigned long now = millis();
  if (last_diag_log_ms_ != 0 && now - last_diag_log_ms_ < kDiagLogIntervalMs) {
    return;
  }
  last_diag_log_ms_ = now;

  esp_netif_t *netif = esp_netif_get_handle_from_ifkey(kStaNetifKey);
  esp_netif_ip_info_t ip_info{};
  const bool has_ip = netif &&
                       esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
                       ip_info.ip.addr != 0;
  LOGI("[Diag] uptime=%lus heap=%u(min=%u) ipv4=%s", now / 1000,
       (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
       has_ip ? IPAddress(ip_info.ip.addr).toString().c_str() : "NONE");
}
