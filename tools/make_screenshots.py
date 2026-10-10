"""Takes screenshots of WinShun's windows and turns them into images for the
README and for posts: the window on a soft background with a soft shadow under
it, at the size each place wants; and for the first image of a post a cover,
the window turned in perspective under a big title, with a few things by it.

  scene    [--theme light|dark] [--wallpaper NAME|photo.jpg]
           Sets Windows' app theme and the desktop wallpaper for the shots;
           the windows' Mica takes its tint from the wallpaper, so one of the
           backgrounds below makes the window match the image it goes on.
           What was there before is noted; `restore` puts it back.
  profile  FOLDER --files DEMO [--recent PATH ...]
           A folder for `WinShun.exe --profile FOLDER`: the settings of this
           user, but only DEMO indexed (every other folder excluded), these
           recent items and a clipboard history of its own, so no shot shows
           a file of one's own. Quit the running WinShun first.
  shoot    -o window.png [--delay SECONDS] [--winshun]
           The window in front (with WinShun's windows right by it, such as
           the bar under an Open dialog) cut out with its real transparency:
           a backdrop is put right behind it, black and then white, and a
           pixel the backdrop shows through by some fraction differs between
           the two shots by that fraction. Rounded corners and the border come
           out as they are, nothing is masked by hand. A third shot, black
           again, must equal the first, or the caret blinked or an animation
           was still running: then it shoots again. Run it elevated when
           WinShun is (the backdrop has to go between its window and the rest).
  matte    black.png white.png [black2.png] -o window.png
           The same from shots taken some other way.
  compose  window.png -o out.png [--preset readme|xhs|x|og|square]
           [--bg NAME|photo.jpg] [--title TEXT] [--subtitle TEXT]
           The window as it is: for the README, and for the pages of a post
           after the cover.
  cover    window.png -o out.png [--preset xhs|x|og|square] [--bg NAME|photo.jpg]
           [--title "LINE\\nLINE"] [--badge "BIG|small" ...] [--keys KEY ...]
           [--tilt YAW PITCH ROLL]
           The first image of a post: the title, its last line in a gradient
           with a sparkle, badges between laurel branches, keys floating by
           the window, and the window turned in 3D, running off the image.
  sheet    window.png -o sheet.png [--preset ...]
           The window on every background side by side, to choose one.

Windows' own shadow round a window comes out in the shot too, but cut off
where the shot ends; it is taken off again (`lifted`), and every window, key
and badge gets a shadow made here instead: three layers, each softer and
further down than the one before, tinted with the colour of the ground they
fall on, worked out on a canvas larger than the image, so that none is cut off.

The backgrounds are generated, so they are free to publish: soft blobs of
colour, "silk" (ribbons of light across black) and "letters" (the name in
huge pale letters on peach, in the colours of "dawn"). Posts get a little grain against banding once a
site recompresses them; the README not, as GitHub shows it as it is and grain
would triple the file. A photo of one's own works too. Text is set in the
bundled Alibaba PuHuiTi.

Usage: python tools/make_screenshots.py <command> ...   (requires Pillow and numpy)
"""
import argparse
import ctypes
import json
import math
import os
import pathlib
import shutil
import sys
import tempfile
import time
import winreg
from ctypes import wintypes as wt

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageGrab

