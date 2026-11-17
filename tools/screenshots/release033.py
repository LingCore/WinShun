"""Export the poster 0.33.0 redoes, from a production preview of its build.

The raw pictures come from a run of the release build's WinShun.exe (an
isolated temporary TEMP, --scale 2 --renderer d3d11, QT_FONT_DPI=96, see
release027.py) into the --work directory:

  style   WinShun.exe --preview-results <work>/style --style-windows

One poster is redone: the folder menu, whose folders now open in columns side
by side (#348) instead of pages with a back row. The preview grabs the menu
and its first column as two windows (style-foldermenu-cascade.png, the two at
their real places); drawn off the screen they have no native frame, so each
gets the rounded corners and hair of edge of a Windows 11 window here, a few
pixels apart, as the two windows stand. Without a poster: pins edited in place,
the translation hotkey, Tab for whole windows (the capture plugin's previews
show no pin or selection of a real desktop), winshun:// links, the Dense size,
Tab into folders and the dialog bar over the taskbar (#362), seen live only.
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

from PIL import Image

import art
import release017
import shots

MENU_WIDTH = 720  # style-foldermenu.png: the menu alone, at 2x
GAP = 6

SOURCES = {
    "foldermenu": ("L-foldermenu", "style/foldermenu-columns.png", None),
}

POSTERS = (
    ("foldermenu", "L-foldermenu", "资源管理器里，[双击空白处]", "文件夹一列列展开，前面的列都留着"),
)


def columns(work: Path) -> None:
    cascade = Image.open(work / "style/style-foldermenu-cascade.png").convert("RGBA")
    alpha = cascade.getchannel("A")
    menu = cascade.crop((0, 0, MENU_WIDTH, cascade.height))
    column_box = alpha.crop((MENU_WIDTH, 0, cascade.width, cascade.height)).getbbox()
    column = cascade.crop((MENU_WIDTH, 0, cascade.width, column_box[3]))
    menu, column = art.screen_piece(menu, radius=16), art.screen_piece(column, radius=16)
    out = Image.new("RGBA", (menu.width + GAP + column.width, menu.height))
    out.alpha_composite(menu, (0, 0))
    out.alpha_composite(column, (menu.width + GAP, 0))
    out.save(work / "style/foldermenu-columns.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    columns(args.work.resolve())
    release017.SOURCES = SOURCES
    shots.README_ONLY = tuple(entry for entry in shots.README_ONLY if entry[0] not in dict(SOURCES)) + POSTERS
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()


if __name__ == "__main__":
    main()
