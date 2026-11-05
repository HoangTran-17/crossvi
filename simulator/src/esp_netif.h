#pragma once

#include <cstdint>

#include "esp_err.h"

struct esp_netif_t;

struct esp_ip4_addr_t {
  uint32_t addr = 0;
};

struct esp_ip_addr_t {
  union {
    esp_ip4_addr_t ip4;
  } u_addr{};
};

struct esp_netif_ip_info_t {
  esp_ip4_addr_t ip;
  esp_ip4_addr_t netmask;
  esp_ip4_addr_t gw;
};

struct esp_netif_dns_info_t {
  esp_ip_addr_t ip;
};

enum esp_netif_dns_type_t { ESP_NETIF_DNS_MAIN = 0 };

#define IPSTR "%d.%d.%d.%d"
#define IP2STR(ipaddr)                                                                                 \
  static_cast<int>((ipaddr)->addr & 0xff), static_cast<int>(((ipaddr)->addr >> 8) & 0xff),            \
      static_cast<int>(((ipaddr)->addr >> 16) & 0xff), static_cast<int>(((ipaddr)->addr >> 24) & 0xff)

inline esp_netif_t* esp_netif_get_handle_from_ifkey(const char*) { return nullptr; }
inline esp_err_t esp_netif_get_ip_info(esp_netif_t*, esp_netif_ip_info_t*) { return ESP_FAIL; }
inline esp_err_t esp_netif_get_dns_info(esp_netif_t*, esp_netif_dns_type_t, esp_netif_dns_info_t*) { return ESP_FAIL; }
