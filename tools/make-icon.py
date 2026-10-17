# Draws res/mixbooster.ico and res/logo.png: the "chip with one core lit" logo on a MixLauncher-style dark tile.
# The same geometry is drawn in the app by draw_logo (src/app.c); keep them in step.
# python tools/make-icon.py  (needs Pillow)
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "..", "res")
SS = 4
N = 256 * SS

TILE = (0x1C, 0x23, 0x1E, 255)   # dark, tinted toward the logo's green like MixLauncher's and MixWidgets' tiles
PIN = 0x2C5E3A
BODY = 0x2C5E3A
CORE_SCALE = 1.06   # the four cores as one group, scaled from the chip's center
CORE = 0x469C5C
LIT = 0xA6F08E


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, 255)


def U(v):
    # art units 0..100 over the inner 68% of the tile
    inner = N * 0.68
    return (N - inner) / 2 + v / 100 * inner


def k(v):
    return v / 100 * N * 0.68


def R(x0, y0, x1, y1):
    return [U(x0), U(y0), U(x1), U(y1)]


def main():
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = int(N * 0.225)
    d.rounded_rectangle([0, 0, N - 1, N - 1], radius=r, fill=TILE)
    d.rounded_rectangle([0, 0, N - 1, N - 1], radius=r, outline=(255, 255, 255, 26), width=SS * 2)

    for o in (32, 50, 68):
        for box in ((o - 4, 2, o + 4, 14), (o - 4, 86, o + 4, 98), (2, o - 4, 14, o + 4), (86, o - 4, 98, o + 4)):
            d.rounded_rectangle(R(*box), radius=k(3), fill=rgb(PIN))

    d.rounded_rectangle(R(14, 14, 86, 86), radius=k(14), fill=rgb(BODY))

    for i, (x, y) in enumerate([(26, 26), (52, 26), (26, 52), (52, 52)]):
        sx, sy = 50 + (x - 50) * CORE_SCALE, 50 + (y - 50) * CORE_SCALE
        d.rounded_rectangle(R(sx, sy, sx + 22 * CORE_SCALE, sy + 22 * CORE_SCALE), radius=k(6 * CORE_SCALE), fill=rgb(LIT if i == 1 else CORE))

    out = img.resize((256, 256), Image.LANCZOS)
    out.save(os.path.join(RES, "logo.png"))
    out.save(os.path.join(RES, "mixbooster.ico"), sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("ok")


if __name__ == "__main__":
    main()
