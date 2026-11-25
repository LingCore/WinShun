"""Capture the 0.48.0 settings page and export the poster this release adds.

New: the clipboard history backed up into one ZIP and restored from it
(docs/images/clipboard-backup.jpg). The shot is the real settings window on
剪贴板 › 保留, one of the pages --preview-settings renders (p07-t2.png), over
the preview's own temporary settings; nothing of this computer's clipboard
is read or written.

Off the screen, from the release build's WinShun.exe, --scale 2 --renderer
d3d11 and QT_FONT_DPI=96, into a fresh --work directory.

  python -I tools/screenshots/release048.py --exe dist/release048/WinShun/WinShun.exe --work C:/wsposter048 --capture-only
  # look at <work>/settings/p07-t2.png, then
  python -I tools/screenshots/release048.py --exe dist/release048/WinShun/WinShun.exe --work C:/wsposter048 --export-only
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
    "clipboard-backup": ("L-clipboard-backup", "settings/p07-t2.png", None),
}

POSTERS = (
    ("clipboard-backup", "L-clipboard-backup", "剪贴板历史，[备份带着走]", "存成一个 ZIP · 换电脑或重装后还原 · 分组、备注和图片都在"),
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
