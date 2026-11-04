"""Capture 0.19 with the isolated previews and the square poster template.

--capture-only renders into a fresh --work: the settings (plugin library,
for comparison), the screenshot toolbar's two rows, the capture settings' tabs (按键 among
them) and the presenter's white board (--preview-presenter). Inspect the raw
renders and preview.log files, then use --export-only with the same --work.
Only the posters in SOURCES are written; the others keep their 0.18 files.
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


# The plugin library (settings/p09-t1.png) is rendered too: its made-up rows
# look as in 0.18, so plugin-library.jpg is kept.
release017.SOURCES = {
    "capture": ("L-capture", "toolbar/toolbar-with-style.png", None),
    # The 按键 tab after the preview's own key presses (a key changed, one
    # refused), down to the row under 收起/展开.
    "capture-keys": ("L-capture-keys", "capture-settings/capture-keys-states.png", 1446),
    "presenter": ("L-presenter", "presenter/presenter-board.png", None),
}
shots.README_ONLY = tuple(
    ("capture", "L-capture", "截图工具条，[两行排开]", "常用的一行贴着选区，其余在第二行 · 按住左侧三个点拖动")
    if entry[0] == "capture" else entry for entry in shots.README_ONLY
) + (
    ("capture-keys", "L-capture-keys", "截图按键，[自己来定]", "按下新键就换 · 冲突当场说明 · 一键恢复默认"),
    ("presenter", "L-presenter", "演示和白板，[边讲边画]", "桌面上直接画 · W 白板、K 黑板 · X 激光笔，Esc 回到程序"),
)
# Expected in the presenter's log: the text and number boxes ask for the keys
# while the off-screen windows never take the focus.
KNOWN = ("which has Qt::WindowDoesNotAcceptFocus set.",)


def capture(exe, plugin, work):
    work.mkdir(parents=True)
    temp = work / "temp"
    temp.mkdir()
    env = {**os.environ, "TEMP": str(temp), "TMP": str(temp)}
    app_dir = ["--app-dir", str(exe.parent)]
    runs = (
        (exe, "settings", "--preview-settings", [], ()),
        (plugin, "toolbar", "--preview-capture-tools", app_dir + ["--toolbar-drag"], ()),
        (plugin, "capture-settings", "--preview-capture-settings", app_dir + ["--groups"], ()),
        (plugin, "presenter", "--preview-presenter", app_dir, KNOWN),
    )
    for program, folder, flag, extra, known in runs:
        print("Capturing", folder, flush=True)
        result = subprocess.run([str(program), flag, str(work / folder), "--scale", "2",
                                 "--renderer", "d3d11", *extra], env=env)
        log = work / folder / "preview.log"
        warnings = [line for line in log.read_text(encoding="utf-8").splitlines()
                    if line.startswith("WARNING:")] if log.is_file() else []
        if result.returncode and (not known or len(warnings) != result.returncode
                                  or any(not line.endswith(known) for line in warnings)):
            result.check_returncode()
        if warnings:
            print(f"Retained {len(warnings)} known warnings in {folder}/preview.log", flush=True)
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    (work / "capture.json").write_text(json.dumps({
        "exe": str(exe), "sha256": digest(exe),
        "capture": str(plugin), "capture_sha256": digest(plugin),
        "capture_app_sha256": digest(plugin.with_name("WinShunCaptureApp.dll")),
        "scale": 2, "renderer": "d3d11",
        "profiles": "isolated preview fixtures; production UI and controllers",
    }, indent=2), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--capture", type=Path)
    ap.add_argument("--work", type=Path, required=True)
    modes = ap.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = ap.parse_args()
    if args.capture_only:
        if not args.capture:
            ap.error("--capture is required for capturing")
        if args.work.exists():
            ap.error("--work must be a fresh directory when capturing")
        capture(args.exe.resolve(strict=True), args.capture.resolve(strict=True), args.work.resolve())
        return
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()


if __name__ == "__main__":
    main()
