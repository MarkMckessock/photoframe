"""Shared image contract between the cluster service and the ESP32 firmware.

The firmware's side of this lives in `firmware/include/pf_image_format.h`. These two
files describe the same 64 bytes; if you change one, change the other in the same
commit and bump the version. `tools/test_format.py` checks that they still agree.
"""

from .format import (
    FMT_SEEED_GFX_4BPP,
    HEADER_LEN,
    MAGIC,
    PALETTE_SPECTRA6,
    PANEL_H,
    PANEL_W,
    VERSION,
    blob_size,
    etag_for,
    pack_header,
    parse_header,
)
from .palette import PALETTES
from .render import encode, open_image

__all__ = [
    "FMT_SEEED_GFX_4BPP", "HEADER_LEN", "MAGIC", "PALETTE_SPECTRA6", "PALETTES",
    "PANEL_H", "PANEL_W", "VERSION", "blob_size", "encode", "etag_for", "open_image",
    "pack_header", "parse_header",
]
