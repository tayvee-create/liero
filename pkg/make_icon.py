"""Draws the macOS app icon (pkg/LieroNet.icns + pkg/Liero-icon.png).

Original pixel art: a green worm with a bazooka in a dug-out cave, with an
explosion. Run with Pillow installed:  python3 pkg/make_icon.py
"""
import os
import random
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
SIZE = 1024
GRID = 32  # pixel-art resolution inside the tile

# Palette (Liero-ish earthy browns, bright worm green)
DIRT = [(122, 76, 38), (138, 88, 44), (108, 66, 32), (150, 98, 50)]
CAVE = (46, 26, 14)
CAVE_EDGE = (78, 46, 22)
ROCK = [(120, 120, 128), (150, 150, 160), (92, 92, 100)]
G_DARK, G_MID, G_LIGHT = (24, 120, 36), (52, 176, 60), (128, 226, 110)
EYE, PUPIL = (250, 250, 240), (20, 20, 20)
GUN, GUN_DARK, GUN_LIGHT = (96, 100, 112), (58, 60, 70), (170, 176, 190)
FIRE = [(255, 250, 200), (255, 220, 70), (255, 150, 30), (220, 70, 20)]


def pixel_art():
    rnd = random.Random(7)
    img = Image.new("RGB", (GRID, GRID))
    px = img.load()

    # Dirt with speckle
    for y in range(GRID):
        for x in range(GRID):
            px[x, y] = rnd.choice(DIRT)

    # Dug-out tunnel: an irregular blob across the middle
    def in_cave(x, y):
        cx, cy = 15.5, 17.0
        dx, dy = (x - cx) / 14.0, (y - cy) / 8.5
        wobble = 0.12 * ((x * 7 + y * 3) % 5) / 5
        return dx * dx + dy * dy < 1.0 - wobble

    for y in range(GRID):
        for x in range(GRID):
            if in_cave(x, y):
                px[x, y] = CAVE
            elif any(in_cave(x + ox, y + oy) for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                px[x, y] = CAVE_EDGE

    # A couple of rocks in the dirt
    for rx, ry in ((3, 4), (25, 27), (26, 3)):
        for oy in range(3):
            for ox in range(4):
                if (ox, oy) not in ((0, 0), (3, 0), (0, 2), (3, 2)):
                    px[rx + ox, ry + oy] = ROCK[0]
        px[rx + 1, ry] = ROCK[1]
        px[rx, ry + 1] = ROCK[1]
        px[rx + 2, ry + 2] = ROCK[2]
        px[rx + 3, ry + 1] = ROCK[2]

    # Worm body: segmented S-curve, tail lower-left, head upper-middle
    body = [(6, 22), (7, 22), (8, 21), (9, 21), (10, 20), (11, 20), (12, 19), (12, 18), (12, 17),
            (13, 16), (13, 15), (13, 14)]
    for i, (x, y) in enumerate(body):
        r = 1 if i < 3 else 2
        for oy in range(-r, r + 1):
            for ox in range(-r, r + 1):
                if ox * ox + oy * oy <= r * r + 1:
                    px[x + ox, y + oy] = G_MID
    # Shading: dark underside, light top
    for i, (x, y) in enumerate(body):
        r = 1 if i < 3 else 2
        px[x, y + r] = G_DARK
        px[x - 1, y - r + (0 if r == 1 else 1)] = G_LIGHT
    # Head
    hx, hy = 14, 11
    for oy in range(-3, 3):
        for ox in range(-3, 4):
            if ox * ox + oy * oy <= 9:
                px[hx + ox, hy + oy] = G_MID
    for ox in range(-2, 3):
        px[hx + ox, hy + 2] = G_DARK
    px[hx - 2, hy - 2] = G_LIGHT
    px[hx - 1, hy - 3] = G_LIGHT
    # Eye looking right
    px[hx + 1, hy - 1] = EYE
    px[hx + 2, hy - 1] = EYE
    px[hx + 1, hy] = EYE
    px[hx + 2, hy] = PUPIL

    # Bazooka over the shoulder, pointing right and slightly up
    for x in range(11, 24):
        y = 14 - (x - 11) // 5
        px[x, y] = GUN
        px[x, y + 1] = GUN_DARK
        px[x, y - 1] = GUN_LIGHT
    px[24, 11] = GUN_DARK
    px[24, 12] = GUN_DARK

    # Explosion at the muzzle
    ex, ey = 27, 11
    for oy in range(-4, 5):
        for ox in range(-4, 5):
            d = (ox * ox + oy * oy) ** 0.5 + rnd.random() * 0.9
            if d < 1.6:
                px[ex + ox, ey + oy] = FIRE[0]
            elif d < 2.6:
                px[ex + ox, ey + oy] = FIRE[1]
            elif d < 3.5:
                px[ex + ox, ey + oy] = FIRE[2]
            elif d < 4.3 and 0 <= ex + ox < GRID:
                px[ex + ox, ey + oy] = FIRE[3]
    # Sparks
    for sx, sy in ((22, 6), (30, 6), (31, 16), (23, 17), (29, 18)):
        px[sx, sy] = FIRE[1]

    return img


def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    return m


def build_icon():
    # Apple's icon grid: ~824px tile centred in 1024 with a soft drop shadow
    tile = 824
    margin = (SIZE - tile) // 2
    art = pixel_art().resize((tile, tile), Image.NEAREST)

    # Subtle top-light vignette for depth
    shade = Image.new("L", (tile, tile), 0)
    sd = ImageDraw.Draw(shade)
    for i in range(tile):
        sd.line((0, i, tile, i), fill=int(70 * (i / tile) ** 1.6))
    art = Image.composite(Image.new("RGB", (tile, tile), (0, 0, 0)), art, shade)

    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))

    shadow = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    sm = rounded_mask(tile, 185)
    shadow.paste((0, 0, 0, 110), (margin, margin + 14), sm)
    shadow = shadow.filter(ImageFilter.GaussianBlur(18))
    canvas = Image.alpha_composite(canvas, shadow)

    tile_img = art.convert("RGBA")
    tile_img.putalpha(rounded_mask(tile, 185))
    canvas.alpha_composite(tile_img, (margin, margin))

    # Thin inner highlight rim
    rim = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle(
        (margin + 2, margin + 2, margin + tile - 3, margin + tile - 3),
        radius=183, outline=(255, 255, 255, 50), width=4)
    return Image.alpha_composite(canvas, rim)


def main():
    icon = build_icon()
    icon.save(os.path.join(HERE, "Liero-icon.png"))

    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "Liero.iconset")
        os.mkdir(iconset)
        for base in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = base * scale
                name = f"icon_{base}x{base}{'@2x' if scale == 2 else ''}.png"
                icon.resize((px, px), Image.LANCZOS).save(os.path.join(iconset, name))
        subprocess.check_call(["iconutil", "-c", "icns", iconset, "-o", os.path.join(HERE, "LieroNet.icns")])


if __name__ == "__main__":
    main()
