# Traps

Every entry here is something that looked like a bug and was not, or looked correct and
was not. They cost real debugging time. Several are places where the *obvious* fix is
the wrong one, so they are written as "if you are about to do X, don't, and here is
why."

---

## 1. `esp_rom_crc32_le` is already zlib. Do not add tildes.

**`firmware/src/core/crc32.h` is a pass-through on purpose.**

The standard CRC-32/ISO-HDLC idiom is `~crc32(~seed, data, len)`, and every instinct
says a bare ROM function must need that wrapper. It does not: `esp_rom_crc32_le`
inverts internally **at both ends**, so

```c
esp_rom_crc32_le(0, buf, len) == zlib.crc32(buf)      // exactly, bit for bit
```

and it chains from `0` across HTTP chunks with no extra work.

Adding the wrapper broke every render with `header crc 0aa0f655 != 60164480`.

**The part that makes this genuinely dangerous:** the host conformance test in
`tools/test_format.py` had modelled the ROM function the same wrong way, so it happily
*proved the broken version correct*. A green test suite did not save us — the hardware
did. The test now pins the real convention against values observed on the device, so it
can't collude with a wrong implementation again.

If you change anything in this area, verify against a physical render, not against the
test alone.

---

## 2. Extend the watchdog *before* the slow thing, not after

`core/deadline.h` force-sleeps the device when its budget expires. `on_panic()` sleeps
**immediately, without publishing anything** — which means a blown deadline and a dead
button look identical from the outside. That cost three separate silent-failure
investigations.

Two ordering rules, both currently correct in `app_state_machine.cpp`, both easy to
undo by accident:

- `pf::extend_panic_sleep(PF_AWAKE_BUDGET_RENDER_MS)` runs **before** `cache::save()`,
  not after. The cache write is slow enough to blow the default 45 s budget on its own.
- `PF_AWAKE_BUDGET_OTA_MS` (180 s) wraps `maybe_update()`. A 1.17 MB download does not
  fit in 45 s.

A render wake legitimately takes ~52 s (see `docs/HARDWARE.md`). The default budget is
45 s. If you add a slow step and don't extend the budget, the device will sleep in the
middle of it and tell you nothing.

The firmware now stashes `pending_error` / `pending_awake_ms` in RTC memory and reports
them on the **next** wake, so a trip is at least visible after the fact. Keep that.

---

## 3. Two ways to cause an infinite OTA reflash loop

Both were hit. The device flashed 1.17 MB twice, 50 seconds apart, on battery.

**Cause A — the publisher lied about the version.** `tools/publish_firmware.py` was
announcing `git describe` output taken at publish time while the binary it uploaded
contained a *different* version string. The device installed it, rebooted, compared the
running version against the announced one, saw a mismatch, and reinstalled. Forever.

Fixed three ways in the tool, all of which matter:
- it **builds first, then reads the version** out of the generated `pf_version.h`
  (reading before building tripped its own staleness guard with a misleading message);
- it verifies the version string is actually present in the binary;
- it refuses to publish a dirty tree unless `--allow-dirty`.

**Cause B — no idempotence on the device.** The firmware now records the installed
image's sha256 in NVS and refuses to install a hash it has already installed:

```c
if (strcasecmp(installed, sha256_hex) == 0) { /* refuse; would loop */ }
```

Given that **rollback does not work on this board**, this guard is the only thing
standing between a version-mismatch bug and a battery flattened by reflashing. Do not
remove it as "redundant" — it is the backstop for cause A, not a duplicate of it.

---

## 4. A missing field fails the *entire* ExternalSecret

This bit the project three times, and once took down credentials cluster-wide for
72 days without anyone noticing.

`template.data` renders `{{ .SOME_FIELD }}` against the extracted 1Password item. If
that field **does not exist**, the render fails and the *whole Secret* stops syncing —
including every other key in it that was perfectly fine. The failure is silent from the
application's point of view; the pod just keeps running with stale or absent values.

**The discipline: probe with a throwaway ExternalSecret before adding a template
reference.** Confirm the field exists and is non-empty, then wire it up.

Also worth knowing, from the same family of bugs elsewhere in the cluster: an
ExternalSecret can report `SecretSynced: True` while the value your app reads is an
**empty string**, because the field exists but is blank. `SecretSynced` means "I copied
what was there," not "what was there is useful." When a service behaves as though a
credential is missing, dump the *lengths* of the secret's keys — not just their names.

---

## 5. Deployment gotchas that are not about this code at all

These are cluster-side and cost more time than anything in the firmware. Manifests live
in `kube-saturn`, under `kubernetes/apps/home-automation/photoframe-webhook/`.

**The Dockerfile must glob.** It used a hand-maintained list of source files and quietly
omitted `notify.py`, producing `ModuleNotFoundError` and `CrashLoopBackOff` — while all
43 unit tests passed, because tests run against a checkout where the file exists. It is
now `COPY server/*.py ./server/`, and **CI builds the image and imports the app inside
it**. Keep that smoke test; it is the only thing that catches this class of error.

**Pin the image by digest, not `latest`.** Spegel (the P2P registry mirror) served a
stale `latest` across nodes *despite* `imagePullPolicy: Always`, so a "deployed" fix
wasn't running. Renovate automerges the digest bumps.

**app-template suffixes Service names.** Defining a second Service renames both — the
backend is `photoframe-webhook-app`, not `photoframe-webhook`. A bare name gives
`HTTPRoute` → `BackendNotFound`.

**Do not annotate the LAN Service with a hostname.** The LoadBalancer Service and the
HTTPRoute both claimed `photoframe.markmckessock.com`, so external-dns wrote an A record
pointing the public name at a private LB IP. The annotation is removed; keep it that way.

**Cloudflare Access will silently eat Twilio's POST.** A catch-all Access application
covers `*.markmckessock.com`, so Twilio received a **302 to a login page** and reported
`11200 HTTP retrieval failure` — which reads like the server is down. There is now an
explicit bypass Access application for this hostname in `terraform/cloudflare/`. If MMS
delivery breaks after any Zero Trust change, check this first.

---

## 6. `TWILIO_WEBHOOK_URL` must match byte for byte

The signature is computed over that exact string. A trailing slash, `http` instead of
`https`, or a stale hostname makes **every** request 403 — not some, all. It is the most
common first-deploy failure and it does not look like a configuration problem.

---

## 7. GPIO43 is the panel power pin *and* UART0 TX

Logging on UART0 fights the display rail. Serial is **USB CDC only**
(`ARDUINO_USB_CDC_ON_BOOT=1`). This is not a preference.
