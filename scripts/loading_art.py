# The default loading screens (loading/*.png): a round little bird on a soft gradient, and a line or two
# of text, for while the game shows nothing (no game, the HOME menu). Drawn here from shapes, so they're
# Chirp's own; the text is set with ffmpeg's drawtext (Segoe UI, Windows' own; or $FFMPEG_FONT).
#   python scripts/loading_art.py     (from the repository's root; then build.ps1 encodes them)
# Swap them for your own: any 1280x720 PNG or JPG in loading/.
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

W, H = 1280, 720


def png(path, px):
    rows = b"".join(b"\0" + bytes(px[y * W * 3:(y + 1) * W * 3]) for y in range(H))
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def hexc(s):
    return tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))


class Canvas:
    def __init__(self, top, bottom):
        t, b = hexc(top), hexc(bottom)
        self.px = bytearray(W * H * 3)
        for y in range(H):
            k = y / (H - 1)
            k = k * k * (3 - 2 * k)  # (eased: softer at the ends)
            c = bytes(int(t[i] + (b[i] - t[i]) * k + 0.5) for i in range(3))
            self.px[y * W * 3:(y + 1) * W * 3] = c * W

    def shape(self, sd, box, color, alpha=1.0):
        """Fill where sd(x, y) < 0 (a signed distance, in pixels: its edge antialiased), within box."""
        col = hexc(color)
        x0, y0, x1, y1 = (max(0, int(box[0])), max(0, int(box[1])), min(W, int(box[2]) + 1), min(H, int(box[3]) + 1))
        p = self.px
        for y in range(y0, y1):
            fy = y + 0.5
            for x in range(x0, x1):
                a = 0.5 - sd(x + 0.5, fy)
                if a <= 0:
                    continue
                a = (1.0 if a >= 1 else a) * alpha
                i = (y * W + x) * 3
                p[i] = int(p[i] + (col[0] - p[i]) * a + 0.5)
                p[i + 1] = int(p[i + 1] + (col[1] - p[i + 1]) * a + 0.5)
                p[i + 2] = int(p[i + 2] + (col[2] - p[i + 2]) * a + 0.5)

    def circle(self, cx, cy, r, color, alpha=1.0):
        self.shape(lambda x, y: math.hypot(x - cx, y - cy) - r, (cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1), color, alpha)

    def ellipse(self, cx, cy, rx, ry, color, angle=0.0, alpha=1.0):
        ca, sa, m = math.cos(angle), math.sin(angle), min(rx, ry)
        def sd(x, y):
            dx, dy = x - cx, y - cy
            u, v = (dx * ca + dy * sa) / rx, (-dx * sa + dy * ca) / ry
            return (math.hypot(u, v) - 1) * m
        r = max(rx, ry) + 1
        self.shape(sd, (cx - r, cy - r, cx + r, cy + r), color, alpha)

    def triangle(self, pts, color, alpha=1.0):
        (ax, ay), (bx, by), (cx, cy) = pts
        if (bx - ax) * (cy - ay) - (by - ay) * (cx - ax) < 0:
            (bx, by), (cx, cy) = (cx, cy), (bx, by)
        edges = []
        for (px, py), (qx, qy) in (((ax, ay), (bx, by)), ((bx, by), (cx, cy)), ((cx, cy), (ax, ay))):
            l = math.hypot(qx - px, qy - py)
            edges.append((px, py, (qy - py) / l, -(qx - px) / l))
        def sd(x, y):
            return max((x - px) * nx + (y - py) * ny for px, py, nx, ny in edges)
        xs, ys = (ax, bx, cx), (ay, by, cy)
        self.shape(sd, (min(xs) - 1, min(ys) - 1, max(xs) + 1, max(ys) + 1), color, alpha)

    def arc(self, cx, cy, r, width, a0, a1, color, alpha=1.0):
        """A rounded stroke along a circle from angle a0 to a1 (radians, clockwise on screen)."""
        mid, half = (a0 + a1) / 2, (a1 - a0) / 2
        ends = [(cx + r * math.cos(a), cy + r * math.sin(a)) for a in (a0, a1)]
        def sd(x, y):
            ang = math.atan2(y - cy, x - cx)
            d = (ang - mid + math.pi) % (2 * math.pi) - math.pi
            if abs(d) <= half:
                return abs(math.hypot(x - cx, y - cy) - r) - width / 2
            return min(math.hypot(x - ex, y - ey) for ex, ey in ends) - width / 2
        self.shape(sd, (cx - r - width, cy - r - width, cx + r + width, cy + r + width), color, alpha)


