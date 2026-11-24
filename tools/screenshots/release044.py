"""Capture the 0.44.0 settings window and export the poster this release adds.

New: Back and Forward in the settings window (docs/images/settings-back.jpg).
The shot is the real App's settings window, taken by SettingsMotionProbe's
--nav-check (--probe-runtime ... --settings-motion-check --nav-check): it walks
常规 → 快捷键 › 打开 Win顺 → 快捷键 › 按键一览 → 外观 and back, then right-clicks
the Forward button, and grabs the window with the history list open
(nav-history-menu.png). Its other grabs (the edge hint at the bottom of 常规)
show the isolated profile's paths under %TEMP%, so they are not used. The
window is grabbed without its native frame, so it gets the rounded corners and
hair of edge of a Windows 11 window in poster_pipeline.export_posters.

The raw picture comes from a run of the release build's WinShun.exe (copy its
folder to C:\\WinShun first, so no Chinese path shows; an isolated temporary
TEMP, --scale 2 --renderer d3d11, QT_FONT_DPI=96) into a fresh --work directory.
The window is real and on the screen for a few seconds; leave the mouse alone
meanwhile.

  nav   WinShun.exe --probe-runtime <work>/nav --settings-motion-check --nav-check

  python tools/screenshots/release044.py --exe C:/WinShun/WinShun.exe --work build/release044-shots --capture-only
  # look at <work>/nav/nav-history-menu.png, then
  python tools/screenshots/release044.py --exe C:/WinShun/WinShun.exe --work build/release044-shots --export-only
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

import poster_pipeline

RUNS = (("nav", "--probe-runtime", ["--settings-motion-check", "--nav-check"]),)

SOURCES = {
    "settings-back": ("L-settings-back", "nav/nav-history-menu.png", None),
}

POSTERS = (
    ("settings-back", "L-settings-back", "设置也能，[后退前进]", "标题栏箭头、Alt+←/→、鼠标侧键，回到原来的位置"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    if args.capture_only:
        if args.work.resolve().exists():
            parser.error("--work must be a fresh directory")
        poster_pipeline.capture_previews(args.exe, args.work, RUNS, only={"nav"})
        return
    poster_pipeline.export_posters(args.work, SOURCES, POSTERS)


if __name__ == "__main__":
    main()
