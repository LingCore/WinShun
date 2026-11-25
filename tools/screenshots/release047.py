"""Capture the 0.47.0 clipboard and export the poster this release adds.

New: a shortcut of its own for a clipboard entry (as in Ditto), and Keep
duplicates (docs/images/clipboard-hotkeys.jpg). The shot is the real clipboard
window over a temporary history, taken by the --clipboard-entry-hotkey run
with --entry-hotkey-poster (src/app/Preview.cpp): after its checks it fills
the history with believable entries (a reply often sent, a work mail, an
address, a query, a link, notes), three of them with keys, and grabs the
window (clipboard-hotkeys-poster.png). Each badge shows its keys beside a
keyboard; nothing of this computer's clipboard is read or written.

Off the screen, from the release build's WinShun.exe, --scale 2 --renderer
d3d11 and QT_FONT_DPI=96, into a fresh --work directory.

  poster   WinShun.exe --preview-results <work>/poster --clipboard-entry-hotkey --entry-hotkey-poster --scale 2 --renderer d3d11

  python -I tools/screenshots/release047.py --exe dist/release047/WinShun/WinShun.exe --work C:/wsposter047 --capture-only
  # look at <work>/poster/clipboard-hotkeys-poster.png, then
  python -I tools/screenshots/release047.py --exe dist/release047/WinShun/WinShun.exe --work C:/wsposter047 --export-only
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True
# python -I leaves the script's own folder off sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import poster_pipeline

RUNS = (("poster", "--preview-results", ["--clipboard-entry-hotkey", "--entry-hotkey-poster"]),)

SOURCES = {
    "clipboard-hotkeys": ("L-clipboard-hotkeys", "poster/clipboard-hotkeys-poster.png", None),
}

POSTERS = (
    ("clipboard-hotkeys", "L-clipboard-hotkeys", "常用的一条，[一键粘贴]", "给剪贴板里的一条设快捷键，在哪个程序里都能直接贴"),
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
        poster_pipeline.capture_previews(args.exe, args.work, RUNS, only={"poster"})
        return
    poster_pipeline.export_posters(args.work, SOURCES, POSTERS)


if __name__ == "__main__":
    main()
