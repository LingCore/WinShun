"""Capture the 0.50.0 formula window and export the poster this release adds.

New: 公式识别, Shift+F on a capture (docs/images/formula.jpg). The shot is the
real FormulaPanel.qml from the screenshot plugin's --preview-formula, reading a
formula with the real model and copied as a Word equation (formula-word.png).

Off the screen, --scale 2 --renderer d3d11 and QT_FONT_DPI=96, into a fresh
--work directory. --components is a folder holding formula-mfr-1.5 (the model
as 设置 › 剪贴板 › 记录 › 公式识别模型 installs it); --ocr-dir a folder with
onnxruntime.dll (a build folder); --app-dir Win顺's folder. Qt's bin folder
must be on PATH when the plugin runs from a build folder.

  python -I tools/screenshots/release050.py --exe build/release/WinShunCapture.exe --work C:/wsposter050 ^
      --app-dir build/release --components build/formula-ocr/preview-components --ocr-dir build/release --capture-only
  # look at <work>/formula/formula-word.png, then
  python -I tools/screenshots/release050.py --work C:/wsposter050 --export-only
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True
# python -I leaves the script's own folder off sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import poster_pipeline

SOURCES = {
    "formula": ("L-formula", "formula/formula-word.png", None),
}

POSTERS = (
    ("formula", "L-formula", "公式识别，[一键转 LaTeX]", "截图按 Shift+F · 复制成 Word 公式、Markdown、Typst"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--app-dir", type=Path)
    parser.add_argument("--components", type=Path)
    parser.add_argument("--ocr-dir", type=Path)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    if args.capture_only:
        if not (args.exe and args.app_dir and args.components and args.ocr_dir):
            parser.error("--capture-only needs --exe, --app-dir, --components and --ocr-dir")
        if args.work.resolve().exists():
            parser.error("--work must be a fresh directory")
        runs = (("formula", "--preview-formula", ["--app-dir", str(args.app_dir.resolve()),
                                                  "--components", str(args.components.resolve()),
                                                  "--ocr-dir", str(args.ocr_dir.resolve())]),)
        poster_pipeline.capture_previews(args.exe, args.work, runs)
        return
    poster_pipeline.export_posters(args.work, SOURCES, POSTERS)


if __name__ == "__main__":
    main()
