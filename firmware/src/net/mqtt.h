// The control plane: how the frame reports what it did, and how you tell it what to do
// next. No image data ever passes through here -- see image_client.h for why.
//
// The awkward fact this module is shaped around is that the device is asleep more than
// 99% of the time, so it cannot receive a push. Commands are therefore *retained*: the
// broker holds the desired state and the frame reads it on its next wake. That is the
// same "retained topic is authoritative state" idea as light/registry in dmx-engine,
// applied to a device that is mostly switched off.
//
// A corollary: the session must be clean. With a persistent session the broker would
// queue every command published across days of sleep and deliver the entire backlog at
// reconnect, which for a config topic means replaying settings you already changed
// your mind about.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace pf {
namespace net {
namespace mqtt {

struct OtaRequest {
  bool valid;
  char version[32];
  char url[192];
  char sha256[65];  // optional; empty means "do not verify", which we refuse to do
};

struct StateDoc {
  const char* wake_cause;
  const char* result;      // no_change | rendered | rendered_from_cache | ...
  const char* error;       // nullptr when fine
  const char* etag;
  const char* deferred_etag;  // a photo held back by low battery, or empty
  const char* panel;       // "clean" | "dirty"
  uint32_t wake_count;
  uint32_t total_renders;
  uint16_t battery_mv;
  uint8_t battery_pct;
  int32_t rssi;
  uint32_t wifi_ms;
  uint32_t awake_ms;
  uint32_t last_render_ms;
  uint32_t rendered_at;
  uint32_t next_wake_s;
  uint8_t consecutive_failures;
};

// Connects, sets the LWT, and subscribes to the command tree. Returns false on
// timeout -- which is not fatal to the wake: the photo matters more than the telemetry.
bool connect(uint32_t timeout_ms);
bool connected();

// Runs the client loop for `ms`, which is how retained command messages get delivered.
void pump(uint32_t ms);

// True if a cmd/clear token we have not acted on yet arrived. Acting on it means
// forgetting the stored ETag so the next fetch is unconditional.
bool clear_requested();

const OtaRequest& ota_request();

bool publish_state(const StateDoc& s);
void publish_discovery_if_stale();

// Publishes "asleep" and closes cleanly, so that the retained availability topic
// distinguishes a normal sleep from the LWT's "offline", which means we died.
void disconnect();

}  // namespace mqtt
}  // namespace net
}  // namespace pf
