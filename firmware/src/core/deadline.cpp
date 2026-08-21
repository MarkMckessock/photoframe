#include "deadline.h"

#include <esp_timer.h>

#include "logger.h"

namespace pf {
namespace {

esp_timer_handle_t g_timer = nullptr;
void (*g_cb)() = nullptr;

void on_expiry(void*) {
  PF_LOGE("PANIC: awake past budget, forcing sleep");
  if (g_cb) g_cb();
}

}  // namespace

void arm_panic_sleep(uint32_t ms, void (*cb)()) {
  g_cb = cb;
  if (!g_timer) {
    const esp_timer_create_args_t args = {
        .callback = &on_expiry,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "panic_sleep",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &g_timer) != ESP_OK) {
      PF_LOGE("could not create panic timer; running unprotected");
      return;
    }
  }
  esp_timer_stop(g_timer);
  esp_timer_start_once(g_timer, (uint64_t)ms * 1000ULL);
}

void extend_panic_sleep(uint32_t ms) {
  if (!g_timer) return;
  esp_timer_stop(g_timer);
  esp_timer_start_once(g_timer, (uint64_t)ms * 1000ULL);
}

}  // namespace pf
