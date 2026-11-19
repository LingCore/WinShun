"""The poster 0.40.0 adds: clipboard notes in colour (GitHub #398).

The release build's production QML renders the clipboard offscreen at 200%
on a temporary history (Preview.cpp, checkClipboardInteractions with a
gallery image): six entries, five noted in colours of their own, the newest
one's box open on its colours. The same run drags the fixture's corner with
the real mouse for a moment (its sizing check; the cursor is put back), so
take F:\\wswt\\ui.lock while it runs.

  python tools/screenshots/release040.py --exe dist/release040/WinShun/WinShun.exe --work build/release040-shots --capture-only
  # look at <work>/gallery/clipboard-notes.png, then
  python tools/screenshots/release040.py --exe dist/release040/WinShun/WinShun.exe --work build/release040-shots --export-only

The other posters keep their pictures: the settings rows 0.40.0 adds
(记住窗口大小, 显示搜索框) are switches in pages pictured already.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True

import release017
import shots

SOURCES = {"clipboard-notes": ("L-clipboard-notes", "gallery/clipboard-notes.png", None)}
POSTERS = (
    ("clipboard-notes", "L-clipboard-notes", "剪贴板备注，[分颜色]", "点一下或按 Tab 换颜色 · 地址、账号、待办一眼分开"),
)


def capture(exe, work):
    if work.exists():
        sys.exit(f"--work must be a fresh directory when capturing: {work}")
    temp = work / "temp"
    temp.mkdir(parents=True)
    env = {**os.environ, "TEMP": str(temp), "TMP": str(temp), "QT_FONT_DPI": "96"}
    picture = shots.HERE / "scenes/区域销售额.png"
    result = subprocess.run([str(exe), "--preview-results", str(work / "gallery"), "--file-metadata",
                             "--gallery-image", str(picture), "--scale", "2", "--renderer", "d3d11"], env=env)
    print("preview exit code", result.returncode, "- see", work / "gallery" / "preview.log", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    if args.capture_only:
        capture(args.exe.absolute(), args.work.resolve())
        return
    release017.SOURCES = SOURCES
    shots.README_ONLY = shots.README_ONLY + POSTERS
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()


if __name__ == "__main__":
    main()
