"""The posters 0.41.0 adds: mouse gestures 2.0's settings window.

The gestures plugin's own window (WinShunGesturesEditor.dll, production QML
and models) renders offscreen at 200% with its sample rules
(`WinShunGestures.exe --editor --preview`, editor/EditorMain.cpp): the
Gestures page with the action menu open, and the With words selected page.
Nothing of this computer's is in them; the window is never shown.

  python tools/screenshots/release041.py --exe dist/release041/WinShun/WinShun.exe ^
      --gestures build/release/library-plugins/winshun.gestures/WinShunGestures.exe --work build/release041-shots --capture-only
  # look at <work>/gestures/*.png, then
  python tools/screenshots/release041.py --exe dist/release041/WinShun/WinShun.exe --work build/release041-shots --export-only

The other posters keep their pictures: the text expander (#403) has only a
plugin card, and the clipboard's 打开时显示 (#404) is a row in a page pictured
already.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True

import release017
import shots

SOURCES = {
    "gestures": ("L-gestures", "gestures/actions.png", None),
    "gestures-selection": ("L-gestures-selection", "gestures/selection.png", None),
}
POSTERS = (
    ("gestures", "L-gestures", "鼠标手势 2.0，[画一下就设好]", "按住右键画，再选要做的事 · 画圈、屏幕角落也行"),
    ("gestures-selection", "L-gestures-selection", "选中再画，[复制·搜索·翻译]", "不经过剪贴板，原来复制的东西还在"),
)
# Each picture: its file, the page, its height (logical px) and what else the preview is told.
RENDERS = (
    ("actions.png", 0, 820, ["--picker", "menu"]),
    ("selection.png", 1, 640, []),
)


def capture(exe, gestures, work):
    if work.exists():
        sys.exit(f"--work must be a fresh directory when capturing: {work}")
    folder = work / "gestures"
    temp = work / "temp"
    folder.mkdir(parents=True)
    temp.mkdir()
    env = {**os.environ, "TEMP": str(temp), "TMP": str(temp)}
    for name, page, height, extra in RENDERS:
        result = subprocess.run([str(gestures), "--editor", "--preview", str(folder / name), "--page", str(page),
                                 "--scale", "2", "--width", "760", "--height", str(height), "--app-dir", str(exe.parent), *extra],
                                env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
        print(name, "exit code", result.returncode, result.stderr.strip()[-400:], flush=True)
        result.check_returncode()
    (work / "capture.json").write_text(json.dumps({
        "exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "gestures": str(gestures), "gestures_sha256": hashlib.sha256(gestures.read_bytes()).hexdigest(),
        "editor_sha256": hashlib.sha256(gestures.with_name("WinShunGesturesEditor.dll").read_bytes()).hexdigest(),
        "scale": 2, "renders": [list(r) for r in RENDERS],
    }, indent=2, ensure_ascii=False), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--gestures", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    if args.capture_only:
        if not args.gestures:
            parser.error("--gestures is required for capturing")
        capture(args.exe.resolve(strict=True), args.gestures.resolve(strict=True), args.work.resolve())
        return
    release017.SOURCES = SOURCES
    shots.README_ONLY = shots.README_ONLY + POSTERS
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()


if __name__ == "__main__":
    main()
