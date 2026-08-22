#include "app_state_machine.h"

#include <Arduino.h>
#include <esp_sleep.h>
#include <string.h>

#include "core/deadline.h"
#include "core/logger.h"
#include "image/image_buffer.h"
#include "image/image_cache.h"
#include "image/renderer.h"
#include "net/image_client.h"
#include "net/mqtt.h"
#include "net/ota.h"
#include "net/wifi_manager.h"
#include "persist/nvs_store.h"
#include "persist/rtc_state.h"
#include "pf_config.h"
#include "pf_version.h"
#include "power/battery.h"
#include "power/buttons.h"
#include "power/sleep.h"
#include "secrets.h"

namespace pf {
namespace {

// One instance, allocated in PSRAM on first use. Separate from Seeed_GFX's own
// framebuffer on purpose: keeping the download out of the framebuffer means a corrupt
// or half-finished transfer can never be one stray update() away from the wall, and it
// leaves the framebuffer free to hold a cached photo we are compositing onto.
ImageBuffer g_img;

struct Ctx {
  uint32_t t0 = 0;
  sleep::WakeCause wake = sleep::WakeCause::PowerOn;
  buttons::Action button = buttons::Action::None;
  uint16_t battery_mv = 0;
  uint8_t battery_pct = 0;
  const char* result = "no_change";
  PfError error = PF_ERR_NONE;
  uint32_t render_ms = 0;
  bool panel_dirty = false;
  bool rendered = false;
  char etag[PF_ETAG_MAX] = "";
};
Ctx ctx;

uint32_t choose_interval() {
  sleep::IntervalInputs in{};
  in.consecutive_failures = rtc.consecutive_failures;
  in.battery_mv = ctx.battery_mv;
  in.configured_poll_s = nvs::poll_seconds();
  in.last_image_epoch = rtc.last_image_epoch;
  in.night_start = nvs::night_start();
  in.night_end = nvs::night_end();
  return sleep::next_interval_s(in);
}

// Every exit from this firmware goes through here.
[[noreturn]] void finish() {
  const uint32_t interval = choose_interval();

  if (net::mqtt::connected()) {
    net::mqtt::StateDoc s{};
    s.wake_cause = sleep::name(ctx.wake);
    s.result = ctx.result;
    s.error = error_name(ctx.error);
    s.etag = rtc.rendered_etag;
    s.deferred_etag = rtc.deferred_etag;
    s.panel = ctx.panel_dirty ? "dirty" : "clean";
    s.wake_count = rtc.wake_count;
    s.total_renders = rtc.total_renders;
    s.battery_mv = ctx.battery_mv;
    s.battery_pct = ctx.battery_pct;
    s.rssi = net::wifi::rssi();
    s.wifi_ms = net::wifi::connect_ms();
    s.awake_ms = millis() - ctx.t0;
    s.last_render_ms = ctx.render_ms;
    s.rendered_at = rtc.last_image_epoch;
    s.next_wake_s = interval;
    s.consecutive_failures = rtc.consecutive_failures;

    if (net::mqtt::publish_state(s)) {
      // Only now is this build proven able to complete a wake end to end, which is
      // the bar for cancelling an OTA rollback.
      net::ota::mark_good();
    }
    net::mqtt::disconnect();
  }

  net::wifi::disconnect();
  renderer::power_down();
  nvs::end();
  sleep::sleep_now(interval);
}

void on_panic() {
  rtc.consecutive_failures++;
  rtc.last_error = PF_ERR_WATCHDOG;
  renderer::power_down();
  sleep::sleep_now(PF_POLL_LOW_BATT_S);
}

void note_failure(PfError e) {
  ctx.error = e;
  // Do not clobber a successful render: "we put the photo up but could not file the
  // paperwork afterwards" is a very different report from "we failed".
  if (!ctx.rendered) ctx.result = "error";
  rtc.last_error = e;
  if (rtc.consecutive_failures < 255) rtc.consecutive_failures++;
}

void note_success() {
  rtc.consecutive_failures = 0;
  rtc.last_error = PF_ERR_NONE;
}

// Records that a refresh happened, in both the volatile and durable stores.
void commit_render(const char* etag, uint32_t ms) {
  ctx.rendered = true;
  ctx.render_ms = ms;
  ctx.panel_dirty = false;
  rtc.total_renders++;
  rtc.last_image_epoch = now_epoch();
  if (etag) {
    strncpy(rtc.rendered_etag, etag, sizeof(rtc.rendered_etag) - 1);
    rtc.rendered_etag[sizeof(rtc.rendered_etag) - 1] = '\0';
    nvs::set_rendered_etag(rtc.rendered_etag);
  }
  nvs::set_render_busy(false);
  nvs::flush_counters(rtc.wake_count);
}

// Draws whatever is in the cache. Used for crash recovery and for the "my panel looks
// ghosted" button, neither of which should need the network.
bool render_from_cache(const char* result_label, const char* etag) {
  if (!cache::begin() || !g_img.alloc() || !cache::load(g_img)) return false;
  pf::extend_panic_sleep(PF_AWAKE_BUDGET_RENDER_MS);
  nvs::set_render_busy(true);
  const uint32_t ms = renderer::push_and_refresh(g_img.pixels());
  if (!ms) {
    nvs::set_render_busy(false);
    return false;
  }
  // `etag` is non-null only when we know which image the cache holds -- i.e. when we
  // are finishing a refresh that a brownout interrupted. Otherwise leave the stored
  // ETag alone: whatever was last successfully rendered is still what is on the glass.
  commit_render(etag, ms);
  ctx.result = result_label;
  return true;
}

}  // namespace

[[noreturn]] void run_wake() {
  ctx.t0 = millis();

  // Undo the pin holds from the previous sleep before anything tries to use them.
  sleep::release_holds();

  const bool cold = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED);
  log::begin(cold ? 800 : 0);
  arm_panic_sleep(PF_AWAKE_BUDGET_MS, on_panic);
  net::ota::note_boot();

