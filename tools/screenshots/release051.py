"""Capture the 0.51.0 file preview and export the poster it changes.

Changed: the words searched for in the file preview have a marker's fill under
them (GitHub #482) as well as the bold, and the first match's whole line a band
(Theme.hover) across the card; docs/images/preview-colors.jpg is shot again
with both. The shot is the real preview window (PreviewBody.qml over the real
FilePreview) showing sample.md with "chapter" searched for: the picture
preview-markdown.png that the gallery run of --preview-results saves (the
Markdown fixture in src/app/Preview.cpp), the run the poster came from in
0.26.0. Its fixtures are in a temporary folder and a temporary history, and
Preview.cpp makes no system clipboard call (the #116 file copy goes to a stub).

THIS RUN IS NOT OFF THE SCREEN THROUGHOUT. The gallery run holds many other
checks, and two of them touch the desktop: the preview-placement check puts the
launcher and preview fixture windows inside the work area (not activated), and
the clipboard window's sizing check makes its window topmost, moves the real
cursor (put back) and, when nothing lies over the window's corner, sends a real
mouse drag with SendInput. Run it only while nobody uses the PC. Both checks
can also fail here (exit 4) without touching the picture: "room beside" needs a
work area wider than about 2960 physical pixels at --scale 2, and the corner may
be covered by another window (here a WinShun tool window). This script accepts only those, by
name, and still requires the colours' own checks (a fill under the words, the
band on the first match's line) to have passed. A run that only draws the
colours would need a flag in src/app/Preview.cpp.

Taken from the release build's WinShun.exe, --scale 2 --renderer d3d11 and
QT_FONT_DPI=96, into a fresh --work directory. Qt's bin folder must
be on PATH when WinShun.exe runs from a build folder.

  gallery  WinShun.exe --preview-results <work>/gallery --file-metadata --gallery-image tools/screenshots/scenes/区域销售额.png --scale 2 --renderer d3d11

  python -I tools/screenshots/release051.py --exe build/release/WinShun.exe --work build/poster051/work --capture-only
  # look at <work>/gallery/preview-markdown.png (the fill under "chapter", a band across its line), then
  python -I tools/screenshots/release051.py --work build/poster051/work --export-only
"""
import argparse
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
# python -I leaves the script's own folder off sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import poster_pipeline

HERE = Path(__file__).resolve().parent

RUNS = (
    ("gallery", "--preview-results",
     ["--file-metadata", "--gallery-image", str(HERE / "scenes" / "区域销售额.png")]),
)

# What the gallery run logs besides the picture's own checks (see the docstring).
KNOWN_WARNINGS = (
    poster_pipeline.KNOWN_FRAME_WARNING,
    # the stalled-file fixture says so on purpose
    "a file has kept the preview's reading thread at: starting",
    "the preview has waited 5 s for a .md file",
    # screen-dependent
    "preview placement regression: room beside: the preview covered the launcher",
    "clipboard sizing: corner_is_the_windows_own",
    "clipboard sizing: real_mouse_corner_drag",
    "clipboard regression: clipboard sizing regression",
)

SOURCES = {
    "preview-colors": ("L-preview-colors", "gallery/preview-markdown.png", None),
}

# The subtitle was "代码、Markdown、JSON 按语法着色，搜索的词加粗标出" until 0.50.0.
POSTERS = (
    ("preview-colors", "L-preview-colors", "预览文档，[语法上色]", "代码、Markdown、JSON 按语法着色，搜索的词标黄加粗"),
)


def verify_colours(work: Path) -> None:
    """The colours' own checks (a fill under the words, the band on the first match's line) passed."""
    log = (work / "gallery" / "preview.log").read_text(encoding="utf-8")
    marks = re.search(r"preview marks: (\d+) px of fill in the code, (\d+) in the JSON", log)
    if not marks or min(int(count) for count in marks.groups()) <= 200 or "preview colours:" not in log:
        raise SystemExit("the preview's colour checks did not pass: see gallery/preview.log")
    if not (work / "gallery" / "preview-markdown.png").is_file():
        raise SystemExit("gallery/preview-markdown.png was not saved")
    print(f"Fill under the searched words: {marks.group(1)} px in the code, {marks.group(2)} px in the JSON", flush=True)


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
        poster_pipeline.capture_previews(args.exe, args.work, RUNS, known_warnings=KNOWN_WARNINGS, accepted_codes=(2, 4))
        verify_colours(args.work.resolve())
        return
    poster_pipeline.export_posters(args.work, SOURCES, POSTERS)


if __name__ == "__main__":
    main()
