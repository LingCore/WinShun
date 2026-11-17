"""Draw the 拾穗计划 page's QQ group QR code: resources/gleaning/qq-group.svg.

The link was read from QQ's own share picture; this draws it afresh in the
page's warm ink, round modules and rounded finder eyes, transparent, so it
sits on the page's light plate. Run it again when the group link changes:

    python -I tools/make_qq_qr.py https://qm.qq.com/q/...

It checks its own drawing by rasterising it and decoding it back.
Needs opencv-python (cv2) with the wechat_qrcode decoder (opencv-contrib).
"""
import sys
from pathlib import Path

import cv2
import numpy as np

INK = "#5C300F"   # WarmPalette.ink (light)
EYE = "#B9471A"   # the finders' centres: a deep version of WarmPalette.accent
DOT = 0.40        # module radius, in modules
QUIET = 2         # quiet zone kept in the SVG, in modules (the plate adds more)


def matrix(text: str) -> np.ndarray:
    params = cv2.QRCodeEncoder_Params()
    params.correction_level = cv2.QRCodeEncoder_CORRECT_LEVEL_Q
    m = cv2.QRCodeEncoder.create(params).encode(text)
    dark = m == 0
    # The encoder adds a 4-module border; trim it to the symbol.
    rows = np.where(dark.any(axis=1))[0]
    cols = np.where(dark.any(axis=0))[0]
    return dark[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1]


def finders(n: int):
    return [(0, 0), (0, n - 7), (n - 7, 0)]


def in_finder(r: int, c: int, n: int) -> bool:
    return any(fr <= r < fr + 7 and fc <= c < fc + 7 for fr, fc in finders(n))


def svg(dark: np.ndarray) -> str:
    n = dark.shape[0]
    size = n + 2 * QUIET
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}">']
    dots = []
    for r in range(n):
        for c in range(n):
            if dark[r, c] and not in_finder(r, c, n):
                x, y = c + QUIET + 0.5, r + QUIET + 0.5
                dots.append(f"M{x - DOT:g} {y:g}a{DOT:g} {DOT:g} 0 1 0 {2 * DOT:g} 0a{DOT:g} {DOT:g} 0 1 0 {-2 * DOT:g} 0z")
    out.append(f'<path fill="{INK}" d="{"".join(dots)}"/>')
    for fr, fc in finders(n):
        x, y = fc + QUIET, fr + QUIET
        # Ring: 7x7 rounded square less a 5x5 one, then a 3x3 rounded centre.
        out.append(f'<path fill="{INK}" fill-rule="evenodd" d="'
                   f'M{x + 2} {y}h3a2 2 0 0 1 2 2v3a2 2 0 0 1 -2 2h-3a2 2 0 0 1 -2 -2v-3a2 2 0 0 1 2 -2z'
                   f'M{x + 2.2} {y + 1}h2.6a1.2 1.2 0 0 1 1.2 1.2v2.6a1.2 1.2 0 0 1 -1.2 1.2h-2.6a1.2 1.2 0 0 1 -1.2 -1.2v-2.6a1.2 1.2 0 0 1 1.2 -1.2z"/>')
        out.append(f'<rect fill="{EYE}" x="{x + 2}" y="{y + 2}" width="3" height="3" rx="0.9"/>')
    out.append("</svg>")
    return "\n".join(out) + "\n"


def raster(dark: np.ndarray, scale: int = 12) -> np.ndarray:
    """The same drawing on white, for checking."""
    n = dark.shape[0]
    size = (n + 8) * scale
    img = np.full((size, size, 3), 255, np.uint8)
    ink = tuple(int(INK[i:i + 2], 16) for i in (5, 3, 1))
    eye = tuple(int(EYE[i:i + 2], 16) for i in (5, 3, 1))
    o = 4 * scale
    for r in range(n):
        for c in range(n):
            if dark[r, c] and not in_finder(r, c, n):
                centre = (o + int((c + 0.5) * scale), o + int((r + 0.5) * scale))
                cv2.circle(img, centre, int(DOT * scale), ink, -1, cv2.LINE_AA)
    for fr, fc in finders(n):
        x, y = o + fc * scale, o + fr * scale
        cv2.rectangle(img, (x, y), (x + 7 * scale - 1, y + 7 * scale - 1), ink, -1)
        cv2.rectangle(img, (x + scale, y + scale), (x + 6 * scale - 1, y + 6 * scale - 1), (255, 255, 255), -1)
        cv2.rectangle(img, (x + 2 * scale, y + 2 * scale), (x + 5 * scale - 1, y + 5 * scale - 1), eye, -1)
    return img


def main() -> None:
    text = sys.argv[1] if len(sys.argv) > 1 else "https://qm.qq.com/q/btbyWvs5ag"
    dark = matrix(text)
    img = raster(dark)
    plain = cv2.QRCodeDetector().detectAndDecode(img)[0]
    wechat = cv2.wechat_qrcode_WeChatQRCode().detectAndDecode(img)[0]
    small = cv2.resize(img, None, fx=0.25, fy=0.25, interpolation=cv2.INTER_AREA)
    wechat_small = cv2.wechat_qrcode_WeChatQRCode().detectAndDecode(small)[0]
    print("modules:", dark.shape[0], "decoded:", repr(plain), wechat, wechat_small)
    if plain != text or text not in wechat or text not in wechat_small:
        sys.exit("the drawing does not decode back to the link")
    out = Path(__file__).resolve().parent.parent / "resources" / "gleaning" / "qq-group.svg"
    out.write_text(svg(dark), encoding="utf-8")
    print("wrote", out)


if __name__ == "__main__":
    main()
