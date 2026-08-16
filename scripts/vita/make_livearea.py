#!/usr/bin/env python3
"""Build the LiveArea assets from source artwork.

The Vita wants 8-bit palette PNGs: icon0 at 128x128, bg at 840x500 and startup at
280x158. Photographic art bands badly at 256 colours, so the quantiser dithers.

  python make_livearea.py --background <image> --logo <image> [--outdir sce_sys]
"""

import argparse
import os

from PIL import Image

ICON_SIZE = (128, 128)
BACKGROUND_SIZE = (840, 500)
STARTUP_SIZE = (280, 158)

BACKDROP = (12, 14, 16)


def fill(image, size):
    """Cover the target size, cropping the overflowing axis from the centre."""
    source_aspect = image.width / image.height
    target_aspect = size[0] / size[1]

    if source_aspect > target_aspect:
        width = int(round(image.height * target_aspect))
        left = (image.width - width) // 2
        image = image.crop((left, 0, left + width, image.height))
    else:
        height = int(round(image.width / target_aspect))
        top = (image.height - height) // 2
        image = image.crop((0, top, image.width, top + height))

    return image.resize(size, Image.LANCZOS)


def fit_onto(image, size, background):
    """Scale to fit inside size without cropping, centred on a flat backdrop."""
    scale = min(size[0] / image.width, size[1] / image.height)
    scaled = image.resize((max(1, int(image.width * scale)), max(1, int(image.height * scale))),
                          Image.LANCZOS)

    canvas = Image.new("RGB", size, background)
    offset = ((size[0] - scaled.width) // 2, (size[1] - scaled.height) // 2)
    if scaled.mode == "RGBA":
        canvas.paste(scaled, offset, scaled)
    else:
        canvas.paste(scaled, offset)
    return canvas


def to_palette(image):
    """8-bit with an adaptive palette; dithering hides the banding at 256 colours."""
    return image.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT,
                                         dither=Image.FLOYDSTEINBERG)


def save(image, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image.save(path, "PNG", optimize=True)
    with Image.open(path) as written:
        print("  %-42s %-10s %-4s %7d bytes"
              % (path, "%dx%d" % written.size, written.mode, os.path.getsize(path)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--background", required=True)
    ap.add_argument("--logo", required=True)
    ap.add_argument("--outdir", default="sce_sys")
    args = ap.parse_args()

    background = Image.open(args.background).convert("RGB")
    logo = Image.open(args.logo).convert("RGBA")

    contents = os.path.join(args.outdir, "livearea", "contents")

    print("livearea assets:")
    save(to_palette(fill(background, BACKGROUND_SIZE)), os.path.join(contents, "bg.png"))

    # the gate sits on the background, so the logo keeps the same backdrop behind it
    save(to_palette(fit_onto(logo, STARTUP_SIZE, BACKDROP)),
         os.path.join(contents, "startup.png"))

    # the icon is square and the logo is not, so it gets the logo over a dark tile
    save(to_palette(fit_onto(logo, ICON_SIZE, BACKDROP)),
         os.path.join(args.outdir, "icon0.png"))


if __name__ == "__main__":
    main()
