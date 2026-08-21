#!/usr/bin/env python3
"""Turn a photo into a .pfrm blob, from the command line.

A thin wrapper over the `pfrm` package -- the same code the cluster service runs, so
what you get here is byte-for-byte what a texted photo would produce.

    python tools/encode_image.py photo.jpg -o latest.pfrm
    python tools/encode_image.py photo.jpg -o latest.pfrm --crop --saturation 1.6 \
        --preview /tmp/what-the-panel-will-show.png
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from pfrm import PANEL_H, PANEL_W, etag_for, parse_header  # noqa: E402
from pfrm.palette import DEFAULT_PALETTE, PALETTES  # noqa: E402
from pfrm.render import encode, open_image  # noqa: E402


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
    ap.add_argument("--palette", choices=sorted(PALETTES), default=DEFAULT_PALETTE)
    ap.add_argument("--preview", type=Path,
                    help="also write a PNG of exactly what the panel will show")
    args = ap.parse_args()

    raw = args.source.read_bytes()
    img = open_image(raw)
    if min(img.size) < 400:
        print(f"warning: source is only {img.size[0]}x{img.size[1]}; carriers re-encode "
              f"MMS hard and this will look soft", file=sys.stderr)

    blob, preview = encode(img, width=args.width, height=args.height, crop=args.crop,
                           saturation=args.saturation, contrast=args.contrast,
                           palette=args.palette, want_preview=bool(args.preview))
    args.out.write_bytes(blob)
    if preview is not None:
        preview.save(args.preview)

    h = parse_header(blob)  # round-trips through the same validation the firmware does
    print(f"{args.out}: {len(blob)} bytes, {h['width']}x{h['height']}, "
          f"crc32=0x{h['data_crc32']:08x}")
    print(f"etag: {etag_for(blob)}")


if __name__ == "__main__":
    main()
