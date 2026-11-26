"""Capture the 0.49.0 settings row and export the poster this release adds.

New: a result row's buttons chosen and ordered in the settings
(docs/images/row-buttons.jpg). The shot is the real settings window with
结果行按钮 found by its search, one of the pictures --preview-settings renders
(search-结果行按钮.png), over the preview's own temporary settings.

Off the screen, from the release build's WinShun.exe, --scale 2 --renderer
d3d11 and QT_FONT_DPI=96, into a fresh --work directory.

  python -I tools/screenshots/release049.py --exe dist/release049/WinShun/WinShun.exe --work C:/wsposter049 --capture-only
  # look at <work>/settings/search-结果行按钮.png, then
  python -I tools/screenshots/release049.py --exe dist/release049/WinShun/WinShun.exe --work C:/wsposter049 --export-only
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True
# python -I leaves the script's own folder off sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import poster_pipeline

RUNS = (("settings", "--preview-settings", []),)

SOURCES = {
    "row-buttons": ("L-row-buttons", "settings/search-结果行按钮.png", None),
}

POSTERS = (
    ("row-buttons", "L-row-buttons", "结果行按钮，[自己选自己排]", "拖动换顺序 · 点一下取消 · 还能加上预览、复制名称、收藏"),
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
        poster_pipeline.capture_previews(args.exe, args.work, RUNS, only={"settings"})
        return
    poster_pipeline.export_posters(args.work, SOURCES, POSTERS)


if __name__ == "__main__":
    main()
