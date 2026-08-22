#include "nvs_store.h"

#include <Preferences.h>
#include <string.h>

namespace pf {
namespace nvs {
namespace {

Preferences g_prefs;
bool g_open = false;

// NVS keys are capped at 15 characters, hence the abbreviations.
constexpr const char* kRenderedEtag = "rendered_etag";
constexpr const char* kRenderBusy   = "render_busy";
constexpr const char* kRenderEtag   = "render_etag";
constexpr const char* kPollSeconds  = "poll_s";
constexpr const char* kNightStart   = "night_start";
constexpr const char* kNightEnd     = "night_end";
constexpr const char* kOtaVersion   = "ota_version";
constexpr const char* kHaDiscVer    = "ha_disc_ver";
constexpr const char* kInstalledSha = "installed_sha";
constexpr const char* kClearToken   = "clear_token";
constexpr const char* kSetupCard    = "setup_card";
constexpr const char* kLowBattCard  = "lowbatt_card";
constexpr const char* kTotalWakes   = "total_wakes";
constexpr const char* kTotalRenders = "total_renders";

}  // namespace

void begin() {
  if (!g_open) {
    g_prefs.begin(PF_NVS_NAMESPACE, /*readOnly=*/false);
    g_open = true;
  }
}

void end() {
  if (g_open) {
    g_prefs.end();
    g_open = false;
  }
}

void get_rendered_etag(char* out, size_t len) {
  out[0] = '\0';
  g_prefs.getString(kRenderedEtag, out, len);
}

void set_rendered_etag(const char* etag) { g_prefs.putString(kRenderedEtag, etag); }

bool render_busy() { return g_prefs.getUChar(kRenderBusy, 0) != 0; }
void set_render_busy(bool busy) { g_prefs.putUChar(kRenderBusy, busy ? 1 : 0); }

void get_render_etag(char* out, size_t len) {
  out[0] = '\0';
  g_prefs.getString(kRenderEtag, out, len);
}

void set_render_etag(const char* etag) { g_prefs.putString(kRenderEtag, etag); }

uint32_t poll_seconds() {
  uint32_t s = g_prefs.getUInt(kPollSeconds, PF_POLL_DEFAULT_S);
  if (s < PF_POLL_MIN_S) s = PF_POLL_MIN_S;
  if (s > PF_POLL_MAX_S) s = PF_POLL_MAX_S;
  return s;
}

void set_poll_seconds(uint32_t s) {
  if (s < PF_POLL_MIN_S) s = PF_POLL_MIN_S;
  if (s > PF_POLL_MAX_S) s = PF_POLL_MAX_S;
  g_prefs.putUInt(kPollSeconds, s);
}

uint8_t night_start() { return g_prefs.getUChar(kNightStart, PF_NIGHT_START_H); }
uint8_t night_end() { return g_prefs.getUChar(kNightEnd, PF_NIGHT_END_H); }

void set_night(uint8_t start_h, uint8_t end_h) {
  if (start_h > 23 || end_h > 23) return;
  g_prefs.putUChar(kNightStart, start_h);
  g_prefs.putUChar(kNightEnd, end_h);
}

void get_ota_version(char* out, size_t len) {
  out[0] = '\0';
  g_prefs.getString(kOtaVersion, out, len);
}

void set_ota_version(const char* v) { g_prefs.putString(kOtaVersion, v); }

void get_clear_token(char* out, size_t len) {
  out[0] = '\0';
  g_prefs.getString(kClearToken, out, len);
}

void set_clear_token(const char* token) { g_prefs.putString(kClearToken, token); }

void get_installed_sha(char* out, size_t len) {
  out[0] = '\0';
  g_prefs.getString(kInstalledSha, out, len);
}

void set_installed_sha(const char* sha) { g_prefs.putString(kInstalledSha, sha); }

uint8_t ha_discovery_version() { return g_prefs.getUChar(kHaDiscVer, 0); }
void set_ha_discovery_version(uint8_t v) { g_prefs.putUChar(kHaDiscVer, v); }

bool setup_card_shown() { return g_prefs.getUChar(kSetupCard, 0) != 0; }
void set_setup_card_shown(bool v) { g_prefs.putUChar(kSetupCard, v ? 1 : 0); }

bool low_batt_card_shown() { return g_prefs.getUChar(kLowBattCard, 0) != 0; }
void set_low_batt_card_shown(bool v) { g_prefs.putUChar(kLowBattCard, v ? 1 : 0); }

uint32_t total_wakes() { return g_prefs.getUInt(kTotalWakes, 0); }

void flush_counters(uint32_t wakes) { g_prefs.putUInt(kTotalWakes, wakes); }

uint32_t bump_total_renders() {
  const uint32_t n = g_prefs.getUInt(kTotalRenders, 0) + 1;
  g_prefs.putUInt(kTotalRenders, n);
  return n;
}

}  // namespace nvs
}  // namespace pf
