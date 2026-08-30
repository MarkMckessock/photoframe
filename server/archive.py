"""Keep the original photos, and a record of who sent what.

The frame only needs the most recent image, and the render pipeline destroys the
original on the way to the panel: resized, saturation-boosted, dithered to six colours
and packed to 4 bpp. None of that is reversible. This module keeps the untouched
original so the photos outlive the frame, and records one row per received photo so
they can be looked up later.

Two stores, deliberately separate:

  photos   an NFS mount of the Synology, written inside the directory the photo
           library already scans, so they appear alongside everything else.
  index    SQLite on a small PVC -- explicitly NOT on the NFS mount. SQLite's locking
           is built on POSIX advisory locks, which NFS implements unreliably; putting
           a database file on an NFS share is a well known way to corrupt one. That
           split is the reason this feature needs its own small volume rather than
           just writing next to the photos.

Archiving is best-effort and never fails a request. The NAS being down, full, or
squashing our UID must not stop a photo reaching the frame -- that is the actual
product, and this is the bonus. Every entry point swallows its exceptions and logs.
"""

import hashlib
import logging
import os
import sqlite3
import time
from datetime import datetime, timezone
from pathlib import Path

logger = logging.getLogger("photoframe.archive")

# Twilio delivers a narrow set of types; anything unknown keeps .bin so a mystery file
# is still obviously a file rather than something the library will try to open.
_EXTENSIONS = {
    "image/jpeg": ".jpg",
    "image/jpg": ".jpg",
    "image/png": ".png",
    "image/gif": ".gif",
    "image/webp": ".webp",
    "image/bmp": ".bmp",
    "image/heic": ".heic",
    "image/heif": ".heic",
}

SCHEMA = """
CREATE TABLE IF NOT EXISTS photos (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    received_at  INTEGER NOT NULL,
    sender       TEXT    NOT NULL,
    path         TEXT    NOT NULL,
    bytes        INTEGER NOT NULL,
    sha256       TEXT    NOT NULL,
    content_type TEXT,
    caption      TEXT,
    etag         TEXT,
    width        INTEGER,
    height       INTEGER
);
CREATE INDEX IF NOT EXISTS photos_received_at ON photos (received_at DESC);
CREATE INDEX IF NOT EXISTS photos_sender      ON photos (sender);
"""


