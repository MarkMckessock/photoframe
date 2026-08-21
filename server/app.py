"""Twilio MMS -> e-ink photo frame.

Two jobs, deliberately split across two Kubernetes Services even though one process
serves both:

  POST /mms          public route, authenticated by Twilio's request signature.
                     Fetches the media, renders it to a panel-native blob, stores it.
  GET  /latest.pfrm  LAN-only Service. The frame polls this with If-None-Match, and
                     a 304 is the answer almost every time.

The image endpoint must not be public. Everything on *.markmckessock.com sits behind
the Cloudflare tunnel and is public by default, and the photos friends send should not
leave the network -- so the Gateway route matches only /mms, and /latest.pfrm is
reachable solely through the LoadBalancer address the frame is configured with.

All the expensive work happens here rather than on the MCU: the frame validates a CRC
and memcpy's the result onto the panel. See pfrm/render.py.

Configuration is entirely environment variables; required ones fail fast at import.
"""

import io
import json
import logging
import os
import sys
import time
from functools import wraps
from pathlib import Path

import requests
from flask import Flask, Response, abort, jsonify, request
from flask_limiter import Limiter
from twilio.request_validator import RequestValidator
from twilio.twiml.messaging_response import MessagingResponse

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from pfrm import PANEL_H, PANEL_W, etag_for  # noqa: E402
from pfrm.palette import DEFAULT_PALETTE, PALETTES  # noqa: E402
from pfrm.render import HEIF_SUPPORTED, encode, open_image  # noqa: E402

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)s %(name)s: %(message)s")
logger = logging.getLogger("photoframe")

app = Flask(__name__)


def _env_bool(name, default=False):
    return os.environ.get(name, str(default)).strip().lower() in ("1", "true", "yes", "on")


def _env_set(name):
    return {n.strip() for n in os.environ.get(name, "").split(",") if n.strip()}


# --- required ----------------------------------------------------------------
TWILIO_AUTH_TOKEN = os.environ["TWILIO_AUTH_TOKEN"]
TWILIO_ACCOUNT_SID = os.environ["TWILIO_ACCOUNT_SID"]
TWILIO_WEBHOOK_URL = os.environ["TWILIO_WEBHOOK_URL"]

# --- optional ----------------------------------------------------------------
STORE_DIR = Path(os.environ.get("STORE_DIR", "/data"))
ALLOWED_NUMBERS = _env_set("ALLOWED_NUMBERS")   # empty = anyone who knows the number
ADMIN_NUMBERS = _env_set("ADMIN_NUMBERS")
RATE_LIMIT = os.environ.get("RATE_LIMIT", "10 per minute; 50 per hour")

PANEL_WIDTH = int(os.environ.get("PANEL_WIDTH", PANEL_W))
PANEL_HEIGHT = int(os.environ.get("PANEL_HEIGHT", PANEL_H))
IMAGE_CROP = _env_bool("IMAGE_CROP", False)
IMAGE_SATURATION = float(os.environ.get("IMAGE_SATURATION", 1.4))
IMAGE_CONTRAST = float(os.environ.get("IMAGE_CONTRAST", 1.0))
IMAGE_PALETTE = os.environ.get("IMAGE_PALETTE", DEFAULT_PALETTE)

MAX_MEDIA_BYTES = int(os.environ.get("MAX_MEDIA_BYTES", 8 * 1024 * 1024))
MEDIA_TIMEOUT_S = float(os.environ.get("MEDIA_TIMEOUT_S", 20))
# Friends' photos should not sit on Twilio's servers indefinitely. We already have the
# only copy we need.
DELETE_TWILIO_MEDIA = _env_bool("DELETE_TWILIO_MEDIA", True)

MQTT_HOST = os.environ.get("MQTT_HOST", "")
MQTT_PORT = int(os.environ.get("MQTT_PORT", 1883))
MQTT_USERNAME = os.environ.get("MQTT_USERNAME") or None
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD") or None
TOPIC_ROOT = os.environ.get("MQTT_TOPIC_ROOT", "home/photoframe")