  const bool warm = rtc_restore();
  nvs::begin();
  if (!warm) {
    // A battery swap wipes RTC memory but not the panel, which is still showing the
    // last photo. Reseeding from flash is what stops a cell change from costing a
    // pointless thirty-second re-render of an image already on the wall.
    nvs::get_rendered_etag(rtc.rendered_etag, sizeof(rtc.rendered_etag));
    rtc.total_renders = 0;
  }
  rtc.wake_count++;

  ctx.wake = sleep::classify();
  PF_LOGI("wake #%lu cause=%s fw=%s free_psram=%u", (unsigned long)rtc.wake_count,
          sleep::name(ctx.wake), PF_VERSION, (unsigned)ESP.getFreePsram());

  // If this flag is still set we died partway through a refresh, so the panel is
  // showing garbage rather than a photo. This is the one situation where redrawing
  // without being asked is unambiguously an improvement.
  const bool interrupted_render = nvs::render_busy();
  if (interrupted_render) {
    ctx.panel_dirty = true;
    PF_LOGW("previous refresh did not finish; panel contents are not trustworthy");
  }

  // Before the radio: once WiFi starts transmitting the rail sags in bursts and the
  // ADC reads low, and a false low reading makes us refuse to render, which looks
  // exactly like a broken frame.
  ctx.battery_mv = battery::read_mv();
  ctx.battery_pct = battery::percent(ctx.battery_mv);
  rtc.last_battery_mv = ctx.battery_mv;

  if (ctx.battery_mv && ctx.battery_mv < PF_BATT_MV_CRITICAL) {
    // Nothing at all: no radio, no panel. Buttons still wake us, so plugging in a
    // charger and pressing one gets you back immediately.
    PF_LOGE("battery critical (%u mV); sleeping without doing anything", ctx.battery_mv);
    note_failure(PF_ERR_LOW_BATTERY);
    renderer::power_down();
    nvs::end();
    sleep::sleep_now(PF_POLL_CRITICAL_S);
  }

  if (ctx.wake == sleep::WakeCause::Button) {
    ctx.button = buttons::classify(sleep::ext1_status());
    PF_LOGI("button action: %s", buttons::name(ctx.button));

    if (ctx.button == buttons::Action::Spurious) {
      // A knock on the wall costs about as much charge as a real scheduled check if we
      // let it reach the radio. Do not let it.
      ctx.result = "no_change";
      renderer::power_down();
      // Interval first: choose_interval() reads NVS, so closing it earlier would
      // silently hand back defaults.
      const uint32_t s = choose_interval();
      nvs::end();
      sleep::sleep_now(s);
    }
    if (ctx.button == buttons::Action::RenderCache) {
      if (!render_from_cache("rendered_from_cache", nullptr)) {
        PF_LOGW("no usable cached image to redraw");
      }
      renderer::power_down();
      const uint32_t s = choose_interval();
      nvs::end();
      sleep::sleep_now(s);
    }
  }

