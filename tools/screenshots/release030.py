"""Export the poster 0.30.0 adds, from a production preview of its build.

The raw picture comes from the release build's capture plugin (WS_APP_DIR and
--app-dir at the deployed WinShun folder, --scale 2 --renderer d3d11,
QT_FONT_DPI=96) into the --work directory:

  ocr    WinShunCapture.exe --preview-ocr <work>/ocr --translation

One poster is new: the text recognition window with a translation under the
words. Without a poster: the translation drawn over a pinned capture (a
native window with no preview that writes a picture), the clipboard's
"edit the text" editor, browser history rows (this PC's own history), the
line outlines of click-to-hide, letter jump and the typed web address row.
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

import release017
import shots

SOURCES = {
    "ocr-translate": ("L-ocr-translate", "ocr/ocr-translation-done.png", None),
}

POSTERS = (
    ("ocr-translate", "L-ocr-translate", "截图里的字，[直接翻译]", "原文在上，译文在下 · 也能把译文贴回截图原位"),
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
