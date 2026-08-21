#include "logger.h"

#include <stdio.h>
#include <string.h>

namespace pf {
namespace log {
namespace {

char g_ring[kRingLines][kRingWidth];
size_t g_head = 0;   // next slot to write
size_t g_count = 0;
bool g_serial = false;

}  // namespace

void begin(uint32_t wait_ms) {
#if CORE_DEBUG_LEVEL > 0
  Serial.begin(115200);
  // USB CDC enumerates lazily; without a short grace period the first few lines of a
  // wake -- which are the ones that explain why we woke -- are lost. We only pay this
  // when a host is actually attached, so it costs nothing on battery.
  const uint32_t deadline = millis() + wait_ms;
  while (!Serial && millis() < deadline) {
    delay(10);
  }
  g_serial = true;
#else
  (void)wait_ms;
#endif
}

void write(char level, const char* fmt, va_list ap) {
  char* slot = g_ring[g_head];
  const int n = snprintf(slot, kRingWidth, "%7lu %c ", (unsigned long)millis(), level);
  if (n > 0 && (size_t)n < kRingWidth) {
    vsnprintf(slot + n, kRingWidth - n, fmt, ap);
  }
  g_head = (g_head + 1) % kRingLines;
  if (g_count < kRingLines) g_count++;

  if (g_serial) {
    Serial.println(slot);
  }
}

void printf(char level, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  write(level, fmt, ap);
  va_end(ap);
}

size_t ring_count() { return g_count; }

const char* ring_line(size_t i) {
  if (i >= g_count) return "";
  // g_head points at the next slot, which is also the oldest once we have wrapped.
  const size_t oldest = (g_count == kRingLines) ? g_head : 0;
  return g_ring[(oldest + i) % kRingLines];
}

}  // namespace log
}  // namespace pf
