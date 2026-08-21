"""Photo in, panel-native blob out.

Everything expensive lives here rather than on the MCU: EXIF rotation, resampling,
saturation boost, Floyd-Steinberg dithering to six colours, and nibble packing. The
firmware's whole job is to check a CRC and memcpy the result into the framebuffer,
which is what keeps a battery wake down to a few seconds.
"""

import io
import time

import numpy as np
from PIL import Image, ImageEnhance, ImageOps

from .format import PANEL_H, PANEL_W, pack_header
from .palette import DEFAULT_PALETTE, PALETTES

# iPhones are the common case and sometimes deliver HEIC even through MMS. Optional,
# because the dependency is awkward on some platforms and JPEG covers almost everything.
try:
    import pillow_heif

    pillow_heif.register_heif_opener()
    HEIF_SUPPORTED = True
except Exception:  # pragma: no cover - depends on the environment
    HEIF_SUPPORTED = False


def open_image(data):
    """Decode bytes into an RGB image, with orientation already applied."""
    img = Image.open(io.BytesIO(data))
    img.load()
    return ImageOps.exif_transpose(img).convert("RGB")


def _palette_image(entries):
    """A 'P'-mode image carrying the palette, for Image.quantize(palette=...).

    Pillow pads unused palette slots with black and the dither will happily aim at
    those phantom entries, which shows up as muddy shadows. Repeating our six colours
    across all 256 slots means every index maps to a real colour, and `index % 6`
    recovers which one.
    """
    flat = []
    while len(flat) < 256 * 3:
        for rgb, _nibble in entries:
            flat.extend(rgb)
    pal = Image.new("P", (1, 1))
    pal.putpalette(flat[: 256 * 3])
    return pal


def prepare(img, width=PANEL_W, height=PANEL_H, crop=False, saturation=1.4, contrast=1.0):
    img = img.convert("RGB")
    if crop:
        img = ImageOps.fit(img, (width, height), method=Image.LANCZOS, centering=(0.5, 0.5))
    else:
        # Letterbox onto white. For the group photos this thing will mostly show, that
        # beats cropping somebody out of frame, and white bars vanish against the
        # panel's own white border.
        img = ImageOps.pad(img, (width, height), method=Image.LANCZOS, color=(255, 255, 255))
    if contrast != 1.0:
        img = ImageEnhance.Contrast(img).enhance(contrast)
    if saturation != 1.0:
        # E-ink primaries are muted. Without a pre-boost a dithered photo reads like a
        # washed-out newspaper print.
        img = ImageEnhance.Color(img).enhance(saturation)
    return img


def quantize(img, entries):
    """Dither to the palette. Returns indices into `entries`, one byte per pixel."""
    q = img.quantize(palette=_palette_image(entries), dither=Image.Dither.FLOYDSTEINBERG)
    return np.asarray(q, dtype=np.uint8) % len(entries)


def pack_indices(idx, entries):
    """Pack to 4bpp: two pixels per byte, high nibble is the even-x (left) pixel."""
    lut = np.array([nibble for _rgb, nibble in entries], dtype=np.uint8)
    nib = lut[idx]
    packed = (nib[:, 0::2] << 4) | nib[:, 1::2]
    return packed.astype(np.uint8).tobytes()


def preview_from_indices(idx, entries):
    """An RGB image of exactly what the panel will show. For humans, not for the frame."""
    rgb = np.array([c for c, _n in entries], dtype=np.uint8)[idx]
    return Image.fromarray(rgb, "RGB")


def encode(source, width=PANEL_W, height=PANEL_H, crop=False, saturation=1.4,
           contrast=1.0, palette=DEFAULT_PALETTE, created_at=None, want_preview=False):
    """Photo (bytes or PIL Image) -> (blob, preview_or_None).

    `width` must be even: two pixels share a byte.
    """
    if width % 2:
        raise ValueError("width must be even (two pixels share a byte)")
    entries = PALETTES[palette]

    img = open_image(source) if isinstance(source, (bytes, bytearray)) else source
    img = prepare(img, width, height, crop, saturation, contrast)

    idx = quantize(img, entries)
    data = pack_indices(idx, entries)
    expected = width * height // 2
    if len(data) != expected:
        raise ValueError(f"packed {len(data)} bytes, expected {expected}")

    blob = pack_header(data, width, height,
                       time.time() if created_at is None else created_at) + data
    preview = preview_from_indices(idx, entries) if want_preview else None
    return blob, preview
