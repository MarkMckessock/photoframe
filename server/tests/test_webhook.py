"""The Twilio side: signatures, allowlists, media handling, and the reply."""

import sys
from pathlib import Path

import pytest
from twilio.request_validator import RequestValidator

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from pfrm import parse_header  # noqa: E402

URL = "http://localhost/mms"
TOKEN = "test_token"


def signed_post(client, form):
    """POST /mms with a signature Twilio would have produced."""
    sig = RequestValidator(TOKEN).compute_signature(URL, form)
    return client.post("/mms", data=form, headers={"X-Twilio-Signature": sig})


def media_form(sender="+15551234567", content_type="image/jpeg", body=""):
    return {
        "From": sender,
        "Body": body,
        "NumMedia": "1",
        "MediaUrl0": "https://api.twilio.com/2010-04-01/Accounts/ACtest/Messages/MM1/Media/ME1",
        "MediaContentType0": content_type,
    }


@pytest.fixture()
def no_network(monkeypatch, srv_mod, photo_bytes):
    monkeypatch.setattr(srv_mod, "fetch_media", lambda url: photo_bytes)
    monkeypatch.setattr(srv_mod, "delete_media", lambda url: None)


def test_unsigned_request_is_rejected(client):
    r = client.post("/mms", data=media_form())
    assert r.status_code == 403


def test_wrong_signature_is_rejected(client):
    r = client.post("/mms", data=media_form(), headers={"X-Twilio-Signature": "nope"})
    assert r.status_code == 403


def test_photo_is_rendered_and_stored(client, srv_mod, no_network):
    r = signed_post(client, media_form())
    assert r.status_code == 200
    assert "on the frame" in r.get_data(as_text=True)

    blob = srv_mod.BLOB_PATH.read_bytes()
    parse_header(blob)                      # raises if the firmware would reject it
    assert srv_mod.PREVIEW_PATH.exists()
    meta = srv_mod.read_meta()
    assert meta["from"] == "+15551234567"
    assert meta["bytes"] == len(blob)


def test_caption_is_recorded(client, srv_mod, no_network):
    signed_post(client, media_form(body="beach day"))
    assert srv_mod.read_meta()["caption"] == "beach day"


def test_twilio_media_is_deleted_after_download(client, srv_mod, monkeypatch, photo_bytes):
    """Friends' photos should not linger on Twilio -- we already have the only copy."""
    deleted = []
    monkeypatch.setattr(srv_mod, "fetch_media", lambda url: photo_bytes)
    monkeypatch.setattr(srv_mod, "delete_media", lambda url: deleted.append(url))
    signed_post(client, media_form())
    assert len(deleted) == 1


def test_unsupported_media_type_is_declined_without_downloading(client, srv_mod, monkeypatch):
    called = []
    monkeypatch.setattr(srv_mod, "fetch_media", lambda url: called.append(url) or b"")
    r = signed_post(client, media_form(content_type="video/mp4"))
    assert r.status_code == 200
    assert "can't read" in r.get_data(as_text=True)
    assert called == []
    assert not srv_mod.BLOB_PATH.exists()


def test_download_failure_leaves_the_previous_image_alone(client, srv_mod, monkeypatch,
                                                          photo_bytes):
    monkeypatch.setattr(srv_mod, "fetch_media", lambda url: photo_bytes)
    monkeypatch.setattr(srv_mod, "delete_media", lambda url: None)
    signed_post(client, media_form())
    good = srv_mod.BLOB_PATH.read_bytes()

    def boom(url):
        raise RuntimeError("twilio is having a day")

    monkeypatch.setattr(srv_mod, "fetch_media", boom)
    r = signed_post(client, media_form())
    assert r.status_code == 200
    assert "couldn't download" in r.get_data(as_text=True)
    assert srv_mod.BLOB_PATH.read_bytes() == good


def test_undecodable_image_leaves_the_previous_image_alone(client, srv_mod, monkeypatch,
                                                           photo_bytes):
    monkeypatch.setattr(srv_mod, "fetch_media", lambda url: photo_bytes)
    monkeypatch.setattr(srv_mod, "delete_media", lambda url: None)
    signed_post(client, media_form())
    good = srv_mod.BLOB_PATH.read_bytes()

    monkeypatch.setattr(srv_mod, "fetch_media", lambda url: b"this is not a JPEG")
    r = signed_post(client, media_form())
    assert "couldn't make sense" in r.get_data(as_text=True)
    assert srv_mod.BLOB_PATH.read_bytes() == good


def test_oversized_media_is_refused(client, srv_mod, monkeypatch):
    class FakeResp:
        status_code = 200

        def raise_for_status(self):
            pass

        def iter_content(self, n):
            for _ in range(200):
                yield b"\x00" * 1024 * 1024

        def close(self):
            pass

    monkeypatch.setattr(srv_mod.requests, "get", lambda *a, **k: FakeResp())
    with pytest.raises(ValueError, match="exceeds"):
        srv_mod.fetch_media("https://example.invalid/media")


def test_allowlist_silently_drops_strangers(client, srv_mod, monkeypatch, no_network):
    monkeypatch.setattr(srv_mod, "ALLOWED_NUMBERS", {"+15550000001"})
    r = signed_post(client, media_form(sender="+15559999999"))
    assert r.status_code == 200
    # Silence rather than an error: do not confirm to a stranger that the number is live.
    assert "Message" not in r.get_data(as_text=True)
    assert not srv_mod.BLOB_PATH.exists()


def test_text_with_no_photo_gets_a_nudge(client):
    r = signed_post(client, {"From": "+15551234567", "Body": "hello", "NumMedia": "0"})
    assert "Send me a photo" in r.get_data(as_text=True)


def test_help_is_available_to_anyone(client):
    r = signed_post(client, {"From": "+15551234567", "Body": "/help", "NumMedia": "0"})
    assert "Text me a photo" in r.get_data(as_text=True)


def test_admin_commands_are_ignored_for_non_admins(client, srv_mod):
    r = signed_post(client, {"From": "+15559999999", "Body": "/refresh", "NumMedia": "0"})
    # Falls through to the generic nudge rather than acting or acknowledging.
    assert "Send me a photo" in r.get_data(as_text=True)


def test_admin_refresh_publishes_a_changing_token(client, srv_mod, monkeypatch):
    published = []
    monkeypatch.setattr(srv_mod, "mqtt_publish",
                        lambda t, p, retain=True: published.append((t, p)) or True)
    r = signed_post(client, {"From": "+15550000001", "Body": "/refresh", "NumMedia": "0"})
    assert "re-fetch" in r.get_data(as_text=True)
    assert published[0][0].endswith("/cmd/clear")
    assert published[0][1].isdigit()   # a token that changes, so it fires once
