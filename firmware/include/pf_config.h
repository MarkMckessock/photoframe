// Tunables and the board pin map. Secrets live in firmware/src/secrets.h.
//
// Anything a human might reasonably want to change at runtime (poll interval, quiet
// hours) is only a *default* here -- the live value comes from NVS, set over the
// retained home/<device>/cmd/config topic. Anything that is a property of the
// hardware is a hard constant.
#pragma once

#include <stdint.h>

#include "pf_image_format.h"  // PFRM_HEADER_LEN

// ---------------------------------------------------------------------------
// Panel geometry. Must agree with Setup510 in Seeed_GFX and with the encoder.
// ---------------------------------------------------------------------------
#define PF_PANEL_W        1200
#define PF_PANEL_H        1600
#define PF_PANEL_STRIDE   (PF_PANEL_W / 2)                 // 600 bytes, 4bpp
#define PF_PIXEL_BYTES    (PF_PANEL_STRIDE * PF_PANEL_H)   // 960000
#define PF_BLOB_BYTES     (PF_PIXEL_BYTES + PFRM_HEADER_LEN)

// ---------------------------------------------------------------------------
// Pins.
//
// The panel pins (SCK/MOSI/CS/CS1/DC/BUSY/RST/ENABLE) are defined by Seeed_GFX in
// User_Setups/EPaper_Board_Pins_Setups.h and must NOT be redefined here -- we only
// mirror the two we have to touch ourselves, because the library never does.
//
// The button and battery pins are not in the library at all; they come from the EE02
// schematic (202000224_XIAO_ePaper_Display_Board_EE02_V1.pdf).
// ---------------------------------------------------------------------------
#define PF_PIN_EPD_ENABLE   43   // == TFT_ENABLE. Also UART0 TX: log over USB CDC only.
#define PF_PIN_EPD_CS1      41   // == TFT_CS1, the slave controller's chip select.

#define PF_PIN_BTN1          2   // "refresh"  -- force a refetch
#define PF_PIN_BTN2          3   // "previous" -- re-render from the local cache
#define PF_PIN_BTN3          5   // "next"     -- long-press: diagnostics + OTA window
#define PF_BUTTON_MASK      ((1ULL << PF_PIN_BTN1) | (1ULL << PF_PIN_BTN2) | (1ULL << PF_PIN_BTN3))

#define PF_PIN_BATT_ADC      1   // A0, behind a divider
#define PF_PIN_BATT_EN       6   // A5, enables the divider. Leaving this high leaks.
#define PF_BATT_SCALE     7.16f  // volts at full scale (12-bit ADC)
#define PF_BATT_SETTLE_MS   10
#define PF_BATT_SAMPLES      9   // median-of-N

// ---------------------------------------------------------------------------
// Power policy. Every threshold is a *resting* millivolt reading, taken before the
// radio is brought up (the ADC is noisy once WiFi starts transmitting).
// ---------------------------------------------------------------------------
#define PF_BATT_MV_OTA_OK     3700  // below this, skip OTA (a brownout mid-flash is bad)
#define PF_BATT_MV_RENDER_OK  3600  // below this, defer renders but keep polling
#define PF_BATT_MV_WARN       3450  // below this, show the low-battery card once
#define PF_BATT_MV_CRITICAL   3300  // below this, do nothing at all but sleep

// ---------------------------------------------------------------------------
// Timing. Milliseconds unless the name says otherwise.
// ---------------------------------------------------------------------------
#define PF_WIFI_FAST_TIMEOUT_MS   4000   // cached BSSID + static IP path
#define PF_WIFI_SLOW_TIMEOUT_MS  12000   // full scan + DHCP fallback
#define PF_MQTT_TIMEOUT_MS        5000
#define PF_HTTP_CONNECT_MS        5000
#define PF_HTTP_STALL_MS          5000   // no bytes for this long -> abort
#define PF_HTTP_TOTAL_MS         30000

// The backstop. A wedged network stack should cost one wake cycle, not a battery.
#define PF_AWAKE_BUDGET_MS       45000
#define PF_AWAKE_BUDGET_RENDER_MS 120000

#define PF_BUTTON_SETTLE_MS         60   // released faster than this == a bump
#define PF_BUTTON_LONG_MS         5000
#define PF_MAINTENANCE_MS       300000   // 5 minutes of ArduinoOTA

// ---------------------------------------------------------------------------
// Sleep intervals, in seconds.
// ---------------------------------------------------------------------------
#define PF_POLL_DEFAULT_S      900   // 15 min
#define PF_POLL_MIN_S           60
#define PF_POLL_MAX_S        86400
#define PF_POLL_BURST_S        300   // after a photo lands, poll faster for a while
#define PF_BURST_WINDOW_S     7200   // ...for two hours. Photos arrive in clusters.
#define PF_POLL_NIGHT_S       3600
#define PF_POLL_LOW_BATT_S    3600
#define PF_POLL_CRITICAL_S   43200   // 12 h, button wake still works
// POSIX TZ string, so night mode follows local time including DST.
#define PF_TIMEZONE "PST8PDT,M3.2.0,M11.1.0"
#define PF_NIGHT_START_H        22
#define PF_NIGHT_END_H           7

// Backoff ladder after consecutive failures.
#define PF_BACKOFF_S { 300, 900, 1800, 3600 }

// ---------------------------------------------------------------------------
// The one-time setup card, drawn on first boot before any photo has ever arrived.
// Not a secret, just a string, so it lives here rather than in secrets.h.
// ---------------------------------------------------------------------------
#define PF_SETUP_HEADLINE "Send me a photo"
#define PF_SETUP_DETAIL   "Text a photo to (415) 855-3459"

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
#define PF_NVS_NAMESPACE   "pf"
#define PF_CACHE_PATH      "/latest.pfrm"
#define PF_CACHE_TMP_PATH  "/latest.tmp"

// ETags are opaque; we only ever compare them. 64 chars covers a quoted sha256.
#define PF_ETAG_MAX 72
