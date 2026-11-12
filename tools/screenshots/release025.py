"""Capture 0.25.0 production previews and export the posters this release changes.

Use a fresh --work for --capture-only, inspect the PNGs and reports, then run
--export-only with that directory (and --onboarding, the first page from
tools/verify-onboarding.ps1's `capture` case). One poster is new: the quick
filters' section of Settings > Result list > Kinds and filters, under a kind
bound to a folder. The panel's own chips have no poster yet: the navigation
checks narrow the window and hover a chip, so their pictures (kept under
--work/navigation as evidence) show a tooltip over the labels. Two are redone because their windows
changed: the first-run setup (two panes) and the file types (a kind bound to a
folder, behind an icon button). The other posters stay as 0.24.0 left them.
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
import release017
import shots


# (shot name, raw picture under --work, height kept from its top in pixels)
release017.SOURCES = {
    "quick-filters": ("L-quick-filters", "file-types/file-types-bottom.png", None),
    "file-types": ("L-file-types", "file-types/file-types-top.png", None),
    "onboarding": ("L-onboarding", "onboarding.png", None),
}

shots.README_ONLY = tuple(
    ("onboarding", "L-onboarding", "第一次打开，[两步设好]", "左边试按键，右边做选择，一屏看完不用滚动")
    if entry[0] == "onboarding" else entry for entry in shots.README_ONLY
) + (
    ("quick-filters", "L-quick-filters", "常用条件，[一点就用]", "存成快捷筛选，面板里点一下；文件类型还能绑定文件夹"),
)

RUNS = (
    ("settings", "--preview-settings", []),
    ("file-types", "--preview-settings", ["--file-types"]),
    ("navigation", "--preview-results", ["--navigation-workflows"]),
)


def capture(exe, work):
    work.mkdir(parents=True)
    temporary = work / "temp"
    temporary.mkdir()
    environment = {**os.environ, "TEMP": str(temporary), "TMP": str(temporary),
                   "PYTHONDONTWRITEBYTECODE": "1", "__COMPAT_LAYER": "RunAsInvoker"}
    try:
        for folder, flag, extra in RUNS:
            print("Capturing", folder, flush=True)
            result = subprocess.run([str(exe), flag, str(work / folder), "--scale", "2", "--renderer", "d3d11", *extra],
                                    env=environment, timeout=600)
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
    parser.add_argument("--onboarding", type=Path)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    if args.capture_only:
        if args.work.exists():
            parser.error("--work must be a fresh directory")
        capture(args.exe.resolve(strict=True), args.work.resolve())
        return
    if args.onboarding:
        Image.open(args.onboarding).save(args.work / "onboarding.png")
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()


if __name__ == "__main__":
    main()
