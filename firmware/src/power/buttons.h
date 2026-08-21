// What the three front buttons mean, and how we tell a real press from a knock.
//
// A wall-mounted frame gets bumped. Every spurious EXT1 wake that goes on to bring up
// WiFi costs about as much charge as a legitimate scheduled check, so the first thing
// we do on a button wake is decide whether anybody actually pressed anything -- and
// if not, go straight back to sleep without touching the radio.
#pragma once

#include <stdint.h>

namespace pf {
namespace buttons {

enum class Action : uint8_t {
  None,          // not a button wake
  Spurious,      // released before the settle window: a bump, not a press
  ForceRefetch,  // button 1 -- ignore the cached ETag and pull again
  RenderCache,   // button 2 -- redraw from the local cache, no network at all
  Maintenance,   // button 3 held -- diagnostics screen + OTA window
};

const char* name(Action a);

// Blocks for up to PF_BUTTON_LONG_MS while it works out what happened.
Action classify(uint64_t ext1_mask);

}  // namespace buttons
}  // namespace pf
