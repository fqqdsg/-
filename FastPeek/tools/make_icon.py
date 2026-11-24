"""Generates app.ico for FastPeek: a dark rounded tile with a photo glyph."""
from PIL import Image, ImageDraw

BASE = 512


def rounded(draw, box, r, fill):
    draw.rounded_rectangle(box, radius=r, fill=fill)


def make(size):
    s = BASE
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # tile
    rounded(d, (24, 24, s - 24, s - 24), 96, (36, 38, 43, 255))
    # inner "screen"
    inner = (96, 120, s - 96, s - 120)
    rounded(d, inner, 56, (16, 17, 20, 255))

    # sky band
    d.rounded_rectangle((inner[0] + 14, inner[1] + 14, inner[2] - 14, inner[3] - 14),
                        radius=44, fill=(28, 34, 46, 255))

    # sun
    cx, cy, r = 176, 196, 26
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 214, 102, 255))

    # mountains
    d.polygon([(104, 392), (222, 260), (300, 350), (352, 302), (408, 392)],
              fill=(78, 156, 255, 255))
    d.polygon([(104, 392), (200, 300), (268, 392)], fill=(52, 118, 200, 255))

    # play badge, bottom right
    bx, by, br = 372, 372, 66
    d.ellipse((bx - br, by - br, bx + br, by + br), fill=(255, 255, 255, 255))
    d.polygon([(bx - 18, by - 32), (bx - 18, by + 32), (bx + 34, by)],
              fill=(36, 38, 43, 255))

    return img.resize((size, size), Image.LANCZOS)


sizes = [256, 128, 64, 48, 32, 16]
imgs = [make(s) for s in sizes]
imgs[0].save("app.ico", format="ICO", sizes=[(s, s) for s in sizes])
print("wrote app.ico")