  // Recover a garbled panel before touching the network: it is the most visible
  // problem the device has, and it does not need WiFi to fix.
  if (interrupted_render && (!ctx.battery_mv || ctx.battery_mv >= PF_BATT_MV_WARN)) {
    char in_flight[PF_ETAG_MAX];
    nvs::get_render_etag(in_flight, sizeof(in_flight));
    if (render_from_cache("rendered_from_cache", in_flight[0] ? in_flight : nullptr)) {
      PF_LOGI("recovered the panel from cache after an interrupted refresh");
    } else {
      // Nothing to redraw with. Clear the flag anyway so we do not retry forever.
      nvs::set_render_busy(false);
    }
  }

  // One-shot critical-battery warning, composited onto the photo rather than replacing
  // it. This is the only failure a user can actually act on, which is what makes it
  // worth a refresh.
  if (ctx.battery_mv && ctx.battery_mv < PF_BATT_MV_WARN &&
      !nvs::low_batt_card_shown()) {
    if (cache::begin() && g_img.alloc() && cache::load(g_img)) {
      nvs::set_render_busy(true);
      pf::extend_panic_sleep(PF_AWAKE_BUDGET_RENDER_MS);
      const uint32_t ms = renderer::push_with_low_battery_banner(g_img.pixels(),
                                                                 ctx.battery_mv);
      if (ms) commit_render(nullptr, ms);
      else nvs::set_render_busy(false);
      nvs::set_low_batt_card_shown(true);
    }
  } else if (ctx.battery_mv > 3800 && nvs::low_batt_card_shown()) {
    nvs::set_low_batt_card_shown(false);  // charged; re-arm the warning
  }

  if (!net::wifi::connect()) {
    note_failure(PF_ERR_WIFI);
    finish();
  }
  net::wifi::sync_time_if_stale();
  note_success();

  if (ctx.button == buttons::Action::Maintenance) {
    renderer::Diagnostics d{};
    d.version = PF_VERSION;
    d.git = PF_GIT_SHA;
    d.ip = net::wifi::ip_str();
    d.etag = rtc.rendered_etag;
    d.last_error = error_name((PfError)rtc.last_error);
    d.rssi = net::wifi::rssi();
    d.battery_mv = ctx.battery_mv;
    d.battery_pct = ctx.battery_pct;
    d.wake_count = rtc.wake_count;
    d.next_wake_s = choose_interval();
    pf::extend_panic_sleep(PF_AWAKE_BUDGET_RENDER_MS + PF_MAINTENANCE_MS);
    nvs::set_render_busy(true);
    const uint32_t ms = renderer::draw_diagnostics(d);
    if (ms) commit_render(nullptr, ms);
    else nvs::set_render_busy(false);
    ctx.result = "rendered_from_cache";
    net::ota::maintenance_window();
    finish();
  }

  // The conditional GET. A 304 is the overwhelmingly common answer and is what makes
  // the battery estimate work: most wakes never touch the panel.
  const bool force = (ctx.button == buttons::Action::ForceRefetch);
  const char* inm = force ? "" : rtc.rendered_etag;
  if (force) PF_LOGI("button 1: ignoring cached ETag");

  const auto r = net::image::fetch(g_img, IMAGE_URL, inm, ctx.etag, sizeof(ctx.etag));

  if (r != net::image::Result::Error) {
    // A completed conditional GET is proof that this build can do its actual job:
    // boot, associate, and talk to the image service. That is the right bar for
    // confirming an OTA image, and it deliberately does not include MQTT -- telemetry
    // being down should not roll back a working firmware. Note this matters on every
    // wake, not just after an update: a deep-sleep wake is a boot, so an unconfirmed
    // image gets rolled back at the next one.
    net::ota::mark_good();
  }