ROOT = pathlib.Path(__file__).resolve().parent.parent
BOLD = ROOT / "resources/fonts/AlibabaPuHuiTi-3-85-Bold.ttf"
REGULAR = ROOT / "resources/fonts/AlibabaPuHuiTi-3-55-Regular.ttf"
# OPPO Sans 4.0 (one variable font, weights 100-700) where it is installed;
# the bundled Alibaba PuHuiTi otherwise.
OPPO = next((f for f in (pathlib.Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft/Windows/Fonts",
                         pathlib.Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts")
             for f in [f / "OPPO Sans 4.0.ttf"] if f.is_file()), None)

# Canvas size; None: the window at its own size with a margin round it.
PRESETS = {
    "readme": None,
    "xhs": (1440, 1920),  # Xiaohongshu, 3:4
    "x": (1600, 900),  # X, 16:9
    "og": (1280, 640),  # GitHub's social preview
    "square": (1440, 1440),
}

# Whether it is dark, the base colour, and blobs of colour over it: x and y
# as fractions of the width and height, the radius of the diagonal.
BACKGROUNDS = {
    "dawn": (False, "#F6D2BC", [(0.0, 0.0, 0.50, "#FFB07C"), (1.0, 0.05, 0.45, "#F5A3C7"),
                                (0.05, 1.0, 0.50, "#BBA8F4"), (1.0, 1.0, 0.45, "#FFD08A"),
                                (0.55, 0.45, 0.30, "#FFE8D8")]),
    "sky": (False, "#CFE3FA", [(0.0, 0.0, 0.50, "#8FC2FF"), (1.0, 0.0, 0.45, "#C7D2FF"),
                               (0.0, 1.0, 0.45, "#A8EBE0"), (1.0, 1.0, 0.50, "#B9C8FF"),
                               (0.5, 0.5, 0.30, "#EAF3FF")]),
    "mint": (False, "#DDF2E6", [(0.0, 0.0, 0.50, "#9BE3C0"), (1.0, 0.0, 0.45, "#BFE6F7"),
                                (0.0, 1.0, 0.45, "#E9F5B8"), (1.0, 1.0, 0.50, "#8FD8D0"),
                                (0.5, 0.5, 0.30, "#F2FBF5")]),
    "logo": (False, "#F3F4F8", [(0.0, 0.0, 0.42, "#FFB3AA"), (1.0, 0.0, 0.42, "#9FE3B5"),
                                (0.0, 1.0, 0.42, "#A9CBFF"), (1.0, 1.0, 0.42, "#FFDD94")]),
    "mist": (False, "#ECEFF4", [(0.0, 0.0, 0.50, "#D6E2F0"), (1.0, 0.1, 0.45, "#E6DDF2"),
                                (0.2, 1.0, 0.50, "#DCEDE6"), (1.0, 1.0, 0.40, "#F3E9DD")]),
    "aurora": (True, "#0D1524", [(0.0, 0.0, 0.50, "#1D6A74"), (1.0, 0.0, 0.45, "#3A2A78"),
                                 (0.0, 1.0, 0.45, "#123E6A"), (1.0, 1.0, 0.50, "#1C5A48")]),
    "dusk": (True, "#181329", [(0.0, 0.0, 0.50, "#65305E"), (1.0, 0.1, 0.45, "#2B3C78"),
                               (0.1, 1.0, 0.45, "#9A4A3E"), (1.0, 1.0, 0.50, "#3A2560")]),
    "graphite": (True, "#1A1C20", [(0.0, 0.0, 0.55, "#2E3743"), (1.0, 0.1, 0.45, "#352F40"),
                                   (0.2, 1.0, 0.50, "#24323A"), (1.0, 1.0, 0.40, "#3A3530")]),
}

# The ribbons of "silk": where the middle of each is at the left and at the
# right (fractions of the height), how far it bows down between, how far its
# light reaches (a fraction of the short side), towards which side (-1 up,
# 1 down; the other edge is sharp), and its colours from left to right.
RIBBONS = (
    (0.14, 0.02, -0.06, 0.11, -1, ("#38D3FF", "#3D74FF", "#6A4BFF", "#B57CFF")),
    (0.74, 0.90, 0.08, 0.13, 1, ("#6B3CFF", "#3F5CFF", "#38A6FF", "#A884FF")),
)

# The last line of a cover's title, left to right, on dark and light grounds.
GRADIENT = {True: ("#86DFFF", "#7C8DFF", "#B98BFF"), False: ("#FF7A45", "#F0436F", "#B83DB8")}

# What a cover says unless told otherwise (from the top of the README).
TITLE = "双击 Ctrl\n搜遍整台电脑"
BADGES = ("300 万文件|几毫秒搜完", "免费开源|MIT 许可")

# Shadows, three layers: how far each is blurred and drops, as fractions of
# how high the thing is lifted off the ground, and how dark it is on a light
# ground and on a dark one.
SHADOW = ((0.12, 0.05, 0.12, 0.28), (0.45, 0.25, 0.10, 0.28), (1.3, 0.9, 0.18, 0.40))


def rgb(colour):
    return np.array(Image.new("RGB", (1, 1), colour).getpixel((0, 0)), np.float32)


def grid(size):
    w, h = size
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return x + 0.5, y + 0.5


def ramp(colours, t):
    """The colours spread evenly over t from 0 to 1, blended in between."""
    stops = np.array([rgb(c) for c in colours])
    position = np.clip(t, 0, 1) * (len(colours) - 1)
    i = np.minimum(position.astype(int), len(colours) - 2)
    f = (position - i)[..., None]
    return stops[i] * (1 - f) + stops[i + 1] * f


def mesh(size, base, blobs):
    """The base colour with blobs of colour melting into it and each other."""
    w, h = size
    x, y = grid(size)
    diagonal = (w * w + h * h) ** 0.5
    weight = np.full((h, w), 0.35, np.float32)
    total = weight[..., None] * rgb(base)
    for bx, by, radius, colour in blobs:
        g = np.exp(-((x - bx * w) ** 2 + (y - by * h) ** 2) / (radius * diagonal) ** 2)
        total += g[..., None] * rgb(colour)
        weight += g
    return total / weight[..., None]


def silk(size):
    """Ribbons of light like silk across a ground of near black, one over the
    top and one low down, shading from cyan through blue to violet: each has
    a sharp bright edge, fine threads along it, and fades out on one side."""
    w, h = size
    x, y = grid(size)
    unit = min(w, h)
    u = x / w
    pixels = mesh(size, "#05060D", [(0.0, 0.05, 0.45, "#0A1838"), (1.0, 0.2, 0.40, "#1A1040"),
                                     (0.0, 0.95, 0.45, "#140C34"), (1.0, 1.0, 0.45, "#0C1638")])
    for left, right, bow, reach, side, colours in RIBBONS:
        middle = h * (left + (right - left) * u + bow * np.sin(np.pi * u))
        slope = h * (right - left + bow * np.pi * np.cos(np.pi * u)) / w
        d = side * (y - middle) / np.sqrt(1 + slope ** 2) / unit  # + towards where it fades
        width = reach * (0.8 + 0.3 * np.sin(2.3 * np.pi * u + 7 * left))
        body = np.exp(-np.maximum(d, 0) / width) / (1 + np.exp(np.clip(-d / 0.003, -50, 50)))
        threads = 0.82 + 0.18 * np.sin(d / 0.0045 + 5 * u)
        halo = 0.22 * np.exp(-(d / (0.3 * width)) ** 2)
        edge = 0.55 * np.exp(-(d / 0.002) ** 2)
        light = np.clip((body * threads + halo + edge)[..., None] * ramp(colours, u), 0, 255)
        pixels = 255 - (255 - pixels) * (1 - light / 255)
    return pixels


def letters(size, word="Win顺"):
    """Peach fading into cream downwards, the colours of "dawn" (so that a
    window shot on that matches), the name across it in huge letters a shade
    lighter than the ground, slanting up."""
    w, h = size
    _, y = grid(size)
    pixels = mesh(size, "#FBE2D4", [(0.0, 0.0, 0.50, "#FFC7A6"), (1.0, 0.1, 0.45, "#F9C4DC"),
                                     (0.5, 1.05, 0.55, "#FFF7F1")])
    font = face(BOLD, 0.34 * max(w, h * 0.75))
    pad = Image.new("L", (round(w * 1.7), round(h * 1.7)))
    draw = ImageDraw.Draw(pad)
    step = round(font.size * 0.95)
    for row, top in enumerate(range(-step, pad.height + step, step)):
        draw.text((-(row % 2) * font.size * 1.1, top), "  ".join([word] * 8), font=font, fill=255)
    pad = pad.rotate(10, resample=Image.BICUBIC)
    left, top = (pad.width - w) // 2, (pad.height - h) // 2
    mask = np.asarray(pad.crop((left, top, left + w, top + h)), np.float32) / 255
    mask *= 1 - 0.6 * y / h
    shade = offset(smooth(mask, 0.008 * w), 0.004 * w, 0.007 * w)
    pixels = pixels * (1 - 0.04 * shade[..., None])
    return pixels * (1 - 0.26 * mask[..., None]) + 255 * 0.26 * mask[..., None]


GENERATED = {"silk": (True, silk), "letters": (False, letters)}
NAMES = (*BACKGROUNDS, *GENERATED)


def photo(path, size):
    """The photo cropped to fill the canvas."""
    image = Image.open(path).convert("RGB")
    w, h = size
    scale = max(w / image.width, h / image.height)
    image = image.resize((round(image.width * scale), round(image.height * scale)), Image.LANCZOS)
    left, top = (image.width - w) // 2, (image.height - h) // 2
    return np.asarray(image.crop((left, top, left + w, top + h)), np.float32)


def background(name, size):
    """The canvas as floats, and whether it is dark."""
    if name in GENERATED:
        dark, make = GENERATED[name]
        return make(size), dark
    if name in BACKGROUNDS:
        dark, base, blobs = BACKGROUNDS[name]
        return mesh(size, base, blobs), dark
    pixels = photo(name, size)
    return pixels, float((pixels @ np.array([0.2126, 0.7152, 0.0722], np.float32)).mean()) < 110


def rounded(size, box, radius):
    """How much of each pixel a rounded rectangle (left, top, right, bottom)
    covers, from its distance to the edge: smooth edges, at any fraction of a
    pixel."""
    x, y = grid(size)
    left, top, right, bottom = box
    qx = np.abs(x - (left + right) / 2) - ((right - left) / 2 - radius)
    qy = np.abs(y - (top + bottom) / 2) - ((bottom - top) / 2 - radius)
    distance = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - radius
    return np.clip(0.5 - distance, 0, 1)


def smooth(values, sigma):
    """A Gaussian blur of a float array (three box blurs along each axis),
    what lies beyond its edges taken as 0."""
    if sigma < 0.5:
        return values
    r = max(1, round(((4 * sigma * sigma + 1) ** 0.5 - 1) / 2))
    out = values.astype(np.float64)
    for axis in (0, 1):
        n = out.shape[axis]
        pad = [(0, 0)] * out.ndim
        pad[axis] = (r + 1, r)
        for _ in range(3):
            total = np.cumsum(np.pad(out, pad), axis=axis)
            out = (np.take(total, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis)
                   - np.take(total, np.arange(n), axis=axis)) / (2 * r + 1)
    return out.astype(np.float32)


def offset(values, dx, dy):
    """The array moved right by dx and down by dy, 0 coming in."""
    dx, dy = round(dx), round(dy)
    h, w = values.shape[:2]
    out = np.zeros_like(values)
    out[max(dy, 0):h + min(dy, 0), max(dx, 0):w + min(dx, 0)] = \
        values[max(-dy, 0):h - max(dy, 0), max(-dx, 0):w - max(dx, 0)]
    return out


def dilated(mask, r):
    """The mask grown by r pixels (a square)."""
    for axis in (0, 1):
        n = mask.shape[axis]
        pad = [(0, 0), (0, 0)]
        pad[axis] = (r, r)
        padded = np.pad(mask, pad)
        grown = np.zeros_like(mask)
        for k in range(2 * r + 1):
            grown |= np.take(padded, np.arange(k, k + n), axis=axis)
        mask = grown
    return mask


def filled(values, known):
    """`values` where they are known, spread in from round about elsewhere:
    blurred known values over the blurred share of known pixels, from near to
    ever further."""
    weight = known.astype(np.float32)
    seen = np.where(known, values, 0).astype(np.float32)
    out, done = seen.copy(), known.copy()
    for sigma in (1.5, 4, 10, 25, 60):
        share = smooth(weight, sigma)
        new = ~done & (share > 0.02)
        out[new] = smooth(seen, sigma)[new] / share[new]
        done |= new
    return out


def window_box(alpha):
    """Where the window itself is: the rows and columns mostly opaque, not its
    shadow nor the logo peeking out from behind it."""
    opaque = alpha >= 0.9
    ys, xs = np.nonzero(opaque.mean(axis=1) > 0.5)[0], np.nonzero(opaque.mean(axis=0) > 0.5)[0]
    return int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1


def logo_boxes(alpha, box):
    """Where something sticks out well beyond a side of the window (the logo
    peeking out from behind it): from the edge of the image to where the
    window, or the bar beside it, starts."""
    h, w = alpha.shape
    left, top, right, bottom = box
    opaque = alpha >= 0.6
    found = []
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        # The side as lines of pixels going out from the window: depth, along.
        if dy:
            lines = opaque[:top][::-1] if dy < 0 else opaque[bottom:]
        else:
            lines = (opaque[top:bottom, :left][:, ::-1] if dx < 0 else opaque[top:bottom, right:]).T
        far = np.nonzero(lines[4:].any(axis=0))[0]  # not just the edge of a wider window beside
        if far.size < 8:
            continue
        a0, a1 = max(int(far.min()) - 2, 0), min(int(far.max()) + 3, lines.shape[1])

        def reach(i):  # how far the window goes beyond the side, on line i
            if not 0 <= i < lines.shape[1]:
                return 0
            gaps = np.nonzero(~lines[:, i])[0]
            return int(gaps[0]) if gaps.size else lines.shape[0]

        cut = max(reach(a0 - 2), reach(a1 + 1))
        if dy:
            found.append((a0, 0, a1, top - cut) if dy < 0 else (a0, bottom + cut, a1, h))
        else:
            found.append((0, top + a0, left - cut, top + a1) if dx < 0 else (right + cut, top + a0, w, top + a1))
    return found


def lifted(window):
    """The windows without Windows' own shadow round them, cropped, and where
    the window is in that. The shadow is black of some alpha s. A window
    (alpha l, colour L) over it shows as alpha a = s + (1 - s) l and colour
    C = l L / a; the logo peeking out from behind the window is under it
    instead, and shows as the same alpha and C = (1 - s) l L / a. Clear of
    the windows a is s; under their edges, and under the logo (part
    see-through), s is filled in from round about."""
    pixels = np.asarray(window.convert("RGBA"), np.float32)
    a = pixels[..., 3] / 255
    box = window_box(a)
    known = ~dilated(a >= 0.6, 2)  # the shadow is up to 0.5 by a dark window; a gap may be 6 px
    under = np.zeros_like(known)
    for x0, y0, x1, y1 in logo_boxes(a, box):
        known[max(y0 - 3, 0):y1 + 3, max(x0 - 3, 0):x1 + 3] = False
        under[y0:y1, x0:x1] = True
    s = np.minimum(filled(a, known), 0.95)
    l = np.clip((a - s) / (1 - s), 0, 1)
    shown = np.where(under, (1 - s) * l, l)  # how much of L is in C
    colour = np.clip(pixels[..., :3] * (a / np.maximum(shown, 1e-3))[..., None], 0, 255)
    l[l < 0.02] = 0
    ys, xs = np.nonzero(l)
    top, left = ys.min(), xs.min()
    out = np.dstack([colour, l * 255])[top:ys.max() + 1, left:xs.max() + 1]
    image = Image.fromarray(np.round(out).astype(np.uint8), "RGBA")
    return image, (box[0] - left, box[1] - top, box[2] - left, box[3] - top)


# A sprite: premultiplied colour (0-255) and alpha (0-1), as float arrays.

def sprite(image):
    pixels = np.asarray(image.convert("RGBa"), np.float32)
    return pixels[..., :3], pixels[..., 3] / 255


def spots(canvas_shape, piece_shape, x, y):
    """Where a piece with its top left at (x, y) overlaps the canvas: slices
    into the canvas and into the piece, or None."""
    h, w = canvas_shape[:2]
    ph, pw = piece_shape[:2]
    x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + pw, w), min(y + ph, h)
    if x0 >= x1 or y0 >= y1:
        return None
    return (slice(y0, y1), slice(x0, x1)), (slice(y0 - y, y1 - y), slice(x0 - x, x1 - x))


