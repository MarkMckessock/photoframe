// Wake classification, the interval policy, and the one function that ends every
// code path in this firmware.
#pragma once

#include <stdint.h>

namespace pf {
namespace sleep {

enum class WakeCause : uint8_t {
  PowerOn,  // cold boot, reset button, or a fresh flash
  Timer,
  Button,
  Fault,    // brownout or panic reset -- we did not choose to be here
};

// Undo what sleep_now() latched. MUST be called early in boot: the panel enable pin
// is held low across deep sleep, and the button pins are left in RTC mode, so without
// this the display can never be powered and the buttons cannot be read.
void release_holds();

WakeCause classify();
const char* name(WakeCause c);

// Bitmask of which buttons pulled us out of EXT1, in PF_PIN_* terms. Valid only
// immediately after a Button wake.
uint64_t ext1_status();

struct IntervalInputs {
  uint8_t  consecutive_failures;
  uint16_t battery_mv;
  uint32_t configured_poll_s;
  uint32_t last_image_epoch;
  uint8_t  night_start;
  uint8_t  night_end;
};

uint32_t next_interval_s(const IntervalInputs& in);

// Powers down the panel rail and the battery divider, arms the timer and the three
// buttons, and deep sleeps. Never returns.
[[noreturn]] void sleep_now(uint32_t seconds);

}  // namespace sleep
}  // namespace pf
