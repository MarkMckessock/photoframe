#!/usr/bin/env python3
"""Conformance test for the PFRM wire format.

The firmware's validation lives in firmware/src/image/image_buffer.cpp and cannot be
run on a laptop, so this reimplements it from the *header file's* stated offsets --
independently of encode_image.py's struct format string -- and checks that good blobs
pass and every interesting corruption is caught.

That independence is the point: if someone reorders a field in pf_image_format.h and
updates only one side, this fails.

    .venv/bin/python tools/test_format.py
"""

import binascii
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "firmware/include/pf_image_format.h"
PY_ENC = ROOT / "tools/encode_image.py"

PANEL_W, PANEL_H = 1200, 1600
HDR_LEN = 64
PIXELS = PANEL_W * PANEL_H // 2
TOTAL = HDR_LEN + PIXELS

# Field offsets, as documented in pf_image_format.h.
OFF = {
    "magic": 0, "version": 4, "header_len": 6, "width": 8, "height": 10,
    "format": 12, "rotation": 13, "flags": 14, "data_len": 16, "data_crc32": 20,
    "created_at": 24, "palette_id": 32, "header_crc32": 36,
}
LEGAL_NIBBLES = {0x0, 0x2, 0x6, 0xB, 0xD, 0xF}

failures = []


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))
    if not cond:
        failures.append(name)


def parse_c_header():
    """Pull the offsets back out of the C header's comments, so the two cannot drift."""
    text = HEADER.read_text()
    found = {}
    for line in text.splitlines():
        m = re.match(r"\s*\w[\w\s]*?\s(\w+)(?:\[\d+\])?;\s*//\s*(0x[0-9A-Fa-f]+)", line)
        if m:
            found[m.group(1)] = int(m.group(2), 16)
    return found


def validate(blob, declared_len=None):
    """Mirror of ImageBuffer::validate_header + validate_payload. Returns None or a reason."""
    declared = len(blob) if declared_len is None else declared_len
    if len(blob) < HDR_LEN:
        return "size"
    h = blob[:HDR_LEN]
    magic, = struct.unpack_from("<I", h, OFF["magic"])
    if magic != 0x4D524650:
        return "header"
    version, = struct.unpack_from("<H", h, OFF["version"])
    hlen, = struct.unpack_from("<H", h, OFF["header_len"])
    if version != 1 or hlen != HDR_LEN:
        return "header"
    hcrc, = struct.unpack_from("<I", h, OFF["header_crc32"])
    if hcrc != binascii.crc32(h[:36]) & 0xFFFFFFFF:
        return "header"
    w, = struct.unpack_from("<H", h, OFF["width"])
    ht, = struct.unpack_from("<H", h, OFF["height"])
    if (w, ht) != (PANEL_W, PANEL_H):
        return "header"
    fmt = h[OFF["format"]]
    pal, = struct.unpack_from("<I", h, OFF["palette_id"])
    if fmt != 1 or pal != 1:
        return "header"
    dlen, = struct.unpack_from("<I", h, OFF["data_len"])
    if dlen != PIXELS or dlen + HDR_LEN != declared:
        return "size"
    if len(blob) != declared:
        return "size"
    dcrc, = struct.unpack_from("<I", h, OFF["data_crc32"])
    if dcrc != binascii.crc32(blob[HDR_LEN:]) & 0xFFFFFFFF:
        return "crc"
    return None


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if src is None:
        # Self-contained: synthesise a source image rather than depending on a file.
        from PIL import Image, ImageDraw
        im = Image.new("RGB", (800, 1000), (240, 240, 240))
        d = ImageDraw.Draw(im)
        for i, c in enumerate([(220, 30, 40), (240, 200, 20), (30, 110, 60), (40, 60, 200)]):
            d.rectangle([20, 20 + i * 200, 780, 200 + i * 200], fill=c)
        src = Path("/tmp/pf_conformance_src.png")
        im.save(src)

    out = Path("/tmp/pf_conformance.pfrm")
    subprocess.run([sys.executable, str(PY_ENC), str(src), "-o", str(out)],
                   check=True, stdout=subprocess.DEVNULL)
    blob = out.read_bytes()

    print("header layout")
    c_offsets = parse_c_header()
    for name, off in OFF.items():
        if name in c_offsets:
            check(f"pf_image_format.h documents {name} at 0x{off:02x}",
                  c_offsets[name] == off, f"header says 0x{c_offsets[name]:02x}")
    check("struct is 64 bytes", max(OFF.values()) + 4 <= HDR_LEN)

    print("\na well-formed blob")
    check("total size is 960064", len(blob) == TOTAL, f"got {len(blob)}")
    check("validates", validate(blob) is None, validate(blob) or "")
    nibbles = set()
    for b in blob[HDR_LEN:HDR_LEN + 200000]:
        nibbles.add(b >> 4)
        nibbles.add(b & 0xF)
    check("only legal palette nibbles", nibbles <= LEGAL_NIBBLES,
          " ".join(hex(n) for n in sorted(nibbles - LEGAL_NIBBLES)) or "clean")

    print("\ncorruptions the firmware must reject")
    cases = [
        ("one flipped payload byte", lambda b: b[:HDR_LEN + 500] +
            bytes([b[HDR_LEN + 500] ^ 0xFF]) + b[HDR_LEN + 501:], "crc"),
        ("flipped last payload byte", lambda b: b[:-1] + bytes([b[-1] ^ 0x01]), "crc"),
        ("truncated to half", lambda b: b[: len(b) // 2], "size"),
        ("truncated by one byte", lambda b: b[:-1], "size"),
        ("bad magic", lambda b: b"XXXX" + b[4:], "header"),
        ("bumped version", lambda b: b[:4] + struct.pack("<H", 2) + b[6:], "header"),
        ("wrong width", lambda b: b[:8] + struct.pack("<H", 800) + b[10:], "header"),
        ("unknown palette", lambda b: b[:32] + struct.pack("<I", 99) + b[36:], "header"),
        ("corrupt header crc", lambda b: b[:36] + struct.pack("<I", 0xDEADBEEF) + b[40:], "header"),
    ]
    for name, mutate, want in cases:
        got = validate(mutate(blob))
        check(f"{name} -> {want}", got == want, f"got {got}")

    print("\nCRC convention")
    # The firmware computes this with esp_rom_crc32_le, which needs inverting at both
    # ends to agree with zlib. Model the ROM routine and prove the wrapper is right.
    def rom(crc, data):
        for byte in data:
            crc ^= byte
            for _ in range(8):
                crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
        return crc & 0xFFFFFFFF
    sample = blob[HDR_LEN:HDR_LEN + 4096]
    wrapped = (~rom(~0 & 0xFFFFFFFF, sample)) & 0xFFFFFFFF
    check("pf_crc32 == zlib.crc32", wrapped == binascii.crc32(sample) & 0xFFFFFFFF)
    check("raw esp_rom_crc32_le would NOT match (this is why pf_crc32 exists)",
          rom(0, sample) != binascii.crc32(sample) & 0xFFFFFFFF)

    print()
    if failures:
        print(f"{len(failures)} FAILED: " + ", ".join(failures))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
