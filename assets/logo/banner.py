"""Makes the Calx art: the crystal photo, dithered, with the Calx logo on top.

    python assets/logo/banner.py

Needs Pillow and numpy. Writes:

- calx-banner.png and calx-banner-dark.png, the README's banner, for light and dark themes
- calx-social.png, the card that link previews show (the website's og:image), 1200 by 630
- calx-tones.jpg, the tone map the website dithers live (see below)

The photo is reduced to a grid of 4-pixel cells and dithered with Atkinson's error diffusion,
which keeps highlights clean, in ink on paper, with the photo's warm veins as a third color by
ordered dithering. The dither fades out in an ellipse around the logo, which is drawn on top
from calx-logo.svg, with a knockout of paper around it so no dot touches it.

The website dithers the same crystal live, in the browser, with these steps and constants, so
the art and the site match. It starts from calx-tones.jpg, which holds this script's darkness and
warmth for the whole photo, so the site can take whatever crop and dot size each place needs.
"""

import functools
import math
import re
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
PHOTO = HERE / "banner-photo.jpg"
LOGO = HERE / "calx-logo.svg"

CELL = 4             # pixels per dither cell
TOP = 0.30           # where the photo's band starts, as a share of its height
GAMMA = 1.5          # above 1 lightens the midtones, so ink gathers on edges
DETAIL = 1.2         # local contrast, so crystal facets carry the ink

# (paper, ink, vein, logo)
LIGHT = ((247, 243, 236), (30, 31, 36), (214, 172, 112), (30, 31, 36))
DARK = ((13, 17, 23), (222, 216, 204), (196, 150, 88), (240, 246, 252))

# name: (width, height, logo width, rounded corners, colors). Corners are transparent outside.
# The social card is opaque and square, since link previews crop and round cards themselves.
OUTPUTS = {
    "calx-banner.png": (1600, 480, 640, 20, LIGHT),
    "calx-banner-dark.png": (1600, 480, 640, 20, DARK),
    "calx-social.png": (1200, 630, 560, 0, LIGHT),
}
TONE_MAP = "calx-tones.jpg"

