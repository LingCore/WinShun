"""Export the poster 0.28.0 adds, from a production preview of its build.

The raw picture comes from a run of the release build's WinShun.exe (an
isolated temporary profile, --scale 2 --renderer d3d11, QT_FONT_DPI=96, see
release027.py) into the --work directory:

  pure   WinShun.exe --preview-results <work>/pure --pure-search

One poster is new: the row that searches the web when nothing was found.
Without a poster: results while an input method is composing (a real input
method's candidate window), the clipboard's hidden entries and a copied text
file's content (no preview writes a picture of them), Send to in Explorer's
menu (Explorer's own menu) and the keys on a pin.
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

import release017
import shots

SOURCES = {
    "web-fallback": ("L-web-fallback", "pure/web-fallback.png", None),
}

POSTERS = (
    ("web-fallback", "L-web-fallback", "没搜到，[网页接着搜]", "什么都没找到时多出一行，回车用第一个网站搜这些字"),
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
