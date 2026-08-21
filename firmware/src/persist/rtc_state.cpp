#include "rtc_state.h"

#include <Arduino.h>
#include <string.h>
#include <time.h>

#include "../core/logger.h"

namespace pf {

namespace {
constexpr uint32_t kMagic = 0x50465231;  // "PFR1"
constexpr uint32_t kVersion = 1;
}  // namespace

RTC_DATA_ATTR RtcState rtc;

const char* error_name(PfError e) {
  switch (e) {
    case PF_ERR_NONE:        return nullptr;
    case PF_ERR_WIFI:        return "wifi";
    case PF_ERR_HTTP:        return "http";
    case PF_ERR_HTTP_STATUS: return "http_status";
    case PF_ERR_SIZE:        return "size";
    case PF_ERR_HEADER:      return "header";
    case PF_ERR_CRC:         return "crc";
    case PF_ERR_ALLOC:       return "alloc";
    case PF_ERR_MQTT:        return "mqtt";
    case PF_ERR_WATCHDOG:    return "watchdog";
    case PF_ERR_LOW_BATTERY: return "low_battery";
  }
  return "unknown";
}

bool rtc_restore() {
  if (rtc.magic == kMagic && rtc.version == kVersion) {
    return true;
  }
  // Cold boot, or we just flashed a build that changed the layout. Either way the
  // contents are meaningless; zero them and let the caller reseed from NVS.
  PF_LOGI("rtc state cold (magic=%08lx ver=%lu)", (unsigned long)rtc.magic,
          (unsigned long)rtc.version);
  memset(&rtc, 0, sizeof(rtc));
  rtc.magic = kMagic;
  rtc.version = kVersion;
  return false;
}

void rtc_note_sleep(uint32_t seconds) {
  if (rtc.epoch_estimate) {
    // Deep sleep uses the RTC oscillator, which drifts by seconds per day. That is
    // wildly good enough for deciding whether it is night.
    rtc.epoch_estimate += seconds + (millis() / 1000);
  }
}

uint32_t now_epoch() {
  const time_t sys = time(nullptr);
  // Once SNTP has run in this boot, the system clock is authoritative.
  if (sys > 1700000000) return (uint32_t)sys;
  if (rtc.epoch_estimate) return rtc.epoch_estimate + (millis() / 1000);
  return 0;
}

bool is_night(uint8_t start_h, uint8_t end_h) {
  const uint32_t e = now_epoch();
  if (!e) return false;  // no idea what time it is; assume day and keep polling
  time_t t = (time_t)e;
  struct tm lt;
  localtime_r(&t, &lt);
  const uint8_t h = (uint8_t)lt.tm_hour;
  // Handles the usual wrap (22 -> 07) as well as a non-wrapping window.
  return (start_h <= end_h) ? (h >= start_h && h < end_h)
                            : (h >= start_h || h < end_h);
}

}  // namespace pf
