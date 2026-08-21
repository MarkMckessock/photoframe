// Entry point.
//
// Everything happens in setup() and ends in deep sleep, so loop() is unreachable by
// design -- see app_state_machine.h for why this device is not built as a set of
// FreeRTOS tasks the way the splitflap firmware is.
#include <Arduino.h>

#include "app_state_machine.h"

#ifdef PF_BRINGUP
namespace pf {
[[noreturn]] void run_bringup();
}
#endif

void setup() {
#ifdef PF_BRINGUP
  pf::run_bringup();
#else
  pf::run_wake();
#endif
}

void loop() {
  // Not reached: both entry points end in esp_deep_sleep_start().
}
