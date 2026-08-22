"""Image pipeline behaviour that is easy to get wrong and expensive to get wrong.

Expensive because a bad frame sticks: the panel has no partial refresh, so whatever
lands stays on the wall until the next photo arrives.
"""

import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from pfrm import PANEL_H, PANEL_W, parse_header  # noqa: E402
from pfrm.palette import LEGAL_NIBBLES, PALETTES  # noqa: E402
from pfrm.render import PAPER, encode, open_image  # noqa: E402

WHITE_NIBBLE = 0x0


def unpack(blob):
    """Blob -> (H, W) array of palette nibbles, as the panel would see it."""
    rows = np.frombuffer(blob[64:], dtype=np.uint8).reshape(PANEL_H, PANEL_W // 2)
    nib = np.empty((PANEL_H, PANEL_W), dtype=np.uint8)
    nib[:, 0::2] = rows >> 4        # high nibble is the even-x (left) pixel
    nib[:, 1::2] = rows & 0x0F
    return nib


def transparent_png(underlying=(0, 0, 0)):
    """An RGBA image whose transparent pixels hide `underlying` RGB.

    Encoders commonly leave zeros there. A plain .convert("RGB") exposes them, which
    is how a logo with a transparent background becomes a black rectangle.
    """
    img = Image.new("RGBA", (600, 400), underlying + (0,))
    img.paste(Image.new("RGBA", (200, 150), (220, 30, 40, 255)), (200, 125))
    return img


def test_transparency_flattens_to_white_not_black():
    blob, _ = encode(transparent_png(underlying=(0, 0, 0)))
    nib = unpack(blob)
    corners = [nib[2, 2], nib[2, PANEL_W - 3], nib[PANEL_H - 3, 2], nib[PANEL_H - 3, PANEL_W - 3]]
    assert all(c == WHITE_NIBBLE for c in corners), \
        f"transparent background rendered as {[hex(int(c)) for c in corners]}, expected white"


def test_transparent_area_matches_the_letterbox_padding():
    """The whole point: no seam between the image's own transparency and the pad bars."""
    blob, _ = encode(transparent_png(underlying=(0, 0, 0)))
    nib = unpack(blob)
    pad_row = nib[5, :]                 # padding above the fitted image
    inner_row = nib[PANEL_H // 2, 5:15]  # transparent region inside the image itself
    assert set(pad_row.tolist()) == {WHITE_NIBBLE}
    assert set(inner_row.tolist()) == {WHITE_NIBBLE}


def test_paper_constant_is_shared_with_padding():
    """Guards against the two whites drifting apart in a later edit."""
    assert PAPER == (255, 255, 255)


@pytest.mark.parametrize("mode", ["RGBA", "LA", "P"])
def test_all_alpha_carrying_modes_are_handled(mode):
    src = Image.new("RGBA", (300, 300), (0, 0, 0, 0))
    src.paste(Image.new("RGBA", (100, 100), (40, 60, 200, 255)), (100, 100))
    if mode == "LA":
        src = src.convert("LA")
    elif mode == "P":
        src = src.convert("P", palette=Image.Palette.ADAPTIVE)
        src.info["transparency"] = 0
    out = open_image(src.tobytes() if False else _to_png(src))
    assert out.mode == "RGB"


def _to_png(img):
    import io
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


def test_opaque_images_are_untouched():
    """A normal photo must not be altered by the alpha path."""
    src = Image.new("RGB", (800, 600), (12, 34, 56))
    assert open_image(_to_png(src)).getpixel((0, 0)) == (12, 34, 56)


def test_output_only_uses_legal_palette_nibbles():
    blob, _ = encode(transparent_png())
    nib = unpack(blob)
    assert set(np.unique(nib).tolist()) <= LEGAL_NIBBLES


def test_blob_still_validates_after_flattening():
    blob, _ = encode(transparent_png())
    h = parse_header(blob)
    assert (h["width"], h["height"]) == (PANEL_W, PANEL_H)
    assert len(blob) == 64 + PANEL_W * PANEL_H // 2


def test_landscape_source_is_padded_white_top_and_bottom():
    """The real-world case: an 800x533 landscape photo onto a 1200x1600 portrait panel."""
    blob, _ = encode(Image.new("RGB", (800, 533), (200, 40, 40)))
    nib = unpack(blob)
    assert nib[2, PANEL_W // 2] == WHITE_NIBBLE
    assert nib[PANEL_H - 3, PANEL_W // 2] == WHITE_NIBBLE
    assert nib[PANEL_H // 2, PANEL_W // 2] != WHITE_NIBBLE   # the photo itself is there
