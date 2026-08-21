// Firmware updates for a device that is asleep 99.9% of the time.
//
// Pushing is hopeless -- ArduinoOTA needs the target to be listening, and this one is
// switched off. So the primary path is a pull: the retained cmd/ota topic states the
// desired version, and on its next scheduled wake the frame notices the mismatch and
// fetches. Worst case is one poll interval of delay and no coordination at all.
//
// Rollback is the real point. A new image boots as PENDING_VERIFY and is only marked
// good once it has proved it can complete a whole wake -- WiFi, fetch, MQTT publish.
// A build that panics on boot, or that cannot reach the network, un-installs itself
// on the next reset with nobody involved. On a battery device inside a picture frame
// on a wall, that is not a nice-to-have.
#pragma once

#include <stdint.h>

namespace pf {
namespace net {
namespace ota {

struct Request;  // see mqtt.h OtaRequest

// Note whether this boot is running an image that has not yet been confirmed good.
void note_boot();
bool pending_verify();

// Call only after a wake has genuinely succeeded end to end. Cancels the rollback.
void mark_good();

// Returns true if it started an update, in which case the device reboots and this
// never returns. `desired` may be empty (nothing requested).
bool maybe_update(const char* desired_version, const char* url, const char* sha256_hex);

// Hands-on path: stay awake advertising ArduinoOTA so `pio run -e photoframe_ota
// -t upload` works, exactly like the splitflap chainlink_ota env. Blocks for up to
// PF_MAINTENANCE_MS, or until an update completes.
void maintenance_window();

}  // namespace ota
}  // namespace net
}  // namespace pf
