#include "buttons.h"

#include <Arduino.h>

#include "../core/logger.h"
#include "pf_config.h"

namespace pf {
namespace buttons {
namespace {

bool pressed(int pin) { return digitalRead(pin) == LOW; }

bool any_pressed() {
  return pressed(PF_PIN_BTN1) || pressed(PF_PIN_BTN2) || pressed(PF_PIN_BTN3);
}

}  // namespace

const char* name(Action a) {
  switch (a) {
    case Action::None:         return "none";
    case Action::Spurious:     return "spurious";
    case Action::ForceRefetch: return "force_refetch";
    case Action::RenderCache:  return "render_cache";
    case Action::Maintenance:  return "maintenance";
  }
  return "?";
}

Action classify(uint64_t ext1_mask) {
  if (!ext1_mask) return Action::None;

  pinMode(PF_PIN_BTN1, INPUT_PULLUP);
  pinMode(PF_PIN_BTN2, INPUT_PULLUP);
  pinMode(PF_PIN_BTN3, INPUT_PULLUP);
  delay(5);  // let the pull-ups win against pad capacitance

  // A genuine press is still held this long after the wake; a knock is not.
  const uint32_t settle_until = millis() + PF_BUTTON_SETTLE_MS;
  while (millis() < settle_until) {
    delay(2);
  }
  if (!any_pressed()) {
    PF_LOGI("button wake mask=%llx but nothing held: bump", (unsigned long long)ext1_mask);
    return Action::Spurious;
  }

  // Button 3 is the only one with a long-press meaning, so it is the only one we wait
  // on. The others act the moment we know they are really down.
  if (pressed(PF_PIN_BTN3)) {
    const uint32_t long_at = millis() + PF_BUTTON_LONG_MS;
    while (millis() < long_at) {
      if (!pressed(PF_PIN_BTN3)) {
        // Short press on 3. Nothing is bound to it in a latest-photo-only frame, so
        // treat it as a plain refetch rather than doing nothing surprising.
        return Action::ForceRefetch;
      }
      delay(20);
    }
    return Action::Maintenance;
  }

  if (pressed(PF_PIN_BTN2)) return Action::RenderCache;
  return Action::ForceRefetch;  // button 1, or several at once
}

}  // namespace buttons
}  // namespace pf
