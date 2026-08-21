# photoframe

Firmware for a battery-powered 13.3" colour e-ink photo frame. Friends text a photo to
a Twilio number; a service in the cluster turns it into a panel-native blob; the frame
wakes up every so often, notices, and puts it on the wall.

**Hardware:** Seeed XIAO ePaper Display Board **EE02** (XIAO ESP32-S3 Plus — 16 MB
flash, 8 MB *octal* PSRAM) driving a **13.3" E Ink Spectra 6** panel, 1200×1600, six
colours, on a LiPo.

---

## How it works

Two protocols, each doing the thing it is good at:

| | |
|---|---|
| **Image** | one conditional `GET /latest.pfrm`. `If-None-Match` → `304` is the common answer and costs about two hundred bytes. |
| **Control** | MQTT: state, availability, commands, Home Assistant discovery. Never more than a few hundred bytes. |

**All image processing happens server-side.** The cluster does the EXIF rotation, the
resize, the saturation boost, the Floyd–Steinberg dither to six colours, and the 4 bpp
packing — in exactly the byte layout Seeed_GFX's framebuffer uses. The firmware checks
a CRC, `memcpy`s the blob into the framebuffer, and calls `update()`. That division is
what makes a battery device viable: the common wake is a few seconds long.

The device is asleep more than 99% of the time and cannot receive a push, so commands
are **retained**: the broker holds the desired state and the frame reads it when it
next wakes. Same idea as `light/registry` in `dmx-engine`, applied to a device that is
mostly switched off.

### Why this is not built like the splitflap firmware

No `Task<T>`, no task-per-concern. That layout is right for the splitflap, which runs
continuously and has genuinely concurrent work — kHz motor stepping must not be starved
by an MQTT reconnect. This device is a strictly serial pipeline: you cannot fetch before
you connect, render before you validate, or sleep before you report. Concurrency buys
nothing measurable here and costs extra stacks plus nondeterministic ordering, which
makes *"why was I awake for forty seconds"* much harder to answer — and on battery that
question is the whole ballgame.

So the firmware is one function, top to bottom, ending in `esp_deep_sleep_start()`.
`loop()` is unreachable. See `firmware/src/app_state_machine.cpp`.

The one concession to concurrency is `core/deadline.h`: an `esp_timer` that force-sleeps
the device no matter what it is doing, so a wedged network stack costs one wake cycle
rather than a whole battery.

### Failure policy: the photo stays up

E-ink retains its last image for free, and a full refresh takes ~30 seconds, costs real
charge, and cannot be partially undone. Replacing a photo of a friend's dog with
`MQTT CONNECT FAILED rc=-2` would therefore be expensive *and* destroy the point of the
object. **Errors never touch the panel.** They are reported over MQTT and surface in
Home Assistant.

Exactly five things may write to the glass:

1. a validated new photo;
2. the one-time setup card, on a device that has never shown anything;
3. crash/brownout recovery, when the panel genuinely *is* showing garbage;
4. a one-time critical-battery warning, composited onto the cached photo rather than
   replacing it — the only failure a user can actually act on;
5. the diagnostics screen, which you asked for by holding a button.

---

## MQTT topics

```
TOPIC                                RET  QOS  DIR   PAYLOAD
home/photoframe/availability          Y    1   dev   "online" | "asleep" | "offline" (LWT)
home/photoframe/state                 Y    1   dev   status JSON
home/photoframe/cmd/config            Y    1   srv   {"poll_seconds":900,"night_start":22,"night_end":7}
home/photoframe/cmd/ota               Y    1   srv   {"version":"1.2.0","url":"...","sha256":"..."}
home/photoframe/cmd/clear             Y    1   srv   any changing token; forgets the stored ETag
homeassistant/sensor/photoframe_*/config  Y  1  dev  discovery
```

`availability` is three-valued so that `offline` — the LWT — means *died unexpectedly*,
which is worth an alert, rather than *asleep*, which is not.

HA discovery deliberately omits `availability_topic` and uses `expire_after` at 2.5× the
poll interval instead. A device that is offline by design would otherwise show every
entity as unavailable essentially always.

Set the poll interval live:

```bash
mosquitto_pub -h 10.0.70.131 -t home/photoframe/cmd/config -r \
  -m '{"poll_seconds":300,"night_start":23,"night_end":7}'
```

Force a re-fetch of an image the frame thinks it already has:

```bash
mosquitto_pub -h 10.0.70.131 -t home/photoframe/cmd/clear -r -m "$(date +%s)"
```

---

## The `PFRM` wire format

`firmware/include/pf_image_format.h` is the contract; `tools/encode_image.py` is the
reference implementation of it. Change one, change the other in the same commit.

