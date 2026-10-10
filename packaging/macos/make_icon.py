"""
Draw Pakon.app's icon (the pixel-art logo, brand/logo.png, on the paper tile
of the website and web UI) as an .iconset folder; build.sh turns it into
AppIcon.icns.

Usage: python3 make_icon.py OUT.iconset
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

LOGO = Path(__file__).resolve().parents[2] / "brand" / "logo.png"
INK, PAPER = (26, 25, 24), (244, 241, 234)


def draw(size):
    """The icon at `size` px: the tile on the macOS grid (824/1024 with a
    100/1024 margin), hard shadow, and the sprite at a whole-number scale."""
    u = size / 1024
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([118 * u, 118 * u, 942 * u, 942 * u], radius=185 * u, fill=INK)
    d.rounded_rectangle([100 * u, 100 * u, 912 * u, 912 * u], radius=185 * u,
                        fill=PAPER, outline=INK, width=max(1, round(22 * u)))
    sprite = Image.open(LOGO).convert("RGBA")
    k = int(size * 0.66 // sprite.width)                 # 1024 px: x5
    sprite = sprite.resize((sprite.width * k, sprite.height * k), Image.NEAREST)
    c = round(506 * u)
    im.alpha_composite(sprite, (c - sprite.width // 2, c - sprite.height // 2))
    return im


def main(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    big = draw(1024)
    for pt in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = pt * scale
            name = f"icon_{pt}x{pt}{'@2x' if scale == 2 else ''}.png"
            # whole art pixels where they fit; below that they are too small
            # to see, so scale the big one down smoothly
            im = draw(px) if px >= 256 else big.resize((px, px), Image.LANCZOS)
            im.save(out / name)


if __name__ == "__main__":
    main(sys.argv[1])
