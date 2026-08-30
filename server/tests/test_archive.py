"""Keeping the originals, and the index that makes them findable.

The load-bearing property here is that archiving is *best-effort*: the frame is the
product and the NAS is the bonus, so every failure mode in this file must degrade to
"photo still reaches the panel".
"""

import sqlite3
import sys
import time
from pathlib import Path

import pytest
from twilio.request_validator import RequestValidator

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.archive import Archive  # noqa: E402

URL = "http://localhost/mms"
TOKEN = "test_token"


@pytest.fixture()
def archive(tmp_path):
    return Archive(tmp_path / "photos", tmp_path / "db" / "photos.db")


def rows(arc):
    with sqlite3.connect(arc.db_path) as db:
        db.row_factory = sqlite3.Row
        return [dict(r) for r in db.execute("SELECT * FROM photos ORDER BY id")]


# --- the three things the index must record ---------------------------------

def test_records_time_sender_and_path(archive):
    at = int(time.time())
    row_id = archive.save(b"\xff\xd8original", "+15551234567", "image/jpeg",
                          "hello", at)
    assert row_id is not None

    (row,) = rows(archive)
    assert row["received_at"] == at
    assert row["sender"] == "+15551234567"
    assert (archive.dir / row["path"]).read_bytes() == b"\xff\xd8original"


def test_stores_the_untouched_original(archive):
    """Not the rendered blob -- the exact bytes the sender's phone produced."""
    raw = bytes(range(256)) * 40
    archive.save(raw, "+15551234567", "image/png")
    (row,) = rows(archive)
    assert (archive.dir / row["path"]).read_bytes() == raw
    assert row["bytes"] == len(raw)


def test_path_is_relative_to_the_mount(archive):
    """So the index survives the mount being moved to a different path."""
    archive.save(b"x", "+15551234567", "image/jpeg")
    (row,) = rows(archive)
    assert not Path(row["path"]).is_absolute()
    assert str(archive.dir) not in row["path"]


# --- privacy -----------------------------------------------------------------

def test_filename_does_not_leak_the_sender(archive):
    """These land in a photo library that gets browsed and shared."""
    archive.save(b"x", "+15551234567", "image/jpeg")
    (row,) = rows(archive)
    assert "5551234567" not in row["path"]
    assert "+1" not in row["path"]


# --- file handling -----------------------------------------------------------

def test_extension_follows_the_content_type(archive):
    for ctype, ext in [("image/jpeg", ".jpg"), ("image/png", ".png"),
                       ("image/heic", ".heic"), ("application/nonsense", ".bin")]:
        archive.save(b"x" + ctype.encode(), "+1555", ctype)
    assert sorted(Path(r["path"]).suffix for r in rows(archive)) == \
        [".bin", ".heic", ".jpg", ".png"]


def test_names_are_sortable_and_unique(archive):
    """Two photos in the same second must not collide, and names must sort by time."""
    at = 1700000000
    archive.save(b"one", "+1555", "image/jpeg", "", at)
    archive.save(b"two", "+1555", "image/jpeg", "", at)   # same second, different bytes
    archive.save(b"later", "+1555", "image/jpeg", "", at + 60)
    paths = [r["path"] for r in rows(archive)]

    assert len(set(paths)) == 3                                  # no collision
    assert paths[2] == max(paths)                                # newest sorts last
    assert all(p.startswith("20231114T") for p in paths[:2])     # same-second prefix


def test_leaves_no_partial_files_behind(archive):
    """A half-written file with a real extension is something a library scanner will
    try to import, which is why the temp name is hidden and renamed into place."""
    archive.save(b"x", "+1555", "image/jpeg")
    assert [p.name for p in archive.dir.iterdir() if p.name.startswith(".")] == []


# --- best effort -------------------------------------------------------------

def test_disabled_when_no_directory_is_configured(tmp_path):
    arc = Archive("", tmp_path / "photos.db")
    assert arc.enabled is False
    assert arc.save(b"x", "+1555", "image/jpeg") is None
    assert arc.stats() == {"enabled": False}
    assert arc.recent() == []


