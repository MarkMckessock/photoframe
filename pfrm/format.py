"""The PFRM header: 64 bytes, little-endian, with a CRC over itself.

Mirror of `firmware/include/pf_image_format.h`. Field order and offsets must match
exactly -- the firmware casts the first 64 bytes straight to a packed struct.
"""

import binascii
import hashlib
import struct

MAGIC = 0x4D524650  # 'PFRM' read back as a little-endian uint32
VERSION = 1
HEADER_LEN = 64

FMT_SEEED_GFX_4BPP = 1
PALETTE_SPECTRA6 = 1

PANEL_W, PANEL_H = 1200, 1600

# Bytes 0x00..0x23. The header CRC covers exactly this much, and then lives at 0x24.
_BODY = "<IHHHHBBHIIQI"
_BODY_LEN = struct.calcsize(_BODY)
assert _BODY_LEN == 36, _BODY_LEN


def blob_size(width=PANEL_W, height=PANEL_H):
    return HEADER_LEN + width * height // 2


def pack_header(data, width=PANEL_W, height=PANEL_H, created_at=0, rotation=0, flags=0):
    """Build the 64-byte header for an already-packed pixel payload."""
    body = struct.pack(
        _BODY,
        MAGIC,
        VERSION,
        HEADER_LEN,
        width,
        height,
        FMT_SEEED_GFX_4BPP,
        rotation,
        flags,
        len(data),
        binascii.crc32(data) & 0xFFFFFFFF,
        int(created_at),
        PALETTE_SPECTRA6,
    )
    # The header carrying its own CRC is what lets the firmware reject a wrong-format
    # or wrong-dimension transfer after 64 bytes instead of after a megabyte -- which
    # matters because the alternative is spending thirty seconds of panel refresh on it.
    header = body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF) + b"\x00" * 24
    assert len(header) == HEADER_LEN, len(header)
    return header


def parse_header(blob):
    """Validate and unpack. Raises ValueError with the same reasons the firmware uses."""
    if len(blob) < HEADER_LEN:
        raise ValueError("size: shorter than a header")
    h = blob[:HEADER_LEN]
    (magic, version, header_len, width, height, fmt, rotation, flags, data_len,
     data_crc32, created_at, palette_id) = struct.unpack_from(_BODY, h)
    if magic != MAGIC:
        raise ValueError("header: bad magic")
    if version != VERSION or header_len != HEADER_LEN:
        raise ValueError("header: unsupported version")
    (header_crc32,) = struct.unpack_from("<I", h, _BODY_LEN)
    if header_crc32 != binascii.crc32(h[:_BODY_LEN]) & 0xFFFFFFFF:
        raise ValueError("header: bad header crc")
    if data_len + HEADER_LEN != len(blob):
        raise ValueError("size: data_len disagrees with the blob")
    if data_crc32 != binascii.crc32(blob[HEADER_LEN:]) & 0xFFFFFFFF:
        raise ValueError("crc: payload crc mismatch")
    return {
        "version": version, "width": width, "height": height, "format": fmt,
        "rotation": rotation, "flags": flags, "data_len": data_len,
        "data_crc32": data_crc32, "created_at": created_at, "palette_id": palette_id,
    }


def etag_for(blob):
    """The HTTP ETag for a blob: a hash of the *pixels*, quoted as RFC 9110 requires.

    Content-addressed rather than mtime-based, so a service restart or a re-run of the
    pipeline does not cost the frame a pointless thirty-second refresh.

    Deliberately excludes the header, because the header contains created_at. Hashing
    the whole blob would give the same photo a new identity every time it was encoded,
    which is exactly the refresh we are trying to avoid. The ETag answers "will the
    panel look different?", and only the payload can change that.
    """
    return '"%s"' % hashlib.sha256(blob[HEADER_LEN:]).hexdigest()[:32]
