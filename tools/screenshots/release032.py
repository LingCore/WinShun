"""Export the poster 0.32.0 redoes, from a production preview of its build.

The raw picture comes from a run of the release build's WinShun.exe (an
isolated temporary profile, --scale 2 --renderer d3d11, QT_FONT_DPI=96, see
release027.py) into the --work directory:

  gallery   WinShun.exe --preview-results <work>/gallery --file-metadata --gallery-image scenes/区域销售额.png

One poster is redone: F2 rename, its box now under the name (#335). At 200 %
that run's "room beside" placement check fails (the preview's screen is
sized for 150 %); at 1.5 every check passes. Without a poster: the right
drag out (Explorer's own menu), the plugin library's What's new dialog (the
preview's library has no notes), directory links, the file dialog keys and
the fixes (nothing new to see).
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

import release017
import shots

SOURCES = {
    "rename": ("L-rename", "gallery/rename-box.png", None),
}

POSTERS = (
    ("rename", "L-rename", "按 F2，[就地改名]", "输入框在名字下面，原名一直看得见"),
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
