"""Builds a small demo folder (images + one video) used for screenshots."""
import math
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = os.path.join(os.path.dirname(__file__), "..", "demo")
os.makedirs(OUT, exist_ok=True)
os.makedirs(os.path.join(OUT, "2026 春游"), exist_ok=True)


def font(size):
    for name in ("msyh.ttc", "msyhbd.ttc", "simhei.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def grad(w, h, c1, c2):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        r = int(c1[0] + (c2[0] - c1[0]) * t)
        g = int(c1[1] + (c2[1] - c1[1]) * t)
        b = int(c1[2] + (c2[2] - c1[2]) * t)
        for x in range(w):
            px[x, y] = (r, g, b)
    return img


def scene(size, c1, c2, title, sub, accent):
    w, h = size
    img = grad(w, h, c1, c2).convert("RGBA")
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for i in range(5):
        r = int(min(w, h) * (0.16 + i * 0.09))
        cx = int(w * (0.18 + 0.16 * i))
        cy = int(h * (0.72 - 0.07 * i))
        a = 60 - i * 9
        d.ellipse((cx - r, cy - r, cx + r, cy + r),
                  fill=(accent[0], accent[1], accent[2], max(10, a)))
    layer = layer.filter(ImageFilter.GaussianBlur(radius=min(w, h) // 90))
    img = Image.alpha_composite(img, layer)

    d = ImageDraw.Draw(img)
    f1 = font(int(h * 0.11))
    f2 = font(int(h * 0.045))
    d.text((int(w * 0.06), int(h * 0.62)), title, font=f1, fill=(255, 255, 255, 235))
    d.text((int(w * 0.065), int(h * 0.78)), sub, font=f2, fill=(255, 255, 255, 180))
    d.rounded_rectangle((int(w * 0.06), int(h * 0.09), int(w * 0.06) + int(w * 0.16),
                         int(h * 0.09) + int(h * 0.055)),
                        radius=int(h * 0.014), fill=(255, 255, 255, 40))
    return img.convert("RGB")


def main():
    items = [
        ("01 晨雾.svg.png", (1600, 1000), (28, 42, 72), (92, 148, 199), "晨雾", "Misty morning · 06:20", (255, 255, 255)),
        ("02 山脊.png", (1200, 1600), (26, 30, 38), (92, 66, 48), "山脊", "Ridge line · ISO 100", (255, 196, 120)),
        ("03 湖面.png", (1600, 900), (12, 46, 52), (78, 160, 150), "湖面", "Still water · f/8", (180, 255, 240)),
        ("04 夜街.png", (1500, 1000), (18, 16, 34), (74, 40, 96), "夜街", "Night street · 1/15s", (255, 120, 200)),
    ]
    for name, size, c1, c2, title, sub, accent in items:
        scene(size, c1, c2, title, sub, accent).save(os.path.join(OUT, name), optimize=True)

    # a transparent png so the checkerboard path is exercised
    w = h = 900
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for i in range(12):
        a = 255 - i * 18
        d.ellipse((40 + i * 18, 40 + i * 18, w - 40 - i * 18, h - 40 - i * 18),
                  outline=(120, 200, 255, a), width=6)
    d.text((60, h - 130), "透明 PNG", font=font(80), fill=(255, 255, 255, 230))
    img.save(os.path.join(OUT, "05 透明.png"))

    # a jpeg with exif orientation 6 (rotated) to prove auto-rotate
    base = scene((1400, 933), (40, 24, 60), (140, 60, 110), "EXT", "orientation 6", (255, 200, 240))
    exif = base.getexif()
    exif[274] = 6
    base.save(os.path.join(OUT, "2026 春游", "06 竖拍.jpg"), quality=88, exif=exif)

    for i, name in enumerate(["07 露营.png", "08 长曝光.png"]):
        scene((1500, 1000), (20, 30, 26), (70, 110, 80), name[:2], "camp · long exposure", (200, 255, 200)).save(
            os.path.join(OUT, "2026 春游", name))

    ffmpeg = None
    try:
        import imageio_ffmpeg
        ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        pass
    if ffmpeg:
        clip = os.path.join(OUT, "09 演示片段.mp4")
        subprocess.run([
            ffmpeg, "-y", "-v", "error",
            "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=6",
            "-f", "lavfi", "-i", "sine=frequency=440:duration=6",
            "-c:v", "libx264", "-pix_fmt", "yuv420p", "-profile:v", "high", "-crf", "26",
            "-c:a", "aac", "-b:a", "96k", "-shortest", clip,
        ], check=False)
        print("video:", os.path.getsize(clip) if os.path.exists(clip) else "failed")

    print("demo folder ready:", os.path.abspath(OUT))


if __name__ == "__main__":
    main()
