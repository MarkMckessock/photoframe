# Operations

Everything needed to diagnose a live frame. `server/DEPLOYING.md` covers first-time
setup; this covers the thing already running on the wall.

## Coordinates

| What | Where |
|---|---|
| Frame (DHCP, on the IoT VLAN) | `10.0.60.152` |
| Image + firmware server, LAN only | `10.0.70.133` |
| Mosquitto | `10.0.70.131:1883`, anonymous |
| Public route (Twilio only) | `https://photoframe.markmckessock.com/mms` |
| Twilio number | (415) 855-3459 |
| Photo archive (NFS) | `granite.markmckessock.com:/volume1/Users/mmckessock/Pictures/Picture Frame Uploads` → `/archive` |
| Archive index | SQLite on PVC `photoframe-archive-db` → `/db/photos.db` |
| Kubernetes | ns `home-automation`, deploy `photoframe-webhook` |
| Manifests | `kube-saturn`, `kubernetes/apps/home-automation/photoframe-webhook/` |

The frame and the server are on **different VLANs**. Two firewall rules make this work:
frame → `10.0.70.133:80` (image + firmware) and frame → `10.0.70.131:1883` (MQTT). If a
frame goes permanently silent right after network changes, check those before touching
any code.

## Look at it

The retained MQTT state is the single most useful thing in the system. The device is
asleep >99% of the time, so this is a *snapshot of its last wake*, not live telemetry:

```bash
mosquitto_sub -h 10.0.70.131 -t 'home/photoframe/#' -v -W 5
```

The same payload, plus what the server thinks, in one place:

```bash
curl -s http://10.0.70.133/status | python3 -m json.tool
```

`/status` reports both halves: `frame` is whatever the device last published, `image` is
what the server currently holds. **Disagreement between the two `etag` values is the
single most diagnostic signal in this system** — it means a photo is waiting that the
frame has not rendered.

A healthy state payload looks like this:

```json
{"fw":"b6811f1","wake_cause":"button","battery_mv":4216,"battery_pct":100,
 "rssi":-26,"wifi_ms":631,"awake_ms":51887,"last_render_ms":30210,
 "result":"rendered","panel":"clean","consecutive_failures":0,"error":null,
 "next_wake_s":3600,"free_psram":6448016}
```

`result` ∈ `no_change | rendered | rendered_from_cache | no_image_yet |
deferred_low_battery | error`.

## "I texted a photo and nothing happened"

Work down this list. Most of the time the answer is the last one.

**1. Did the server receive it?**
```bash
kubectl -n home-automation logs -l app.kubernetes.io/name=photoframe-webhook --tail=50
curl -s http://10.0.70.133/status | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["has_image"], d["image"]["received_at"])'
```
No log line at all → Twilio never reached us. Check the Twilio console's error log; a
`11200 HTTP retrieval failure` almost always means Cloudflare Access is intercepting the
POST, not that the server is down (see `docs/TRAPS.md` §5). A 403 in *our* logs means
`TWILIO_WEBHOOK_URL` doesn't match byte for byte (§6).

**2. Do the ETags differ?**
```bash
curl -s http://10.0.70.133/status | python3 -c 'import sys,json; d=json.load(sys.stdin); print("server:",d["image"]["etag"]); print("frame :",d["frame"]["etag"])'
```
Same → the frame has already rendered this photo and the panel should be showing it.
Different → the frame simply has not woken up yet. Which is normal.

**3. Check `next_wake_s`.** This is the usual answer and it is not a fault.

- `3600` — night mode (22:00–07:00 local) **or** low battery. Nothing will happen until
  morning. This surprised us more than once.
- `900` — the default 15-minute poll.
- `300` — burst mode, for two hours after a photo lands.

**Press KEY1 if you want it now.** That is exactly what the button is for.

**4. Look at `result` and `error`.**
- `deferred_low_battery` — under 3600 mV. It is polling and reporting but will not spend
  30 seconds of panel current. Charge it. Note the ETag is deliberately *not* recorded as
  rendered, so the photo is not lost — it renders once charged.
- `error` with `consecutive_failures` climbing — read `error`. Backoff is
  5 m → 15 m → 30 m → 1 h.
- `no_image_yet` — the server has never held an image.

**5. Only now suspect the device.** `battery_mv` and `wake_cause` from the last wake
tell you whether it is waking at all. If the last state is hours stale and
`availability` is `offline` (the LWT — meaning *died unexpectedly*, not *asleep*), it
stopped mid-wake.

## Common tasks