class Archive:
    """Original-photo storage plus its SQLite index.

    Disabled unless ARCHIVE_DIR is set, in the same style as the notifier's single
    toggle: one variable turns the whole feature on, and everything no-ops when it is
    absent so tests and local runs touch no NAS.
    """

    def __init__(self, archive_dir, db_path):
        self.dir = Path(archive_dir) if archive_dir else None
        self.db_path = Path(db_path) if db_path else None
        self.enabled = bool(self.dir and self.db_path)
        self._ready = False
        if self.enabled:
            self._ready = self._prepare()

    # --- setup ---------------------------------------------------------------

    def _prepare(self):
        """Create the directories and the schema once, at startup.

        Failing here disables archiving for the life of the process rather than
        retrying on every photo: if the mount is missing, it is missing because the
        pod was started without it, and that needs a human.
        """
        try:
            self.dir.mkdir(parents=True, exist_ok=True)
            self.db_path.parent.mkdir(parents=True, exist_ok=True)
            with self._connect() as db:
                db.executescript(SCHEMA)
            return True
        except Exception:
            logger.exception("archive unavailable (dir=%s db=%s); photos will still "
                             "reach the frame but will not be kept",
                             self.dir, self.db_path)
            return False

    def _connect(self):
        db = sqlite3.connect(self.db_path, timeout=10)
        # WAL survives an unclean pod kill far better than the rollback journal, and
        # lets /status read while a photo is being written.
        db.execute("PRAGMA journal_mode=WAL")
        db.execute("PRAGMA synchronous=NORMAL")
        db.row_factory = sqlite3.Row
        return db

    # --- writing -------------------------------------------------------------

    def save(self, raw, sender, content_type="", caption="", received_at=None):
        """Write the original bytes to the mount and index them. Returns a row id.

        Called with the bytes exactly as Twilio served them, before any processing, so
        an image that later fails to render is still preserved -- those are precisely
        the ones worth having a copy of.

        Returns None if archiving is off or anything at all goes wrong.
        """
        if not (self.enabled and self._ready):
            return None
        try:
            received_at = int(received_at or time.time())
            digest = hashlib.sha256(raw).hexdigest()
            name = self._filename(received_at, digest, content_type)
            self._write_file(name, raw)
            return self._insert(received_at, sender, name, raw, digest,
                                content_type, caption)
        except Exception:
            logger.exception("could not archive photo from %s (not fatal)", sender)
            return None

    @staticmethod
    def _filename(received_at, digest, content_type):
        """Sortable, unique, and free of anything you would not want on a screen.

        The sender is in the database, not the filename: these files land in a photo
        library that gets browsed and shared, and a phone number in a filename is a
        privacy leak the index does not have.
        """
        stamp = datetime.fromtimestamp(received_at, timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        return f"{stamp}-{digest[:8]}{_EXTENSIONS.get(content_type, '.bin')}"

    def _write_file(self, name, raw):
        """Write via a hidden temp file and rename.

        The leading dot matters: the photo library scans this directory, and a
        half-written file with a real image extension is something it will happily try
        to import. The rename is atomic within a directory, so a scanner sees the whole
        file or nothing.
        """
        final = self.dir / name
        tmp = self.dir / f".{name}.part"
        with open(tmp, "wb") as f:
            f.write(raw)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, final)

    def _insert(self, received_at, sender, name, raw, digest, content_type, caption):
        with self._connect() as db:
            cur = db.execute(
                "INSERT INTO photos (received_at, sender, path, bytes, sha256, "
                "content_type, caption) VALUES (?, ?, ?, ?, ?, ?, ?)",
                (received_at, sender, name, len(raw), digest, content_type,
                 (caption or "")[:200]))
            logger.info("archived %s (%d bytes) from %s", name, len(raw), sender)
            return cur.lastrowid

    def record_render(self, row_id, etag, size=None):
        """Attach the rendered blob's ETag to the original it came from.

        This is what lets you ask "which of these originals is the one currently on the
        panel", given that the frame reports only an ETag.
        """
        if not (self.enabled and self._ready) or row_id is None:
            return
        try:
            width, height = size if size else (None, None)
            with self._connect() as db:
                db.execute("UPDATE photos SET etag = ?, width = ?, height = ? WHERE id = ?",
                           (etag, width, height, row_id))
        except Exception:
            logger.exception("could not record render info for archive row %s", row_id)

    # --- reading -------------------------------------------------------------

    def stats(self):
        """Small summary for /status, so the mount can be verified without a shell."""
        if not self.enabled:
            return {"enabled": False}
        if not self._ready:
            return {"enabled": True, "ready": False}
        try:
            with self._connect() as db:
                row = db.execute("SELECT COUNT(*) AS n, MAX(received_at) AS latest "
                                 "FROM photos").fetchone()
            return {"enabled": True, "ready": True, "dir": str(self.dir),
                    "count": row["n"], "latest_at": row["latest"]}
        except Exception:
            logger.exception("could not read archive stats")
            return {"enabled": True, "ready": False}

    def recent(self, limit=20):
        """Most recent photos, newest first. Includes sender numbers -- LAN only."""
        if not (self.enabled and self._ready):
            return []
        try:
            with self._connect() as db:
                rows = db.execute(
                    "SELECT id, received_at, sender, path, bytes, content_type, "
                    "caption, etag, width, height FROM photos "
                    "ORDER BY received_at DESC, id DESC LIMIT ?", (limit,)).fetchall()
            return [dict(r) for r in rows]
        except Exception:
            logger.exception("could not read archive")
            return []