def over(colour, alpha, piece, x, y):
    """Lays a sprite over another (alpha None: an opaque canvas), in place."""
    found = spots(colour.shape, piece[1].shape, round(x), round(y))
    if found:
        to, of = found
        a = piece[1][of]
        colour[to] = colour[to] * (1 - a[..., None]) + piece[0][of]
        if alpha is not None:
            alpha[to] = alpha[to] * (1 - a) + a


def mirrored(piece):
    return piece[0][:, ::-1], piece[1][:, ::-1]


def put(pixels, piece, at, lift=0.0, dark=False, glow=None):
    """Lays a sprite on the canvas with its top left at `at` (anywhere, partly
    off the canvas too) over its shadow: SHADOW's layers for something `lift`
    px above the ground, worked out on a canvas larger by their reach, so
    that whatever runs off the image takes its shadow with it. `glow`: a
    colour it lights the ground with round it. Returns the new canvas."""
    h, w = pixels.shape[:2]
    x, y = round(at[0]), round(at[1])
    if lift > 0:
        m = math.ceil(4.2 * lift) + 2
        cover = np.zeros((h + 2 * m, w + 2 * m), np.float32)
        found = spots(cover.shape, piece[1].shape, x + m, y + m)
        if found:
            cover[found[0]] = piece[1][found[1]]
        if glow is not None:
            light = np.clip(smooth(cover, 1.5 * lift)[m:m + h, m:m + w, None] * rgb(glow) * 0.6, 0, 255)
            pixels = 255 - (255 - pixels) * (1 - light / 255)
        keep = np.ones((h, w), np.float32)
        for spread, drop, light_ground, dark_ground in SHADOW:
            layer = smooth(cover, spread * lift)
            d = round(drop * lift)
            keep *= 1 - (dark_ground if dark else light_ground) * layer[m - d:m - d + h, m:m + w]
        shade = (1 - keep)[..., None]
        pixels = pixels * (1 - shade) + pixels * (pixels / 255) * 0.42 * shade
    over(pixels, None, piece, x, y)
    return pixels


def flat(window, box, width):
    """The window scaled so that its body is `width` px wide, and where the
    body's centre is in it."""
    scale = width / (box[2] - box[0])
    if abs(scale - 1) > 1e-3:
        window = window.convert("RGBa").resize((round(window.width * scale), round(window.height * scale)),
                                               Image.LANCZOS)
    return sprite(window), ((box[0] + box[2]) / 2 * scale, (box[1] + box[3]) / 2 * scale)


def homography(source, target):
    """The coefficients of the perspective map taking the four source points
    to the four target ones, the way PIL takes them."""
    rows, values = [], []
    for (x, y), (u, v) in zip(source, target):
        rows += [[x, y, 1, 0, 0, 0, -u * x, -u * y], [0, 0, 0, x, y, 1, -v * x, -v * y]]
        values += [u, v]
    return tuple(np.linalg.solve(np.array(rows, float), np.array(values, float)))


def tilted(window, box, width, tilt, focal, k=2):
    """The window turned in 3D (degrees about the upright axis, the level one
    and the line of sight) and seen in perspective from `focal` px away, its
    body `width` px wide. Drawn at k times the size and scaled down, so that
    the turned text stays smooth. Returns the sprite, where the body's centre
    is in it, and the body's corners from there."""
    yaw, pitch, roll = np.radians(tilt)
    turn = (np.array([[np.cos(yaw), 0, np.sin(yaw)], [0, 1, 0], [-np.sin(yaw), 0, np.cos(yaw)]])
            @ np.array([[1, 0, 0], [0, np.cos(pitch), -np.sin(pitch)], [0, np.sin(pitch), np.cos(pitch)]])
            @ np.array([[np.cos(roll), -np.sin(roll), 0], [np.sin(roll), np.cos(roll), 0], [0, 0, 1]]))
    scale = width / (box[2] - box[0])
    centre = ((box[0] + box[2]) / 2, (box[1] + box[3]) / 2)

    def seen(u, v):
        x, y, z = turn @ np.array([(u - centre[0]) * scale, (v - centre[1]) * scale, 0])
        return focal * x / (focal + z), focal * y / (focal + z)

    corners = [(0, 0), (window.width, 0), (window.width, window.height), (0, window.height)]
    points = [seen(u, v) for u, v in corners]
    left, top = math.floor(min(p[0] for p in points)) - 1, math.floor(min(p[1] for p in points)) - 1
    right, bottom = math.ceil(max(p[0] for p in points)) + 1, math.ceil(max(p[1] for p in points)) + 1
    target = [((x - left) * k, (y - top) * k) for x, y in points]
    image = window.convert("RGBa").transform(((right - left) * k, (bottom - top) * k), Image.PERSPECTIVE,
                                             homography(target, corners), Image.BICUBIC).reduce(k)
    body = [seen(u, v) for u, v in ((box[0], box[1]), (box[2], box[1]), (box[2], box[3]), (box[0], box[3]))]
    return sprite(image), (-left, -top), body


