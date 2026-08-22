"""The frame's side of the contract: one conditional GET, and what it must return."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from pfrm import PANEL_H, PANEL_W, etag_for, parse_header  # noqa: E402
from pfrm.render import encode, open_image  # noqa: E402


def _store(srv_mod, photo_bytes):
    blob, preview = encode(open_image(photo_bytes), want_preview=True)
    import io
    buf = io.BytesIO()
    preview.save(buf, "PNG")
    srv_mod.store_image(blob, buf.getvalue(), {"etag": etag_for(blob)})
    return blob


def test_404_before_any_photo(client):
    assert client.get("/latest.pfrm").status_code == 404
    assert client.get("/latest.png").status_code == 404


def test_serves_blob_with_etag_and_length(client, srv_mod, photo_bytes):
    blob = _store(srv_mod, photo_bytes)

    r = client.get("/latest.pfrm")
    assert r.status_code == 200
    assert r.data == blob
    assert r.headers["ETag"] == etag_for(blob)
    # The firmware refuses a response without a usable Content-Length: it cannot size
    # the PSRAM buffer up front, and a chunked reply would let a truncation look
    # like a complete image.
    assert int(r.headers["Content-Length"]) == len(blob)
    assert len(blob) == 64 + PANEL_W * PANEL_H // 2


def test_conditional_get_returns_304(client, srv_mod, photo_bytes):
    blob = _store(srv_mod, photo_bytes)
    etag = etag_for(blob)

    r = client.get("/latest.pfrm", headers={"If-None-Match": etag})
    assert r.status_code == 304
    assert r.headers["ETag"] == etag
    assert r.data == b""


def test_stale_etag_gets_the_new_image(client, srv_mod, photo_bytes):
    _store(srv_mod, photo_bytes)
    r = client.get("/latest.pfrm", headers={"If-None-Match": '"not-the-current-one"'})
    assert r.status_code == 200


def test_blob_passes_the_firmware_validation(client, srv_mod, photo_bytes):
    _store(srv_mod, photo_bytes)
    h = parse_header(client.get("/latest.pfrm").data)
    assert (h["width"], h["height"]) == (PANEL_W, PANEL_H)
    assert h["format"] == 1 and h["palette_id"] == 1


def test_etag_is_stable_when_the_same_photo_is_re_encoded(client, srv_mod, photo_bytes):
    """Re-rendering an identical photo must not cost the frame a 30 s refresh."""
    first = _store(srv_mod, photo_bytes)
    r1 = client.get("/latest.pfrm").headers["ETag"]
    second = _store(srv_mod, photo_bytes)
    r2 = client.get("/latest.pfrm").headers["ETag"]
    assert r1 == r2
    assert first[64:] == second[64:]  # identical pixels; only created_at may differ


def test_etag_changes_for_a_different_photo(client, srv_mod, photo_bytes):
    _store(srv_mod, photo_bytes)
    before = client.get("/latest.pfrm").headers["ETag"]

    import io

    from PIL import Image
    other = Image.new("RGB", (900, 1200), (10, 90, 200))
    buf = io.BytesIO()
    other.save(buf, "JPEG")
    _store(srv_mod, buf.getvalue())
    assert client.get("/latest.pfrm").headers["ETag"] != before


def test_healthz(client):
    assert client.get("/healthz").status_code == 200


# --- firmware / OTA endpoints -------------------------------------------------

def test_firmware_404_when_none_published(client):
    assert client.get("/firmware.bin").status_code == 404


def test_firmware_served_with_content_length(client, srv_mod):
    """The firmware sizes its flash write from Content-Length and refuses without one.

    A truncated image that still gets committed is the single failure mode OTA must
    never have, so this header is load-bearing rather than cosmetic.
    """
    blob = bytes(range(256)) * 40
    srv_mod.STORE_DIR.mkdir(parents=True, exist_ok=True)
    srv_mod.FIRMWARE_PATH.write_bytes(blob)

    r = client.get("/firmware.bin")
    assert r.status_code == 200
    assert r.data == blob
    assert int(r.headers["Content-Length"]) == len(blob)


def test_firmware_metadata_round_trips(client, srv_mod):
    import hashlib
    import json
    blob = b"\xde\xad\xbe\xef" * 100
    srv_mod.STORE_DIR.mkdir(parents=True, exist_ok=True)
    srv_mod.FIRMWARE_PATH.write_bytes(blob)
    srv_mod.FIRMWARE_META_PATH.write_text(json.dumps(
        {"version": "1.2.3", "sha256": hashlib.sha256(blob).hexdigest()}))

    meta = client.get("/firmware.json").get_json()
    assert meta["version"] == "1.2.3"
    assert meta["sha256"] == hashlib.sha256(blob).hexdigest()

    # and it shows up in /status alongside the image
    assert client.get("/status").get_json()["firmware"]["version"] == "1.2.3"


def test_no_upload_endpoint_exists(client, srv_mod):
    """There must be no write path for firmware: this service has a public route."""
    for method in ("POST", "PUT"):
        for path in ("/firmware.bin", "/firmware", "/firmware.json"):
            code = client.open(path, method=method).status_code
            assert code in (404, 405), f"{method} {path} returned {code}"
