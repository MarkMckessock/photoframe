#include "renderer.h"

#include <Arduino.h>
#include <string.h>

// driver.h (firmware/include) selects BOARD_SCREEN_COMBO 510 + the EE02 pin map, and
// TFT_eSPI.h pulls it in. Including this header anywhere else in the project is a
// mistake: the EPaper object below owns the framebuffer and there must be exactly one.
#include <TFT_eSPI.h>

#include "core/logger.h"
#include "pf_config.h"

#ifndef EPAPER_ENABLE
#error "Seeed_GFX did not enable the ePaper path -- check firmware/include/driver.h"
#endif

namespace pf {
namespace renderer {
namespace {

// MUST be global. The EPaper constructor calls setColorDepth(4) and createSprite(),
// which ps_calloc()s the 960 KB framebuffer -- at static-init time, before setup()
// runs. There is no opportunity to configure PSRAM first, which is why platformio.ini
// has to get memory_type = qio_opi right.
EPaper epaper;

bool g_ready = false;

// Layout constants for the text cards. The panel is 1200x1600 portrait; the built-in
// font is 6x8 per unit of text size, so size 6 is a comfortable 36x48.
constexpr int16_t kMargin = 60;

void banner(int16_t y, int16_t h, uint16_t bg) {
  epaper.fillRect(0, y, PF_PANEL_W, h, bg);
}

uint32_t refresh() {
  const uint32_t t0 = millis();
  // Blocking, and on this panel there is no partial mode -- every update is a full
  // ~25-35 s flashing cycle. The audible buzz during the boost phase is normal.
  epaper.update();
  const uint32_t ms = millis() - t0;
  PF_LOGI("panel refresh took %lums", (unsigned long)ms);
  return ms ? ms : 1;
}

}  // namespace

bool begin() {
  if (g_ready) return true;

  // sleep_now() latches the enable pin low across deep sleep; make sure that hold is
  // released before the library tries to drive it. (power::sleep::release_holds()
  // does this at boot, but begin() is cheap to make idempotent and hard to debug if
  // it silently draws into a powered-down panel.)
  pinMode(PF_PIN_EPD_ENABLE, OUTPUT);
  digitalWrite(PF_PIN_EPD_ENABLE, HIGH);

  if (epaper.getPointer() == nullptr) {
    PF_LOGE("framebuffer is null: PSRAM missing or memory_type is not qio_opi "
            "(psram=%u)", (unsigned)ESP.getPsramSize());
    return false;
  }

  epaper.begin();
  epaper.setRotation(0);
  g_ready = true;
  return true;
}

uint32_t push_and_refresh(const uint8_t* pixels) {
  if (!begin()) return 0;
  // A straight memcpy is the whole point of doing the dithering server-side: the blob
  // is already in the framebuffer's own 4bpp encoding, so there is no per-pixel work
  // here at all.
  memcpy(epaper.getPointer(), pixels, PF_PIXEL_BYTES);
  return refresh();
}

uint32_t push_with_low_battery_banner(const uint8_t* pixels, uint16_t battery_mv) {
  if (!begin()) return 0;
  memcpy(epaper.getPointer(), pixels, PF_PIXEL_BYTES);

  // Deliberately a band across the bottom rather than a full-screen warning: this is
  // the only alert worth spending a refresh on, and it should not cost the user the
  // photo it is interrupting.
  constexpr int16_t kBandH = 90;
  const int16_t y = PF_PANEL_H - kBandH;
  banner(y, kBandH, TFT_RED);
  epaper.setTextDatum(MC_DATUM);
  epaper.setTextColor(TFT_WHITE, TFT_RED);
  epaper.setTextSize(5);
  char msg[64];
  snprintf(msg, sizeof(msg), "LOW BATTERY %u.%02uV - PLEASE CHARGE", battery_mv / 1000,
           (battery_mv % 1000) / 10);
  epaper.drawString(msg, PF_PANEL_W / 2, y + kBandH / 2);
  return refresh();
}

uint32_t draw_setup_card() {
  if (!begin()) return 0;
  epaper.fillScreen(TFT_WHITE);
  epaper.setTextDatum(MC_DATUM);

  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.setTextSize(10);
  epaper.drawString(PF_SETUP_HEADLINE, PF_PANEL_W / 2, PF_PANEL_H / 2 - 120);

  epaper.setTextColor(TFT_RED, TFT_WHITE);
  epaper.setTextSize(6);
  epaper.drawString(PF_SETUP_DETAIL, PF_PANEL_W / 2, PF_PANEL_H / 2 + 40);

  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.setTextSize(3);
  epaper.drawString("it will show up here", PF_PANEL_W / 2, PF_PANEL_H / 2 + 160);
  return refresh();
}

uint32_t draw_diagnostics(const Diagnostics& d) {
  if (!begin()) return 0;
  epaper.fillScreen(TFT_WHITE);
  epaper.setTextDatum(TL_DATUM);
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);

  epaper.setTextSize(6);
  epaper.drawString("photoframe", kMargin, kMargin);

  int16_t y = kMargin + 90;
  epaper.setTextSize(3);
  char line[128];