def face(path, size):
    """A font of the given size: BOLD or REGULAR, in OPPO Sans if there."""
    if OPPO is None:
        return ImageFont.truetype(str(path), max(1, round(size)))
    font = ImageFont.truetype(str(OPPO), max(1, round(size)))
    font.set_variation_by_axes([700 if path == BOLD else 400])
    return font


def text_piece(text, font, fill):
    """Text as a sprite, its ink from (2, 2), filled with a colour or with a
    gradient (a tuple of colours, left to right)."""
    left, top, right, bottom = font.getbbox(text)
    mask = Image.new("L", (right - left + 4, bottom - top + 4))
    ImageDraw.Draw(mask).text((2 - left, 2 - top), text, font=font, fill=255)
    alpha = np.asarray(mask, np.float32) / 255
    if isinstance(fill, tuple):
        x, _ = grid(mask.size)
        colour = ramp(fill, (x - 2) / max(right - left, 1))
    else:
        colour = np.broadcast_to(rgb(fill), alpha.shape + (3,))
    return colour * alpha[..., None], alpha


def sparkle(radius, colour="#FFFFFF"):
    """A four-pointed star with curved sides, a soft glow round it; its centre
    in the middle of the sprite."""
    size, k = math.ceil(radius * 3.2), 4
    t = np.linspace(0, 2 * np.pi, 720, endpoint=False)
    c, s = np.cos(t), np.sin(t)
    xs = size * k / 2 + radius * k * 0.9 * np.sign(c) * np.abs(c) ** 2.6
    ys = size * k / 2 + radius * k * 1.1 * np.sign(s) * np.abs(s) ** 2.6
    mask = Image.new("L", (size * k, size * k))
    ImageDraw.Draw(mask).polygon(list(zip(xs, ys)), fill=255)
    star = np.asarray(mask.reduce(k), np.float32) / 255
    alpha = 1 - (1 - star) * (1 - 0.55 * smooth(star, 0.3 * radius))
    return alpha[..., None] * rgb(colour), alpha


def keycap(text, height, dark):
    """A key of a keyboard seen from the front and a little above: its top
    face rising from the sides, lit from above, the name on it."""
    font = face(BOLD, height * 0.3)
    width = max(round(height * 1.3), round(font.getlength(text) + height * 0.75))
    size = (width, height)
    _, y = grid(size)
    radius = height * 0.22
    body = rounded(size, (0, 0, width, height), radius)
    inset = height * 0.075
    top_box = (inset, inset * 0.55, width - inset, height * 0.85)
    top = rounded(size, top_box, radius * 0.8)[..., None]
    across = (y - top_box[1]) / (top_box[3] - top_box[1])
    if dark:
        side, faces, ink, gleam = ramp(("#2B303D", "#111218"), y / height), ramp(("#454C60", "#2D3240"), across), \
            "#F2F4FA", 0.25
    else:
        side, faces, ink, gleam = ramp(("#DDE1E9", "#A8AFBD"), y / height), ramp(("#FFFFFF", "#ECEFF4"), across), \
            "#3C4252", 0.9
    colour = side * (1 - top) + faces * top
    edge = np.clip(top[..., 0] - rounded(size, (top_box[0], top_box[1] + max(1.5, height * 0.014), top_box[2],
                                                top_box[3]), radius * 0.8), 0, 1)[..., None]
    colour = colour * (1 - gleam * edge) + 255 * gleam * edge
    left, t, right, b = font.getbbox(text)
    mask = Image.new("L", size)
    middle = ((top_box[0] + top_box[2]) / 2, (top_box[1] + top_box[3]) / 2)
    ImageDraw.Draw(mask).text((middle[0] - (left + right) / 2, middle[1] - (t + b) / 2), text, font=font, fill=255)
    ink_cover = np.asarray(mask, np.float32)[..., None] / 255
    colour = colour * (1 - ink_cover) + rgb(ink) * ink_cover
    return Image.fromarray(np.round(np.dstack([np.clip(colour, 0, 255), body * 255])).astype(np.uint8), "RGBA")


def turned(image, degrees):
    """An image turned about its centre (counterclockwise), as a sprite."""
    return sprite(image.convert("RGBa").rotate(degrees, resample=Image.BICUBIC, expand=True))


def laurel(height, colour):
    """A laurel branch curving up the left of a badge `height` px high, as a
    sprite."""
    k = 4
    big = height * k
    cx, cy, radius = 0.62 * big, 0.5 * big, 0.42 * big
    shapes = []

    def leaf(base, direction, length, width):
        d = direction / np.linalg.norm(direction)
        across = np.array([-d[1], d[0]])
        s = np.linspace(0, 1, 32)
        half = width / 2 * np.sin(np.pi * s) ** 0.8 * (1 - 0.25 * s)
        spine = base + np.outer(s * length, d)
        return np.vstack([spine + np.outer(half, across), (spine - np.outer(half, across))[::-1]])

    def at(angle):  # a point on the arc, and the way up along it and out from it
        return (np.array([cx + radius * np.cos(angle), cy - radius * np.sin(angle)]),
                np.array([np.sin(angle), np.cos(angle)]), np.array([np.cos(angle), -np.sin(angle)]))

    angles = np.radians(np.linspace(246, 128, 7))
    for i, angle in enumerate(angles):
        point, up, out = at(angle)
        grow = 1 - 0.2 * i / (len(angles) - 1)
        shapes.append(leaf(point, up * np.cos(0.72) + out * np.sin(0.72), 0.23 * big * grow, 0.1 * big * grow))
        if 0 < i:
            shapes.append(leaf(point, up * np.cos(0.6) - out * np.sin(0.6), 0.15 * big * grow, 0.075 * big * grow))
    point, up, _ = at(np.radians(122))
    shapes.append(leaf(point, up, 0.17 * big, 0.08 * big))
    stem = [at(a)[0] for a in np.radians(np.linspace(252, 122, 40))]
    points = np.vstack(shapes + [np.array(stem)])
    shift = -points.min(axis=0) + 2 * k
    w = math.ceil((points[:, 0].max() + shift[0] + 2 * k) / k) * k
    mask = Image.new("L", (w, math.ceil(big / k) * k))
    draw = ImageDraw.Draw(mask)
    draw.line([tuple(p + shift) for p in stem], fill=255, width=max(1, round(0.024 * big)), joint="curve")
    for shape in shapes:
        draw.polygon([tuple(p + shift) for p in shape], fill=255)
    alpha = np.asarray(mask.reduce(k), np.float32) / 255
    if isinstance(colour, tuple):  # top to bottom
        return alpha[..., None] * ramp(colour, grid(alpha.shape[::-1])[1] / alpha.shape[0]), alpha
    return alpha[..., None] * rgb(colour), alpha


def badge(big, small, height, colour, leaves):
    """Two lines of text between laurel branches (`leaves`: their colours,
    top to bottom), as a sprite."""
    branch = laurel(height, leaves)
    texts = [text_piece(big, face(BOLD, height * 0.3), colour)]
    if small:
        texts.append(text_piece(small, face(BOLD, height * 0.17), colour))
    gap = round(height * 0.04)
    inner = max(t[1].shape[1] for t in texts)
    bw = branch[1].shape[1]
    w = 2 * bw + 2 * gap + inner
    colours, alpha = np.zeros((height, w, 3), np.float32), np.zeros((height, w), np.float32)
    over(colours, alpha, branch, 0, 0)
    over(colours, alpha, mirrored(branch), w - bw, 0)
    spacing = round(height * 0.05)
    block = sum(t[1].shape[0] for t in texts) + spacing * (len(texts) - 1)
    y = (height - block) / 2 - height * 0.02
    for t in texts:
        over(colours, alpha, t, (w - t[1].shape[1]) / 2, y)
        y += t[1].shape[0] + spacing
    return colours, alpha


