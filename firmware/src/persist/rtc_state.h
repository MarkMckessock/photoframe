// State that survives deep sleep in RTC slow memory.
//
// RULE: RTC RAM is lost on power loss, on a battery swap, and -- crucially -- on a
// brownout reset. Nothing in here may be the only copy of anything that matters.
// Anything that must outlive a flat battery goes in NVS (see nvs_store.h).
//
// What earns its place here is per-wake churn we do not want to write to flash 96
// times a day, plus the WiFi fast-connect cache, which is the single highest-value
// optimisation in the whole firmware: reusing a known BSSID, channel and IP turns a
// ~3.5 s associate-and-DHCP into well under a second, on a device whose entire duty
// cycle is a few seconds.
#pragma once

#include <stdint.h>

#include "pf_config.h"

namespace pf {

// Recorded in RTC and reported in the MQTT state doc, so a frame on a wall can
// explain itself without a serial cable.
enum PfError : uint8_t {
  PF_ERR_NONE = 0,
  PF_ERR_WIFI,
  PF_ERR_HTTP,
  PF_ERR_HTTP_STATUS,
  PF_ERR_SIZE,
  PF_ERR_HEADER,
  PF_ERR_CRC,
  PF_ERR_ALLOC,
  PF_ERR_MQTT,
  PF_ERR_WATCHDOG,
  PF_ERR_LOW_BATTERY,
};

const char* error_name(PfError e);

struct RtcState {
  uint32_t magic;
  uint32_t version;

  // Mirror of the NVS value, so the common "nothing changed" wake never has to open
  // NVS at all. Reseeded from flash on cold boot.
  char rendered_etag[PF_ETAG_MAX];
  // Seen on the server but not rendered because the battery was too low. Kept
  // separate so a deferred photo is not silently lost -- we render it once the cell
  // recovers rather than treating it as already shown.
  char deferred_etag[PF_ETAG_MAX];

  uint32_t wake_count;
  uint32_t total_renders;       // mirrored to NVS occasionally, not every wake
  uint32_t last_image_epoch;    // drives burst mode
  uint32_t epoch_estimate;      // wall clock, seeded by SNTP, advanced across sleeps
  uint32_t last_sntp_epoch;     // when we last actually asked a time server

  uint8_t  consecutive_failures;
  uint8_t  last_error;          // PfError
  uint16_t last_battery_mv;

  // A watchdog trip cannot report itself: on_panic() sleeps immediately, so the wake
  // ends with nothing published and looks from the outside exactly like a button that
  // was ignored. Stash it here instead and report it on the next wake. Every confusing
  // silence during bring-up was this, and each one cost far more than the two fields.
  uint8_t  pending_error;       // PfError from the wake that died
  uint32_t pending_awake_ms;    // how long it had been awake when the deadline fired

  // WiFi fast-connect cache.
  bool     wifi_valid;
  uint8_t  bssid[6];
  uint8_t  channel;
  uint32_t ip, gw, mask, dns;
};

extern RtcState rtc;

// True if the struct came back intact from a previous wake; false on cold boot or
// after a layout change, in which case it has been zeroed and needs reseeding.
bool rtc_restore();

// Advance the wall-clock estimate across a sleep of `seconds`.
void rtc_note_sleep(uint32_t seconds);

uint32_t now_epoch();
bool is_night(uint8_t start_h, uint8_t end_h);

}  // namespace pf
