// Battery voltage, behind a divider that we switch on only long enough to sample.
//
// Two things matter here and both are easy to get wrong:
//
//  1. Read BEFORE the radio comes up. Once WiFi starts transmitting, the supply rail
//     sags in bursts and the ADC reads low and noisy -- and a false low reading makes
//     the firmware refuse to render, which looks exactly like "the frame is broken".
//
//  2. Turn the divider back off. It is a resistor across the cell; leaving the enable
//     pin high leaks continuously, which on a device that sleeps 99.9% of the time is
//     a much bigger deal than it sounds.
#pragma once

#include <stdint.h>

namespace pf {
namespace battery {

// Enables the divider, settles, takes a median of PF_BATT_SAMPLES readings, and
// disables it again. Blocking, roughly PF_BATT_SETTLE_MS + a few ms.
uint16_t read_mv();

// Rough LiPo state of charge. Resting voltage only -- under load this reads pessimistic,
// which is the direction we want for a gate that protects against brownout.
uint8_t percent(uint16_t mv);

// Belt and braces: make sure the divider is off before sleeping.
void shutdown();

}  // namespace battery
}  // namespace pf
