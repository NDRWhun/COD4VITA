#!/usr/bin/env python3
"""Build the LiveArea assets from source artwork.

The Vita wants 8-bit palette PNGs: icon0 at 128x128, bg at 840x500 and startup at
280x158. The gate sits on top of the background, so startup keeps its alpha and gets
a glow to read against any part of it; the background is graded down so the gate and
the version text stay legible.

  python make_livearea.py --background <image> --logo <image> [--outdir sce_sys]
"""

import argparse
import os

from PIL import Image, ImageChops, ImageEnhance, ImageFilter

ICON_SIZE = (128, 128)
BACKGROUND_SIZE = (840, 500)
STARTUP_SIZE = (280, 158)

TRANSPARENT_INDEX = 255

# the artwork is desaturated green, so the halo picks that up rather than going pure white
GLOW = (214, 226, 198)


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


def vignette(size, strength):
    """A soft radial falloff, darkest in the corners."""
    mask = Image.new("L", (size[0] // 8, size[1] // 8), 0)
    for y in range(mask.height):
        for x in range(mask.width):
            dx = (x - mask.width / 2) / (mask.width / 2)
            dy = (y - mask.height / 2) / (mask.height / 2)
            distance = min(1.0, (dx * dx + dy * dy) ** 0.5 / 1.25)
            mask.putpixel((x, y), int(255 * (1.0 - strength * distance * distance)))
    return mask.resize(size, Image.BICUBIC).filter(ImageFilter.GaussianBlur(12))


def grade(image):
    """Darken and deepen the artwork so overlaid text and the gate stay readable."""
    image = ImageEnhance.Color(image).enhance(1.15)
    image = ImageEnhance.Contrast(image).enhance(1.28)
    image = ImageEnhance.Brightness(image).enhance(0.62)

    shaded = ImageChops.multiply(image, Image.merge("RGB", [vignette(image.size, 0.85)] * 3))

    # the version string sits bottom left, so weight the lower edge down further
    gradient = Image.linear_gradient("L").resize(image.size)
    floor = Image.merge("RGB", [gradient.point(lambda v: 255 - int(v * 0.45))] * 3)
    return ImageChops.multiply(shaded, floor)


def fit(image, size, margin):
    """Scale to fit inside size less a margin, without cropping."""
    room = (size[0] - margin * 2, size[1] - margin * 2)
    scale = min(room[0] / image.width, room[1] / image.height)
    return image.resize((max(1, int(image.width * scale)), max(1, int(image.height * scale))),
                        Image.LANCZOS)


def glow(logo, size, radius, opacity, colour):
    """A blurred copy of the logo's own alpha; a light halo lifts it off dark artwork."""
    canvas = Image.new("RGBA", size, (0, 0, 0, 0))
    offset = ((size[0] - logo.width) // 2, (size[1] - logo.height) // 2)
    canvas.paste(logo, offset, logo)

    alpha = canvas.getchannel("A")

    # a wide soft halo for separation, a tight bright one to pick out the letterforms
    wide = alpha.filter(ImageFilter.GaussianBlur(radius * 2)).point(lambda v: int(v * opacity))
    tight = alpha.filter(ImageFilter.GaussianBlur(radius / 2)).point(lambda v: int(v * opacity))
    halo = ImageChops.lighter(wide, tight)

    lit = Image.new("RGBA", size, colour + (0,))
    lit.putalpha(halo)
    return Image.alpha_composite(lit, canvas)


def to_palette(image):
    """8-bit with an adaptive palette; dithering hides banding at 256 colours."""
    return image.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT,
                                         dither=Image.FLOYDSTEINBERG)


def to_palette_with_alpha(image):
    """8-bit keeping one index transparent, which is what the gate needs."""
    alpha = image.getchannel("A")

    # quantise the colour on a black field, leaving the last index free for the hole
    flattened = Image.alpha_composite(Image.new("RGBA", image.size, (0, 0, 0, 255)), image)
    quantised = flattened.convert("RGB").quantize(colors=TRANSPARENT_INDEX,
                                                  method=Image.MEDIANCUT,
                                                  dither=Image.FLOYDSTEINBERG)

    palette = quantised.getpalette()[:TRANSPARENT_INDEX * 3] + [0, 0, 0]
    quantised.putpalette(palette)

    hole = alpha.point(lambda v: 255 if v < 128 else 0).convert("1")
    quantised.paste(TRANSPARENT_INDEX, (0, 0), hole)
    quantised.info["transparency"] = TRANSPARENT_INDEX
    return quantised


def save(image, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if "transparency" in image.info:
        image.save(path, "PNG", optimize=True, transparency=image.info["transparency"])
    else:
        image.save(path, "PNG", optimize=True)

    with Image.open(path) as written:
        print("  %-46s %-10s %-3s %s %7d bytes"
              % (path, "%dx%d" % written.size, written.mode,
                 "alpha" if "transparency" in written.info else "     ",
                 os.path.getsize(path)))


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
    save(to_palette(grade(fill(background, BACKGROUND_SIZE))),
         os.path.join(contents, "bg.png"))

    save(to_palette_with_alpha(glow(fit(logo, STARTUP_SIZE, 16), STARTUP_SIZE, 7, 0.7, GLOW)),
         os.path.join(contents, "startup.png"))

    # the icon is square and opaque, so the logo sits on a graded crop of the artwork
    tile = grade(fill(background, ICON_SIZE))
    mark = fit(logo, ICON_SIZE, 8)
    tile.paste(mark, ((ICON_SIZE[0] - mark.width) // 2, (ICON_SIZE[1] - mark.height) // 2), mark)
    save(to_palette(tile), os.path.join(args.outdir, "icon0.png"))


if __name__ == "__main__":
    main()