def maker(height, dark):
    """LingCore's logo with its name beside it, as a sprite."""
    logo = sprite(Image.open(ROOT / "resources/lingcore.png").convert("RGBA").resize(
        (round(height), round(height)), Image.LANCZOS))
    name = text_piece("LingCore", face(BOLD, height * 0.5), "#F4F5F8" if dark else "#15171D")
    gap = round(height * 0.22)
    w = round(height) + gap + name[1].shape[1]
    colours, alpha = np.zeros((round(height), w, 3), np.float32), np.zeros((round(height), w), np.float32)
    over(colours, alpha, logo, 0, 0)
    over(colours, alpha, name, round(height) + gap, (height - name[1].shape[0]) / 2)
    return colours, alpha


def mark(height, dark):
    """The logo with the name beside it, as a sprite."""
    sys.path.insert(0, str(ROOT / "tools"))
    import make_icon
    logo = sprite(make_icon.render(round(height)))
    name = text_piece("Win顺", face(BOLD, height * 0.6), "#F4F5F8" if dark else "#15171D")
    gap = round(height * 0.22)
    w = round(height) + gap + name[1].shape[1]
    colours, alpha = np.zeros((round(height), w, 3), np.float32), np.zeros((round(height), w), np.float32)
    over(colours, alpha, logo, 0, 0)
    over(colours, alpha, name, round(height) + gap, (height - name[1].shape[0]) / 2)
    return colours, alpha


def title_block(canvas, top, title, subtitle, ink, soft, em, accent=None):
    """Draws the title and subtitle centred from `top` down, the title `em`
    px high, what is in [brackets] in it in the `accent` gradient; returns
    the y under them."""
    draw = ImageDraw.Draw(canvas)
    y = top
    for text, path, size, colour, gap in ((title, BOLD, 1.0, ink, 0.32), (subtitle, REGULAR, 0.48, soft, 0.0)):
        if not text:
            continue
        font = face(path, em * size)
        for line in text.replace("\\n", "\n").split("\n"):
            runs = [(run, i % 2 == 1) for i, run in enumerate(line.replace("]", "[").split("["))]
            left, line_top, right, _ = draw.textbbox((0, 0), "".join(run for run, _ in runs), font=font)
            x, base = (canvas.width - (right - left)) / 2 - left, y - line_top
            for run, accented in runs:
                if accented and accent:
                    mask = Image.new("L", canvas.size)
                    ImageDraw.Draw(mask).text((x, base), run, font=font, fill=255)
                    across, _ = grid(canvas.size)
                    paint = ramp(accent, (across - x) / max(font.getlength(run), 1))
                    canvas.paste(Image.fromarray(np.round(paint).astype(np.uint8)), mask=mask)
                elif run:
                    draw.text((x, base), run, font=font, fill=colour)
                x += font.getlength(run)
            y += round(font.size * 1.25)
        y += round(font.size * gap)
    return y


def grain(pixels, seed):
    return pixels + np.random.default_rng(seed).normal(0, 1.4, pixels.shape[:2] + (1,)).astype(np.float32)


def compose(window, preset="readme", bg="sky", title=None, subtitle=None, seed=1):
    window, box = lifted(window)
    body_w, body_h = box[2] - box[0], box[3] - box[1]
    size = PRESETS[preset]
    titled = bool(title or subtitle) and size is not None
    if size is None:
        margin = round(0.09 * body_w)
        size = (body_w + 2 * margin, body_h + 2 * margin)
        width, middle, title_top = body_w, (size[0] / 2, size[1] / 2), 0
    else:
        # The title near the top, under the logo and name, and the window as
        # large as fits under it (a little larger than shot at most), in the
        # middle of the room left.
        w, h = size
        em = 0.075 * min(w, 1.1 * h)
        title_top = round(0.145 * h)
        title_h = title_block(Image.new("RGB", size), 0, title, subtitle, 0, 0, em) \
            if titled else 0
        gap = 0.045 * h if titled else 0
        room = (w - 0.12 * w, h - title_top - title_h - gap - 0.06 * h)
        width = min(room[0], room[1] * body_w / body_h, 1.2 * body_w)
        spare = room[1] - width * body_h / body_w
        middle = (w / 2, title_top + title_h + gap + 0.5 * spare + width * body_h / body_w / 2)

    pixels, dark = background(bg, size)
    piece, centre = flat(window, box, width)
    pixels = put(pixels, piece, (middle[0] - centre[0], middle[1] - centre[1]))  # no shadow under the program
    if preset != "readme":
        pixels = put(pixels, mark(0.06 * min(size), dark), (0.06 * min(size), 0.055 * min(size)))
        made = maker(0.06 * min(size), dark)
        pixels = put(pixels, made, (size[0] - 0.06 * min(size) - made[1].shape[1], 0.055 * min(size)))
        pixels = grain(pixels, seed)
    canvas = Image.fromarray(np.clip(np.round(pixels), 0, 255).astype(np.uint8))
    if titled:
        ink, soft = ("#F5F5F7", "#C9CBD3") if dark else ("#1B1B1F", "#4A4D57")
        title_block(canvas, title_top, title, subtitle, ink, soft, em, GRADIENT[dark])
    if preset == "readme":
        corners = rounded(size, (0, 0, size[0], size[1]), 0.022 * size[0])
        canvas.putalpha(Image.fromarray(np.round(corners * 255).astype(np.uint8)))
    return canvas


def cover(window, preset="xhs", bg="silk", title=TITLE, badges=BADGES, keys=("Ctrl", "Ctrl"), tilt=None, seed=1):
    """The first image of a post. Upright images: the name at the top left,
    the title centred under it, the badges in a row, and the window turned
    below them, running off the bottom; wide ones: the title and badges on
    the left, the window on the right running off the edges."""
    size = PRESETS[preset] or PRESETS["xhs"]
    w, h = size
    pixels, dark = background(bg, size)
    window, box = lifted(window)
    ink = "#F5F6FA" if dark else "#12141A"
    lines = title.replace("\\n", "\n").split("\n")
    upright = h >= w
    u = w / 1440 if upright else h / 900
    v = u * min(1.0, 0.75 * h / w) if upright else u  # upright but less tall (square): tighter down the page

    corner = (84 * u, 80 * u) if upright else (64 * u, 60 * u)
    pixels = put(pixels, mark((88 if upright else 72) * u, dark), corner)
    made = maker((88 if upright else 72) * u, dark)
    pixels = put(pixels, made, (w - corner[0] - made[1].shape[1], corner[1]))

    # The title, its last line in the gradient with a sparkle at its end.
    size_t = 132 * v if upright else 92 * u
    pitch = size_t * 1.3
    font = face(BOLD, size_t)
    pieces = [text_piece(line, font, GRADIENT[dark] if i == len(lines) - 1 else ink) for i, line in enumerate(lines)]
    if not upright:
        widest = max(p[1].shape[1] for p in pieces)
        if widest > 0.48 * w:  # keep it to the left half
            font = face(BOLD, size_t * 0.48 * w / widest)
            pitch *= 0.48 * w / widest
            pieces = [text_piece(line, font, GRADIENT[dark] if i == len(lines) - 1 else ink)
                      for i, line in enumerate(lines)]
    y = 236 * v if upright else 210 * u
    x_text = 96 * u
    last = None
    for p in pieces:
        x = (w - p[1].shape[1]) / 2 if upright else x_text
        pixels = put(pixels, p, (x - 2, y - 2))
        last = (x, y, x + p[1].shape[1] - 4, y + p[1].shape[0] - 4)
        y += pitch
    star = sparkle(0.24 * font.size)
    pixels = put(pixels, star, (last[2] + 0.16 * font.size - star[1].shape[1] / 2,
                                last[1] - 0.06 * font.size - star[1].shape[0] / 2))

    # The badges.
    badge_h = round(150 * v if upright else 112 * u)
    leaves = ("#FFE3A6", "#F4BE62") if dark else ("#FFBE6A", "#F59A45")  # warm light gold, warm amber
    made = [badge(*(b.split("|", 1) + [""])[:2], badge_h, ink, leaves) for b in badges]
    top = last[3] + (96 * v if upright else 70 * u)
    if made:
        space = 90 * u if upright else 40 * u
        total = sum(m[1].shape[1] for m in made) + space * (len(made) - 1)
        x = (w - total) / 2 if upright else x_text - 0.12 * badge_h
        for m in made:
            pixels = put(pixels, m, (x, top))
            x += m[1].shape[1] + space
        top += badge_h

    # The window, upright unless told otherwise, its top a little under the
    # badges, running off the bottom (and in wide images off the right).
    width = 1240 * u if upright else 0.6 * w
    if tilt:
        piece, centre, corners = tilted(window, box, width, tilt, focal=1.5 * max(w, h))
    else:
        piece, centre = flat(window, box, width)
        half = (width / 2, width * (box[3] - box[1]) / (box[2] - box[0]) / 2)
        corners = [(-half[0], -half[1]), (half[0], -half[1]), (half[0], half[1]), (-half[0], half[1])]
    if upright:
        middle = (w / 2, top + 190 * v - min(c[1] for c in corners))
    else:  # its left edge clear of the title
        middle = (0.47 * w - min(c[0] for c in corners), 0.3 * h - min(c[1] for c in corners))
    pixels = put(pixels, piece, (middle[0] - centre[0], middle[1] - centre[1]))  # no shadow under the program

    # The keys, on the window's top edge by its right corner.
    corner = (min(middle[0] + corners[1][0], w - 0.03 * w), middle[1] + corners[1][1])  # on the image
    key_h = round(120 * u if upright else 96 * u)
    for i, key in enumerate(keys):
        piece = turned(keycap(key, key_h, False), (-8, 5)[i % 2])
        x = corner[0] - (0.75 + 1.4 * i) * key_h - piece[1].shape[1] / 2
        y = corner[1] - (0.42 + 0.22 * i) * key_h - piece[1].shape[0] / 2
        pixels = put(pixels, piece, (x, y), 0.3 * key_h, dark)

    return Image.fromarray(np.clip(np.round(grain(pixels, seed)), 0, 255).astype(np.uint8))