if IMAGE_PALETTE not in PALETTES:
    raise SystemExit(f"IMAGE_PALETTE={IMAGE_PALETTE!r} is not one of {sorted(PALETTES)}")

BLOB_PATH = STORE_DIR / "latest.pfrm"
PREVIEW_PATH = STORE_DIR / "latest.png"
META_PATH = STORE_DIR / "latest.json"

ACCEPTED_TYPES = {"image/jpeg", "image/jpg", "image/png", "image/gif", "image/webp",
                  "image/bmp", "image/heic", "image/heif"}

twilio_validator = RequestValidator(TWILIO_AUTH_TOKEN)
limiter = Limiter(app=app,
                  key_func=lambda: request.form.get("From", request.remote_addr),
                  storage_uri="memory://")

logger.info("store=%s panel=%dx%d palette=%s crop=%s saturation=%.2f heif=%s",
            STORE_DIR, PANEL_WIDTH, PANEL_HEIGHT, IMAGE_PALETTE, IMAGE_CROP,
            IMAGE_SATURATION, HEIF_SUPPORTED)
logger.info("allowlist=%d admins=%d (0 allowed = open) mqtt=%s",
            len(ALLOWED_NUMBERS), len(ADMIN_NUMBERS), MQTT_HOST or "disabled")


# --- storage -----------------------------------------------------------------

def _atomic_write(path, data):
    """Write via a sibling temp file and rename, so a reader never sees a partial blob.

    The rename is what makes this safe: POSIX guarantees a reader gets either the whole
    old file or the whole new one, never a half-written megabyte.
    """
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def store_image(blob, preview_png, meta):
    STORE_DIR.mkdir(parents=True, exist_ok=True)
    # Blob last: if we crash midway, a stale-but-consistent blob is better than a new
    # blob whose sidecars disagree with it.
    if preview_png is not None:
        _atomic_write(PREVIEW_PATH, preview_png)
    _atomic_write(META_PATH, json.dumps(meta, indent=2).encode())
    _atomic_write(BLOB_PATH, blob)


_etag_cache = {}


def read_blob():
    """Return (bytes, etag) for the current image, or (None, None).

    Opens first and stats the handle, so we always hash the same inode we return. The
    ETag is cached against (inode, mtime, size) -- a megabyte of sha256 is only a few
    milliseconds, but there is no reason to spend it on every poll.
    """
    try:
        f = open(BLOB_PATH, "rb")
    except FileNotFoundError:
        return None, None
    with f:
        st = os.fstat(f.fileno())
        key = (st.st_ino, st.st_mtime_ns, st.st_size)
        data = f.read()
    etag = _etag_cache.get(key)
    if etag is None:
        etag = etag_for(data)
        _etag_cache.clear()
        _etag_cache[key] = etag
    return data, etag


def read_meta():
    try:
        return json.loads(META_PATH.read_text())
    except (OSError, ValueError):
        return {}


# --- twilio ------------------------------------------------------------------

