// Bring-up firmware: panel and power only, no radio.
//
// Built by `pio run -e bringup -t upload`. This is stages 1 and 2 of the verification
// sequence in the README, and it deliberately comes before any application logic --
// two of the three numbers that decide whether this project works at all (deep-sleep
// current and refresh duration) can only be measured, not reasoned about.
//
// On the first (power-on) boot it draws the test pattern. After that every wake just
// reports and sleeps again, so you can leave a meter on it and get a clean reading.
#ifdef PF_BRINGUP

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "core/logger.h"
#include "image/renderer.h"
#include "persist/rtc_state.h"
#include "pf_config.h"
#include "power/battery.h"
#include "power/buttons.h"
#include "power/sleep.h"

namespace pf {

[[noreturn]] void run_bringup() {
  sleep::release_holds();
  log::begin(1500);

  const bool warm = rtc_restore();
  rtc.wake_count++;

  const auto cause = sleep::classify();
  PF_LOGI("=== bringup wake #%lu cause=%s ===", (unsigned long)rtc.wake_count,
          sleep::name(cause));

  // Stage 0 assertions. If either of these is wrong, nothing downstream can work:
  // the 960 KB framebuffer is ps_calloc'd at static-init time and silently fails
  // without octal PSRAM.
  PF_LOGI("psram %u bytes (want 8388608), free %u", (unsigned)ESP.getPsramSize(),
          (unsigned)ESP.getFreePsram());
  PF_LOGI("flash %u bytes (want 16777216)", (unsigned)ESP.getFlashChipSize());
  if (ESP.getPsramSize() != 8 * 1024 * 1024) {
    PF_LOGE("PSRAM is not 8 MB -- board_build.arduino.memory_type must be qio_opi");
  }

  rtc.last_battery_mv = battery::read_mv();
  PF_LOGI("battery %u mV (%u%%)", rtc.last_battery_mv,
          battery::percent(rtc.last_battery_mv));

  pinMode(PF_PIN_BTN1, INPUT_PULLUP);
  pinMode(PF_PIN_BTN2, INPUT_PULLUP);
  pinMode(PF_PIN_BTN3, INPUT_PULLUP);
  PF_LOGI("buttons now: b1=%d b2=%d b3=%d (0 = pressed)", digitalRead(PF_PIN_BTN1),
          digitalRead(PF_PIN_BTN2), digitalRead(PF_PIN_BTN3));
  if (cause == sleep::WakeCause::Button) {
    PF_LOGI("ext1 mask 0x%llx -> action %s", (unsigned long long)sleep::ext1_status(),
            buttons::name(buttons::classify(sleep::ext1_status())));
  }

  // Only redraw when asked. Every refresh is ~30 seconds and a chunk of charge, and
  // the point of the later wakes is to measure sleep current, not to burn it.
  const bool draw = !warm || cause == sleep::WakeCause::Button;
  if (draw) {
    PF_LOGI("drawing test pattern -- expect 25-35 s and an audible buzz");
    const uint32_t ms = renderer::draw_test_pattern();
    PF_LOGI("refresh took %lu ms", (unsigned long)ms);
    PF_LOGI("photograph the panel now: check the six bar colours against their labels,");
    PF_LOGI("and that LEFT/RIGHT/TOP/BOTTOM and the corner squares are where they say.");
  }

  PF_LOGI("sleeping 60 s -- measure deep-sleep current now (needs a uA-capable meter)");
  renderer::power_down();
  sleep::sleep_now(60);
}

}  // namespace pf

#endif  // PF_BRINGUP
