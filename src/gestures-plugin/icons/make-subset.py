#!/usr/bin/env python3
"""Rebuilds PhosphorGestures.ttf: the few Phosphor (Regular) icons that the
mouse gestures plugin draws in its cards, cut out of the project's own
third_party/phosphor/Phosphor.ttf with fontTools, so that the plugin (no Qt)
carries a few KB and not the whole 150 KB font.

    python src/gestures-plugin/icons/make-subset.py

The names are Phosphor's (third_party/phosphor/metadata/regular.css maps
them to code points). The code points are the font's own, so Cards.cpp
spells them as constants: change ICONS here, run this, and copy the
printed table into the `Icon` enum of Cards.cpp. The font is embedded in
WinShunGestures.exe as the RCDATA resource PHOSPHOR_GESTURES
(resources/WinShunGestures.rc).
"""

import re
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

# Where each is used (Cards.cpp).
ICONS = [
    "power",  # the confirm card: 关机
    "arrow-clockwise",  # 重启
    "sign-out",  # 注销
    "moon",  # 睡眠
    "translate",  # the translation bubble's header
    "check",  # a note: done (已复制, 已剪切, 已置顶)
    "info",  # a note or a bubble: nothing to do, or what went wrong
    "copy",
    "x",
    "magnifying-glass",
    "scissors",
    "push-pin",
    "arrow-up",  # a note's trigger: an edge, the wheel
    "arrow-down",
    "arrow-left",
    "arrow-right",
    "arrow-up-left",  # a corner
    "arrow-up-right",
    "arrow-down-left",
    "arrow-down-right",
    "cursor",  # any other trigger
]

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "third_party" / "phosphor" / "Phosphor.ttf"
CSS = ROOT / "third_party" / "phosphor" / "metadata" / "regular.css"
TARGET = Path(__file__).resolve().parent / "PhosphorGestures.ttf"


def code_points() -> dict[str, int]:
    found = {}
    pattern = re.compile(r'\.ph\.ph-([a-z0-9-]+):before\s*\{\s*content:\s*"\\([0-9a-fA-F]+)"')
    for name, code in pattern.findall(CSS.read_text(encoding="utf-8")):
        found[name] = int(code, 16)
    return found


def main() -> int:
    points = code_points()
    missing = [name for name in ICONS if name not in points]
    if missing:
        print("not in regular.css:", ", ".join(missing), file=sys.stderr)
        return 1
    wanted = [points[name] for name in ICONS]

    options = subset.Options()
    options.layout_features = []  # no ligatures or alternates: one code point, one glyph
    options.hinting = False
    options.glyph_names = False
    options.notdef_outline = False
    options.name_IDs = [1, 2, 3, 4, 6]
    options.drop_tables += ["DSIG", "GSUB", "GPOS", "GDEF", "kern"]
    font = subset.load_font(str(SOURCE), options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=wanted)
    subsetter.subset(font)
    subset.save_font(font, str(TARGET), options)
    font.close()

    check = TTFont(str(TARGET))
    cmap = check.getBestCmap()
    for name in ICONS:
        if points[name] not in cmap:
            print("lost in the subset:", name, file=sys.stderr)
            return 1
    print(f"{TARGET.relative_to(ROOT)}: {TARGET.stat().st_size} bytes, {len(ICONS)} icons")
    for name in ICONS:
        print(f"  {name:18s} U+{points[name]:04X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
