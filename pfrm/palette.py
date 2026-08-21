"""The six Spectra colours, and the nibble each one is stored as.

The nibbles are Seeed_GFX's *framebuffer* encoding, not the panel's wire codes -- the
library's COLOR_GET macro translates on the way out over SPI. Packing the framebuffer
encoding is what lets the firmware memcpy a downloaded blob straight in with no
per-pixel work at all.

    we pack   panel sees
    0x0 white   -> 0x1        0x2 green  -> 0x6
    0x6 red     -> 0x3        0xB yellow -> 0x2
    0xD blue    -> 0x5        0xF black  -> 0x0

Anything outside the set falls through COLOR_GET to white, so an encoder bug shows up
as a washed-out picture rather than as noise.
"""

# The RGB values are what the dither aims at. Which set you want is a real tradeoff:
#
#   "measured"  approximates what the panel actually produces. Better skin tones and
#               shadows, and the right default for photos of people.
#   "saturated" uses pure primaries. Punchier, less faithful, better for graphics.
#
# TODO(bring-up): reconcile "measured" against Seeed's own dither.cpp (PAL_E6) in
# Seeed_Arduino_LCD/examples/ePaper/reTerminal_SDcard_Bitmap/reTerminal_E1002_SDcard_Color6/,
# which is a port of their browser img2bitmap tool. Matching it means the web preview
# and the wall agree.
PALETTES = {
    "measured": [
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

DEFAULT_PALETTE = "measured"

LEGAL_NIBBLES = {nibble for entries in PALETTES.values() for _rgb, nibble in entries}