def test_an_unwritable_mount_does_not_raise(tmp_path, monkeypatch):
    """The NAS being down is the expected case, not an exceptional one."""
    arc = Archive(tmp_path / "photos", tmp_path / "db" / "photos.db")

    def boom(*a, **k):
        raise OSError("NFS server not responding")

    monkeypatch.setattr(arc, "_write_file", boom)
    assert arc.save(b"x", "+1555", "image/jpeg") is None   # no exception escapes


def test_a_broken_directory_disables_rather_than_crashing(tmp_path):
    clash = tmp_path / "not-a-dir"
    clash.write_text("i am a file")
    arc = Archive(clash, tmp_path / "db" / "photos.db")
    assert arc._ready is False
    assert arc.save(b"x", "+1555", "image/jpeg") is None


def test_record_render_tolerates_a_missing_row(archive):
    archive.record_render(None, '"abc"', (1200, 1600))     # save() failed earlier


# --- linking back to what was displayed --------------------------------------

def test_record_render_links_the_original_to_the_blob(archive):
    row_id = archive.save(b"x", "+15551234567", "image/jpeg")
    archive.record_render(row_id, '"deadbeef"', (900, 1200))
    (row,) = rows(archive)
    assert row["etag"] == '"deadbeef"'
    assert (row["width"], row["height"]) == (900, 1200)


def test_stats_and_recent(archive):
    for i in range(3):
        archive.save(f"photo{i}".encode(), "+1555", "image/jpeg", "", 1700000000 + i)
    st = archive.stats()
    assert st["count"] == 3 and st["latest_at"] == 1700000002
    assert [r["received_at"] for r in archive.recent()] == \
        [1700000002, 1700000001, 1700000000]
    assert len(archive.recent(limit=2)) == 2


# --- through the actual webhook ----------------------------------------------

def signed_post(client, form):
    sig = RequestValidator(TOKEN).compute_signature(URL, form)
    return client.post("/mms", data=form, headers={"X-Twilio-Signature": sig})


@pytest.fixture()
def wired(client, srv_mod, tmp_path, monkeypatch, photo_bytes):
    monkeypatch.setattr(srv_mod, "fetch_media", lambda url: photo_bytes)
    monkeypatch.setattr(srv_mod, "delete_media", lambda url: None)
    arc = Archive(tmp_path / "photos", tmp_path / "db" / "photos.db")
    monkeypatch.setattr(srv_mod, "archive", arc)
    return client, srv_mod, arc, photo_bytes


def test_webhook_archives_the_original_it_received(wired):
    client, srv_mod, arc, photo = wired
    form = {"From": "+15551234567", "Body": "for the frame", "NumMedia": "1",
            "MediaUrl0": "https://api.twilio.com/x", "MediaContentType0": "image/jpeg"}
    assert signed_post(client, form).status_code == 200

    (row,) = rows(arc)
    assert row["sender"] == "+15551234567"
    assert row["caption"] == "for the frame"
    # The original, not the 960064-byte panel blob.
    assert (arc.dir / row["path"]).read_bytes() == photo
    # ...and it is linked to what actually went on the panel.
    assert row["etag"] == srv_mod.read_meta()["etag"]
    assert row["received_at"] == srv_mod.read_meta()["received_at"]


def test_a_failing_archive_still_puts_the_photo_on_the_frame(wired, monkeypatch):
    """The whole point of the design: the NAS is not in the critical path."""
    client, srv_mod, arc, _ = wired
    monkeypatch.setattr(arc, "_write_file",
                        lambda *a, **k: (_ for _ in ()).throw(OSError("stale handle")))
    form = {"From": "+15551234567", "Body": "", "NumMedia": "1",
            "MediaUrl0": "https://api.twilio.com/x", "MediaContentType0": "image/jpeg"}
    r = signed_post(client, form)
    assert r.status_code == 200
    assert "on the frame" in r.get_data(as_text=True)
    assert srv_mod.BLOB_PATH.exists()
    assert rows(arc) == []
