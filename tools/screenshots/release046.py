"""Capture the 0.46.0 search panel and export the poster this release adds.

New: the search panel's 名称 filter (GitHub #457): three boxes, 开头是, 结尾是
and 不包含 (words kept out, split by spaces), in the panel's filter row
(docs/images/name-filter.jpg). The shot is the real App's search window, taken
by the --name-filter-poster probe (--preview-results <dir> --name-filter-poster,
src/app/NavigationProbe.cpp): it indexes a believable folder of office files
(D:\工作\2026 年度 and its neighbours, sizes and dates filled, nothing real),
searches 报告 and sets the boxes the way a person looking for this year's
reports would: 开头是 2026, 结尾是 .docx, 不包含 草稿 旧 (name-filter-poster.png).
The files the boxes leave out are in the index too, so the filter visibly
narrows the list. The window is grabbed without its native frame, so it gets
the rounded corners and hair of edge of a Windows 11 window in
poster_pipeline.export_posters.

The probe runs off the screen (its windows are placed far outside it) from the
release build's WinShun.exe, with an isolated temporary TEMP, --scale 2
--renderer d3d11 and QT_FONT_DPI=96, into a fresh --work directory. (The
probe's other run, --navigation-workflows, has the same boxes on test names
such as wsquick; it is not used for the poster.)

  poster   WinShun.exe --preview-results <work>/poster --name-filter-poster --scale 2 --renderer d3d11

  python -I tools/screenshots/release046.py --exe dist/release046/WinShun/WinShun.exe --work C:/wsposter046 --capture-only
  # look at <work>/poster/name-filter-poster.png, then
  python -I tools/screenshots/release046.py --exe dist/release046/WinShun/WinShun.exe --work C:/wsposter046 --export-only
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True
# python -I leaves the script's own folder off sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import poster_pipeline

RUNS = (("poster", "--preview-results", ["--name-filter-poster"]),)

SOURCES = {
    "name-filter": ("L-name-filter", "poster/name-filter-poster.png", None),
}

POSTERS = (
    ("name-filter", "L-name-filter", "名称筛选，[开头 结尾 排除]", "开头是、结尾是、不包含，搜索面板里填一填"),
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