def moved(black, check):
    """Where two shots that should be the same differ, if anywhere."""
    differ = np.abs(np.asarray(check, np.float32) - np.asarray(black, np.float32)).max(axis=2) > 6
    if differ.sum() <= 4:
        return None
    ys, xs = np.nonzero(differ)
    return f"{differ.sum()} pixels around ({xs.min()}..{xs.max()}, {ys.min()}..{ys.max()})"


def matte(black, white):
    """The window with its transparency, from the same spot shot with a black
    and a white backdrop, cropped to what isn't fully transparent."""
    black = np.asarray(black.convert("RGB"), np.float32)
    white = np.asarray(white.convert("RGB"), np.float32)
    alpha = np.clip(1 - (white - black).mean(axis=2) / 255, 0, 1)
    colour = np.where(alpha[..., None] > 1 / 255, black / np.maximum(alpha[..., None], 1 / 255), 0)
    ys, xs = np.nonzero(alpha > 2 / 255)
    pad = 2
    top, left = max(ys.min() - pad, 0), max(xs.min() - pad, 0)
    bottom, right = ys.max() + 1 + pad, xs.max() + 1 + pad
    out = np.dstack([np.clip(colour, 0, 255), alpha * 255])[top:bottom, left:right]
    return Image.fromarray(np.round(out).astype(np.uint8), "RGBA")


class Desktop:
    """The few Win32 calls the shots need, in physical pixels."""

    WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
    ENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    class WNDCLASSEXW(ctypes.Structure):
        _fields_ = [("cbSize", wt.UINT), ("style", wt.UINT), ("lpfnWndProc", ctypes.c_void_p),
                    ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int), ("hInstance", wt.HINSTANCE),
                    ("hIcon", wt.HICON), ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                    ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR), ("hIconSm", wt.HICON)]

    def __init__(self):
        self.user32 = u = ctypes.WinDLL("user32", use_last_error=True)
        self.dwmapi = ctypes.WinDLL("dwmapi")
        self.gdi32 = g = ctypes.WinDLL("gdi32")
        self.kernel32 = k = ctypes.WinDLL("kernel32", use_last_error=True)
        u.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per monitor, v2
        u.GetForegroundWindow.restype = wt.HWND
        u.GetWindowLongPtrW.restype = ctypes.c_ssize_t
        u.GetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int]
        u.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
        u.IsWindowVisible.argtypes = [wt.HWND]
        u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
        u.EnumWindows.argtypes = [self.ENUMPROC, wt.LPARAM]
        u.CreateWindowExW.restype = wt.HWND
        u.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int,
                                      ctypes.c_int, ctypes.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE, ctypes.c_void_p]
        u.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT]
        u.SetClassLongPtrW.restype = ctypes.c_size_t
        u.SetClassLongPtrW.argtypes = [wt.HWND, ctypes.c_int, ctypes.c_ssize_t]
        u.InvalidateRect.argtypes = [wt.HWND, ctypes.c_void_p, wt.BOOL]
        u.UpdateWindow.argtypes = [wt.HWND]
        u.DestroyWindow.argtypes = [wt.HWND]
        u.PeekMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT, wt.UINT]
        u.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
        u.RegisterClassExW.argtypes = [ctypes.POINTER(self.WNDCLASSEXW)]
        u.UnregisterClassW.argtypes = [wt.LPCWSTR, wt.HINSTANCE]
        u.DefWindowProcW.restype = ctypes.c_ssize_t
        u.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]
        u.SendMessageTimeoutW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPCWSTR, wt.UINT, wt.UINT,
                                          ctypes.c_void_p]
        self.dwmapi.DwmGetWindowAttribute.argtypes = [wt.HWND, wt.DWORD, ctypes.c_void_p, wt.DWORD]
        g.CreateSolidBrush.restype = wt.HBRUSH
        g.DeleteObject.argtypes = [wt.HANDLE]
        k.GetModuleHandleW.restype = wt.HMODULE
        k.OpenProcess.restype = wt.HANDLE
        k.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
        k.CloseHandle.argtypes = [wt.HANDLE]

    def windows(self):
        """The visible top-level windows, front to back."""
        found = []
        self.user32.EnumWindows(self.ENUMPROC(lambda h, _: found.append(h) or True), 0)
        return [h for h in found if self.user32.IsWindowVisible(h) and not self.cloaked(h)]

    def cloaked(self, hwnd):
        value = wt.DWORD()
        self.dwmapi.DwmGetWindowAttribute(hwnd, 14, ctypes.byref(value), 4)  # DWMWA_CLOAKED
        return value.value != 0

    def frame(self, hwnd):
        r = wt.RECT()
        self.dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(r), ctypes.sizeof(r))  # extended frame bounds
        return r.left, r.top, r.right, r.bottom

    def class_name(self, hwnd):
        name = ctypes.create_unicode_buffer(256)
        self.user32.GetClassNameW(hwnd, name, 256)
        return name.value

    def topmost(self, hwnd):
        return bool(self.user32.GetWindowLongPtrW(hwnd, -20) & 0x8)  # WS_EX_TOPMOST

    def program(self, hwnd):
        pid = wt.DWORD()
        self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        process = self.kernel32.OpenProcess(0x1000, False, pid.value)  # query limited information
        if not process:
            return ""
        name = ctypes.create_unicode_buffer(1024)
        length = wt.DWORD(1024)
        ok = self.kernel32.QueryFullProcessImageNameW(process, 0, name, ctypes.byref(length))
        self.kernel32.CloseHandle(process)
        return pathlib.PureWindowsPath(name.value).name.lower() if ok else ""

    def pump(self):
        msg = wt.MSG()
        while self.user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            self.user32.TranslateMessage(ctypes.byref(msg))
            self.user32.DispatchMessageW(ctypes.byref(msg))

    def settle(self):
        """Until DWM has put the latest change on the screen."""
        self.pump()
        for _ in range(2):
            self.dwmapi.DwmFlush()
        time.sleep(0.02)


