# Hardware notes — what bring-up actually found

The plan in `firmware/README.md` listed things that could only be settled by putting
the firmware on the board. This is the record of what happened when we did, on
**2026-08-21/22**. Where a question is now answered, the answer is here rather than in
the bring-up guide, so nobody re-litigates it. Where it is still open, it says so
plainly — those are the real risks.

## The board

Seeed **XIAO ePaper Display Board EE02** carrying a **XIAO ESP32-S3 Plus**
(16 MB flash, 8 MB *octal* PSRAM, `board = seeed_xiao_esp32_s3_plus`, `qio_opi`),
driving a **13.3" E Ink Spectra 6** panel: 1200×1600, six colours, **no partial
refresh**, two chip-selects.

Confirmed live on the device: `free_psram` reports ~6.45 MB *after* the 960 KB
framebuffer is allocated, so octal PSRAM is genuinely working. If you ever see
~0 PSRAM, stop — `EPaper`'s constructor `ps_calloc`s the framebuffer at static-init
time, **before `setup()` runs**, and fails silently. Every downstream symptom will be
confusing.

### Buttons: there is no BOOT button

The silkscreen reads **KEY1 / KEY2 / KEY3 / RESET**. That trips people up, because
every ESP32 flashing guide says "hold BOOT."

You do not need it. `esptool` drives the ROM downloader over USB CDC automatically, and
every upload in this project has worked with nothing held down. If a board ever does
refuse to enter download mode, the BOOT button is the small one **on the XIAO module
itself**, beside the USB-C connector — not on the carrier board. Hold it, tap RESET,
release.

KEY1/2/3 are GPIO 2/3/5, active-LOW, and all three are RTC-capable, which is what makes
EXT1 wake work. They map to buttons 1/2/3 in `firmware/README.md`.

## Settled: the palette and the geometry

**The nibble table in `pf_image_format.h` is correct as written.** The colour-bar test
pattern rendered with the right colours in the right places, and a photo rendered with
correct geometry — no left/right swap, no top/bottom split, no transposition.

So: the panel behaves as Seeed_GFX models it, we pack *framebuffer* nibbles (not panel
wire codes), and `pfrm/palette.py` needs no correction. This was risk #2 in the original
plan. It is closed.

```
White 0x0   Green 0x2   Red 0x6   Yellow 0xB   Blue 0xD   Black 0xF
```

An all-white frame is all `0x00`. Anything outside the set falls through to white, so an
encoder bug looks washed-out rather than like noise — which is a deliberate property,
not an accident.

## Settled: real timings

Measured from the device's own `state` payload, not estimated:

| | |
|---|---|
| Full panel refresh | **~30.2 s** (`last_render_ms` 30210) |
| Whole wake, including a render | **~52 s** (`awake_ms` 51887) |
| WiFi connect, cached fast path | **~0.6 s** (`wifi_ms` 631) |
| Battery at full charge | 4216 mV → 100% |
| RSSI at its mounted position | −26 dBm |

The cached-WiFi path works and is worth what it costs. The 30 s refresh matches the
panel datasheet, so `PF_AWAKE_BUDGET_RENDER_MS` (120 s) has comfortable headroom — but
note a render wake is ~52 s, which is why the *default* 45 s budget must be extended
before any render begins. See `docs/TRAPS.md`.

## Settled, and it is bad news: OTA rollback does not work

**Tested on hardware, 2026-08-22.** A deliberately broken build was flashed via OTA. It
survived a second wake and was never rolled back.

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is present in the sdkconfig, but the
**prebuilt arduino-esp32 bootloader shipped by pioarduino does not act on it**. The
anti-rollback machinery in the application (`esp_ota_mark_app_valid_cancel_rollback()`
at PUBLISH_STATE) runs correctly and is still worth keeping, but nothing enforces it.

This matters more than it sounds. It was the stated justification for pull-OTA on a
battery device inside a frame on a wall:

> A build that panics on boot, or can't reach the network, rolls itself back
> automatically. For a battery device inside a frame on a wall this is not optional.

That safety net is **not there**. The consequences:

- A build that boots but cannot reach WiFi or the image server **cannot recover itself**
  and cannot be updated over the air. Recovery is a USB cable.
- Therefore: test any change to WiFi, HTTP, or the state machine's early stages over
  USB *before* publishing it as an OTA image.
- The guard in `firmware/src/net/ota.cpp` that records the installed sha256 in NVS and
  refuses to reinstall the same hash is doing load-bearing work — it is what stops a
  version-mismatch loop from flashing the same image forever. Do not remove it.

The long-term fix is building a custom bootloader with rollback genuinely enabled, or
moving to ESP-IDF. Neither is done.

## Still unmeasured — and one of these dominates the whole project

**1. Deep-sleep current has never been measured.** This was risk #1 in the plan and it
is still open. At 150 µA sleep is ~17% of the budget; at 1 mA it *is* the budget and no
software decision in this repo matters.

It needs a µA-capable instrument — PPK2, Joulescope, µCurrent. **A USB power meter
cannot resolve 150 µA**, so "I plugged in a USB meter and it read 0.00 A" is not a
measurement.

Measure it before and after the panel power-down path, because Seeed_GFX works against
you twice here:
- its `EPD_SLEEP` only asserts `TFT_CS` and never reaches the **slave** controller, so
  half the panel may stay powered;
- nothing in the library ever lowers `TFT_ENABLE` (GPIO43).

The firmware handles both explicitly. Whether it handles them *successfully* is exactly
what has not been confirmed.

**2. Panel refresh current.** Sets the low-battery gate. Currently guessed.

**3. The soak test.** No 72-hour battery discharge run has been done, so
`PF_POLL_DEFAULT_S` (900 s) is still the original guess and the "roughly 100 days"
estimate in `firmware/README.md` remains arithmetic, not evidence.

Until 1 and 3 are done, **nobody knows how long this thing runs on a charge.**

## Known defect: counters reset on OTA reboot

`wake_count` and `total_renders` live in `RTC_DATA_ATTR` memory, which does not survive
the reboot that an OTA install performs. After the last OTA the device reported
`wake_count: 2` despite having been through many more.

Harmless for operation, misleading for diagnosis — if these numbers look implausibly
low, check whether an OTA landed recently before concluding the device is rebooting.
The fix is to mirror both into NVS alongside the other lifetime counters; it has not
been done.

## Power and safety

Keep the ESP32 brownout detector at its default trip point. Do not disable it to "get
through" a refresh — the `render_busy` NVS flag plus cache-recovery path exists
precisely so that a brownout mid-refresh is survivable, and that design assumes the
brownout detector actually fires.