```bash
# Force a refetch of an image the frame thinks it already has
mosquitto_pub -h 10.0.70.131 -t home/photoframe/cmd/clear -r -m "$(date +%s)"

# Change the poll interval live (read on next wake)
mosquitto_pub -h 10.0.70.131 -t home/photoframe/cmd/config -r \
  -m '{"poll_seconds":300,"night_start":23,"night_end":7}'

# See exactly what the panel should be showing
open http://10.0.70.133/latest.png
```

Remember every command is **retained** and acts on the *next wake*. Nothing happens
instantly unless you also press a button.

## Shipping firmware

```bash
.venv/bin/python tools/publish_firmware.py          # builds, verifies, uploads, announces
```

This builds first, reads the version out of the generated `pf_version.h`, checks that
string is really in the binary, refuses a dirty tree, uploads to the server, and
publishes the retained `cmd/ota` message. The device picks it up on its next wake.

**Read `docs/HARDWARE.md` on rollback before you publish anything.** It does not work on
this board. A build that boots but cannot reach WiFi or the image server cannot be
recovered over the air — only over USB. Test changes to WiFi, HTTP, or the early state
machine over a cable first.

```bash
kubectl -n home-automation rollout restart deploy/photoframe-webhook   # ship the server
```

## The photo archive

Every photo received is kept untouched on the NAS and indexed in SQLite. The archive
directory is a subdirectory of the one the photo library already scans, so photos appear
there with no import step.

Check it without a shell:

```bash
curl -s http://10.0.70.133/status  | python3 -c 'import sys,json; print(json.load(sys.stdin)["archive"])'
curl -s "http://10.0.70.133/archive?limit=5" | python3 -m json.tool
```

`archive.ready: false` means the mount or the database is not usable — the pod logs the
reason once at startup. **Photos still reach the frame in that state**; that is the
intended behaviour, not a fault to panic about, but nothing is being kept.

Query the index directly:

```bash
POD=$(kubectl -n home-automation get pod -l app.kubernetes.io/name=photoframe-webhook -o name | head -1)
kubectl -n home-automation exec $POD -- python3 -c "
import sqlite3
db = sqlite3.connect('/db/photos.db')
for r in db.execute('SELECT received_at, sender, path FROM photos ORDER BY id DESC LIMIT 10'):
    print(r)"
```

### If photos stop being archived

1. `curl -s http://10.0.70.133/status` → `archive.ready`. If false, read the pod logs.
2. **Is the pod stuck in `ContainerCreating`?** Then the NFS mount itself failed, which
   is a different and more disruptive failure — the whole service is down, not just the
   archive. The usual cause is the export path not existing on the NAS. The directory
   name contains spaces; it is quoted in the HelmRelease and must stay quoted.
3. **Permissions.** The pod writes as UID 1000. `fsGroup` does *not* apply to NFS
   volumes — the NAS's own permissions govern, so the target directory must be writable
   by that UID. It is currently `0777`, which is why this works.
4. The database is on its own PVC precisely so a NAS outage cannot corrupt it. If the
   index and the directory ever disagree, the files on the NAS are the source of truth —
   the index can be rebuilt from them, but not the other way round.

## Notifications

Pushover, controlled by **one** env var: `NOTIFY_ENABLED` (default off). Two events:
a photo rendered, and battery critically low. Low-battery uses hysteresis — fires at
3500 mV, re-arms only above 3800 mV — so a frame hovering at the threshold does not
notify every wake. State persists in `notify.json` on the volume, so a pod restart does
not re-fire.

### Sender names

Notifications say "Photo from Alice" rather than a phone number, without any name or
number appearing in this repo. `server/contacts.py` parses a `CONTACTS` JSON env var
supplied through the ExternalSecret, keyed on the **last 10 digits** so formatting
differences don't matter:

```json
{"5551234567": "Alice", "5559876543": "Bob"}
```

An unknown number renders as a masked `…4567`, never in full.

## Privacy invariants

This repo is **public** and the frame receives photos from friends. These are not
stylistic preferences:

- **Names and phone numbers never enter git.** They live only in 1Password, reaching the
  pod through the ExternalSecret. That is the whole reason `contacts.py` reads an env var
  instead of a checked-in mapping.
- **Full numbers in local logs are fine and intentional** — that is how you identify a
  sender when debugging. They must not appear in notifications or anything public.
- **`/latest.pfrm`, `/latest.png` and `/status` are LAN-only** and must stay that way.
  They serve a photo someone sent you in confidence, and `/status` additionally exposes
  the sender's full number.

The isolation is entirely in the routing: the `HTTPRoute` matches `PathPrefix: /mms` and
nothing else, while the image endpoints are reached only through a separate LoadBalancer
Service with no route attached. **Both Services point at the same pod — widening that
path match publishes the photos.**

Verify after any routing change:

```bash
curl -sI https://photoframe.markmckessock.com/latest.pfrm   # must NOT be 200
```