BAYER = np.array([[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
                  [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
                  [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
                  [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]) / 64 + 1 / 128


# --- the logo, rasterized from its SVG -----------------------------------------------------------

def arc_points(x1, y1, rx, ry, phi, large, sweep, x2, y2):
    """Points along an SVG arc, by the endpoint-to-center conversion in SVG 1.1, F.6.5."""
    if rx == 0 or ry == 0:
        return [(x2, y2)]
    c, s = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    xp, yp = c * dx + s * dy, -s * dx + c * dy
    rx, ry = abs(rx), abs(ry)
    grow = (xp / rx) ** 2 + (yp / ry) ** 2
    if grow > 1:
        rx, ry = rx * math.sqrt(grow), ry * math.sqrt(grow)
    num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp
    k = math.sqrt(max(0.0, num / (rx * rx * yp * yp + ry * ry * xp * xp)))
    if large == sweep:
        k = -k
    cxp, cyp = k * rx * yp / ry, -k * ry * xp / rx
    cx, cy = c * cxp - s * cyp + (x1 + x2) / 2, s * cxp + c * cyp + (y1 + y2) / 2
    ux, uy = (xp - cxp) / rx, (yp - cyp) / ry
    vx, vy = (-xp - cxp) / rx, (-yp - cyp) / ry
    start = math.atan2(uy, ux)
    turn = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
    if not sweep and turn > 0:
        turn -= 2 * math.pi
    elif sweep and turn < 0:
        turn += 2 * math.pi
    n = max(8, int(abs(turn) * max(rx, ry) / 0.5))
    points = []
    for i in range(1, n + 1):
        t = start + turn * i / n
        ex, ey = rx * math.cos(t), ry * math.sin(t)
        points.append((c * ex - s * ey + cx, s * ex + c * ey + cy))
    return points


def path_polygons(d):
    """The closed polygons an SVG path's subpaths make. Handles M, L, H, V, A and Z."""
    tokens = re.findall(r"[MmLlHhVvAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", d)
    polygons, current, x, y, cmd, i = [], [], 0.0, 0.0, None, 0

    def num():
        nonlocal i
        i += 1
        return float(tokens[i - 1])

    while i < len(tokens):
        if tokens[i].isalpha():
            cmd = tokens[i]
            i += 1
            if cmd in "Zz":
                if current:
                    polygons.append(current)
                    x, y = current[0]
                current = []
                continue
        rel, op = cmd.islower(), cmd.upper()
        if op == "M":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            if current:
                polygons.append(current)
            current = [(x, y)]
            cmd = "l" if rel else "L"  # coordinates after a move are lines
        elif op == "L":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            current.append((x, y))
        elif op == "H":
            nx = num()
            x = x + nx if rel else nx
            current.append((x, y))
        elif op == "V":
            ny = num()
            y = y + ny if rel else ny
            current.append((x, y))
        elif op == "A":
            rx, ry, phi, large, sweep, nx, ny = (num() for _ in range(7))
            nx, ny = (x + nx, y + ny) if rel else (nx, ny)
            current.extend(arc_points(x, y, rx, ry, phi, int(large), int(sweep), nx, ny))
            x, y = nx, ny
        else:
            raise ValueError(f"path command {cmd} isn't handled")
    if current:
        polygons.append(current)
    return polygons


def rect_polygons(x, y, w, h, r):
    return path_polygons(
        f"M{x + r} {y} L{x + w - r} {y} A{r} {r} 0 0 1 {x + w} {y + r} L{x + w} {y + h - r} "
        f"A{r} {r} 0 0 1 {x + w - r} {y + h} L{x + r} {y + h} A{r} {r} 0 0 1 {x} {y + h - r} "
        f"L{x} {y + r} A{r} {r} 0 0 1 {x + r} {y} Z")


def fill(polygons, evenodd, width, height, scale, ox, oy):
    """Which pixel centers the polygons cover, by scanlines, with SVG's fill rules."""
    edges = []
    for polygon in polygons:
        points = [((px - ox) * scale, (py - oy) * scale) for px, py in polygon]
        for (ax, ay), (bx, by) in zip(points, points[1:] + points[:1]):
            if ay != by:
                edges.append((ax, ay, bx, by))
    ax, ay, bx, by = np.array(edges).T
    low, high = np.minimum(ay, by), np.maximum(ay, by)
    direction = np.where(by > ay, 1, -1)
    centers = np.arange(width) + 0.5
    mask = np.zeros((height, width), dtype=bool)
    for row in range(height):
        yc = row + 0.5
        hit = (low <= yc) & (yc < high)
        if not hit.any():
            continue
        cross = ax[hit] + (yc - ay[hit]) / (by[hit] - ay[hit]) * (bx[hit] - ax[hit])
        winding = np.zeros(width, dtype=int)
        for cx, d in zip(cross, direction[hit]):
            winding[centers > cx] += 1 if evenodd else d
        mask[row] = winding % 2 == 1 if evenodd else winding != 0
    return mask


@functools.cache
def logo_alpha(width, supersample=4):
    """The logo's coverage, 0 to 1, at `width` pixels wide."""
    root = ET.parse(LOGO).getroot()
    vx, vy, vw, vh = (float(v) for v in root.get("viewBox").split())
    scale = width * supersample / vw
    w, h = width * supersample, math.ceil(vh * scale / supersample) * supersample
    covered = np.zeros((h, w), dtype=bool)
    for el in root.iter():
        tag = el.tag.rsplit("}", 1)[-1]
        if tag == "path":
            polygons, evenodd = path_polygons(el.get("d")), el.get("fill-rule") == "evenodd"
        elif tag == "rect":
            x, y, rw, rh, r = (float(el.get(k, 0)) for k in ("x", "y", "width", "height", "rx"))
            polygons, evenodd = rect_polygons(x, y, rw, rh, r), False
        else:
            continue
        covered |= fill(polygons, evenodd, w, h, scale, vx, vy)
    return covered.reshape(h // supersample, supersample, width, supersample).mean(axis=(1, 3))


def knockout(alpha, pad):
    """The logo's outline with its counters filled, grown by `pad` with round corners."""
    m = Image.fromarray(((alpha > 0.05) * 255).astype(np.uint8))
    m = m.crop((-2 * pad, -2 * pad, m.width + 2 * pad, m.height + 2 * pad))
    ImageDraw.floodfill(m, (0, 0), 128)
    solid = Image.fromarray(((np.asarray(m) != 128) * 255).astype(np.uint8))
    return np.asarray(solid.filter(ImageFilter.GaussianBlur(pad / 1.6)), float) / 255 > 0.08


# --- the photo, dithered -------------------------------------------------------------------------

@functools.cache
def tones(width, height):
    """Each cell's darkness, for the ink, and warmth, for the veins, from 0 to 1.

    The image takes a band of the photo as wide as the photo, from TOP down, or as far down as
    the band can start and still fit."""
    photo = Image.open(PHOTO).convert("RGB")
    band = round(photo.width * height / width)
    assert band <= photo.height, "the image is taller, for its width, than the photo"
    y0 = min(round(photo.height * TOP), photo.height - band)
    cells = (-(-width // CELL), -(-height // CELL))
    return measure(photo.crop((0, y0, photo.width, y0 + band)).resize(cells, Image.LANCZOS), 6)


def measure(photo, blur):
    """Darkness and warmth from 0 to 1, for each pixel of photo. blur is in pixels; the banner's
    is 6 of its 400 cells across."""
    rgb = np.asarray(photo, float) / 255
    lum = rgb @ np.array([0.2126, 0.7152, 0.0722])
    blurred = Image.fromarray((lum * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(blur))
    detail = lum + DETAIL * (lum - np.asarray(blurred, float) / 255)
    lo, hi = np.percentile(detail, [0.5, 99.5])
    dark = (1 - np.clip((detail - lo) / (hi - lo), 0, 1)) ** GAMMA
    warm = rgb[..., 0] - rgb[..., 2]
    lo, hi = np.percentile(warm, [80, 99.7])
    return dark, np.clip((warm - lo) / (hi - lo), 0, 1)


def tone_map(width=480, quality=75):
    """The whole photo's darkness and warmth, for the website to dither live at any size: one
    grayscale JPEG, darkness (0 paper to 255 full ink) on top and warmth below."""
    photo = Image.open(PHOTO).convert("RGB")
    height = round(width * photo.height / photo.width)
    dark, warm = measure(photo.resize((width, height), Image.LANCZOS), 6 * width / 400)
    both = Image.fromarray((np.vstack([dark, warm]) * 255).round().astype(np.uint8), "L")
    both.save(HERE / TONE_MAP, quality=quality, optimize=True)
    print(f"wrote {TONE_MAP}")


def quiet(shape, logo_shape):
    """0 under the logo, rising to 1 in an ellipse around it."""
    h, w = shape
    yy, xx = np.mgrid[0:h, 0:w]
    lh, lw = logo_shape[0] / CELL, logo_shape[1] / CELL
    r = np.hypot((xx - w / 2) / (lw * 0.575), (yy - h / 2) / (lh * 0.92))
    return np.clip((r - 0.55) / 0.75, 0, 1) ** 1.5


def atkinson(tone):
    """Atkinson's dither: each cell passes 6/8 of its error on, so highlights stay clean."""
    h, w = tone.shape
    v = np.pad(tone, ((0, 2), (1, 2)))
    out = np.zeros((h, w), dtype=bool)
    for y in range(h):
        row, below, below2 = v[y], v[y + 1], v[y + 2]
        for x in range(1, w + 1):
            on = row[x] >= 0.5
            err = (row[x] - on) / 8
            out[y, x - 1] = on
            row[x + 1] += err
            row[x + 2] += err
            below[x - 1] += err
            below[x] += err
            below[x + 1] += err
            below2[x] += err
    return out


def render(width, height, logo_width, corner, paper, ink, vein, logo):
    alpha = logo_alpha(logo_width)
    dark, warm = tones(width, height)
    calm = quiet(dark.shape, alpha.shape)
    inked = atkinson(dark * calm)
    h, w = warm.shape
    veins = ~inked & (warm * calm * 0.9 > np.tile(BAYER, (h // 8 + 1, w // 8 + 1))[:h, :w])
    grow = lambda m: np.kron(m, np.ones((CELL, CELL), dtype=bool))[:height, :width]
    img = np.empty((height, width, 3))
    img[:] = paper
    img[grow(veins)] = vein
    img[grow(inked)] = ink

    pad = 10
    clear = knockout(alpha, pad)
    lh, lw = alpha.shape
    oy, ox = (height - lh) // 2, (width - lw) // 2
    ch, cw = clear.shape
    img[oy - 2 * pad:oy - 2 * pad + ch, ox - 2 * pad:ox - 2 * pad + cw][clear] = paper
    a = alpha[..., None]
    img[oy:oy + lh, ox:ox + lw] = img[oy:oy + lh, ox:ox + lw] * (1 - a) + np.array(logo, float) * a

    image = Image.fromarray(img.round().astype(np.uint8))
    if not corner:
        return image
    # Rounded corners, antialiased 4x.
    corners = Image.new("L", (width * 4, height * 4), 0)
    ImageDraw.Draw(corners).rounded_rectangle((0, 0, width * 4 - 1, height * 4 - 1), corner * 4, fill=255)
    rgba = image.convert("RGBA")
    rgba.putalpha(corners.resize((width, height), Image.LANCZOS))
    return rgba


def main():
    for name, (width, height, logo_width, corner, colors) in OUTPUTS.items():
        image = render(width, height, logo_width, corner, *colors)
        # Few colors, so a palette keeps the file small.
        image.quantize(64, method=Image.Quantize.FASTOCTREE).save(HERE / name, optimize=True)
        print(f"wrote {name}")
    tone_map()


if __name__ == "__main__":
    main()
