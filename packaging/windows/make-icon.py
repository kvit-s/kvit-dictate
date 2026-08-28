# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
r"""Generate `kvit-dictate.ico`, the executable's icon.

`src/tray_icon.h` explains at length why the *notification-area* icon is drawn at run time and
not shipped: it has to answer nine states, two colour schemes and four sizes at once, which is
seventy-two bitmaps against forty lines of geometry. That argument does not extend to the
executable's own icon. Explorer, the taskbar, Alt-Tab and the installer all read a resource, and
there is exactly one of it — no state, no scheme.

So this exists to keep the one binary asset in the repository **reproducible**: the geometry
below is the same capsule-on-a-cradle the tray draws, at the same proportions, and regenerating
the file is one command rather than an afternoon in an image editor nobody has installed.

    py -3.12 -m pip install pillow
    py -3.12 packaging/windows/make-icon.py

The result is committed. Running this again should produce the same bytes; if it does not, the
geometry changed and the commit should say why.
"""

from __future__ import annotations

import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover - the message is the whole point
    sys.exit("this needs Pillow: py -3.12 -m pip install pillow")

# Sizes Windows actually asks for. 256 is the one Explorer's extra-large view uses and the one
# an installer scales its banner from; 16 is the one that decides whether the icon reads at all.
SIZES = (16, 20, 24, 32, 48, 64, 128, 256)

# The glyph is drawn at 8x and downsampled, which is how the curves survive at 16 px. Direct2D
# does this for the tray icon with anti-aliasing; here it is supersampling, and the result is
# indistinguishable at every size that matters.
SUPERSAMPLE = 8

# The pill's "idle" blue, so the executable icon, the tray icon and the overlay's state dot are
# recognisably the same object. Deliberately not the recording red: an application icon that is
# permanently red says "recording" about a program that spends almost all of its life idle.
FOREGROUND = (0x3B, 0x82, 0xF6, 0xFF)


def draw_microphone(size: int) -> "Image.Image":
    """The same capsule, cradle and stand as `draw_microphone` in src/tray_icon.cpp."""
    s = size * SUPERSAMPLE
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Below 24 px the glyph is simplified rather than scaled. This is the same trade a type
    # designer calls hinting and tray_icon.cpp calls snapping: at 16 px a foot bar three pixels
    # from the stem it hangs off merges with it, and a "microphone" that reads as a smudge with
    # a dot over it is worse than one with fewer parts. The parts that survive are the two that
    # carry the recognition — the capsule and the cradle under it.
    tiny = size < 24
    stroke = max(1, round(s * (0.105 if tiny else 0.082)))
    cx = s * 0.5

    # The capsule: the microphone itself.
    cap_w = s * (0.30 if tiny else 0.27)
    cap_top = s * 0.11
    cap_bottom = s * 0.47
    # Filled below 24 px, hollow above. A 16 px capsule is five pixels across, and a two-pixel
    # outline on either side leaves an interior one pixel wide that the downsample turns to mud.
    # Filling it is the only way the shape survives, and the meaning the tray attaches to a fill
    # ("the microphone is open") does not travel here: this glyph never changes.
    d.rounded_rectangle(
        (cx - cap_w / 2, cap_top, cx + cap_w / 2, cap_bottom),
        radius=cap_w / 2,
        fill=FOREGROUND if tiny else None,
        outline=FOREGROUND,
        width=stroke,
    )

    # The cradle: the lower half of a circle the capsule hangs inside. Drawn as an arc from 0 to
    # 180 degrees — Pillow measures clockwise from three o'clock, so this is the bottom half.
    cradle_r = s * (0.33 if tiny else 0.31)
    cradle_y = s * 0.45
    d.arc(
        (cx - cradle_r, cradle_y - cradle_r, cx + cradle_r, cradle_y + cradle_r),
        start=0,
        end=180,
        fill=FOREGROUND,
        width=stroke,
    )

    # The stand: the stem down from the cradle, and — above 20 px — the foot it rests on.
    stem_top = cradle_y + cradle_r
    stem_bottom = s * (0.94 if tiny else 0.90)
    d.line((cx, stem_top, cx, stem_bottom), fill=FOREGROUND, width=stroke)
    if not tiny:
        foot = s * 0.19
        d.line((cx - foot, stem_bottom, cx + foot, stem_bottom), fill=FOREGROUND, width=stroke)

    return img.resize((size, size), Image.LANCZOS)


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "kvit-dictate.ico")
    frames = [draw_microphone(n) for n in SIZES]
    # Pillow writes every size given in `sizes` from the base image, so the largest is passed as
    # the base and the rest as explicit frames — otherwise the 16 px entry is a naive downscale
    # of the 256 and loses the stroke weight the small sizes were drawn for.
    frames[-1].save(out, format="ICO", sizes=[(n, n) for n in SIZES], append_images=frames[:-1])
    print(f"wrote {out} ({os.path.getsize(out)} bytes, sizes {', '.join(map(str, SIZES))})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