  if (r == net::image::Result::NotModified) {
    ctx.result = "no_change";
  } else if (r == net::image::Result::Error) {
    note_failure(g_img.error() != PF_ERR_NONE ? g_img.error() : PF_ERR_HTTP);
    if (!rtc.rendered_etag[0] && !nvs::setup_card_shown()) {
      // Nothing has ever been shown and there is nothing to show. Say so, once.
      pf::extend_panic_sleep(PF_AWAKE_BUDGET_RENDER_MS);
      if (renderer::draw_setup_card()) {
        nvs::set_setup_card_shown(true);
        ctx.result = "no_image_yet";
      }
    }
  } else {  // Updated
    if (!ctx.etag[0]) {
      // The server did not send an ETag, so it cannot tell us "nothing changed" and we
      // would re-download -- and, much worse, re-refresh -- the same photo on every
      // single wake. The blob carries a CRC of its own pixels, so use that as a
      // synthetic identity: we still pay for the download, but not for a pointless
      // thirty-second refresh.
      snprintf(ctx.etag, sizeof(ctx.etag), "crc32:%08lx",
               (unsigned long)g_img.header()->data_crc32);
      PF_LOGW("server sent no ETag; falling back to %s", ctx.etag);
    }
    if (ctx.battery_mv && ctx.battery_mv < PF_BATT_MV_RENDER_OK) {
      // Deliberately do NOT record this as rendered: the photo must not be lost, it
      // must be shown once the cell recovers.
      strncpy(rtc.deferred_etag, ctx.etag, sizeof(rtc.deferred_etag) - 1);
      ctx.result = "deferred_low_battery";
      PF_LOGW("battery %u mV: holding a new photo until it recovers", ctx.battery_mv);
    } else if (strcmp(ctx.etag, rtc.rendered_etag) == 0) {
      // Same image, different HTTP answer (a server with no ETag support, or a cache
      // miss). The download is already spent; the refresh does not have to be.
      PF_LOGI("fetched image matches what is on the panel; not refreshing");
      ctx.result = "no_change";
    } else {
      // Extend the deadline BEFORE the cache write, not after. Writing 960 KB to
      // LittleFS is the slowest step outside the refresh itself, and leaving it
      // inside the 45 s base budget means a slow flash write trips the watchdog --
      // which sleeps immediately, so the photo is never drawn AND no state is
      // published. That failure is silent from the outside, which is how it went
      // unnoticed: the only symptom is a button press that appears to do nothing.
      pf::extend_panic_sleep(PF_AWAKE_BUDGET_RENDER_MS);

      if (cache::begin()) {
        const uint32_t t0 = millis();
        if (cache::save(g_img.raw(), g_img.received())) {
          PF_LOGI("cache write took %lums", (unsigned long)(millis() - t0));
        }
      }

      // Radio off for the refresh. Thirty seconds of transmitter idle current is
      // worth more than the ~1 s it costs to reconnect afterwards on the fast path.
      net::wifi::disconnect();
      setCpuFrequencyMhz(80);

      nvs::set_render_etag(ctx.etag);
      nvs::set_render_busy(true);
      const uint32_t ms = renderer::push_and_refresh(g_img.pixels());
      setCpuFrequencyMhz(240);

      if (ms) {
        commit_render(ctx.etag, ms);
        rtc.deferred_etag[0] = '\0';
        ctx.result = "rendered";
        PF_LOGI("rendered %s in %lums", ctx.etag, (unsigned long)ms);
      } else {
        nvs::set_render_busy(false);
        note_failure(PF_ERR_ALLOC);
      }
      renderer::power_down();
      net::wifi::connect();  // fast path, just to report
    }
  }

  if (net::mqtt::connect(PF_MQTT_TIMEOUT_MS)) {
    // Retained commands land immediately after SUBACK; a short pump is all it takes.
    net::mqtt::pump(1200);
    if (net::mqtt::clear_requested()) {
      PF_LOGI("cmd/clear: forgetting the stored ETag");
      rtc.rendered_etag[0] = '\0';
      nvs::set_rendered_etag("");
    }
    net::mqtt::publish_discovery_if_stale();

    const auto& ota = net::mqtt::ota_request();
    if (ota.valid && ctx.battery_mv >= PF_BATT_MV_OTA_OK && net::wifi::rssi() > -75) {
      net::ota::maybe_update(ota.version, ota.url, ota.sha256);  // reboots on success
    }
  } else if (ctx.error == PF_ERR_NONE) {
    note_failure(PF_ERR_MQTT);
  }

  finish();
}

}  // namespace pf
