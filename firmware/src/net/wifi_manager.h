// WiFi, optimised for a device whose entire duty cycle is a few seconds.
//
// The fast path -- reconnecting to a remembered BSSID on a remembered channel with a
// remembered IP, skipping both the scan and DHCP -- is the single highest-value
// optimisation in this firmware. It turns a ~3.5 s association into well under a
// second, and since a no-op check is only about four seconds long to begin with, that
// is roughly a 40% swing in battery life. If bring-up does not show that difference,
// something is wrong and it is worth stopping to find out.
#pragma once

#include <stdint.h>

namespace pf {
namespace net {
namespace wifi {

// Tries the cached path, then falls back to a full scan + DHCP, caching the result.
// Returns false if neither worked within the configured budgets.
bool connect();

// Radio off, not just disassociated. Called before a 30-second panel refresh (there
// is nothing to talk to while we draw) and before sleeping.
void disconnect();

bool connected();
int32_t rssi();
const char* ip_str();
uint32_t connect_ms();
bool used_fast_path();

// SNTP, but only if the cached wall clock is more than a day stale. We need the time
// only to decide whether it is night, so being a few seconds out is irrelevant and
// asking every wake would be waste.
void sync_time_if_stale();

}  // namespace wifi
}  // namespace net
}  // namespace pf
