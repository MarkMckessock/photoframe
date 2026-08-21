#include "wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "core/logger.h"
#include "persist/rtc_state.h"
#include "pf_config.h"
#include "secrets.h"

namespace pf {
namespace net {
namespace wifi {
namespace {

char g_ip[16] = "";
uint32_t g_connect_ms = 0;
bool g_fast = false;

constexpr uint32_t kSntpMaxAgeS = 24 * 3600;

bool wait_connected(uint32_t timeout_ms) {
  const uint32_t deadline = millis() + timeout_ms;
  while (millis() < deadline) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(20);
  }
  return false;
}

void cache_connection() {
  const uint8_t* bssid = WiFi.BSSID();
  if (bssid) memcpy(rtc.bssid, bssid, 6);
  rtc.channel = (uint8_t)WiFi.channel();
  rtc.ip = (uint32_t)WiFi.localIP();
  rtc.gw = (uint32_t)WiFi.gatewayIP();
  rtc.mask = (uint32_t)WiFi.subnetMask();
  rtc.dns = (uint32_t)WiFi.dnsIP();
  rtc.wifi_valid = rtc.channel != 0 && rtc.ip != 0;
}

}  // namespace

bool connect() {
  const uint32_t t0 = millis();
  g_fast = false;

  WiFi.persistent(false);   // do not rewrite NVS with credentials on every boot
  WiFi.mode(WIFI_STA);

  if (rtc.wifi_valid) {
    // No scan, no DHCP. Both of those are round trips we already know the answers to.
    WiFi.config(IPAddress(rtc.ip), IPAddress(rtc.gw), IPAddress(rtc.mask),
                IPAddress(rtc.dns));
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, rtc.channel, rtc.bssid, true);
    if (wait_connected(PF_WIFI_FAST_TIMEOUT_MS)) {
      g_fast = true;
      g_connect_ms = millis() - t0;
      strncpy(g_ip, WiFi.localIP().toString().c_str(), sizeof(g_ip) - 1);
      PF_LOGI("wifi fast path: %s ch%u %lums rssi %ld", g_ip, rtc.channel,
              (unsigned long)g_connect_ms, (long)WiFi.RSSI());
      return true;
    }
    // The AP moved channel, the router rebooted, or the lease is gone. Forget it and
    // do this the slow way; the cache will be rebuilt on success.
    PF_LOGW("wifi fast path failed; falling back to a full scan");
    rtc.wifi_valid = false;
    WiFi.disconnect(true, true);
    delay(50);
    WiFi.mode(WIFI_STA);
  }

  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);  // back to DHCP
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (!wait_connected(PF_WIFI_SLOW_TIMEOUT_MS)) {
    PF_LOGE("wifi: no connection to %s after %lums", WIFI_SSID,
            (unsigned long)(millis() - t0));
    WiFi.disconnect(true, true);
    return false;
  }

  cache_connection();
  g_connect_ms = millis() - t0;
  strncpy(g_ip, WiFi.localIP().toString().c_str(), sizeof(g_ip) - 1);
  PF_LOGI("wifi slow path: %s ch%u %lums rssi %ld", g_ip, rtc.channel,
          (unsigned long)g_connect_ms, (long)WiFi.RSSI());
  return true;
}

void disconnect() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
}

bool connected() { return WiFi.status() == WL_CONNECTED; }
int32_t rssi() { return WiFi.RSSI(); }
const char* ip_str() { return g_ip; }
uint32_t connect_ms() { return g_connect_ms; }
bool used_fast_path() { return g_fast; }

void sync_time_if_stale() {
  // The timezone has to be set on every boot, not just when we ask a time server:
  // localtime_r() would otherwise run on UTC and night mode would start at the wrong
  // hour for most of the year.
  setenv("TZ", PF_TIMEZONE, 1);
  tzset();

  const uint32_t now = now_epoch();
  if (rtc.last_sntp_epoch && now && (now - rtc.last_sntp_epoch) < kSntpMaxAgeS) {
    // Seed the system clock from our own estimate so localtime() works without a
    // network round trip.
    struct timeval tv = {.tv_sec = (time_t)now, .tv_usec = 0};
    settimeofday(&tv, nullptr);
    return;
  }

  configTzTime(PF_TIMEZONE, "pool.ntp.org", "time.nist.gov");
  const uint32_t deadline = millis() + 3000;
  while (millis() < deadline) {
    const time_t t = time(nullptr);
    if (t > 1700000000) {
      rtc.last_sntp_epoch = (uint32_t)t;
      rtc.epoch_estimate = (uint32_t)t;
      PF_LOGI("sntp ok (%lu)", (unsigned long)t);
      return;
    }
    delay(100);
  }
  PF_LOGW("sntp timed out; night mode will use the drifting estimate");
}

}  // namespace wifi
}  // namespace net
}  // namespace pf
