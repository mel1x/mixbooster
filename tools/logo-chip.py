# Four takes on the "chip with one core lit" logo, more modern: soft gradients and a glow on the lit core.
# Writes res/logo-variants/chip-N.png and chip-sheet.png.
# python tools/logo-chip.py  (needs Pillow)
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "res", "logo-variants")
SS = 4
N = 256 * SS


def rgb(v, a=255):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, a)


def gradient(c0, c1, vertical=True, diag=False):
    """A full-tile gradient image from c0 (top / top-left) to c1."""
    g = Image.new("RGBA", (N, N))
    px = g.load()
    a, b = rgb(c0), rgb(c1)
    # a small strip is enough, then resize
    strip = Image.new("RGBA", (256, 256))
    sp = strip.load()
    for y in range(256):
        for x in range(256):
            t = ((x + y) / 510) if diag else (y / 255 if vertical else x / 255)
            sp[x, y] = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(4))
    return strip.resize((N, N), Image.BILINEAR)


def mask_of(draw_fn):
    m = Image.new("L", (N, N), 0)
    draw_fn(ImageDraw.Draw(m))
    return m


def fill(img, draw_fn, c0, c1=None, diag=False):
    m = mask_of(draw_fn)
    src = gradient(c0, c1 if c1 is not None else c0, diag=diag)
    img.paste(src, (0, 0), m)


def glow(img, draw_fn, color, radius, strength=1.0):
    m = mask_of(draw_fn).filter(ImageFilter.GaussianBlur(radius * SS))
    if strength != 1.0:
        m = m.point(lambda v: min(255, int(v * strength)))
    layer = Image.new("RGBA", (N, N), rgb(color))
    layer.putalpha(m)
    img.alpha_composite(layer)


def U(v):
    inner = N * 0.66
    return (N - inner) / 2 + v / 100 * inner


def R(x0, y0, x1, y1):
    return [U(x0), U(y0), U(x1), U(y1)]


def k(v):
    return v / 100 * N * 0.66


def tile(top=0x2A2E38, bottom=0x191B21):
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    r = int(N * 0.225)
    fill(img, lambda d: d.rounded_rectangle([0, 0, N - 1, N - 1], radius=r, fill=255), top, bottom)
    # hairline, a touch brighter at the top like a lit edge
    edge = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle([0, 0, N - 1, N - 1], radius=r, outline=(255, 255, 255, 34), width=SS * 2)
    fade = gradient(0xFFFFFF, 0xFFFFFF)
    fade.putalpha(Image.linear_gradient("L").resize((N, N)).point(lambda v: 255 - int(v * 0.55)))
    edge.putalpha(Image.composite(edge.getchannel("A"), Image.new("L", (N, N), 0), fade.getchannel("A")))
    img.alpha_composite(edge)
    return img


def pins(img, color0, color1, spread=(32, 50, 68), length=11, w=7):
    def draw(d):
        for o in spread:
            d.rounded_rectangle(R(o - w / 2, 14 - length, o + w / 2, 16), radius=k(w / 2), fill=255)
            d.rounded_rectangle(R(o - w / 2, 84, o + w / 2, 86 + length), radius=k(w / 2), fill=255)
            d.rounded_rectangle(R(14 - length, o - w / 2, 16, o + w / 2), radius=k(w / 2), fill=255)
            d.rounded_rectangle(R(84, o - w / 2, 86 + length, o + w / 2), radius=k(w / 2), fill=255)
    fill(img, draw, color0, color1)


# 1: the original, refined: gradient body, 2x2 cores, the lit one glowing
def chip1():
    img = tile()
    pins(img, 0x3E8A55, 0x2A5E3A)
    fill(img, lambda d: d.rounded_rectangle(R(14, 14, 86, 86), radius=k(16), fill=255), 0x3A7D4E, 0x24502F)
    # inner bevel: a lighter rim at the top
    fill(img, lambda d: d.rounded_rectangle(R(14, 14, 86, 86), radius=k(16), outline=255, width=int(k(1.6))), 0x6CC487, 0x2C5E3A)
    cells = [(25, 25), (52, 25), (25, 52), (52, 52)]
    for i, (x, y) in enumerate(cells):
        if i == 1:
            continue
        fill(img, lambda d, x=x, y=y: d.rounded_rectangle(R(x, y, x + 23, y + 23), radius=k(6), fill=255), 0x4FA066, 0x3C8152)
    lit = lambda d: d.rounded_rectangle(R(52, 25, 75, 48), radius=k(6), fill=255)
    glow(img, lit, 0xA6F08E, 9, 1.3)
    fill(img, lit, 0xD7FFB8, 0x7FDB6A)
    return img


