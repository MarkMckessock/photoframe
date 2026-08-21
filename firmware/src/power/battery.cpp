#include "battery.h"

#include <Arduino.h>

#include "../core/logger.h"
#include "pf_config.h"

namespace pf {
namespace battery {
namespace {

int cmp_u16(const void* a, const void* b) {
  const uint16_t x = *(const uint16_t*)a, y = *(const uint16_t*)b;
  return (x > y) - (x < y);
}

// Resting-voltage discharge curve for a single-cell LiPo. Deliberately coarse: this
// number exists to draw a bar in Home Assistant, not to do coulomb counting.
struct Point { uint16_t mv; uint8_t pct; };
constexpr Point kCurve[] = {
    {4200, 100}, {4100, 92}, {4000, 84}, {3900, 74}, {3850, 66}, {3800, 58},
    {3750, 48},  {3700, 38}, {3650, 28}, {3600, 20}, {3500, 10}, {3400, 4},
    {3300, 0},
};

}  // namespace

uint16_t read_mv() {
  pinMode(PF_PIN_BATT_EN, OUTPUT);
  digitalWrite(PF_PIN_BATT_EN, HIGH);
  analogReadResolution(12);
  delay(PF_BATT_SETTLE_MS);

  uint16_t samples[PF_BATT_SAMPLES];
  for (size_t i = 0; i < PF_BATT_SAMPLES; i++) {
    samples[i] = (uint16_t)analogRead(PF_PIN_BATT_ADC);
    delayMicroseconds(200);
  }
  digitalWrite(PF_PIN_BATT_EN, LOW);

  qsort(samples, PF_BATT_SAMPLES, sizeof(samples[0]), cmp_u16);
  const uint16_t raw = samples[PF_BATT_SAMPLES / 2];

  // PF_BATT_SCALE folds in both the divider ratio and the ADC's full-scale voltage.
  // It is an empirical constant: check it against a multimeter during bring-up and
  // correct it there rather than adding a fudge factor here.
  const uint16_t mv = (uint16_t)((raw / 4096.0f) * PF_BATT_SCALE * 1000.0f);
  PF_LOGI("battery raw=%u mv=%u", raw, mv);
  return mv;
}

uint8_t percent(uint16_t mv) {
  if (mv >= kCurve[0].mv) return 100;
  const size_t n = sizeof(kCurve) / sizeof(kCurve[0]);
  for (size_t i = 1; i < n; i++) {
    if (mv >= kCurve[i].mv) {
      const Point& hi = kCurve[i - 1];
      const Point& lo = kCurve[i];
      const int span = hi.mv - lo.mv;
      return (uint8_t)(lo.pct + (int)(hi.pct - lo.pct) * (mv - lo.mv) / span);
    }
  }
  return 0;
}

void shutdown() {
  pinMode(PF_PIN_BATT_EN, OUTPUT);
  digitalWrite(PF_PIN_BATT_EN, LOW);
}

}  // namespace battery
}  // namespace pf
