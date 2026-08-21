// The whole firmware, as one pass.
//
// Deliberately NOT the task-per-concern shape of the splitflap firmware. That design
// is right for a machine that runs continuously and has genuinely concurrent work --
// kHz motor stepping must not be starved by an MQTT reconnect. This device is a
// strictly serial pipeline: you cannot fetch before you connect, render before you
// validate, or sleep before you report. Concurrency here buys nothing measurable and
// costs extra stacks plus nondeterministic ordering, which makes "why was I awake for
// forty seconds" much harder to answer -- and that question is the whole ballgame on
// battery.
//
// So: one function, top to bottom, ending in deep sleep. If you are tempted to bring
// core/task.h across from the splitflap project, this comment is why it is not here.
#pragma once

namespace pf {

// Runs one wake cycle and deep sleeps. Never returns.
[[noreturn]] void run_wake();

}  // namespace pf
