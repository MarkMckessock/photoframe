"""Test fixtures.

The service reads its configuration at import time and fails fast on missing secrets,
so the environment has to be in place before `server.app` is imported at all.
"""

import os
import sys
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

STORE = tempfile.mkdtemp(prefix="pf-test-")

os.environ.update({
    "TWILIO_ACCOUNT_SID": "ACtest",
    "TWILIO_AUTH_TOKEN": "test_token",
    "TWILIO_WEBHOOK_URL": "http://localhost/mms",
    "STORE_DIR": STORE,
    "ADMIN_NUMBERS": "+15550000001",
    "MQTT_HOST": "",          # admin commands degrade gracefully with no broker
    "RATE_LIMIT": "1000 per minute",
})

from server import app as srv  # noqa: E402


@pytest.fixture()
def client(tmp_path, monkeypatch):
    # A fresh store per test, so ordering never matters.
    monkeypatch.setattr(srv, "STORE_DIR", tmp_path)
    monkeypatch.setattr(srv, "BLOB_PATH", tmp_path / "latest.pfrm")
    monkeypatch.setattr(srv, "PREVIEW_PATH", tmp_path / "latest.png")
    monkeypatch.setattr(srv, "META_PATH", tmp_path / "latest.json")
    srv._etag_cache.clear()
    srv.app.config["TESTING"] = True
    with srv.app.test_client() as c:
        yield c


@pytest.fixture()
def srv_mod():
    return srv


@pytest.fixture()
def photo_bytes():
    """A synthetic photo, so the suite does not depend on a fixture file."""
    import io

    from PIL import Image, ImageDraw

    im = Image.new("RGB", (900, 1200), (245, 245, 240))
    d = ImageDraw.Draw(im)
    for i, c in enumerate([(220, 30, 40), (240, 200, 20), (30, 110, 60), (40, 60, 200)]):
        d.rectangle([40, 40 + i * 260, 860, 260 + i * 260], fill=c)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=90)
    return buf.getvalue()
