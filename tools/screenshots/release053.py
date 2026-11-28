"""Capture the 0.53.0 settings pages and export the posters they change.

New posters:
- docs/images/favorite-keys.jpg: 收藏夹 › 添加快捷键, the form as the
  --keys-form run fills it in through the real recorder (新建文件夹, xj,
  Ctrl+Shift+N). droppie.jpg (the 收藏夹 list) is not shot again: the
  preview's list holds the temporary folder and the exe's own path, which
  would show this PC's build folders.
- docs/images/portable-apps.jpg: 搜索范围 › 应用 (#505), a folder of
  portable programs and the apps named, from the settings preview's made-up
  catalog (D:\PortableApps; nothing is read from this PC).

Both come from --preview-settings: the real SettingsWindow and SettingsEditor
over a temporary profile (TEMP points into the work folder), off the screen,
no system clipboard, theme or scale touched. Taken from the release build's
WinShun.exe, --scale 2 --renderer d3d11 and QT_FONT_DPI=96, into a fresh
--work directory. Qt's bin folder must be on PATH when WinShun.exe runs from
a build folder.

  python -I tools/screenshots/release053.py --exe build/release/WinShun.exe --work build/poster053/work --capture-only
  # look at <work>/keys/keys-filled.png and <work>/settings/p03-t5.png, then
  python -I tools/screenshots/release053.py --work build/poster053/work --export-only
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True
# python -I leaves the script's own folder off sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import poster_pipeline

RUNS = (
    ("settings", "--preview-settings", []),
    ("keys", "--preview-settings", ["--keys-form"]),
)

SOURCES = {
    "favorite-keys": ("L-favorite-keys", "keys/keys-filled.png", 1404),
    "portable-apps": ("L-portable-apps", "settings/p03-t5.png", None),
}

POSTERS = (
    ("favorite-keys", "L-favorite-keys", "收藏快捷键，[打字就按下]", "起个关键词 · 打 xj 按 Enter，就按下了 Ctrl+Shift+N"),
    ("portable-apps", "L-portable-apps", "便携软件，[也能搜到]", "加上放软件的文件夹 · 给应用起别名 · 不想看到的隐藏"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    if args.capture_only:
        if not args.exe:
            parser.error("--capture-only needs --exe")
        if args.work.resolve().exists():
            parser.error("--work must be a fresh directory")
        poster_pipeline.capture_previews(args.exe, args.work, RUNS)
        return
    poster_pipeline.export_posters(args.work, SOURCES, POSTERS)


if __name__ == "__main__":
    main()
