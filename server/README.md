# photoframe-webhook

Twilio MMS in, panel-native blob out.

## Endpoints

| Path | Method | Exposure | |
|---|---|---|---|
| `/mms` | POST | **public** | Twilio webhook. Rejects anything without a valid `X-Twilio-Signature`. |
| `/latest.pfrm` | GET | LAN only | What the frame polls. `ETag` + `Content-Length`, honours `If-None-Match`. |
| `/latest.png` | GET | LAN only | Preview of exactly what the panel will show. |
| `/firmware.bin` | GET | LAN only | OTA image for the frame. `Content-Length` is load-bearing. |
| `/firmware.json` | GET | LAN only | Version + sha256 of the published image. |
| `/status` | GET | LAN only | JSON: current image metadata, published firmware, and the frame's last retained MQTT state. |
| `/healthz` | GET | LAN only | Probe target. |

The exposure column is enforced by routing, not by the app — both Services point at the
same pod. See [DEPLOYING.md](DEPLOYING.md).

## Texting it

Send a photo and it goes up. Send `/help` for the list. Admins (`ADMIN_NUMBERS`) also get:

- `/status` — reads the frame's retained MQTT state topic and texts it back. This is
  the payoff of the frame publishing state as a *retained* topic: a device that is
  asleep 99% of the time can still answer questions about itself.
- `/refresh` — publishes a changing token to `cmd/clear`, so the frame forgets its
  stored ETag and re-fetches on its next wake.

Anyone not in `ADMIN_NUMBERS` gets the ordinary "send me a photo" reply rather than a
refusal — there is no reason to tell a stranger which commands exist.

## Running locally

```bash
python3 -m venv ../.venv && ../.venv/bin/pip install -r requirements.txt
cp .env.example .env          # then fill it in
env $(grep -v '^#' .env | xargs) STORE_DIR=/tmp/pf ../.venv/bin/python app.py
```

Twilio signature validation is on in local runs too, so the easiest way to exercise the
webhook by hand is the test suite, which signs its own requests:

```bash
../.venv/bin/python -m pytest tests -q
```

## Notes on the image pipeline

Lives in [`../pfrm/render.py`](../pfrm/render.py) and is shared with
`../tools/encode_image.py`, so what you get from the CLI is byte-for-byte what a texted
photo produces.

- **Letterbox, don't crop** (`IMAGE_CROP=false`). For group photos, cropping somebody
  out of frame is worse than a white bar, and white bars vanish against the panel's own
  border.
- **Saturation is boosted to 1.4 by default.** E-ink primaries are muted; without a
  pre-boost a dithered photo reads like a washed-out newspaper print.
- **Carriers re-encode MMS hard.** You will sometimes receive 500×500. The service
  upscales with Lanczos and warns the sender by SMS when the short edge is under 400 px,
  rather than silently putting something soft on the wall.
- **Twilio media is deleted after download** (`DELETE_TWILIO_MEDIA=true`). We already
  have the only copy we need, and friends' photos should not sit on a third party's
  servers indefinitely.


## Shipping firmware over the air

```bash
python tools/publish_firmware.py            # build, ship, announce
python tools/publish_firmware.py --dry-run  # show what it would do
```

It builds, computes the sha256, copies the image into the pod's volume with an atomic
rename, and publishes a retained `cmd/ota` doc. The frame reads that on its next wake,
fetches into the inactive OTA slot, verifies the hash **before** committing, and reboots.

There is **no upload endpoint**, on purpose. This service has a public route, and an
unauthenticated write path that lands executable code on a device is not something to
expose to the internet. Publishing goes through `kubectl cp`, which already requires
cluster credentials.

If the new image cannot complete a full wake — WiFi, fetch, MQTT publish — it is never
marked valid and the bootloader rolls back to the previous slot automatically. That is
what makes over-the-air updates safe on a device you would otherwise have to take off
a wall and open up.
