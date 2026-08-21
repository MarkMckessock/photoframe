#!/usr/bin/env python3
"""Turn an ordinary photo into a .pfrm blob the frame can memcpy straight onto the panel.

This is the reference implementation of the format described in
firmware/include/pf_image_format.h. The same code is what the photoframe-webhook
service runs -- keep the two in sync, and bump PFRM_VERSION in both if the layout
changes.

Everything expensive happens here rather than on the MCU: EXIF rotation, resampling,
saturation boost, Floyd-Steinberg dithering to six colours, and nibble packing. The
firmware's entire job is to check a CRC and push bytes.

    python tools/encode_image.py photo.jpg -o latest.pfrm
    python tools/encode_image.py photo.jpg -o latest.pfrm --crop --saturation 1.6

Requires: pillow, numpy
"""

import argparse
import binascii
import struct
import sys
import time
from pathlib import Path

try:
    import numpy as np
    from PIL import Image, ImageEnhance, ImageOps
except ImportError:  # pragma: no cover
    sys.exit("need pillow and numpy:  pip install pillow numpy")

PFRM_MAGIC = 0x4D524650
PFRM_VERSION = 1
PFRM_FMT_SEEED_GFX_4BPP = 1
PFRM_PALETTE_SPECTRA6 = 1
PFRM_HEADER_LEN = 64

PANEL_W, PANEL_H = 1200, 1600

# The six colours, paired with the nibble Seeed_GFX's framebuffer uses for each.
#
# The RGB values are what we dither *towards*. Two sets, because it is a real
# tradeoff: "measured" approximates what the panel actually produces and gives
# better skin tones and shadows; "saturated" uses pure primaries, which dithers to
# something punchier but less faithful. Photos of people generally want "measured".
#
# TODO(bring-up): reconcile these against Seeed's own dither.cpp (PAL_E6) in
# Seeed_Arduino_LCD/examples/ePaper/reTerminal_SDcard_Bitmap/reTerminal_E1002_SDcard_Color6/.
# Matching it exactly means Seeed's browser img2bitmap preview and the wall agree.
PALETTES = {
    "measured": [
        # (R,   G,   B), framebuffer nibble
        ((0, 0, 0), 0xF),        # black
        ((255, 255, 255), 0x0),  # white
        ((255, 243, 56), 0xB),   # yellow
        ((191, 0, 0), 0x6),      # red
        ((100, 64, 255), 0xD),   # blue
        ((67, 138, 28), 0x2),    # green
    ],
    "saturated": [
        ((0, 0, 0), 0xF),
        ((255, 255, 255), 0x0),
        ((255, 255, 0), 0xB),
        ((255, 0, 0), 0x6),
        ((0, 0, 255), 0xD),
        ((0, 255, 0), 0x2),
    ],
}


def build_palette_image(entries):
    """A 'P'-mode image carrying the palette, for Image.quantize(palette=...).

    Pillow pads unused palette slots with black, and the dither will happily aim at
    those phantom black entries. Repeating our six colours across all 256 slots means
    every possible index maps to a real colour, and `index % 6` recovers which one.
    """
    flat = []
    for _ in range(256 // len(entries) + 1):
        for rgb, _nibble in entries:
            flat.extend(rgb)
    pal = Image.new("P", (1, 1))
    pal.putpalette(flat[: 256 * 3])
    return pal


def prepare(img, width, height, crop, saturation, contrast):
    img = ImageOps.exif_transpose(img).convert("RGB")
    # LANCZOS both up and down: MMS gets re-encoded hard by carriers and you often
    # receive something much smaller than the panel.
    if crop:
        img = ImageOps.fit(img, (width, height), method=Image.LANCZOS, centering=(0.5, 0.5))
    else:
        # Letterbox onto white. For group photos this beats cropping someone out of
        # the frame, and white bars disappear against a white panel border.
        img = ImageOps.pad(img, (width, height), method=Image.LANCZOS, color=(255, 255, 255))
    if contrast != 1.0:
        img = ImageEnhance.Contrast(img).enhance(contrast)
    if saturation != 1.0:
        # E-ink primaries are muted; a pre-boost is what keeps a dithered photo from
        # looking like a washed-out newspaper print.
        img = ImageEnhance.Color(img).enhance(saturation)
    return img


def pack(img, entries):
    """Dither to the palette and pack to 4bpp, high nibble = even-x pixel."""
    quantized = img.quantize(palette=build_palette_image(entries),
                             dither=Image.Dither.FLOYDSTEINBERG)
    idx = np.asarray(quantized, dtype=np.uint8) % len(entries)

    lut = np.array([nibble for _rgb, nibble in entries], dtype=np.uint8)
    nib = lut[idx]                                   # (H, W) of 0x0..0xF

    packed = (nib[:, 0::2] << 4) | nib[:, 1::2]      # (H, W/2)
    return packed.astype(np.uint8).tobytes()


def build_header(data, width, height, created_at):
    body = struct.pack(
        "<IHHHHBBHIIQI",
        PFRM_MAGIC,
        PFRM_VERSION,
        PFRM_HEADER_LEN,
        width,
        height,
        PFRM_FMT_SEEED_GFX_4BPP,
        0,                                  # rotation
        0,                                  # flags
        len(data),
        binascii.crc32(data) & 0xFFFFFFFF,
        created_at,
        PFRM_PALETTE_SPECTRA6,
    )
    assert len(body) == 36, len(body)
    header = body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF) + b"\x00" * 24
    assert len(header) == PFRM_HEADER_LEN, len(header)
    return header


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--width", type=int, default=PANEL_W)
    ap.add_argument("--height", type=int, default=PANEL_H)
    ap.add_argument("--crop", action="store_true",
                    help="centre-crop to fill instead of letterboxing onto white")
    ap.add_argument("--saturation", type=float, default=1.4)
    ap.add_argument("--contrast", type=float, default=1.0)
    ap.add_argument("--palette", choices=sorted(PALETTES), default="measured")
    ap.add_argument("--preview", type=Path,
                    help="also write a PNG of exactly what the panel will show")
    args = ap.parse_args()

    if args.width % 2:
        sys.exit("width must be even (two pixels share a byte)")

    entries = PALETTES[args.palette]
    with Image.open(args.source) as src:
        if min(src.size) < 400:
            print(f"warning: source is only {src.size[0]}x{src.size[1]}; carriers "
                  f"re-encode MMS aggressively and this will look soft", file=sys.stderr)
        img = prepare(src, args.width, args.height, args.crop, args.saturation, args.contrast)

    data = pack(img, entries)
    expected = args.width * args.height // 2
    assert len(data) == expected, f"packed {len(data)}, expected {expected}"

    blob = build_header(data, args.width, args.height, int(time.time())) + data
    args.out.write_bytes(blob)

    if args.preview:
        quant = img.quantize(palette=build_palette_image(entries),
                             dither=Image.Dither.FLOYDSTEINBERG)
        idx = np.asarray(quant, dtype=np.uint8) % len(entries)
        rgb = np.array([c for c, _n in entries], dtype=np.uint8)[idx]
        Image.fromarray(rgb, "RGB").save(args.preview)

    import hashlib
    print(f"{args.out}: {len(blob)} bytes "
          f"({args.width}x{args.height}, crc32=0x{binascii.crc32(data) & 0xFFFFFFFF:08x})")
    print(f"etag: {hashlib.sha256(blob).hexdigest()[:32]}")


if __name__ == "__main__":
    main()