# 2: a 3x3 core grid, one core lit; thinner, lighter body
def chip2():
    img = tile(0x272B34, 0x17191E)
    pins(img, 0x35644A, 0x24402F, spread=(30, 50, 70), length=10, w=6)
    fill(img, lambda d: d.rounded_rectangle(R(13, 13, 87, 87), radius=k(18), fill=255), 0x2F5C40, 0x1F3A29)
    fill(img, lambda d: d.rounded_rectangle(R(13, 13, 87, 87), radius=k(18), outline=255, width=int(k(1.4))), 0x5FAE78, 0x24452F)
    step, sz, o = 20, 15, 22.5
    for r in range(3):
        for c in range(3):
            x, y = o + c * step, o + r * step
            if (r, c) == (0, 2):
                continue
            fill(img, lambda d, x=x, y=y: d.rounded_rectangle(R(x, y, x + sz, y + sz), radius=k(4.5), fill=255), 0x3F7D55, 0x316544)
    x, y = o + 2 * step, o
    lit = lambda d: d.rounded_rectangle(R(x, y, x + sz, y + sz), radius=k(4.5), fill=255)
    glow(img, lit, 0x9CF07E, 8, 1.5)
    fill(img, lit, 0xE2FFC9, 0x86E06F)
    return img


# 3: no grid, one large core in the middle with a bolt cut out: the core the GPU gets a boost from
def chip3():
    img = tile()
    pins(img, 0x3E8A55, 0x2A5E3A, spread=(36, 64), length=11, w=8)
    fill(img, lambda d: d.rounded_rectangle(R(14, 14, 86, 86), radius=k(18), fill=255), 0x2F6A43, 0x1E432A)
    fill(img, lambda d: d.rounded_rectangle(R(14, 14, 86, 86), radius=k(18), outline=255, width=int(k(1.6))), 0x67BE80, 0x24502F)
    core = lambda d: d.rounded_rectangle(R(29, 29, 71, 71), radius=k(10), fill=255)
    glow(img, core, 0xA6F08E, 12, 1.1)
    fill(img, core, 0xC9FFA6, 0x5FCB5A, diag=True)
    bolt = [(54, 33), (39, 52), (49, 52), (46, 67), (61, 47), (51, 47), (55, 33)]
    fill(img, lambda d: d.polygon([(U(x), U(y)) for x, y in bolt], fill=255), 0x1F4A2C, 0x173A22)
    return img


# 4: flat-modern: no body fill, a gradient outline chip with dot pins and the lit core as a pill of light
def chip4():
    img = tile(0x252830, 0x15171B)
    def dots(d):
        for o in (32, 50, 68):
            for x, y in ((o, 6), (o, 94), (6, o), (94, o)):
                d.ellipse(R(x - 3.6, y - 3.6, x + 3.6, y + 3.6), fill=255)
    fill(img, dots, 0x5DB277, 0x2E6440)
    fill(img, lambda d: d.rounded_rectangle(R(14, 14, 86, 86), radius=k(20), outline=255, width=int(k(5))), 0x8FE09A, 0x2C6B42, diag=True)
    cells = [(27, 27), (52, 27), (27, 52), (52, 52)]
    for i, (x, y) in enumerate(cells):
        if i == 1:
            continue
        fill(img, lambda d, x=x, y=y: d.rounded_rectangle(R(x, y, x + 21, y + 21), radius=k(7), fill=255), 0x2F5E3E, 0x264C33)
    lit = lambda d: d.rounded_rectangle(R(52, 27, 73, 48), radius=k(7), fill=255)
    glow(img, lit, 0xB4F59A, 10, 1.6)
    fill(img, lit, 0xEFFFE0, 0x8EE476, diag=True)
    return img


VARIANTS = [("4a. Классика", chip1), ("4b. Сетка 3×3", chip2), ("4c. Ядро-молния", chip3), ("4d. Контур", chip4)]


def main():
    os.makedirs(OUT, exist_ok=True)
    tiles = []
    for i, (name, fn) in enumerate(VARIANTS, 1):
        t = fn().resize((256, 256), Image.LANCZOS)
        t.save(os.path.join(OUT, f"chip-{i}.png"))
        tiles.append((name, t))
    cw, ch = 300, 420
    sheet = Image.new("RGBA", (cw * 4, ch), (32, 32, 34, 255))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("segoeuib.ttf", 22)
    except OSError:
        font = ImageFont.load_default()
    for k_, (name, t) in enumerate(tiles):
        x0 = k_ * cw
        sheet.alpha_composite(t.resize((200, 200), Image.LANCZOS), (x0 + 50, 30))
        d.text((x0 + cw / 2, 254), name, fill=(240, 240, 240), font=font, anchor="mm")
        d.rounded_rectangle([x0 + 40, 290, x0 + 260, 345], radius=10, fill=(243, 243, 245, 255))
        d.rounded_rectangle([x0 + 40, 355, x0 + 260, 410], radius=10, fill=(20, 20, 22, 255))
        for row, yy in enumerate((317, 382)):
            for j, sz in enumerate([48, 32, 24, 16]):
                sheet.alpha_composite(t.resize((sz, sz), Image.LANCZOS), (x0 + 52 + j * 52, yy - sz // 2))
    sheet.save(os.path.join(OUT, "chip-sheet.png"))
    print("ok")


if __name__ == "__main__":
    main()
