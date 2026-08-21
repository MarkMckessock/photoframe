#include "sleep.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include "../core/logger.h"
#include "../persist/rtc_state.h"
#include "battery.h"
#include "pf_config.h"

namespace pf {
namespace sleep {
namespace {

constexpr int kButtons[] = {PF_PIN_BTN1, PF_PIN_BTN2, PF_PIN_BTN3};

}  // namespace

void release_holds() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PF_PIN_EPD_ENABLE);
  for (int pin : kButtons) {
    rtc_gpio_deinit((gpio_num_t)pin);
    pinMode(pin, INPUT_PULLUP);
  }
}

WakeCause classify() {
  const esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_BROWNOUT || rr == ESP_RST_PANIC || rr == ESP_RST_TASK_WDT ||
      rr == ESP_RST_INT_WDT) {
    return WakeCause::Fault;
  }
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER: return WakeCause::Timer;
    case ESP_SLEEP_WAKEUP_EXT1:  return WakeCause::Button;
    default:                     return WakeCause::PowerOn;
  }
}

const char* name(WakeCause c) {
  switch (c) {
    case WakeCause::PowerOn: return "power";
    case WakeCause::Timer:   return "timer";
    case WakeCause::Button:  return "button";
    case WakeCause::Fault:   return "fault";
  }
  return "?";
}

uint64_t ext1_status() { return esp_sleep_get_ext1_wakeup_status(); }

uint32_t next_interval_s(const IntervalInputs& in) {
  if (in.consecutive_failures) {
    static const uint32_t kBackoff[] = PF_BACKOFF_S;
    const size_t n = sizeof(kBackoff) / sizeof(kBackoff[0]);
    const size_t i = (in.consecutive_failures - 1 < n) ? in.consecutive_failures - 1 : n - 1;
    return kBackoff[i];
  }
  if (in.battery_mv && in.battery_mv < PF_BATT_MV_WARN)      return PF_POLL_CRITICAL_S;
  if (in.battery_mv && in.battery_mv < PF_BATT_MV_RENDER_OK) return PF_POLL_LOW_BATT_S;
  if (is_night(in.night_start, in.night_end))                return PF_POLL_NIGHT_S;

  // Burst mode. Photos arrive in clusters -- somebody sends one, then another ninety
  // seconds later -- so poll fast for a while after each arrival. This is what makes
  // the frame feel responsive without paying for a five-minute poll all day.
  const uint32_t now = now_epoch();
  if (now && in.last_image_epoch && (now - in.last_image_epoch) < PF_BURST_WINDOW_S) {
    return PF_POLL_BURST_S;
  }
  return in.configured_poll_s;
}

[[noreturn]] void sleep_now(uint32_t seconds) {
  if (seconds < PF_POLL_MIN_S) seconds = PF_POLL_MIN_S;

  PF_LOGI("sleeping %lus (awake %lums)", (unsigned long)seconds, (unsigned long)millis());

  battery::shutdown();

  // Drop the panel rail. Seeed_GFX raises TFT_ENABLE in T133A01_Init and never lowers
  // it, so if we do not do this the display half of the board stays powered through
  // deep sleep. GPIO43 is a plain digital pad, not an RTC pad, so it needs the digital
  // hold path to stay low once the CPU stops.
  pinMode(PF_PIN_EPD_ENABLE, OUTPUT);
  digitalWrite(PF_PIN_EPD_ENABLE, LOW);
  gpio_hold_en((gpio_num_t)PF_PIN_EPD_ENABLE);
  gpio_deep_sleep_hold_en();

  esp_wifi_stop();

  // Buttons are active-low. Enabling the RTC pull-ups keeps the RTC peripheral domain
  // powered, which costs a few microamps -- but without a pull-up (internal or on the
  // board) the pins float and ANY_LOW fires continuously, which flattens the cell in a
  // night. If bring-up confirms external pull-ups on the schematic, build with
  // -DPF_BUTTONS_EXTERNAL_PULLUP to skip this and save the current.
  for (int pin : kButtons) {
    rtc_gpio_init((gpio_num_t)pin);
    rtc_gpio_set_direction((gpio_num_t)pin, RTC_GPIO_MODE_INPUT_ONLY);
#ifdef PF_BUTTONS_EXTERNAL_PULLUP
    rtc_gpio_pullup_dis((gpio_num_t)pin);
#else
    rtc_gpio_pullup_en((gpio_num_t)pin);
#endif
    rtc_gpio_pulldown_dis((gpio_num_t)pin);
  }
  esp_sleep_enable_ext1_wakeup_io(PF_BUTTON_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);

  rtc_note_sleep(seconds);

  Serial.flush();
  esp_deep_sleep_start();
  __builtin_unreachable();
}

}  // namespace sleep
}  // namespace pf
