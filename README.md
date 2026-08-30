# photoframe

A 13.3" colour e-ink frame on the wall. Friends text a photo to a Twilio number and it
shows up. Runs for months on a battery.

```
   phone ──MMS──▶ Twilio ──POST /mms──▶ photoframe-webhook ──▶ /data/latest.pfrm
                                          (k8s, public route)          │
                                                                       │ LAN only
   ESP32-S3 ◀── 200 + ETag / 304 ── GET /latest.pfrm ◀──────────────────┘
      │
      └──▶ 13.3" Spectra 6 panel, then back to deep sleep
```

## What's here

| | |
|---|---|
| [`firmware/`](firmware/) | ESP32-S3 firmware for the Seeed XIAO ePaper Board EE02. PlatformIO. |
| [`server/`](server/) | The Flask service: Twilio webhook, image pipeline, image endpoint. |
| [`pfrm/`](pfrm/) | The shared image format and rendering pipeline. Imported by both the server and the CLI tools. |
| [`tools/`](tools/) | Offline encoder, a test HTTP server with fault injection, and the wire-format conformance test. |
| [`docs/`](docs/) | [Operations](docs/OPERATIONS.md) — the runbook. [Hardware](docs/HARDWARE.md) — what bring-up actually found. [Traps](docs/TRAPS.md) — things that look like bugs and are not. |

Deployment manifests are **not** here — they live in the `kube-saturn` repo alongside
every other cluster app. See [`server/DEPLOYING.md`](server/DEPLOYING.md).

## The two decisions that shape everything

**All image processing happens server-side.** The cluster does the EXIF rotation, the
resize, the saturation boost, the Floyd–Steinberg dither to six colours, and the 4 bpp
packing — in exactly the byte layout Seeed_GFX's framebuffer uses. The firmware
validates a CRC and `memcpy`s the blob onto the panel. Nothing is decoded on the MCU.

That is what makes a battery device viable: the common wake is a few seconds long.

**HTTP for the image, MQTT for control.** One conditional `GET /latest.pfrm`;
`If-None-Match` → `304` is the answer almost every time and costs about two hundred
bytes. MQTT carries state, availability, commands and Home Assistant discovery, and
never more than a few hundred bytes.

The alternative — a retained MQTT message with the image in it — would park a megabyte
in the broker forever, re-send it to any wildcard subscriber, need a hand-rolled second
topic to avoid pulling it on every wake, and have no way to resume a transfer that dies
at 900 KB. `ETag`/`304` is that whole protocol, already written and already deployed in
every HTTP stack.

## The shared contract

`pfrm/format.py` and `firmware/include/pf_image_format.h` describe the same 64 bytes.
Change one and you must change the other:

```
[ 64-byte header ][ 960000 bytes of packed pixels ]   = 960064 total
```

4 bpp, two pixels per byte, high nibble is the even-x pixel, stride 600, little-endian.
Both header and payload carry a CRC-32. Pixels are packed in Seeed_GFX's *framebuffer*
nibble encoding, not the panel's wire codes — see `pfrm/palette.py`.

`tools/test_format.py` reads the offsets back out of the **C header** and checks the
Python encoder against them, so the two cannot drift apart unnoticed. It also checks
that every interesting corruption is rejected with the right error, and that the CRC
convention is the zlib one — `esp_rom_crc32_le` is not, which is why
`firmware/src/core/crc32.h` exists.

## Working on it

```bash
python3 -m venv .venv && .venv/bin/pip install -r server/requirements.txt pytest

.venv/bin/python -m pytest server/tests -q     # 22 tests, no network
.venv/bin/python tools/test_format.py          # wire format conformance
~/.platformio/penv/bin/pio run -d firmware -e photoframe -e bringup
```

CI runs all three on every push.

### Seeing what a photo will look like, with no hardware

```bash
.venv/bin/python tools/encode_image.py photo.jpg -o /tmp/latest.pfrm \
    --preview /tmp/what-the-panel-shows.png
.venv/bin/python tools/serve_test.py /tmp/latest.pfrm --port 8080
```

Point `IMAGE_URL` in `firmware/src/secrets.h` at your laptop and the frame will fetch
from it. `serve_test.py` can also truncate, corrupt, stall or 500 the response on
demand, which is how the firmware's failure paths get exercised without waiting for a
real network to misbehave.

## Every photo is kept

The render pipeline destroys the original — resized, saturated, dithered to six colours,
packed to 4 bpp, none of it reversible. So before any of that happens, the untouched
original is written to an NFS mount of the NAS, inside the directory the photo library
already scans, and indexed in SQLite: when it arrived, who sent it, and where it landed.

Archiving is best-effort by construction. A NAS that is down or full must not stop a
photo reaching the frame — the frame is the product; the archive is the bonus. See
[`server/archive.py`](server/archive.py).

## Status

**Running on the wall.** Firmware `b6811f1`, rendering in ~30 s, waking on a schedule
and on a button, updating itself over the air.

Two things from the original plan are still open, and one of them matters:

- **Deep-sleep current has never been measured**, so nobody knows how long it runs on a
  charge. It needs a µA-capable meter, not a USB power meter.
- **OTA rollback does not work on this board** — tested, not assumed. A build that boots
  but cannot reach the network can only be recovered over USB.

Both are written up in [`docs/HARDWARE.md`](docs/HARDWARE.md), along with the things
bring-up *did* settle. [`docs/TRAPS.md`](docs/TRAPS.md) is the list of things that cost
a debugging session each; read it before changing the CRC, the OTA publisher, or
anything about the ExternalSecret.
