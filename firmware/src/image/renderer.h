// Everything that is allowed to change what is on the glass.
//
// This is the [SEEED_GFX INTEGRATION POINT]: the only file that includes TFT_eSPI.h,
// and the only file that knows the panel is driven by two chip selects.
//
// Policy, enforced by the state machine rather than by this file: a full refresh takes
// ~30 seconds, costs meaningful charge, and cannot be partially undone, so the panel is
// written for exactly five reasons -- a validated new photo, the one-time setup card, a
// mid-refresh crash recovery, a one-time critical-battery warning, and an explicitly
// requested diagnostics screen. Transient errors never touch it; a stale photo of your
// friend's dog is a far better thing to be showing than "MQTT CONNECT FAILED rc=-2".
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace pf {
namespace renderer {

// Powers and initialises the panel. Only call this when you actually intend to draw.
bool begin();

// Copies packed 4bpp pixels into the framebuffer and does a full refresh.
// Returns the refresh duration in milliseconds, or 0 on failure.
uint32_t push_and_refresh(const uint8_t* pixels);

// Same, but overlays a warning band along the bottom first, so the photo survives.
uint32_t push_with_low_battery_banner(const uint8_t* pixels, uint16_t battery_mv);

// First-boot card. Drawn once in the device's lifetime.
uint32_t draw_setup_card();

struct Diagnostics {
  const char* version;
  const char* git;
  const char* ip;
  const char* etag;
  const char* last_error;
  int32_t rssi;
  uint16_t battery_mv;
  uint8_t battery_pct;
  uint32_t wake_count;
  uint32_t next_wake_s;
};
uint32_t draw_diagnostics(const Diagnostics& d);

// Bring-up pattern. Draws the six palette colours as labelled bars, plus explicit
// LEFT/RIGHT and TOP/BOTTOM markers.
//
// This exists to settle two things that the datasheets and the community drivers
// disagree about, and that everything else depends on:
//   1. which nibble produces which colour on the actual glass, and
//   2. how the two chip selects map onto the panel -- Seeed's driver treats it as two
//      side-by-side 1200x800 halves, while other ports describe a top/bottom split.
// Photograph the result and correct pf_image_format.h and tools/encode_image.py to
// match *before* trusting either description.
uint32_t draw_test_pattern();

// Puts the panel to sleep and cuts its power rail. Safe to call even if begin() was
// never reached -- and it should be, on every path that ends in deep sleep.
void power_down();

}  // namespace renderer
}  // namespace pf