def validate_twilio_request(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        signature = request.headers.get("X-Twilio-Signature", "")
        if not twilio_validator.validate(TWILIO_WEBHOOK_URL, request.form, signature):
            logger.warning("Invalid Twilio signature from ip=%s", request.remote_addr)
            abort(403)
        return f(*args, **kwargs)
    return decorated


def fetch_media(url):
    """Download one MMS attachment, refusing anything implausibly large.

    Streamed with a hard cap rather than trusting Content-Length: this endpoint is
    reachable from the internet, and the signature check proves the *request* came from
    Twilio, not that the media behind it is well behaved.
    """
    resp = requests.get(url, auth=(TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN),
                        timeout=MEDIA_TIMEOUT_S, stream=True)
    resp.raise_for_status()
    chunks, total = [], 0
    for chunk in resp.iter_content(64 * 1024):
        total += len(chunk)
        if total > MAX_MEDIA_BYTES:
            resp.close()
            raise ValueError(f"media exceeds {MAX_MEDIA_BYTES} bytes")
        chunks.append(chunk)
    return b"".join(chunks)


def delete_media(url):
    try:
        r = requests.delete(url, auth=(TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN), timeout=10)
        logger.info("deleted media from Twilio: %s", r.status_code)
    except Exception:
        logger.exception("could not delete media from Twilio (not fatal)")


# --- mqtt (admin commands only; the frame never learns about images this way) ---

def mqtt_publish(topic, payload, retain=True):
    if not MQTT_HOST:
        return False
    try:
        import paho.mqtt.publish as publish
        auth = {"username": MQTT_USERNAME, "password": MQTT_PASSWORD} if MQTT_USERNAME else None
        publish.single(topic, payload=payload, hostname=MQTT_HOST, port=MQTT_PORT,
                       auth=auth, retain=retain, qos=1)
        logger.info("published %s", topic)
        return True
    except Exception:
        logger.exception("MQTT publish to %s failed", topic)
        return False


def mqtt_get_retained(topic, timeout=3.0):
    """Read a retained topic. Used to answer /status by SMS.

    This is the payoff of the frame publishing its state as a retained topic: a device
    that is asleep 99% of the time can still answer questions about itself.
    """
    if not MQTT_HOST:
        return None
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        return None

    result = {}

    def on_connect(client, userdata, flags, reason_code, properties=None):
        client.subscribe(topic, qos=1)

    def on_message(client, userdata, msg):
        result["payload"] = msg.payload.decode("utf-8", "replace")
        client.disconnect()

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        if MQTT_USERNAME:
            client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
        client.on_connect = on_connect
        client.on_message = on_message
        client.connect(MQTT_HOST, MQTT_PORT, keepalive=10)
        deadline = time.time() + timeout
        while "payload" not in result and time.time() < deadline:
            client.loop(timeout=0.1)
        client.disconnect()
    except Exception:
        logger.exception("could not read retained %s", topic)
        return None
    return result.get("payload")


# --- commands ----------------------------------------------------------------

HELP = ("Text me a photo and it goes on the frame.\n"
        "Admin: /status, /refresh, /help")


def handle_command(body, sender):
    cmd = body.strip().lower()
    if cmd in ("/help", "help"):
        return HELP
    if sender not in ADMIN_NUMBERS:
        logger.warning("command %r from non-admin %s ignored", cmd, sender)
        return None
    if cmd == "/status":
        raw = mqtt_get_retained(f"{TOPIC_ROOT}/state")
        if not raw:
            return "No status from the frame (it may not have checked in yet)."
        try:
            s = json.loads(raw)
        except ValueError:
            return f"Frame said: {raw[:200]}"
        return (f"battery {s.get('battery_mv', '?')} mV ({s.get('battery_pct', '?')}%), "
                f"rssi {s.get('rssi', '?')}, {s.get('result', '?')}, "
                f"wake #{s.get('wake_count', '?')}, next in {s.get('next_wake_s', '?')}s"
                + (f", error {s['error']}" if s.get("error") else ""))
    if cmd == "/refresh":
        # A changing token is what makes a retained command fire once instead of on
        # every wake forever. See net/mqtt.cpp handle_clear().
        ok = mqtt_publish(f"{TOPIC_ROOT}/cmd/clear", str(int(time.time())))
        return ("Frame will re-fetch on its next wake." if ok
                else "Could not reach the broker.")
    return f"Unknown command. {HELP}"


# --- routes ------------------------------------------------------------------

@app.route("/mms", methods=["POST"])
@validate_twilio_request
@limiter.limit(RATE_LIMIT)
def mms():
    sender = request.form.get("From", "unknown")
    body = request.form.get("Body", "") or ""
    num_media = int(request.form.get("NumMedia", 0) or 0)
    logger.info("message from=%s media=%d body=%r", sender, num_media, body[:80])

    reply = MessagingResponse()

    if ALLOWED_NUMBERS and sender not in ALLOWED_NUMBERS:
        logger.warning("rejected sender not in allowlist: %s", sender)
        return str(reply)  # silence, not an error page: do not confirm the number exists

    if num_media == 0:
        text = handle_command(body, sender) if body.strip().startswith("/") else None
        reply.message(text or "Send me a photo and it'll go up on the frame.")
        return str(reply)

    media_url = request.form.get("MediaUrl0")
    content_type = (request.form.get("MediaContentType0") or "").lower()
    if content_type not in ACCEPTED_TYPES:
        logger.warning("unsupported media type %r from %s", content_type, sender)
        reply.message(f"I can't read {content_type or 'that'} - try a normal photo.")
        return str(reply)

    try:
        raw = fetch_media(media_url)
    except Exception:
        logger.exception("could not fetch media from Twilio")
        reply.message("I couldn't download that - mind trying again?")
        return str(reply)

    try:
        img = open_image(raw)
        short_edge = min(img.size)
        blob, preview = encode(img, width=PANEL_WIDTH, height=PANEL_HEIGHT,
                               crop=IMAGE_CROP, saturation=IMAGE_SATURATION,
                               contrast=IMAGE_CONTRAST, palette=IMAGE_PALETTE,
                               want_preview=True)
    except Exception:
        logger.exception("could not render image from %s", sender)
        reply.message("I couldn't make sense of that image, sorry.")
        return str(reply)

    buf = io.BytesIO()
    preview.save(buf, "PNG")

    meta = {
        "etag": etag_for(blob),
        "bytes": len(blob),
        "received_at": int(time.time()),
        "from": sender,
        "caption": body[:200],
        "source_size": list(img.size),
        "content_type": content_type,
    }
    store_image(blob, buf.getvalue(), meta)
    logger.info("stored %d bytes etag=%s from=%s source=%dx%d", len(blob), meta["etag"],
                sender, img.size[0], img.size[1])

    if DELETE_TWILIO_MEDIA:
        delete_media(media_url)

    if short_edge < 400:
        reply.message("Got it! Fair warning: your carrier squashed that one quite hard, "
                      "so it may look soft on the frame.")
    else:
        reply.message("Got it - it'll be on the frame shortly.")
    return str(reply)


@app.route("/latest.pfrm", methods=["GET"])
def latest_pfrm():
    """The frame's entire interface. LAN-only; see the module docstring."""
    data, etag = read_blob()
    if data is None:
        return Response("no image yet", status=404, mimetype="text/plain")

    # A 304 is the common answer by a wide margin, and it is what makes the battery
    # budget work: a wake that ends here is a few seconds long.
    if request.headers.get("If-None-Match") == etag:
        return Response(status=304, headers={"ETag": etag, "Cache-Control": "no-cache"})

    return Response(data, mimetype="application/octet-stream",
                    headers={"ETag": etag,
                             "Content-Length": str(len(data)),
                             "Cache-Control": "no-cache"})


@app.route("/latest.png", methods=["GET"])
def latest_png():
    """What the panel will actually show, for humans. Same LAN-only Service."""
    try:
        return Response(PREVIEW_PATH.read_bytes(), mimetype="image/png",
                        headers={"Cache-Control": "no-cache"})
    except OSError:
        return Response("no image yet", status=404, mimetype="text/plain")


@app.route("/status", methods=["GET"])
def status():
    meta = read_meta()
    data, etag = read_blob()
    return jsonify({
        "has_image": data is not None,
        "etag": etag,
        "image": meta,
        "panel": {"width": PANEL_WIDTH, "height": PANEL_HEIGHT},
        "frame": _frame_state(),
    })


def _frame_state():
    raw = mqtt_get_retained(f"{TOPIC_ROOT}/state", timeout=1.5)
    if not raw:
        return None
    try:
        return json.loads(raw)
    except ValueError:
        return None


@app.route("/healthz", methods=["GET"])
def healthz():
    return Response("ok", mimetype="text/plain")


if __name__ == "__main__":
    app.run(debug=True, port=5000)
