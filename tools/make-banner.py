# Draws the README's banner: the logo, the name and what it does on the left, the PC components page on the right,
# running off the edge.
#   docs/banner.png
# python tools/make-banner.py  (needs Pillow; the fonts are Windows' Segoe UI)
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
DOCS = os.path.join(HERE, "..", "docs")
FONTS = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")

W, H = 1800, 760
SS = 2   # drawn at twice the size, then scaled down for smooth edges

BG = (14, 15, 16)
ACCENT = (0x6C, 0xCB, 0x7E)
TEXT = (245, 245, 247)
DIM = (160, 163, 168)


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size * SS)


def s(v):
    return int(round(v * SS))


def glow(size, box, color, alpha, blur):
    # a soft light: an ellipse blurred far past its edge
    layer = Image.new("RGBA", size, color + (0,))
    ImageDraw.Draw(layer).ellipse(box, fill=color + (alpha,))
    return layer.filter(ImageFilter.GaussianBlur(blur))


def rounded_mask(size, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, size[0] - 1, size[1] - 1), radius, fill=255)
    return m


def main():
    size = (s(W), s(H))
    img = Image.new("RGBA", size, BG + (255,))

    # a faint dot grid, and two soft green lights: behind the name and behind the window
    # (translucent shapes go on their own layer: drawn straight onto the image they would replace what is under them)
    dots = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(dots)
    step = s(28)
    for y in range(step // 2, size[1], step):
        for x in range(step // 2, size[0], step):
            d.ellipse((x - s(1), y - s(1), x + s(1), y + s(1)), fill=(255, 255, 255, 9))
    img.alpha_composite(dots)
    img.alpha_composite(glow(size, (s(-200), s(-260), s(700), s(520)), ACCENT, 34, s(140)))
    img.alpha_composite(glow(size, (s(900), s(120), s(1900), s(900)), ACCENT, 46, s(170)))

    # the app, scaled down, with rounded corners, a hairline edge and a shadow, cut off at the right and bottom
    shot = Image.open(os.path.join(DOCS, "screens", "components.png")).convert("RGBA")
    sw = s(1080)
    sh = int(shot.height * sw / shot.width)
    shot = shot.resize((sw, sh), Image.LANCZOS)
    rad = s(18)
    sx, sy = s(820), s(118)
    shadow = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((sx, sy + s(24), sx + sw, sy + sh + s(24)), rad, fill=(0, 0, 0, 170))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(s(36))))
    framed = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    framed.paste(shot, (0, 0), rounded_mask((sw, sh), rad))
    img.alpha_composite(framed, (sx, sy))
    edge = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle((sx, sy, sx + sw - 1, sy + sh - 1), rad, outline=(255, 255, 255, 40), width=s(1.5))
    img.alpha_composite(edge)

    # left: the logo, the name, the line under it and what it tunes
    d = ImageDraw.Draw(img)
    lx, ly = s(110), s(178)
    logo = Image.open(os.path.join(DOCS, "logo.png")).convert("RGBA").resize((s(104), s(104)), Image.LANCZOS)
    img.alpha_composite(logo, (lx, ly))
    d.text((lx, ly + s(136)), "MixBooster", font=font("seguisb.ttf", 92), fill=TEXT)
    d.text((lx + s(4), ly + s(262)), "PC tuning for gaming on Windows 10 and 11", font=font("segoeui.ttf", 30), fill=DIM)

    chips = ["GPU Affinity", "MSI Mode", "Core parking", "Game fixes"]
    cf = font("seguisb.ttf", 21)
    cx, cy, ch = lx, ly + s(330), s(44)
    for label in chips:
        tw = d.textlength(label, font=cf)
        w = int(tw) + s(48)
        pill = Image.new("RGBA", size, (0, 0, 0, 0))
        ImageDraw.Draw(pill).rounded_rectangle((cx, cy, cx + w, cy + ch), ch // 2, fill=(255, 255, 255, 14), outline=(255, 255, 255, 30), width=s(1))
        img.alpha_composite(pill)
        d = ImageDraw.Draw(img)
        dot = s(8)
        d.ellipse((cx + s(18), cy + (ch - dot) // 2, cx + s(18) + dot, cy + (ch + dot) // 2), fill=ACCENT)
        d.text((cx + s(34), cy + ch // 2), label, font=cf, fill=TEXT, anchor="lm")
        cx += w + s(12)

    img = img.resize((W, H), Image.LANCZOS)
    out = os.path.join(DOCS, "banner.png")
    img.convert("RGB").save(out, optimize=True)
    print(out)


if __name__ == "__main__":
    main()