class Backdrop:
    """A window of one colour right behind the given ones (front to back),
    never activated."""

    def __init__(self, desktop, rect, windows):
        self.d = d = desktop
        self.instance = d.kernel32.GetModuleHandleW(None)
        self.brush = d.gdi32.CreateSolidBrush(0)
        self.name = f"WinShunShotBackdrop{id(self)}"
        cls = Desktop.WNDCLASSEXW()
        cls.cbSize = ctypes.sizeof(cls)
        cls.lpfnWndProc = ctypes.cast(d.user32.DefWindowProcW, ctypes.c_void_p).value
        cls.hInstance = self.instance
        cls.hbrBackground = self.brush
        cls.lpszClassName = self.name
        if not d.user32.RegisterClassExW(ctypes.byref(cls)):
            raise ctypes.WinError(ctypes.get_last_error())
        left, top, right, bottom = rect
        topmost = all(d.topmost(h) for h in windows)
        self.hwnd = d.user32.CreateWindowExW(0x08000080 | (0x8 if topmost else 0), self.name, "", 0x80000000,
                                             left, top, right - left, bottom - top, None, None, self.instance, None)
        if not self.hwnd:  # no activate, tool window, maybe topmost; popup
            raise ctypes.WinError(ctypes.get_last_error())
        flags = 0x0010 | 0x0040  # no activate, show
        if topmost:
            # Topmost from the start: made so later (elevated, while another
            # program's window has the focus, as with the clipboard, which
            # doesn't take it) it stays below that window, success reported.
            # Above all, and the windows shot put back over it in their order.
            d.user32.SetWindowPos(self.hwnd, wt.HWND(-1), left, top, right - left, bottom - top, flags)
            for h in reversed(windows):
                d.user32.SetWindowPos(h, wt.HWND(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)  # keep size, place, focus
        else:
            d.user32.SetWindowPos(self.hwnd, windows[-1], left, top, right - left, bottom - top, flags)
        d.settle()
        stack = d.windows()
        if self.hwnd in stack:
            covering = [h for h in stack[:stack.index(self.hwnd)] if h not in windows and near(d.frame(h), rect, 0)
                        and min(d.frame(h)[2] - d.frame(h)[0], d.frame(h)[3] - d.frame(h)[1]) > 40]
            if covering:
                print(f"warning: in front of the backdrop too: {[d.class_name(h) for h in covering]} (shooting "
                      f"{[(d.class_name(h), d.frame(h), d.topmost(h)) for h in windows]})")

    def fill(self, colour):
        brush = self.d.gdi32.CreateSolidBrush(colour)
        self.d.user32.SetClassLongPtrW(self.hwnd, -10, brush)  # GCLP_HBRBACKGROUND
        self.d.gdi32.DeleteObject(self.brush)
        self.brush = brush
        self.d.user32.InvalidateRect(self.hwnd, None, True)
        self.d.user32.UpdateWindow(self.hwnd)
        self.d.settle()

    def close(self):
        self.d.user32.DestroyWindow(self.hwnd)
        self.d.pump()
        self.d.user32.UnregisterClassW(self.name, self.instance)
        self.d.gdi32.DeleteObject(self.brush)


def near(a, b, distance):
    return a[0] < b[2] + distance and b[0] < a[2] + distance and a[1] < b[3] + distance and b[1] < a[3] + distance


def shoot(delay=0.0, winshun=False, margin=64, tries=12):
    """The window in front, or with `winshun` WinShun's frontmost one (the
    clipboard pops up without taking the focus), and WinShun's windows by it
    (the logo), with transparency."""
    d = Desktop()
    time.sleep(delay)
    order = d.windows()
    def size(h):
        f = d.frame(h)
        return f[2] - f[0], f[3] - f[1]
    ours = [h for h in order if d.program(h) == "winshun.exe" and min(size(h)) > 4]
    main = d.user32.GetForegroundWindow()
    if winshun:
        main = next((h for h in ours if size(h)[0] >= 200 and size(h)[1] >= 150), None)  # not the taskbar box
        if main is None:
            sys.exit("no window of WinShun's is up")
    def logo(h):
        return max(size(h)) <= 160
    # A file dialog and the bar against it go together, whichever is in front.
    targets = [main]
    if main in ours:
        targets += [h for h in order if h not in ours and d.class_name(h) == "#32770"
                    and near(d.frame(h), d.frame(main), 24)]
    else:
        targets += [h for h in ours if not logo(h) and near(d.frame(h), d.frame(main), 24)]
    targets += [h for h in ours if logo(h) and any(near(d.frame(h), d.frame(t), 160) for t in targets)]
    frames = [d.frame(h) for h in targets]
    screen = (d.user32.GetSystemMetrics(76), d.user32.GetSystemMetrics(77))  # virtual screen origin
    screen += (screen[0] + d.user32.GetSystemMetrics(78), screen[1] + d.user32.GetSystemMetrics(79))
    region = (max(min(f[0] for f in frames) - margin, screen[0]), max(min(f[1] for f in frames) - margin, screen[1]),
              min(max(f[2] for f in frames) + margin, screen[2]), min(max(f[3] for f in frames) + margin, screen[3]))

    cursor = wt.POINT()
    d.user32.GetCursorPos(ctypes.byref(cursor))
    if near((cursor.x, cursor.y, cursor.x + 1, cursor.y + 1), region, 0):
        d.user32.SetCursorPos(screen[0] + 2, screen[1] + 2)  # out of the way, and hover effects gone
        time.sleep(0.4)

    targets.sort(key=lambda h: order.index(h) if h in order else len(order))
    backdrop = Backdrop(d, region, targets)
    try:
        for _ in range(tries):
            shots = []
            for colour in (0x000000, 0xFFFFFF, 0x000000):
                backdrop.fill(colour)
                shots.append(ImageGrab.grab(region, include_layered_windows=True, all_screens=True).convert("RGB"))
            change = moved(shots[0], shots[2])
            if not change:
                break
            print(f"something moved ({change}), shooting again")
            time.sleep(0.15)
        else:
            sys.exit("the window kept changing")
    finally:
        backdrop.close()
    return matte(shots[0], shots[1])


PROFILE_MARK = "made-by-make_screenshots.txt"  # a profile folder this may empty again


def fixed_drives():
    kernel32 = ctypes.WinDLL("kernel32")
    mask = kernel32.GetLogicalDrives()
    roots = [f"{chr(65 + i)}:\\" for i in range(26) if mask >> i & 1]
    return [root for root in roots if kernel32.GetDriveTypeW(root) == 3]  # DRIVE_FIXED


def everything_but(files):
    """Folders to exclude so that only `files` is left of the fixed drives
    (WinShun can't exclude a whole drive): the top folders of every drive,
    and on the way down to `files` the folders beside it."""
    keep = pathlib.Path(files).resolve()
    chain = [keep, *keep.parents]
    excluded = []
    for root in fixed_drives():
        folder = pathlib.Path(root)
        while True:
            inside = next((p for p in chain if p.parent == folder and p != folder), None)  # towards `files`
            try:
                children = [p for p in folder.iterdir() if p.is_dir()]
            except OSError:
                children = []
            excluded += [p for p in children if p != inside]
            if inside is None or inside == keep:
                break
            folder = inside
    return excluded


def ini_list(paths):
    """A QStringList as QSettings writes one to an INI file."""
    def item(text):
        if any(c in text for c in ',";=\\') or text != text.strip():
            return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'
        return text
    return ", ".join(item(p.as_posix()) for p in paths)


def ini_set(text, section, values):
    """The INI text with these keys of `section` set (added where missing)."""
    lines, current, done, out = text.splitlines(), None, set(), []
    def missing():
        return [f"{k}={v}" for k, v in values.items() if k not in done]
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if current == section:
                out += missing()
                done.update(values)
            current = stripped[1:-1]
        elif current == section and "=" in line and line.split("=", 1)[0].strip() in values:
            key = line.split("=", 1)[0].strip()
            out.append(f"{key}={values[key]}")
            done.add(key)
            continue
        out.append(line)
    if current == section:
        out += missing()
    elif not done:
        out += ["", f"[{section}]"] + missing()
    return "\n".join(out) + "\n"


def make_profile(folder, files, recent):
    """A folder for `WinShun.exe --profile <folder>`: this user's settings,
    but nothing indexed outside `files`, no update check, and `recent` (paths,
    newest first) as the recently opened items; no clipboard history yet."""
    folder = pathlib.Path(folder).resolve()
    if folder.exists() and any(folder.iterdir()):
        if not (folder / PROFILE_MARK).exists():
            sys.exit(f"{folder} is not empty and not a profile made here; not touching it")
        shutil.rmtree(folder)
    (folder / "WinShun").mkdir(parents=True)
    (folder / PROFILE_MARK).write_text("A WinShun profile for screenshots (tools/make_screenshots.py).\n")
    real = pathlib.Path(os.environ["APPDATA"]) / "WinShun" / "WinShun.ini"
    text = real.read_text(encoding="utf-8") if real.exists() else ""
    excluded = everything_but(files)
    text = ini_set(text, "Index", {"ExcludedPaths": ini_list(excluded), "IncludeRemovableDrives": "false",
                                   "Folder": ""})
    text = ini_set(text, "Update", {"Automatic": "false"})
    (folder / "WinShun" / "WinShun.ini").write_text(text, encoding="utf-8")
    now = int(time.time() * 1000)
    lines = [f"{pathlib.Path(p)}\t1\t{now - i * 7 * 60_000}" for i, p in enumerate(recent)]
    (folder / "history.txt").write_text("\r\n".join(lines) + ("\r\n" if lines else ""), encoding="utf-8")
    print(f"profile in {folder}: {len(excluded)} folders excluded, {len(recent)} recent items")


SCENE = pathlib.Path(tempfile.gettempdir()) / "winshun-screenshot-scene.json"
PERSONALIZE = r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"


def apps_light(value=None):
    """Windows' app theme (1: light), set when a value is given."""
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, PERSONALIZE, 0, winreg.KEY_READ | winreg.KEY_SET_VALUE) as key:
        if value is None:
            return winreg.QueryValueEx(key, "AppsUseLightTheme")[0]
        winreg.SetValueEx(key, "AppsUseLightTheme", 0, winreg.REG_DWORD, value)
    d = Desktop()
    d.user32.SendMessageTimeoutW(0xFFFF, 0x001A, 0, "ImmersiveColorSet", 0x2, 2000, None)  # WM_SETTINGCHANGE
    return value


