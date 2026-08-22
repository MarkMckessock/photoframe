#!/usr/bin/env python3
"""Build a firmware image, put it on the frame's image server, and announce it over MQTT.

There is deliberately no upload endpoint on the service: it has a public route, and an
unauthenticated write path that lands executable code on a device is not something to
expose. The image goes in via `kubectl cp` instead, which already requires cluster
credentials.

    python tools/publish_firmware.py                 # build, ship, announce
    python tools/publish_firmware.py --dry-run       # show what would happen
    python tools/publish_firmware.py --no-announce   # stage it without triggering

The frame picks it up on its next wake, verifies the sha256 BEFORE committing, and
reboots. If the new image cannot complete a wake it rolls itself back automatically.
"""

import argparse
import hashlib
import json
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "firmware/.pio/build/photoframe/firmware.bin"
PIO = Path.home() / ".platformio/penv/bin/pio"

NAMESPACE = "home-automation"
SELECTOR = "app.kubernetes.io/name=photoframe-webhook"
REMOTE_DIR = "/data"
BROKER = "10.0.70.131"
TOPIC = "home/photoframe/cmd/ota"
IMAGE_URL = "http://10.0.70.133/firmware.bin"


def run(cmd, **kw):
    print("  $", " ".join(shlex.quote(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def git_version():
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=ROOT, text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--no-build", action="store_true", help="use the existing binary")
    ap.add_argument("--no-announce", action="store_true",
                    help="stage the image but do not publish cmd/ota")
    ap.add_argument("--version", help="override the version string (default: git describe)")
    ap.add_argument("--url", default=IMAGE_URL)
    ap.add_argument("--broker", default=BROKER)
    args = ap.parse_args()

    version = args.version or git_version()
    if not version:
        sys.exit("could not determine a version; pass --version")
    if "dirty" in version and not args.version:
        print(f"warning: working tree is dirty -- version is {version!r}", file=sys.stderr)

    if not args.no_build:
        print("building...")
        run([str(PIO), "run", "-d", str(ROOT / "firmware"), "-e", "photoframe"],
            stdout=subprocess.DEVNULL)

    if not BIN.exists():
        sys.exit(f"no firmware binary at {BIN}")
    data = BIN.read_bytes()
    sha = hashlib.sha256(data).hexdigest()

    meta = {"version": version, "sha256": sha, "bytes": len(data),
            "published_at": int(time.time())}
    print(f"\n  version {version}\n  sha256  {sha}\n  bytes   {len(data)}\n")

    payload = json.dumps({"version": version, "url": args.url, "sha256": sha})

    if args.dry_run:
        print("dry run; would copy the image to the pod and publish:")
        print(f"  {TOPIC} (retained) {payload}")
        return 0

    pod = subprocess.check_output(
        ["kubectl", "-n", NAMESPACE, "get", "pod", "-l", SELECTOR,
         "-o", "jsonpath={.items[0].metadata.name}"], text=True).strip()
    if not pod:
        sys.exit("no photoframe-webhook pod found")
    print(f"pod: {pod}")

    # Copy to a temp name and move into place, so the frame can never fetch a
    # half-written image -- the same reason the photo path uses an atomic rename.
    meta_file = ROOT / "firmware/.pio/build/photoframe/firmware.json"
    meta_file.write_text(json.dumps(meta, indent=2))

    run(["kubectl", "-n", NAMESPACE, "cp", str(BIN), f"{pod}:{REMOTE_DIR}/firmware.tmp"])
    run(["kubectl", "-n", NAMESPACE, "exec", pod, "--",
         "mv", f"{REMOTE_DIR}/firmware.tmp", f"{REMOTE_DIR}/firmware.bin"])
    run(["kubectl", "-n", NAMESPACE, "cp", str(meta_file),
         f"{pod}:{REMOTE_DIR}/firmware.json"])

    if args.no_announce:
        print("\nstaged but not announced. To trigger:")
        print(f"  mosquitto_pub -h {args.broker} -t {TOPIC} -r -m {shlex.quote(payload)}")
        return 0

    # Retained: the frame is asleep when this is published and reads it on next wake.
    run(["mosquitto_pub", "-h", args.broker, "-t", TOPIC, "-r", "-m", payload])
    print(f"\nannounced {version}. The frame updates on its next wake.")
    print("watch:  mosquitto_sub -h %s -t 'home/photoframe/state' -v" % args.broker)
    return 0


if __name__ == "__main__":
    sys.exit(main())
