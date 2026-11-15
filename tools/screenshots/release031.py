"""Export the poster 0.31.0 adds, from a production preview of its build.

The raw picture comes from a run of the release build's WinShun.exe (an
isolated temporary profile, --scale 2 --renderer d3d11, QT_FONT_DPI=96, see
release027.py) into the --work directory:

  font   WinShun.exe --preview-settings <work>/font --font-picker

One poster is new: the interface font list with its own search row. Without
a poster: the folder menu's subfolder pages (the preview's menu has no real
folders to list), the calculator tape (rows of a plugin, no preview writes a
picture of them), answering for Everything (another program's window), and
the fixes (nothing to see).
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

import release017
import shots

SOURCES = {
    "font-search": ("L-font-search", "font/font-list-filtered.png", None),
}

POSTERS = (
    ("font-search", "L-font-search", "界面字体，[打名字就找到]", "字体列表自带搜索行 · 方向键挑，Enter 选中"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    release017.SOURCES = SOURCES
    shots.README_ONLY = shots.README_ONLY + POSTERS
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()


if __name__ == "__main__":
    main()
