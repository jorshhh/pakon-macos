"""
Make the logo from the artwork (brand/artwork.jpeg, an F135 on black): cut
it out and redraw it as true pixel art, a 128-pixel-wide sprite with a
24-colour palette, hard edges and a one-pixel outline, and clear the label
down to "F135" (no Kodak wordmark: the project is not affiliated with them).
Every use scales the sprite by whole numbers.

    python brand/make_logo.py [ARTWORK]

Writes:
  brand/logo.png                                  the sprite (128 px wide)
  website/theme/pakon/assets/images/logo.png      the site header's copy
  website/pakon-icon.png                          site icon, 512 px (Ghost: Publication icon)

The app icon is drawn from brand/logo.png by packaging/macos/make_icon.py.
"""
import shutil
import sys
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[1]
LOGO = REPO / "brand" / "logo.png"
WIDTH = 128         # sprite width in art pixels
COLORS = 24
BLACK = 12          # background is 0..6; the navy outline has blue >= 17
LINE = 30           # the artwork's stray line off the right side is 21 px thick
# Label text cleared in the sprite (x0, y0, x1, y1, inclusive; sprite pixels
# at WIDTH 128): "Kodak" + "FILM SCANNER" (two boxes: the lower one stops
# short of the label's shaded edge), and the "plus ICE" badge. "F135" sits
# between them and stays.
CLEAR = [(44, 28, 68, 40), (56, 41, 68, 42), (82, 40, 91, 47)]


def cut_out(a):
    """Opaque mask: everything but the near-black pixels connected to the edge."""
    h, w, _ = a.shape
    dark = a.max(axis=2) <= BLACK
    bg = np.zeros((h, w), bool)
    q = deque((y, x) for y in range(h) for x in (0, w - 1) if dark[y, x])
    q.extend((y, x) for x in range(w) for y in (0, h - 1) if dark[y, x])
    for y, x in q:
        bg[y, x] = True
    while q:
        y, x = q.popleft()
        for ny, nx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
            if 0 <= ny < h and 0 <= nx < w and dark[ny, nx] and not bg[ny, nx]:
                bg[ny, nx] = True
                q.append((ny, nx))
    opaque = ~bg
    # drop the thin line running from the scanner to the right edge: columns,
    # from the edge inward, that are no thicker than it
    x = w - 1
    while x > 0 and 0 < opaque[:, x].sum() <= LINE:
        opaque[:, x] = False
        x -= 1
    return opaque


def palette_indices(a, opaque):
    """A COLORS-entry palette from the scanner's pixels, and every pixel's
    nearest entry."""
    px = a[opaque]
    pal_img = Image.fromarray(px.reshape(-1, 1, 3).astype(np.uint8)).quantize(
        colors=COLORS, method=Image.Quantize.MEDIANCUT, kmeans=4,
        dither=Image.Dither.NONE)
    pal = np.array(pal_img.getpalette()[:COLORS * 3]).reshape(-1, 3)
    flat = a.reshape(-1, 3)
    idx = np.empty(len(flat), np.int64)
    step = 1 << 18
    for k in range(0, len(flat), step):
        idx[k:k + step] = ((flat[k:k + step, None, :] - pal[None]) ** 2).sum(2).argmin(1)
    return pal, idx.reshape(opaque.shape)


def pixelate(a, opaque):
    """The sprite: per grid cell, the most common palette colour, or
    transparent when the cell is mostly background; then the outline."""
    ys, xs = np.nonzero(opaque)
    a, opaque = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1], \
        opaque[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    pal, idx = palette_indices(a, opaque)
    h, w = opaque.shape
    cell = w / WIDTH
    height = int(round(h / cell))
    out = np.zeros((height, WIDTH, 4), np.uint8)
    for j in range(height):
        y0 = int(j * cell)
        y1 = max(int((j + 1) * cell), y0 + 1)
        for i in range(WIDTH):
            x0 = int(i * cell)
            x1 = max(int((i + 1) * cell), x0 + 1)
            o = opaque[y0:y1, x0:x1]
            if o.mean() < 0.5:
                continue
            out[j, i, :3] = pal[np.bincount(idx[y0:y1, x0:x1][o], minlength=COLORS).argmax()]
            out[j, i, 3] = 255
    # a clean outline: the darkest colour on every pixel next to transparency
    on = out[:, :, 3] > 0
    p = np.pad(on, 1)
    edge = on & ~(p[:-2, 1:-1] & p[2:, 1:-1] & p[1:-1, :-2] & p[1:-1, 2:])
    out[edge, :3] = pal[np.argmin(pal.sum(1))]
    clear_label(out)
    return Image.fromarray(out)


def clear_label(out):
    """Paint everything in the CLEAR boxes that is not label-coloured (the
    text and its edge pixels) with the label's own colour, its commonest
    light pixel. The boxes stay inside the label's border and shading."""
    rgb = out[:, :, :3].astype(int)
    light = rgb.sum(2) > 650
    box = np.zeros(light.shape, bool)
    for x0, y0, x1, y1 in CLEAR:
        box[y0:y1 + 1, x0:x1 + 1] = True
    colours, counts = np.unique(rgb[box & light], axis=0, return_counts=True)
    label = colours[counts.argmax()]
    out[box & ~light & (out[:, :, 3] > 0), :3] = label


def scaled(sprite, factor):
    return sprite.resize((sprite.width * factor, sprite.height * factor), Image.NEAREST)


def on_square(im, size):
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.alpha_composite(im, ((size - im.width) // 2, (size - im.height) // 2))
    return out


def main(art):
    a = np.asarray(Image.open(art).convert("RGB")).astype(np.int32)
    sprite = pixelate(a, cut_out(a))
    sprite.save(LOGO, optimize=True)
    # the header shows it at half size in CSS: one art pixel per device pixel
    # on a retina screen
    shutil.copy(LOGO, REPO / "website/theme/pakon/assets/images/logo.png")
    on_square(scaled(sprite, 512 // WIDTH), 512).save(REPO / "website/pakon-icon.png",
                                                      optimize=True)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else REPO / "brand" / "artwork.jpeg")
