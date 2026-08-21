// Logging over USB CDC, plus a small ring buffer the diagnostics screen can render.
//
// Note there is no option to log over UART0: GPIO43 is the panel's power-enable pin
// on this board, so driving it as TX fights the display rail. platformio.ini sets
// ARDUINO_USB_CDC_ON_BOOT=1 and that is the only supported console.
//
// The ring buffer exists because the most interesting failures happen while the frame
// is on a wall with no serial cable attached. Long-pressing button 3 renders it.
#pragma once

#include <Arduino.h>
#include <stdarg.h>

namespace pf {
namespace log {

static constexpr size_t kRingLines = 16;
static constexpr size_t kRingWidth = 96;

void begin(uint32_t wait_ms = 0);
void write(char level, const char* fmt, va_list ap);
void printf(char level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Oldest-first iteration over retained lines, for the diagnostics screen.
size_t ring_count();
const char* ring_line(size_t i);

}  // namespace log
}  // namespace pf

#define PF_LOGI(...) ::pf::log::printf('I', __VA_ARGS__)
#define PF_LOGW(...) ::pf::log::printf('W', __VA_ARGS__)
#define PF_LOGE(...) ::pf::log::printf('E', __VA_ARGS__)
