"""Capture 0.24.0 production previews and export the posters this release adds.

Use a fresh --work for --capture-only, inspect the PNGs and reports, then run
--export-only with that directory. Two posters are new, both settings pages
rendered by the real program over an isolated profile: pasting into programs
run as administrator (Settings > Clipboard > Shortcuts) and the taskbar search box's
position (Settings > Open WinShun > Taskbar). The other posters stay as
0.23.0 left them.
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

import release017
import shots


# (shot name, raw picture under --work, height kept from its top in pixels)
release017.SOURCES = {
    "admin-paste": ("L-admin-paste", "settings/p06-t1.png", None),
    "taskbar-side": ("L-taskbar-side", "settings/p00-t1.png", None),
}

shots.README_ONLY = tuple(shots.README_ONLY) + (
    ("admin-paste", "L-admin-paste", "管理员程序里，[照样粘贴]", "索引服务替你按一次粘贴键 · CAD 和旧式对话框也能跳转"),
    ("taskbar-side", "L-taskbar-side", "任务栏搜索框，[位置你来定]", "自动避开任务栏图标，或者始终停在托盘旁"),
)


def capture(exe, work):
    work.mkdir(parents=True)
    temporary = work / "temp"
    temporary.mkdir()
    environment = {**os.environ, "TEMP": str(temporary), "TMP": str(temporary),
                   "PYTHONDONTWRITEBYTECODE": "1", "__COMPAT_LAYER": "RunAsInvoker"}
    try:
        print("Capturing settings", flush=True)
        result = subprocess.run([str(exe), "--preview-settings", str(work / "settings"), "--scale", "2", "--renderer", "d3d11"],
                                env=environment, timeout=600)
        print(f"settings: exit {result.returncode} (QML warnings, or 4 when a check failed)", flush=True)
        if result.returncode:
            result.check_returncode()
    finally:
        if temporary.resolve().parent != work.resolve():
            raise ValueError("Temporary profile path escaped the capture directory")
        shutil.rmtree(temporary)
    (work / "capture.json").write_text(json.dumps({
        "exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "renderer": "d3d11", "profiles": "isolated production previews", "scale": 2,
    }, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    if args.capture_only:
        if args.work.exists():
            parser.error("--work must be a fresh directory")
        capture(args.exe.resolve(strict=True), args.work.resolve())
        return
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()


if __name__ == "__main__":
    main()
