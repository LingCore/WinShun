"""Capture the 0.43.0 production preview and export the poster this release adds.

New: the folder menu with its groups at the first level (docs/images/foldermenu-inline.jpg).
Since b104317c each of 收藏夹, 最近的文件夹 and 已打开的文件夹 can be a submenu, its
first few rows or all of them at the menu's first level, with a line between the
groups, and since bc43d7c6 the favorite apps are a section of their own, folded
or unfolded in the menu. The preview (Preview.cpp checkStyleWindows,
style-foldermenu-inline.png) builds the rows with FolderMenu's own
favoriteGroupEntries / groupEntries / assembleGroups: 收藏夹 and 最近的文件夹 all at
the first level, the apps' section unfolded, 已打开的文件夹 a submenu; the folder
icons are the shell's own, the apps' those of this PC's 微信, Visual Studio Code
and Terminal. Drawn off the screen the menu has no native frame, so it gets the
rounded corners and hair of edge of a Windows 11 window here (release017).

The raw picture comes from a run of the release build's WinShun.exe (copy its
folder to C:\\WinShun first, so no Chinese path shows; an isolated temporary
TEMP, --scale 2 --renderer d3d11, QT_FONT_DPI=96, see release027.py) into a
fresh --work directory:

  style   WinShun.exe --preview-results <work>/style --style-windows

  python tools/screenshots/release043.py --exe C:/WinShun/WinShun.exe --work build/release043-shots --capture-only
  # look at <work>/style/style-foldermenu-inline.png, then
  python tools/screenshots/release043.py --exe C:/WinShun/WinShun.exe --work build/release043-shots --export-only

No poster: the pinned search box on the taskbar and in Alt+Tab (the real taskbar),
selecting the words in a pin and Shift+C (the capture plugin's previews show no
pin), Esc stepping out of a screenshot tool, the system context menu by
default, typing folders to index, the narrower taskbar box, the dialog bar
waiting for typing and Directory Opus's This PC: settings rows or real
programs' windows, seen live or in the tests only.
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

import release038
import shots

SOURCES = {
    "foldermenu-inline": ("L-foldermenu-inline", "style/style-foldermenu-inline.png", None),
}

POSTERS = (
    ("foldermenu-inline", "L-foldermenu-inline", "常用文件夹，[一层就看全]", "收藏、最近的文件夹直接列出，收藏的应用可以折叠"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    work = args.work.resolve()
    if args.capture_only:
        if work.exists():
            parser.error("--work must be a fresh directory")
        release038.capture(args.exe.resolve(strict=True), work, {"style"})
        return
    release038.release017.SOURCES = SOURCES
    shots.README_ONLY = tuple(entry for entry in shots.README_ONLY if entry[0] not in SOURCES) + POSTERS
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release038.release017.main()


if __name__ == "__main__":
    main()