def wallpaper(path=None):
    """The desktop wallpaper's file, set (for this session only, not saved)
    when a path is given."""
    d = Desktop()
    if path is None:
        name = ctypes.create_unicode_buffer(1024)
        d.user32.SystemParametersInfoW(0x0073, 1024, name, 0)  # SPI_GETDESKWALLPAPER
        return name.value
    d.user32.SystemParametersInfoW(0x0014, 0, ctypes.c_wchar_p(str(path)), 0x2)  # SPI_SETDESKWALLPAPER, send change
    return path


def scene(theme=None, paper=None):
    saved = json.loads(SCENE.read_text(encoding="utf-8")) if SCENE.exists() else {}
    if theme:
        saved.setdefault("apps_light", apps_light())
        apps_light(1 if theme == "light" else 0)
    if paper:
        current = wallpaper()
        if "wallpaper" not in saved and not pathlib.Path(current).is_file():
            sys.exit(f"the wallpaper now is not a picture file ({current!r}); not changing it")
        saved.setdefault("wallpaper", current)
        if paper in NAMES:
            d = Desktop()
            size = (d.user32.GetSystemMetrics(0), d.user32.GetSystemMetrics(1))
            pixels, _ = background(paper, size)
            file = pathlib.Path(tempfile.gettempdir()) / f"winshun-screenshot-{paper}.png"
            Image.fromarray(np.clip(np.round(pixels), 0, 255).astype(np.uint8)).save(file)
            paper = file
        wallpaper(pathlib.Path(paper).resolve())
    SCENE.write_text(json.dumps(saved), encoding="utf-8")
    time.sleep(1.5)  # the wallpaper fades in, and Mica follows
    print(f"scene set; before: {saved}")


def restore():
    if not SCENE.exists():
        print("nothing to restore")
        return
    saved = json.loads(SCENE.read_text(encoding="utf-8"))
    if "apps_light" in saved:
        apps_light(saved["apps_light"])
    if "wallpaper" in saved:
        wallpaper(saved["wallpaper"])
    SCENE.unlink()
    print(f"restored {saved}")


def sheet(window, preset):
    """The window composed on every background, in a grid with the names."""
    tiles = [(name, compose(window, preset, name).convert("RGB")) for name in NAMES]
    tile_w = 520
    tile_h = round(tiles[0][1].height * tile_w / tiles[0][1].width)
    columns = 4
    rows = (len(tiles) + columns - 1) // columns
    label = 40
    out = Image.new("RGB", (columns * (tile_w + 20) + 20, rows * (tile_h + label + 20) + 20), "#FFFFFF")
    draw = ImageDraw.Draw(out)
    font = face(REGULAR, 26)
    for i, (name, tile) in enumerate(tiles):
        x, y = 20 + (i % columns) * (tile_w + 20), 20 + (i // columns) * (tile_h + label + 20)
        out.paste(tile.resize((tile_w, tile_h), Image.LANCZOS), (x, y))
        draw.text((x, y + tile_h + 6), name, font=font, fill="#333333")
    return out


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    c = commands.add_parser("scene")
    c.add_argument("--theme", choices=("light", "dark"))
    c.add_argument("--wallpaper", help=f"{', '.join(NAMES)} or a photo")
    commands.add_parser("restore")
    c = commands.add_parser("profile")
    c.add_argument("folder")
    c.add_argument("--files", required=True, help="the only folder indexed")
    c.add_argument("--recent", nargs="*", default=[], help="recently opened, newest first")
    c = commands.add_parser("shoot")
    c.add_argument("-o", "--out", required=True)
    c.add_argument("--delay", type=float, default=0.0, help="seconds to wait first, to bring the window up")
    c.add_argument("--winshun", action="store_true", help="WinShun's frontmost window, not the one in front")
    c = commands.add_parser("matte")
    c.add_argument("black")
    c.add_argument("white")
    c.add_argument("check", nargs="?")
    c.add_argument("-o", "--out", required=True)
    for name in ("compose", "sheet"):
        c = commands.add_parser(name)
        c.add_argument("window")
        c.add_argument("-o", "--out", required=True)
        c.add_argument("--preset", choices=PRESETS, default="readme")
        if name == "compose":
            c.add_argument("--bg", default="sky", help=f"{', '.join(NAMES)} or a photo")
            c.add_argument("--title")
            c.add_argument("--subtitle")
    c = commands.add_parser("cover")
    c.add_argument("window")
    c.add_argument("-o", "--out", required=True)
    c.add_argument("--preset", choices=[p for p in PRESETS if PRESETS[p]], default="xhs")
    c.add_argument("--bg", default="silk", help=f"{', '.join(NAMES)} or a photo")
    c.add_argument("--title", default="双击 Ctrl\\n搜遍整台电脑", help=r"lines split at \n; the last in the gradient")
    c.add_argument("--badge", action="append", help='"BIG|small", between laurels; repeat for more, "" for none')
    c.add_argument("--keys", nargs="*", default=["Ctrl", "Ctrl"], help="keys floating by the window")
    c.add_argument("--tilt", nargs=3, type=float, metavar=("YAW", "PITCH", "ROLL"), help="degrees")
    args = parser.parse_args()

    if args.command == "scene":
        return scene(args.theme, args.wallpaper)
    if args.command == "restore":
        return restore()
    if args.command == "profile":
        return make_profile(args.folder, args.files, args.recent)
    if args.command == "shoot":
        image = shoot(args.delay, args.winshun)
    elif args.command == "matte":
        black, white = Image.open(args.black), Image.open(args.white)
        if args.check and (change := moved(black.convert("RGB"), Image.open(args.check).convert("RGB"))):
            sys.exit(f"the shots differ at {change}: something moved, take them again")
        image = matte(black, white)
    elif args.command == "compose":
        image = compose(Image.open(args.window), args.preset, args.bg, args.title, args.subtitle)
    elif args.command == "cover":
        badges = [b for b in args.badge if b] if args.badge is not None else BADGES
        image = cover(Image.open(args.window), args.preset, args.bg, args.title, badges, args.keys, args.tilt)
    else:
        image = sheet(Image.open(args.window), args.preset)
    image.save(args.out, optimize=True)
    print(f"wrote {args.out} ({image.width}x{image.height})")


if __name__ == "__main__":
    main()
