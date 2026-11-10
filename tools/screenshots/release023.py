"""Capture 0.23.0 production previews and export the posters this release changes.

Use a fresh --work for --capture-only, inspect the PNGs and reports, then run
--export-only with that directory. Only the appearance poster (four interface
styles) and the new plugin feedback poster are captured; the other posters
stay as 0.22.0 left them. Preview profiles are isolated.
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


release017.SOURCES = {
    "appearance": ("L-appearance", "settings/p01-t0.png", 1556),
    "plugin-feedback": ("L-plugin-feedback", "plugins/card-menu.png", 1456),
}

overrides = {
    "appearance": ("界面风格，[四种任选]", "精修原生 · 品牌色彩 · 轻拟物质感 · 液态玻璃"),
}
shots.README_ONLY = tuple(
    (entry[0], entry[1], *overrides[entry[0]]) if entry[0] in overrides else entry
    for entry in shots.README_ONLY
) + (
    ("plugin-feedback", "L-plugin-feedback", "插件出问题，[直接找作者]", "卡片“…”菜单直达作者仓库 · 社区插件由作者维护"),
)


def capture(exe, work):
    work.mkdir(parents=True)
    temporary = work / "temp"
    temporary.mkdir()
    environment = {**os.environ, "TEMP": str(temporary), "TMP": str(temporary),
                   "PYTHONDONTWRITEBYTECODE": "1", "__COMPAT_LAYER": "RunAsInvoker"}
    (work / "plugins").mkdir()
    runs = (
        ("settings", ["--preview-settings", str(work / "settings"), "--scale", "2"]),
        ("plugins", ["--preview-plugins", str(work / "plugins/card-menu.png"), "--menu", "card", "--scale", "2"]),
    )
    try:
        for folder, flags in runs:
            print("Capturing", folder, flush=True)
            result = subprocess.run([str(exe), *flags, "--renderer", "d3d11"], env=environment, timeout=240)
            print(f"{folder}: exit {result.returncode} (QML warnings, or 4 when a check failed)", flush=True)
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
