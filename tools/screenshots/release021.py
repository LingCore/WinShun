"""Capture 0.21 with the isolated settings preview and the square poster template.

--capture-only renders the settings into a fresh --work at 200%: the 外观 page
now leads with the three interface styles (appearance.jpg), and the new
运行状况 page (health.jpg, its demo readings while the content index is read).
Inspect the raw renders and settings/preview.log, then use --export-only with
the same --work. Only the posters in SOURCES are written; the others keep
their files. No running WinShun is touched; the preview uses its own profile.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

import release017
import shots


release017.SOURCES = {
    "appearance": ("L-appearance", "settings/p01-t0.png", 1604),
    "health": ("L-health", "settings/p10-t0.png", 1340),
}
shots.README_ONLY = tuple(
    ("appearance", "L-appearance", "界面风格，[三种任选]", "精修原生 · 品牌色彩 · 轻拟物质感，浅色深色都有")
    if entry[0] == "appearance" else entry for entry in shots.README_ONLY
) + (
    ("health", "L-health", "运行状况，[一目了然]", "内存花在哪 · 正在做什么、还要多久 · 一键归还"),
)


def capture(exe, work):
    work.mkdir(parents=True)
    temp = work / "temp"
    temp.mkdir()
    env = {**os.environ, "TEMP": str(temp), "TMP": str(temp)}
    print("Capturing settings", flush=True)
    subprocess.run([str(exe), "--preview-settings", str(work / "settings"), "--scale", "2",
                    "--renderer", "d3d11"], env=env).check_returncode()
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    (work / "capture.json").write_text(json.dumps({
        "exe": str(exe), "sha256": digest(exe), "scale": 2, "renderer": "d3d11",
        "profiles": "isolated preview fixture; production UI and controllers",
    }, indent=2), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--work", type=Path, required=True)
    modes = ap.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = ap.parse_args()
    if args.capture_only:
        if args.work.exists():
            ap.error("--work must be a fresh directory when capturing")
        capture(args.exe.resolve(strict=True), args.work.resolve())
        return
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()


if __name__ == "__main__":
    main()
