"""Capture 0.38.0 production previews and export the two posters this release redoes.

The raw pictures come from runs of the release build's WinShun.exe (copy its
folder to C:\\WinShun first, so no Chinese path shows in a picture; an isolated
temporary TEMP, --scale 2 --renderer d3d11, QT_FONT_DPI=96, see release027.py)
into a fresh --work directory:

  style       WinShun.exe --preview-results <work>/style --style-windows
  file-types  WinShun.exe --preview-settings <work>/file-types --file-types

  python tools/screenshots/release038.py --exe C:/WinShun/WinShun.exe --work build/release038-shots --capture-only
  # look at <work>/style/style-foldermenu*.png and <work>/file-types/quick-filters.png, then
  python tools/screenshots/release038.py --exe C:/WinShun/WinShun.exe --work build/release038-shots --export-only
  # --only quick-filters (or foldermenu) limits both steps to one poster.

Two posters are redone. The folder menu: since #384 each column's width follows
its names, so the columns are tighter; the preview grabs the menu and its first
column as two windows (style-foldermenu-cascade.png), composed as in
release033.py with MENU_WIDTH measured from the new picture (454 px, the
width of style-foldermenu.png; the menu was 720 before #384). The quick
filters: since #392 each filter row has a 排序 choice box after its 搜索前缀
box (the probe sets the first filter's to 大的在前). The picture is
file-types/quick-filters.png, which the probe grabs while its two filters
exist, the page scrolled to its end so the whole card shows (file-types-bottom.png,
which 0.25.0 used, is grabbed after the probe removed them: no row; capture it with
a build whose probe scrolls first, d8be55bc or later). Without a poster, seen
live only: text pins on the screen, settings found from the search box, partial
copy in the clipboard preview, where the clipboard opens, the terminal choice,
empty folder and file marks, and letters everywhere.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True

from PIL import Image

import art
import release017
import shots

MENU_WIDTH = 454  # style-foldermenu.png: the menu alone, at 2x (checked in columns())
# The column's window starts 2 px (one logical px) inside the menu's and is drawn over it: its
# hovered row's left inset (8 px, from x=452) equals its right one, so the cut is made there.
OVERLAP = 2
GAP = 6

SOURCES = {
    "foldermenu": ("L-foldermenu", "style/foldermenu-columns.png", None),
    "quick-filters": ("L-quick-filters", "file-types/quick-filters.png", None),
}

POSTERS = (
    ("foldermenu", "L-foldermenu", "资源管理器里，[双击空白处]", "文件夹一列列展开，列宽跟着名字收紧"),
    ("quick-filters", "L-quick-filters", "常用条件，[一点就用]", "存成快捷筛选，面板里点一下，还能带自己的排序"),
)

KNOWN_WARNING = "Window frame: the window has a new native handle"

RUNS = (
    ("style", "--preview-results", ["--style-windows"]),
    ("file-types", "--preview-settings", ["--file-types"]),
)


def capture(exe: Path, work: Path, only=None) -> None:
    runs = tuple(run for run in RUNS if not only or run[0] in only)
    work.mkdir(parents=True)
    temporary = work / "temp"
    temporary.mkdir()
    environment = {**os.environ, "TEMP": str(temporary), "TMP": str(temporary), "QT_FONT_DPI": "96",
                   "PYTHONDONTWRITEBYTECODE": "1", "__COMPAT_LAYER": "RunAsInvoker"}
    try:
        for folder, flag, extra in runs:
            print("Capturing", folder, flush=True)
            result = subprocess.run([str(exe), flag, str(work / folder), "--scale", "2", "--renderer", "d3d11", *extra],
                                    env=environment, timeout=600)
            print(f"{folder}: exit {result.returncode} (2 = warnings, 4 = a check failed)", flush=True)
            if result.returncode:
                # The style windows are hooked again when a second window opens,
                # and the preview logs that as a warning: accept exactly that.
                lines = (work / folder / "preview.log").read_text(encoding="utf-8").splitlines()
                warnings = [line for line in lines if line.startswith("WARNING:")]
                if result.returncode != 2 or not warnings or any(KNOWN_WARNING not in line for line in warnings):
                    result.check_returncode()
                print(f"Retained {len(warnings)} known warnings in {folder}/preview.log", flush=True)
    finally:
        if temporary.resolve().parent != work.resolve():
            raise ValueError("Temporary profile path escaped the capture directory")
        shutil.rmtree(temporary)
    (work / "capture.json").write_text(json.dumps({
        "exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "renderer": "d3d11", "profiles": "isolated production previews", "scale": 2, "QT_FONT_DPI": 96,
    }, indent=2), encoding="utf-8")


def columns(work: Path) -> None:
    alone = Image.open(work / "style/style-foldermenu.png")
    if alone.width != MENU_WIDTH:
        raise ValueError(f"the menu alone is {alone.width} px wide, not MENU_WIDTH {MENU_WIDTH}")
    cascade = Image.open(work / "style/style-foldermenu-cascade.png").convert("RGBA")
    alpha = cascade.getchannel("A")
    if alpha.getpixel((MENU_WIDTH - 1, cascade.height - 1)) != 255 or alpha.getpixel((MENU_WIDTH, cascade.height - 1)) != 0:
        raise ValueError("the menu does not end at MENU_WIDTH in the cascade picture")
    menu = cascade.crop((0, 0, MENU_WIDTH, cascade.height))
    column_box = alpha.crop((MENU_WIDTH, 0, cascade.width, cascade.height)).getbbox()
    column = cascade.crop((MENU_WIDTH - OVERLAP, 0, cascade.width, column_box[3]))
    menu, column = art.screen_piece(menu, radius=16), art.screen_piece(column, radius=16)
    out = Image.new("RGBA", (menu.width + GAP + column.width, menu.height))
    out.alpha_composite(menu, (0, 0))
    out.alpha_composite(column, (menu.width + GAP, 0))
    out.save(work / "style/foldermenu-columns.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    parser.add_argument("--only", action="append", choices=sorted(SOURCES),
                        help="only this poster's capture and export (repeatable), e.g. quick-filters")
    args = parser.parse_args()
    work = args.work.resolve()
    sources = {name: entry for name, entry in SOURCES.items() if not args.only or name in args.only}
    posters = tuple(entry for entry in POSTERS if entry[0] in sources)
    if args.capture_only:
        if work.exists():
            parser.error("--work must be a fresh directory")
        # A poster's raw pictures are the folder of its source under --work.
        capture(args.exe.resolve(strict=True), work, {entry[1].split("/")[0] for entry in sources.values()})
        return
    if "foldermenu" in sources:
        columns(work)
    release017.SOURCES = sources
    shots.README_ONLY = tuple(entry for entry in shots.README_ONLY if entry[0] not in sources) + posters
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()


if __name__ == "__main__":
    main()