  snprintf(line, sizeof(line), "fw       %s (%s)", d.version, d.git);
  epaper.drawString(line, kMargin, y); y += 40;
  snprintf(line, sizeof(line), "ip       %s   rssi %ld dBm", d.ip, (long)d.rssi);
  epaper.drawString(line, kMargin, y); y += 40;
  snprintf(line, sizeof(line), "battery  %u mV (%u%%)", d.battery_mv, d.battery_pct);
  epaper.drawString(line, kMargin, y); y += 40;
  snprintf(line, sizeof(line), "image    %s", (d.etag && d.etag[0]) ? d.etag : "(none)");
  epaper.drawString(line, kMargin, y); y += 40;
  snprintf(line, sizeof(line), "wakes    %lu   next in %lus",
           (unsigned long)d.wake_count, (unsigned long)d.next_wake_s);
  epaper.drawString(line, kMargin, y); y += 40;
  snprintf(line, sizeof(line), "error    %s", d.last_error ? d.last_error : "none");
  epaper.drawString(line, kMargin, y); y += 40;

  y += 30;
  epaper.setTextColor(TFT_BLUE, TFT_WHITE);
  epaper.setTextSize(2);
  epaper.drawString("recent log", kMargin, y);
  y += 34;
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  for (size_t i = 0; i < ::pf::log::ring_count(); i++) {
    epaper.drawString(::pf::log::ring_line(i), kMargin, y);
    y += 26;
  }
  return refresh();
}

uint32_t draw_test_pattern() {
  if (!begin()) return 0;
  epaper.fillScreen(TFT_WHITE);

  struct Bar { uint16_t colour; const char* label; };
  static const Bar kBars[] = {
      {TFT_BLACK,  "BLACK  0xF"},
      {TFT_WHITE,  "WHITE  0x0"},
      {TFT_YELLOW, "YELLOW 0xB"},
      {TFT_RED,    "RED    0x6"},
      {TFT_BLUE,   "BLUE   0xD"},
      {TFT_GREEN,  "GREEN  0x2"},
  };
  const size_t n = sizeof(kBars) / sizeof(kBars[0]);
  const int16_t bar_h = 150;
  const int16_t top = 200;

  epaper.setTextDatum(ML_DATUM);
  epaper.setTextSize(4);
  for (size_t i = 0; i < n; i++) {
    const int16_t y = top + (int16_t)i * bar_h;
    epaper.fillRect(0, y, PF_PANEL_W, bar_h, kBars[i].colour);
    // Label in a contrasting colour so white-on-white is still readable.
    epaper.setTextColor(kBars[i].colour == TFT_BLACK ? TFT_WHITE : TFT_BLACK,
                        kBars[i].colour);
    epaper.drawString(kBars[i].label, kMargin, y + bar_h / 2);
  }

  // Geometry markers. If the halves are swapped or the axes are transposed, these are
  // unmissable in a photograph -- which is the entire point.
  epaper.setTextDatum(MC_DATUM);
  epaper.setTextSize(12);
  epaper.setTextColor(TFT_RED, TFT_WHITE);
  epaper.drawString("LEFT", PF_PANEL_W / 4, 100);
  epaper.setTextColor(TFT_BLUE, TFT_WHITE);
  epaper.drawString("RIGHT", (PF_PANEL_W * 3) / 4, 100);

  epaper.setTextColor(TFT_GREEN, TFT_WHITE);
  epaper.setTextSize(10);
  epaper.drawString("TOP", PF_PANEL_W / 2, 40);
  epaper.drawString("BOTTOM", PF_PANEL_W / 2, PF_PANEL_H - 60);

  // A one-pixel-wide vertical seam at the midline: if the two controllers are offset
  // or overlapping, this is where it shows.
  epaper.fillRect(PF_PANEL_W / 2 - 2, top, 4, (int16_t)n * bar_h, TFT_WHITE);

  // Corner squares, so a rotation or mirror is obvious.
  epaper.fillRect(0, 0, 60, 60, TFT_BLACK);
  epaper.fillRect(PF_PANEL_W - 60, 0, 60, 60, TFT_RED);
  epaper.fillRect(0, PF_PANEL_H - 60, 60, 60, TFT_BLUE);
  epaper.fillRect(PF_PANEL_W - 60, PF_PANEL_H - 60, 60, 60, TFT_GREEN);

  return refresh();
}

void power_down() {
  if (g_ready) {
    // Best effort: hold the slave chip select low so the sleep command reaches both
    // controllers. Seeed_GFX's EPD_SLEEP only asserts TFT_CS, which leaves the right
    // half of the panel awake -- and its sleep() is guarded by an internal flag that
    // update() has usually already set, so this may well be a no-op.
    //
    // The line below it is what actually saves the current.
    pinMode(PF_PIN_EPD_CS1, OUTPUT);
    digitalWrite(PF_PIN_EPD_CS1, LOW);
    epaper.sleep();
    digitalWrite(PF_PIN_EPD_CS1, HIGH);
  }
  // Cut the panel rail. The library raises TFT_ENABLE in T133A01_Init and never
  // lowers it, so without this the display stays powered through deep sleep.
  // sleep_now() additionally latches this pin low for the duration of the sleep.
  pinMode(PF_PIN_EPD_ENABLE, OUTPUT);
  digitalWrite(PF_PIN_EPD_ENABLE, LOW);
  g_ready = false;
}

}  // namespace renderer
}  // namespace pf
