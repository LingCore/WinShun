"""Capture the 0.44.0 settings window and export the poster this release adds.

New: Back and Forward in the settings window (docs/images/settings-back.jpg).
The shot is the real App's settings window, taken by SettingsMotionProbe's
--nav-check (--probe-runtime ... --settings-motion-check --nav-check): it walks
常规 → 快捷键 › 打开 Win顺 → 快捷键 › 按键一览 → 外观 and back, then right-clicks
the Forward button, and grabs the window with the history list open
(nav-history-menu.png). Its other grabs (the edge hint at the bottom of 常规)
show the isolated profile's paths under %TEMP%, so they are not used. The
window is grabbed without its native frame, so it gets the rounded corners and
hair of edge of a Windows 11 window here (release017).

The raw picture comes from a run of the release build's WinShun.exe (copy its
folder to C:\\WinShun first, so no Chinese path shows; an isolated temporary
TEMP, --scale 2 --renderer d3d11, QT_FONT_DPI=96, see release038.capture) into
a fresh --work directory. The window is real and on the screen for a few
seconds; leave the mouse alone meanwhile.

  nav   WinShun.exe --probe-runtime <work>/nav --settings-motion-check --nav-check

  python tools/screenshots/release044.py --exe C:/WinShun/WinShun.exe --work build/release044-shots --capture-only
  # look at <work>/nav/nav-history-menu.png, then
  python tools/screenshots/release044.py --exe C:/WinShun/WinShun.exe --work build/release044-shots --export-only

No poster: scrolling on to the next page and the links between settings (the
same window, moving), the tray icon's clicks and the settings hotkey (settings
rows), 记录截图 (a settings row), the hints about excluded folders and skipped
files in the search box, Open in Terminal as administrator (#425, a menu row),
the numbers following the scroll, the pinned box's taskbar icon and the
clipboard's preview frames: seen live or in the tests only.
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

import release038
import shots

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
    work = args.work.resolve()
    if args.capture_only:
        if work.exists():
            parser.error("--work must be a fresh directory")
        release038.RUNS = RUNS
        release038.capture(args.exe.resolve(strict=True), work, {"nav"})
        return
    release038.release017.SOURCES = SOURCES
    shots.README_ONLY = tuple(entry for entry in shots.README_ONLY if entry[0] not in SOURCES) + POSTERS
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release038.release017.main()


if __name__ == "__main__":
    main()
