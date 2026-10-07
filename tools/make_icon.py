"""Generates resources/app.ico from the logo in resources/logo.svg.

The logo is the Windows key: four window panes outlined in the four Windows
colours, the top-right pane grown into a leaf. Instead of shrinking one large
bitmap, each icon size is drawn from the geometry with its edges on the pixel
grid, so the 16-32 px tray, title bar and taskbar icons stay sharp. The
outline is opaque, its four colours blended in OKLab.

Usage: python tools/make_icon.py   (requires Pillow and numpy)
"""
import pathlib

import numpy as np
from PIL import Image

SIZES = [16, 20, 24, 28, 32, 40, 48, 64, 96, 128, 256]

# Pane colours: top-left, top-right (the leaf), bottom-left, bottom-right.
RED, GREEN, BLUE, YELLOW = "#FF5A4A", "#33C463", "#2D86FF", "#FFBA26"
PANE_FILL_OPACITY = 0.16

# Proportions of logo.svg (1024 grid): a 36 margin, 72 wide strokes, panes
# 440 across between stroke centres.
MARGIN = 36 / 1024
STROKE = 72 / 1024
CORNER = 132 / 440  # outer rounded corners
LEAF = 275 / 440  # the leaf's two curved corners
VEIN = (149.6 / 440, 272.8 / 440)  # along the leaf's diagonal
VEIN_WIDTH = 40 / 72  # relative to the outline
GRADIENT = (262 / 1024, 762 / 1024)  # where each colour fades into its neighbour


def quad(p0, c, p1, n=24):
    t = np.linspace(0, 1, n + 1)[1:, None]
    return list((1 - t) ** 2 * np.array(p0) + 2 * (1 - t) * t * np.array(c) + t ** 2 * np.array(p1))


def layout(size: int) -> tuple[int, int, int]:
    """Left edge, extent and stroke width of the logo, all in whole pixels so
    every straight edge lies on the pixel grid. Small sizes get a bolder
    stroke (at least 2 px); an odd extent sits half a pixel left of centre."""
    stroke = max(2, int(size * STROKE + 0.5))
    pane = round((size * (1 - 2 * MARGIN) - 3 * stroke) / 2)  # inside of one pane
    extent = 3 * stroke + 2 * pane
    return (size - extent) // 2, extent, stroke


def outlines(left: int, extent: int, stroke: int) -> list[np.ndarray]:
    """The four panes as closed polygons through the stroke centres."""
    a, b, o = left + stroke / 2, left + extent - stroke / 2, left + extent / 2
    r, q = CORNER * (o - a), LEAF * (o - a)
    top_left = [(o, o), (o, a), (a + r, a), *quad((a + r, a), (a, a), (a, a + r)), (a, o)]
    bottom_left = [(o, o), (a, o), (a, b - r), *quad((a, b - r), (a, b), (a + r, b)), (o, b)]
    bottom_right = [(o, o), (o, b), (b - r, b), *quad((b - r, b), (b, b), (b, b - r)), (b, o)]
    leaf = [(o, o), (b - q, o), *quad((b - q, o), (b, o), (b, o - q)), (b, a), (o + q, a),
            *quad((o + q, a), (o, a), (o, a + q))]
    return [np.array(p, dtype=float) for p in (top_left, leaf, bottom_left, bottom_right)]


def distance(points: np.ndarray, segments) -> np.ndarray:
    """Distance from each point to the nearest segment."""
    best = np.full(len(points), np.inf)
    for p, q in segments:
        d = q - p
        t = np.clip((points - p) @ d / max(d @ d, 1e-12), 0, 1)
        best = np.minimum(best, np.hypot(*(points - p - t[:, None] * d).T))
    return best


def inside(points: np.ndarray, polygon: np.ndarray) -> np.ndarray:
    x, y = points.T
    result = np.zeros(len(points), dtype=bool)
    for (x0, y0), (x1, y1) in zip(polygon, np.roll(polygon, -1, axis=0)):
        if y0 != y1:
            result ^= ((y0 > y) != (y1 > y)) & (x < x0 + (y - y0) * (x1 - x0) / (y1 - y0))
    return result


# sRGB <-> OKLab (https://bottosson.github.io/posts/oklab/)
LMS = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
                [0.2119034982, 0.6806995451, 0.1073969566],
                [0.0883024619, 0.2817188376, 0.6299787005]])
LAB = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
                [1.9779984951, -2.4285922050, 0.4505937099],
                [0.0259040371, 0.7827717662, -0.8086757660]])


def rgb(hex_colour: str) -> np.ndarray:
    return np.array([int(hex_colour[i:i + 2], 16) for i in (1, 3, 5)]) / 255


def to_oklab(c: np.ndarray) -> np.ndarray:
    linear = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    return np.cbrt(linear @ LMS.T) @ LAB.T


def from_oklab(lab: np.ndarray) -> np.ndarray:
    linear = np.clip(((lab @ np.linalg.inv(LAB).T) ** 3) @ np.linalg.inv(LMS).T, 0, 1)
    return np.where(linear <= 0.0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - 0.055)


def render(size: int) -> Image.Image:
    left, extent, stroke = layout(size)
    panes = outlines(left, extent, stroke)

    ys, xs = np.mgrid[0:size, 0:size] + 0.5
    points = np.column_stack([xs.ravel(), ys.ravel()])

    # Outline coverage, anti-aliased by distance to the stroke centre.
    segments = [(p, q) for pane in panes for p, q in zip(pane, np.roll(pane, -1, axis=0))]
    coverage = np.clip(stroke / 2 + 0.5 - distance(points, segments), 0, 1)
    if size >= 24:  # too small to read below that
        o, length = left + extent / 2, extent / 2 - stroke / 2
        vein = [np.array([o + t * length, o - t * length]) for t in VEIN]
        width = max(1.0, stroke * VEIN_WIDTH)
        coverage = np.maximum(coverage, np.clip(width / 2 + 0.5 - distance(points, [vein]), 0, 1))

    # The four colours meet in a bilinear blend.
    u = np.clip((points - size * GRADIENT[0]) / (size * (GRADIENT[1] - GRADIENT[0])), 0, 1)
    wx, wy = u[:, :1], u[:, 1:]
    red, green, blue, yellow = (to_oklab(rgb(c)) for c in (RED, GREEN, BLUE, YELLOW))
    colour = from_oklab((red * (1 - wx) + green * wx) * (1 - wy) + (blue * (1 - wx) + yellow * wx) * wy)

    # Tinted panes under the outline.
    fill = np.zeros((len(points), 4))
    for pane, c in zip(panes, (RED, GREEN, BLUE, YELLOW)):
        fill[inside(points, pane)] = [*rgb(c), PANE_FILL_OPACITY]

    alpha = coverage + fill[:, 3] * (1 - coverage)
    out_rgb = (colour * coverage[:, None] + fill[:, :3] * (fill[:, 3] * (1 - coverage))[:, None]) \
        / np.maximum(alpha, 1e-9)[:, None]
    pixels = np.column_stack([out_rgb, alpha]).reshape(size, size, 4)
    return Image.fromarray(np.round(pixels * 255).astype(np.uint8), "RGBA")


def main() -> None:
    root = pathlib.Path(__file__).resolve().parent.parent
    out = root / "resources" / "app.ico"
    frames = [render(s) for s in SIZES]
    frames[-1].save(out, format="ICO", sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
    print(f"wrote {out.relative_to(root).as_posix()}")  # the repo path may not fit the console code page


if __name__ == "__main__":
    main()
