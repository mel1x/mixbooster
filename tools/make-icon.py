# Draws res/mixbooster.ico: a rounded blue tile with a lightning bolt, like the logo in the sidebar.
# python tools/make-icon.py  (needs Pillow)
import os
from PIL import Image, ImageDraw

S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
# vertical gradient tile
grad = Image.new("RGBA", (S, S))
top, bot = (76, 166, 255), (0, 95, 184)
for y in range(S):
    t = y / (S - 1)
    c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (255,)
    ImageDraw.Draw(grad).line([(0, y), (S, y)], fill=c)
mask = Image.new("L", (S, S), 0)
pad = 40
ImageDraw.Draw(mask).rounded_rectangle([pad, pad, S - pad, S - pad], radius=230, fill=255)
img.paste(grad, (0, 0), mask)
# soft highlight
hl = Image.new("L", (S, S), 0)
ImageDraw.Draw(hl).ellipse([150, -260, 1000, 520], fill=14)
white = Image.new("RGBA", (S, S), (255, 255, 255, 255))
hl_masked = Image.new("L", (S, S), 0)
hl_masked.paste(hl, (0, 0), mask)
img = Image.composite(white, img, hl_masked)
# the bolt
d = ImageDraw.Draw(img)
bolt = [(590, 170), (300, 560), (480, 560), (420, 860), (730, 440), (545, 440), (640, 170)]
d.polygon(bolt, fill=(255, 255, 255, 255))
out = os.path.join(os.path.dirname(__file__), "..", "res", "mixbooster.ico")
img.save(out, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
img.resize((256, 256), Image.LANCZOS).save(os.path.join(os.path.dirname(__file__), "..", "res", "logo.png"))
print("ok")
