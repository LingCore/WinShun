"""Generates the installer's wizard images in installer/: the side panel of
the first and last pages, in a light and a dark version (Inno Setup picks one
with WizardStyle=dynamic), and the small image at the top right, transparent
around the logo so one serves both.

The logo is drawn by make_icon.render at the exact size, so it stays sharp;
the name is set in the bundled Alibaba PuHuiTi. Sizes are for 200 % display
scaling; Inno Setup scales them down for lower ones.

Usage: python tools/make_installer_images.py   (requires Pillow and numpy)
"""
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from make_icon import render  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "installer"
BOLD = ROOT / "resources/fonts/AlibabaPuHuiTi-3-85-Bold.ttf"
REGULAR = ROOT / "resources/fonts/AlibabaPuHuiTi-3-55-Regular.ttf"

SIDE = (430, 824)  # WizardImageFile at 200 %
SMALL = 116  # WizardSmallImageFile at 200 %

THEMES = {
    # Background top and bottom, name, tagline: the settings window's greys.
    "light": ("#FBFBFB", "#E9EEF6", "#1B1B1B", "#5C5C5C"),
    "dark": ("#2B2B2B", "#1C1F26", "#F3F3F3", "#A7A7A7"),
}


def gradient(size, top, bottom):
    w, h = size
    image = Image.new("RGB", size)
    t, b = Image.new("RGB", (1, 1), top).getpixel((0, 0)), Image.new("RGB", (1, 1), bottom).getpixel((0, 0))
    draw = ImageDraw.Draw(image)
    for y in range(h):
        f = y / (h - 1)
        draw.line([(0, y), (w, y)], fill=tuple(round(t[i] + (b[i] - t[i]) * f) for i in range(3)))
    return image


def centred(draw, y, text, font, fill, width):
    left, _, right, _ = draw.textbbox((0, 0), text, font=font)
    draw.text(((width - (right - left)) / 2 - left, y), text, font=font, fill=fill)


def side_panel(theme):
    top, bottom, ink, soft = THEMES[theme]
    w, h = SIDE
    image = gradient(SIDE, top, bottom)
    logo = render(176)
    image.paste(logo, ((w - logo.width) // 2, 250), logo)
    draw = ImageDraw.Draw(image)
    centred(draw, 470, "Win顺", ImageFont.truetype(str(BOLD), 64), ink, w)
    centred(draw, 556, "WinShun", ImageFont.truetype(str(REGULAR), 30), soft, w)
    return image


def small():
    image = Image.new("RGBA", (SMALL, SMALL), (0, 0, 0, 0))
    logo = render(96)
    image.paste(logo, ((SMALL - logo.width) // 2, (SMALL - logo.height) // 2), logo)
    return image


def main() -> None:
    for theme in THEMES:
        side_panel(theme).save(OUT / f"wizard-{theme}.png", optimize=True)
    small().save(OUT / "wizard-small.png", optimize=True)
    print("wrote installer/wizard-light.png, wizard-dark.png, wizard-small.png")


if __name__ == "__main__":
    main()
