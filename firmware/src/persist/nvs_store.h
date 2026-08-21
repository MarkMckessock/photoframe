// The things that must survive a flat battery.
//
// Write discipline: this is flash. Touch it only on genuine state changes -- a render
// starting or finishing, a config change, an OTA. That is roughly 5-10 writes a day
// against 100k+ endurance with wear levelling, so wear is a non-issue. It stops being
// a non-issue the moment somebody "helpfully" persists wake_count every 15 minutes,
// which is why the per-wake churn lives in RTC memory instead. Please keep it there.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pf_config.h"

namespace pf {
namespace nvs {

void begin();
void end();

// What is physically on the glass right now. Authoritative across a battery swap,
// which is what stops a cell change from triggering a pointless 30 s re-render of the
// image already being displayed.
void get_rendered_etag(char* out, size_t len);
void set_rendered_etag(const char* etag);

// The brownout flag. Set before a refresh, cleared after. If it is still set at the
// next boot we died mid-refresh and the panel is showing garbage, which is the one
// situation where redrawing without being asked is unambiguously an improvement.
//
// This MUST live in flash: a brownout is precisely the event that wipes RTC RAM.
bool render_busy();
void set_render_busy(bool busy);

// The ETag of the image a refresh was started for. Paired with render_busy: if we
// come back from a brownout and redraw that image from the cache, this is what lets
// us record it as rendered rather than downloading and refreshing it all over again
// -- which is the last thing a marginal battery needs.
void get_render_etag(char* out, size_t len);
void set_render_etag(const char* etag);

// Live config, set over the retained cmd/config topic.
uint32_t poll_seconds();
void set_poll_seconds(uint32_t s);
uint8_t night_start();
uint8_t night_end();
void set_night(uint8_t start_h, uint8_t end_h);

// Desired firmware version, from the retained cmd/ota topic.
void get_ota_version(char* out, size_t len);
void set_ota_version(const char* v);

// The token from the retained cmd/clear topic that we last acted on. Retained
// commands would otherwise re-fire on every wake for the rest of the device's life.
void get_clear_token(char* out, size_t len);
void set_clear_token(const char* token);

uint8_t ha_discovery_version();
void set_ha_discovery_version(uint8_t v);

// One-shot panel writes, so the setup card and the low-battery warning each cost one
// refresh in the device's lifetime rather than one per wake.
bool setup_card_shown();
void set_setup_card_shown(bool v);
bool low_batt_card_shown();
void set_low_batt_card_shown(bool v);

// Lifetime counters. Deliberately NOT bumped every wake -- wake_count lives in RTC
// memory and is flushed here only when we are already writing flash for another
// reason (i.e. during a render). See the write-discipline note at the top.
uint32_t total_wakes();
void flush_counters(uint32_t wakes);
uint32_t bump_total_renders();

}  // namespace nvs
}  // namespace pf