def bird(c, cx, cy, s, body, belly, wing, beak="#F6A436", tilt=0.0, chirp=True, eye_closed=False):
    """A round bird facing right, centred on cx, cy, about 2*s across."""
    c.ellipse(cx + 0.18 * s, cy + 1.02 * s, 0.85 * s, 0.13 * s, "#000000", alpha=0.10)  # (its shadow)
    c.triangle(((cx - 0.80 * s, cy + 0.05 * s), (cx - 1.30 * s, cy - 0.30 * s), (cx - 1.18 * s, cy + 0.32 * s)), body)  # tail
    c.circle(cx, cy, s, body)
    c.ellipse(cx + 0.22 * s, cy + 0.36 * s, 0.62 * s, 0.50 * s, belly)
    c.ellipse(cx - 0.28 * s, cy + 0.10 * s, 0.52 * s, 0.34 * s, wing, angle=-0.35 + tilt)
    ex, ey = cx + 0.42 * s, cy - 0.28 * s
    if eye_closed:
        c.arc(ex, ey - 0.02 * s, 0.10 * s, 0.045 * s, math.radians(20), math.radians(160), "#2B2B33")
    else:
        c.circle(ex, ey, 0.11 * s, "#2B2B33")
        c.circle(ex + 0.035 * s, ey - 0.04 * s, 0.035 * s, "#FFFFFF")
    c.circle(ex - 0.05 * s, ey + 0.22 * s, 0.09 * s, "#FF8FA3", alpha=0.55)  # (a blush)
    c.triangle(((cx + 0.86 * s, cy - 0.20 * s), (cx + 1.22 * s, cy - 0.06 * s), (cx + 0.86 * s, cy + 0.08 * s)), beak)
    c.ellipse(cx - 0.20 * s, cy + 1.00 * s, 0.07 * s, 0.05 * s, beak)  # feet
    c.ellipse(cx + 0.20 * s, cy + 1.00 * s, 0.07 * s, 0.05 * s, beak)
    if chirp:  # (three little lines in front of the beak: it's chirping)
        bx, by = cx + 1.30 * s, cy - 0.07 * s
        for k, r in enumerate((0.20, 0.36, 0.52)):
            c.arc(bx, by, r * s, 0.05 * s, math.radians(-40), math.radians(40), "#FFFFFF", alpha=0.85 - 0.2 * k)


def notes(c, pts, color, alpha=0.5):
    for x, y, s in pts:  # (a music note: a tilted head and a stem)
        c.ellipse(x, y, 0.55 * s, 0.40 * s, color, angle=-0.4, alpha=alpha)
        c.shape(lambda px, py, x=x, y=y, s=s: max(abs(px - (x + 0.45 * s)) - 0.08 * s, abs(py - (y - 0.9 * s)) - 0.9 * s),
                (x + 0.3 * s, y - 1.9 * s, x + 0.6 * s, y + 0.1 * s), color, alpha)


SCREENS = [
    # name, gradient, bird colours (body, belly, wing), what it says, decoration
    ("brb", ("#8EC5FC", "#E0F0FF"), ("#4A90E2", "#DDEEFF", "#2F6FBF"), "Be right back", "The game will be back in a moment", "notes"),
    ("home", ("#FFB88C", "#FFE6D1"), ("#F5C342", "#FFF4D0", "#D9A21B"), "On the HOME menu", "Picking something to play", "sun"),
    ("loading", ("#C3A6F0", "#F1E9FF"), ("#F07CA8", "#FFE3EE", "#C9507F"), "Loading...", "Hang tight", "sleepy"),
]


def draw(name, grad, cols, deco):
    c = Canvas(*grad)
    if deco == "sun":
        c.circle(1110, 130, 70, "#FFFFFF", alpha=0.35)
        c.circle(1110, 130, 48, "#FFFFFF", alpha=0.45)
    for x, y, r in ((150, 120, 46), (205, 105, 58), (265, 125, 42), (980, 560, 40), (1035, 545, 54), (1090, 565, 38)):
        c.circle(x, y, r, "#FFFFFF", alpha=0.55)  # (soft clouds)
    if deco == "notes":
        notes(c, ((530, 230, 26), (585, 175, 20)), "#FFFFFF", 0.7)
    # (the sleepy one's z z z are text: main())
    c.ellipse(310, 470, 290, 26, "#FFFFFF", alpha=0.35)  # (the ground it stands on)
    bird(c, 310, 340, 120, *cols, chirp=deco != "sleepy", eye_closed=deco == "sleepy")
    return c


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "loading")
    os.makedirs(out, exist_ok=True)
    ff = os.environ.get("FFMPEG") or shutil.which("ffmpeg")
    font = os.environ.get("FFMPEG_FONT", "C:/Windows/Fonts/segoeuib.ttf")
    font2 = os.environ.get("FFMPEG_FONT2", "C:/Windows/Fonts/segoeui.ttf")
    if not ff:
        sys.exit("ffmpeg isn't on the PATH (or set FFMPEG): it sets the text")
    esc = lambda p: p.replace("\\", "/").replace(":", "\\:")
    for name, grad, cols, title, sub, deco in SCREENS:
        c = draw(name, grad, cols, deco)
        tmp = os.path.join(tempfile.gettempdir(), "chirp_loading_%s.png" % name)
        png(tmp, c.px)
        # (the title and the line under it, from x 640, each kept inside the screen's right 60 px)
        text = (f"drawtext=fontfile='{esc(font)}':text='{title}':fontsize=62:fontcolor=0x2B2B33:x=640:y=284,"
                f"drawtext=fontfile='{esc(font2)}':text='{sub}':fontsize=34:fontcolor=0x2B2B33@0.75:x=643:y=376")
        if deco == "sleepy":  # (z z z, rising from the sleeping bird)
            for size, x, y, a in ((58, 470, 210, 0.9), (46, 525, 150, 0.75), (36, 570, 100, 0.6)):
                text += f",drawtext=fontfile='{esc(font)}':text='z':fontsize={size}:fontcolor=white@{a}:x={x}:y={y}"
        dst = os.path.join(out, name + ".png")
        subprocess.run([ff, "-hide_banner", "-loglevel", "error", "-y", "-i", tmp, "-vf", text, "-frames:v", "1", dst], check=True)
        os.remove(tmp)
        print("loading/%s.png" % name)


main()
