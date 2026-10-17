# Six logo candidates in the MixLauncher / MixWidgets style: a dark tile with a hairline and a flat mark in three
# shades of one hue, dark to light. Writes res/logo-variants/N.png and a comparison sheet.
# python tools/logo-variants.py  (needs Pillow)
import math
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "res", "logo-variants")
SS = 4            # supersampling
N = 256 * SS      # tile size while drawing
TILE = (0x1E, 0x21, 0x28)


def hexc(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, 255)


def tile():
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = int(N * 0.225)
    d.rounded_rectangle([0, 0, N - 1, N - 1], radius=r, fill=TILE + (255,))
    d.rounded_rectangle([0, 0, N - 1, N - 1], radius=r, outline=(255, 255, 255, 26), width=SS * 2)
    return img, d


def U(v):
    # art units: 0..100 across the inner 62% of the tile, like MixWidgets' draw_app_tile
    inner = N * 0.62
    return (N - inner) / 2 + v / 100 * inner


def P(pts):
    return [(U(x), U(y)) for x, y in pts]


def thick_line(d, pts, w, col):
    pts = P(pts)
    w = w / 100 * N * 0.62
    d.line(pts, fill=col, width=int(w), joint="curve")
    for x, y in (pts[0], pts[-1]):
        d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=col)


BOLT = [(58, 0), (14, 56), (44, 56), (36, 100), (86, 38), (56, 38), (66, 0)]


def v1_bolt_stack(d, c):
    # three bolts offset like MixWidgets' cards
    for i, (dx, dy) in enumerate([(14, -10), (7, -5), (0, 0)]):
        d.polygon(P([(x * 0.86 + dx, y * 0.9 + 6 + dy) for x, y in BOLT]), fill=hexc(c[i]))


def v2_chevrons(d, c):
    # boost: three chevrons rising, the top one lightest
    for i, y in enumerate([84, 52, 20]):
        thick_line(d, [(14, y + 22), (50, y - 6), (86, y + 22)], 15, hexc(c[i]))


def v3_gauge(d, c):
    # a speedometer: three arc segments and a needle near the top
    cx, cy, r, w = U(50), U(60), (U(100) - U(0)) * 0.46, (U(100) - U(0)) * 0.15
    box = [cx - r, cy - r, cx + r, cy + r]
    segs = [(150, 220), (226, 296), (302, 390)]
    for i, (a0, a1) in enumerate(segs):
        d.arc(box, a0, a1, fill=hexc(c[i]), width=int(w))
    ang = math.radians(-58)
    nx, ny = cx + math.cos(ang) * r * 0.78, cy + math.sin(ang) * r * 0.78
    d.line([(cx, cy), (nx, ny)], fill=hexc(c[2]), width=int(w * 0.5))
    hr = w * 0.62
    d.ellipse([cx - hr, cy - hr, cx + hr, cy + hr], fill=hexc(c[2]))
    d.ellipse([nx - w * 0.25, ny - w * 0.25, nx + w * 0.25, ny + w * 0.25], fill=hexc(c[2]))


def v4_chip(d, c):
    # a CPU with one core lit: the core the GPU gets
    s = (U(100) - U(0)) / 100
    d.rounded_rectangle(P([(14, 14), (86, 86)]), radius=int(14 * s), fill=hexc(c[0]))
    for k in range(3):
        o = 32 + k * 18
        for a, b in [((o - 4, 2), (o + 4, 14)), ((o - 4, 86), (o + 4, 98)), ((2, o - 4), (14, o + 4)), ((86, o - 4), (98, o + 4))]:
            d.rounded_rectangle(P([a, b]), radius=int(3 * s), fill=hexc(c[0]))
    cells = [(26, 26), (52, 26), (26, 52), (52, 52)]
    for i, (x, y) in enumerate(cells):
        d.rounded_rectangle(P([(x, y), (x + 22, y + 22)]), radius=int(6 * s), fill=hexc(c[2] if i == 1 else c[1]))


def v5_bars(d, c):
    # rising bars with a spark on top
    s = (U(100) - U(0)) / 100
    for i, (x, h) in enumerate([(8, 40), (38, 64), (68, 92)]):
        d.rounded_rectangle(P([(x, 100 - h), (x + 24, 100)]), radius=int(8 * s), fill=hexc(c[i]))


def v6_layers(d, c):
    # MixLauncher's squashed layers, lifting off, with a bolt on the top one
    sq = 0.53
    for i, dy in enumerate([34, 4, -30]):
        cx, cy, h = 50, 58 + dy, 40
        pts = [(cx, cy - h * sq), (cx + h, cy), (cx, cy + h * sq), (cx - h, cy)]
        d.polygon(P(pts), fill=hexc(c[i]))
    bolt = [(x * 0.30 + 36, y * 0.30 + 13) for x, y in BOLT]
    d.polygon(P(bolt), fill=(0x1E, 0x21, 0x28, 200))


VARIANTS = [
    ("Молния-стек", v1_bolt_stack, [0x8A5A1E, 0xD08A2E, 0xFFC56B]),
    ("Шевроны", v2_chevrons, [0x1F6E6A, 0x2FA89F, 0x7FE3D6]),
    ("Спидометр", v3_gauge, [0x7A3B2E, 0xC8613F, 0xFFA474]),
    ("Чип с ядром", v4_chip, [0x2C5E3A, 0x469C5C, 0xA6F08E]),
    ("Столбцы", v5_bars, [0x264E86, 0x3F7BD0, 0x8EC0FF]),
    ("Слои", v6_layers, [0x8A3B57, 0xC95A82, 0xFF9DC2]),
]


def render(fn, colors):
    img, d = tile()
    fn(d, colors)
    return img.resize((256, 256), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    tiles = []
    for i, (name, fn, cols) in enumerate(VARIANTS, 1):
        t = render(fn, cols)
        t.save(os.path.join(OUT, f"{i}.png"))
        tiles.append((name, t))

    # sheet: big tile, then 48 and 24 px on dark and light backgrounds
    cw, ch = 300, 400
    sheet = Image.new("RGBA", (cw * 3, ch * 2), (32, 32, 34, 255))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("segoeuib.ttf", 22)
        small = ImageFont.truetype("segoeui.ttf", 15)
    except OSError:
        font = small = ImageFont.load_default()
    for k, (name, t) in enumerate(tiles):
        x0, y0 = (k % 3) * cw, (k // 3) * ch
        sheet.alpha_composite(t.resize((200, 200), Image.LANCZOS), (x0 + 50, y0 + 30))
        d.text((x0 + cw / 2, y0 + 252), f"{k + 1}. {name}", fill=(240, 240, 240), font=font, anchor="mm")
        d.rounded_rectangle([x0 + 40, y0 + 290, x0 + 260, y0 + 370], radius=10, fill=(243, 243, 245, 255))
        for j, sz in enumerate([48, 32, 24, 16]):
            sx = x0 + 52 + j * 52
            sheet.alpha_composite(t.resize((sz, sz), Image.LANCZOS), (sx, y0 + 330 - sz // 2))
    sheet.save(os.path.join(OUT, "sheet.png"))
    print("ok")


if __name__ == "__main__":
    main()
