"""Export the poster 0.29.0 adds, from a production preview of its build.

The raw picture comes from a run of the release build's WinShun.exe (an
isolated temporary profile, --scale 2 --renderer d3d11, QT_FONT_DPI=96, see
release027.py) into the --work directory:

  cmd    WinShun.exe --preview-settings <work>/cmd --command-form

One poster is new: a command of the favorites with its switch for Explorer's
right-click menu. Without a poster: Apps › Running and apps found by their
installed names (this PC's own windows and software), the entries in
Explorer's menu (Explorer's own menu), the clipboard's small picture rows and
several picture files (no preview writes a picture of the list with them),
the interface font found by either name (nothing to see).
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

import release017
import shots

SOURCES = {
    "command-menu": ("L-command-menu", "cmd/command-form-bottom.png", None),
}

POSTERS = (
    ("command-menu", "L-command-menu", "自己的命令，[进右键菜单]", "勾一下，右键文件就能把它交给你的命令"),
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
