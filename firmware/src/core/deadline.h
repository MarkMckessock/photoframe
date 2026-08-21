// The "you have been awake too long" backstop.
//
// Everything else in this firmware is a single linear pass with per-step timeouts, but
// a timeout only helps if the code reaches the check. A wedged TCP stack, a library
// that busy-waits on a BUSY pin that never goes high, a lost interrupt -- any of those
// leaves the device awake at ~100 mA until the cell is flat, which on a wall-mounted
// frame means you discover it a week later.
//
// So: an esp_timer that force-sleeps us no matter what the state machine is doing.
// Calling esp_deep_sleep_start() from the timer task is safe.
#pragma once

#include <stdint.h>

namespace pf {

// cb must not return -- it is expected to record the failure and deep sleep.
void arm_panic_sleep(uint32_t ms, void (*cb)());

// Extend an already-armed deadline (a render legitimately takes ~30 s longer).
void extend_panic_sleep(uint32_t ms);

}  // namespace pf