```
[ 64-byte header ][ 960000 bytes of packed pixels ]   = 960064 total
```

4 bpp, two pixels per byte, **high nibble is the even-x (left) pixel**, row-major,
stride 600, no padding. Little-endian throughout.

Pixels are packed in **Seeed_GFX's framebuffer encoding**, not the panel's wire codes —
the library's `COLOR_GET` macro translates on the way out over SPI:

| Colour | We pack | Panel sees |
|---|---|---|
| White | `0x0` | `0x1` |
| Green | `0x2` | `0x6` |
| Red | `0x6` | `0x3` |
| Yellow | `0xB` | `0x2` |
| Blue | `0xD` | `0x5` |
| Black | `0xF` | `0x0` |

An all-white frame is all `0x00` bytes. Anything outside the set falls through to white,
so an encoder bug looks washed out rather than like noise.

Both the header and the payload carry a CRC-32 (`esp_rom_crc32_le`, same polynomial as
zlib's `crc32`). The header's own CRC lets a wrong-format transfer abort after 64 bytes
instead of after a megabyte, and the payload CRC is checked before any refresh — thirty
seconds of panel current is far too much to spend on a blob nobody verified, especially
when the result *sticks* until the next photo arrives.

---

## Building

PlatformIO. Note `pio` is not on `PATH` on this machine:

```bash
~/.platformio/penv/bin/pio run -e photoframe            # build
~/.platformio/penv/bin/pio run -e photoframe -t upload  # flash over USB
~/.platformio/penv/bin/pio device monitor               # USB CDC console
```

`cp firmware/src/secrets.h.example firmware/src/secrets.h` and fill it in first.

Three environments:

- **`photoframe`** — the real thing.
- **`bringup`** — panel and power only, no radio. Use this first.
- **`photoframe_ota`** — wireless upload, only reachable during a maintenance window.

The platform is the **pioarduino** fork, because the official `espressif32` has no XIAO
ESP32S3 **Plus** board and is frozen on Arduino 2.0.x without the ESP-IDF 5.x sleep and
OTA APIs. The first build pulls a large toolchain.

Serial is **USB CDC only**. GPIO43 is the panel's power-enable pin on this board and
also the UART0 TX pad; logging on UART0 fights the display rail.

---

## Buttons

| | Short press | Long press (5 s) |
|---|---|---|
| **1** | force a re-fetch, ignoring the stored ETag | — |
| **2** | redraw from the local cache, no network — the "it looks ghosted" button | — |
| **3** | force a re-fetch | diagnostics screen + 5-minute ArduinoOTA window |

All three are EXT1 wake sources, so any of them gets an immediate check instead of
waiting for the next poll. A press that is released within 60 ms of the wake is treated
as a bump and goes straight back to sleep without touching the radio — a knock on the
wall otherwise costs as much charge as a real scheduled check.

---

## Bring-up

Each stage is independently checkable. **Do not skip stage 2**; two of the three numbers
that decide whether this project works can only be measured.

**0 — toolchain.** `pio run -e bringup -t upload`, then watch the console. It asserts
8 MB PSRAM and 16 MB flash. If either is wrong, stop: the 960 KB framebuffer is
`ps_calloc`'d at static-init time and fails silently without octal PSRAM.

**1 — panel.** The bring-up firmware draws six labelled colour bars plus `LEFT` /
`RIGHT` / `TOP` / `BOTTOM` markers and four coloured corner squares. Photograph it.

This settles the two things the sources disagree about: which nibble produces which
colour, and how the panel's **two chip selects** map onto it. Seeed's driver treats it as
two side-by-side 1200×800 halves; other ports describe a top/bottom split. Correct
`pf_image_format.h` and `tools/encode_image.py` to match what you actually see *before*
writing any more of the image pipeline. Note the refresh duration the firmware prints.

**2 — sleep and current.** Press each button and confirm EXT1 wake fires and is
attributed correctly. Then **measure deep-sleep current with a µA-capable instrument**
(PPK2, Joulescope, µCurrent — a USB power meter cannot resolve 150 µA).

This is the single most important measurement in the project. At 150 µA, sleep is about
17% of the budget; at 1 mA it *is* the budget and no software decision matters. Measure
it before and after the panel power-down, because Seeed_GFX works against you here: its
`EPD_SLEEP` only asserts `TFT_CS` and never reaches the slave controller, and nothing in
the library ever lowers `TFT_ENABLE`.

**3 — WiFi.** Flash `photoframe`. Watch `wifi_ms` over ~20 boots, cold and cached.
Expect roughly 1 s on the cached path against 3.5 s with a full scan and DHCP. If that
gap is not there, stop and find out why — the battery budget leans on it.

**4 — MQTT.**
```bash
mosquitto_sub -h 10.0.70.131 -t 'home/photoframe/#' -v
```
Confirm the entities appear in Home Assistant and expire sensibly.

**4b — the wire format, with no hardware at all.**
```bash
python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt
.venv/bin/python tools/test_format.py
```
This reimplements the firmware's validation from the offsets *documented in
`pf_image_format.h`* — independently of `encode_image.py`'s struct string — and checks
that good blobs pass, that every interesting corruption is caught with the right error,
and that the CRC convention is the zlib one. Reorder a header field and update only one
side, and this fails. Run it before flashing anything.

**5 — the image path, with no phone and no cluster.**
```bash
.venv/bin/python tools/encode_image.py photo.jpg -o /tmp/latest.pfrm --preview /tmp/p.png
.venv/bin/python tools/serve_test.py /tmp/latest.pfrm --port 8080
# point IMAGE_URL in secrets.h at http://<laptop>:8080/latest.pfrm
```
Expect `200` → render on the first wake, `304` → straight back to sleep on the second.
`--preview` writes a PNG of exactly what the panel should show; compare them.

**6 — break it on purpose.** These are the paths that matter and each is one flag:
```bash
tools/serve_test.py blob --truncate 500000   # stall timeout + length check
tools/serve_test.py blob --corrupt           # payload CRC rejects; panel untouched
tools/serve_test.py blob --status 500        # error path, no refresh
tools/serve_test.py blob --stall 20          # mid-body stall
```
Also drop a bench supply to 3.0 V during a refresh: the next boot must find
`render_busy` set and recover the panel from the cache. And ship one deliberately broken
build to prove OTA rollback works (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on in this
Arduino build, and a deep-sleep wake counts as a boot, so an unconfirmed image is rolled
back at the next wake unless it completes a fetch).

**7 — end to end.** Deploy the webhook, point a Twilio MMS number at it, text a photo.

**8 — soak.** 72 h on battery with `battery_mv` graphed in HA. Linear-fit the discharge
and set the poll interval from that, not from the estimate below.

### Battery estimate — calibration only

2000 mAh, ~150 µA sleep *assumed*: a no-op check is ~4 s at ~95 mA ≈ 0.11 mAh; an image
update adds ~34 s ≈ 0.60 mAh. At the default 15-minute poll with three photos a day
that is **roughly 100 days**; at five minutes, roughly 40. Every number here is a guess
until stage 2.

Burst mode (five-minute polling for two hours after any photo arrives) and night mode
are what make the default interval feel responsive without paying for it all day.

---

## The server side

Not in this repo. It is a sibling of `splitflap_webhook`: same Flask + Twilio-signature
+ rate-limit skeleton, plus Pillow, deployed as another Flux app under
`kubernetes/apps/home-automation/`.

- `POST /mms` — **public** route, authenticated by Twilio signature. Fetch `MediaUrl0`,
  run it through the same pipeline as `tools/encode_image.py`, write the blob.
- `GET /latest.pfrm` — **LAN-only** Service on a Cilium LoadBalancer IP, the way
  mosquitto has `10.0.70.131`. Not on `*.markmckessock.com`: everything there is public
  by default, and the photos your friends send should not leave the network. `send_file`
  handles `ETag` and `If-None-Match` natively.
- Needs a small PVC for the blob — the webhook runs `readOnlyRootFilesystem: true` with
  only an emptyDir `/tmp`.

`tools/encode_image.py` is the code that drops in. No Mosquitto changes are needed; that
is the main dividend of keeping the megabyte off the message bus.

---

## Known unknowns

Ordered by how much they can hurt:

1. **Deep-sleep current** (stage 2). Dominates everything.
2. **Dual-controller geometry** (stage 1). Determines the server's packing.
3. **Panel refresh current.** Only matters at high photo volume, but it sets the
   low-battery gate.
4. **Battery ADC.** `pf_config.h` carries GPIO1 + an enable on GPIO6 and an empirical
   `PF_BATT_SCALE`, from the EE02 schematic. Check it against a multimeter and correct
   the constant there rather than adding a fudge factor in `battery.cpp`.
5. **No microSD.** The EE02 wiki's hardware overview lists none, so the image cache is
   LittleFS on the `storage` partition. If the schematic shows a slot, SD is a drop-in
   behind `image_cache`.
6. **The palette RGB anchors** in `encode_image.py` are approximations. Reconcile them
   against Seeed's own `dither.cpp` (`PAL_E6`) so the browser preview and the wall agree.
