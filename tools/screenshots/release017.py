"""Compose 0.17 posters with the existing square brand template.

Capture with the deployed release, an unused --work directory and isolated
preview profiles. The optional --onboarding and --foldermenu images come from
verify-onboarding.ps1 and tst_foldermenu's WINSHUN_MENU_EVIDENCE output.
--export-only allows inspecting the raw renders before writing docs/images.
No existing WinShun process is stopped and no user's data directory is used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from PIL import Image
import art
import shots
from release016 import SOURCES as PREVIOUS

SOURCES = {
    **PREVIOUS,
    "numbers": ("L-numbers", "numbers/number-column-main.png", None),
    "content-details": ("L-content-details", "content/hits.png", None),
    "onboarding": ("L-onboarding", "onboarding.png", None),
    # The first eight demo favorites; the rest of this native fixture stress
    # tests scrolling and intentionally overlong names, outside this crop.
    "foldermenu": ("L-foldermenu", "foldermenu.png", 668),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--capture", type=Path)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--onboarding", type=Path)
    ap.add_argument("--foldermenu", type=Path)
    modes = ap.add_mutually_exclusive_group()
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = ap.parse_args()
    exe = args.exe.resolve(strict=True)
    work = args.work.resolve()
    capture = args.capture.resolve(strict=True) if args.capture else None
    if not args.export_only:
        if not capture:
            ap.error("--capture is required for capturing")
        if work.exists():
            ap.error("--work must be a fresh directory when capturing")
        work.mkdir(parents=True)
        temp = work / "temp"
        temp.mkdir()
        env = {**os.environ, "TEMP": str(temp), "TMP": str(temp)}
        demo = shots.HERE / "scenes/区域销售额.png"
        runs = (
            # Native sizing checks use the desktop's actual 150% scale;
            # pure offscreen renders use 200% for poster detail.
            (exe, "results", "--preview-results", "1.5", ["--file-metadata", "--gallery-image", str(demo)]),
            (exe, "numbers", "--preview-results", "2", ["--number-column"]),
            (exe, "content", "--preview-results", "2", ["--content-pane"]),
            (exe, "settings", "--preview-settings", "2", []),
            (exe, "file-types", "--preview-settings", "2", ["--file-types"]),
            (capture, "toolbar", "--preview-capture-tools", "2", ["--toolbar-drag"]),
            (capture, "mp4", "--preview-recording", "2", ["--auto"]),
            (capture, "gif", "--preview-recording", "2", ["--auto", "--gif", "--clicks"]),
        )
        for program, folder, flag, scale, extra in runs:
            print("Capturing", folder, flush=True)
            app_dir = ["--app-dir", str(exe.parent)] if program == capture else []
            result = subprocess.run([str(program), flag, str(work / folder), *app_dir,
                                     "--scale", scale, "--renderer", "d3d11", *extra], env=env)
            if result.returncode:
                # The combined results fixture emits two known QML render
                # warnings when replacing its models. Do not hide them or
                # accept a behavioral check failure as a successful capture.
                log = (work / folder / "preview.log").read_text(encoding="utf-8")
                warnings = [line for line in log.splitlines() if line.startswith("WARNING:")]
                known = ("TypeError: Cannot read property 'selectedCount' of null",
                         'QML QQuickImage*: Cannot open: qrc:/qt/qml/WinShun/undefined')
                reports_ok = False
                if folder == "results":
                    sizing = json.loads((work / folder / "clipboard-sizing.json").read_text(encoding="utf-8"))
                    navigation = [json.loads((work / folder / name).read_text(encoding="utf-8"))
                                  for name in ("search-navigation.json", "dialog-navigation.json")]
                    reports_ok = bool(sizing) and all(value is True for value in sizing.values()) and all(
                        report.get("passed") is True for report in navigation)
                if not reports_ok or not warnings or any(not line.endswith(known) for line in warnings):
                    result.check_returncode()
                print(f"Retained {len(warnings)} known render warnings in {folder}/preview.log", flush=True)
        (work / "capture.json").write_text(json.dumps({
            "exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
            "capture": str(capture), "capture_sha256": hashlib.sha256(capture.read_bytes()).hexdigest(),
            "capture_app_sha256": hashlib.sha256(capture.with_name("WinShunCaptureApp.dll").read_bytes()).hexdigest(),
            "scale": {"results": 1.5, "other": 2}, "renderer": "d3d11",
            "profiles": "isolated preview fixtures; production UI and controllers",
        }, indent=2), encoding="utf-8")
    for source, name in ((args.onboarding, "onboarding.png"), (args.foldermenu, "foldermenu.png")):
        if source:
            Image.open(source).save(work / name)
    if args.capture_only:
        return
    shots_dir, masters = work / "shots", work / "poster-masters"
    shots_dir.mkdir(exist_ok=True)
    masters.mkdir(exist_ok=True)
    selected = {}
    for file, (shot, raw, height) in SOURCES.items():
        source = work / raw
        if not source.is_file():
            raise FileNotFoundError(source)
        with Image.open(source) as image:
            rgba = image.convert("RGBA")
            if height:
                rgba = rgba.crop((0, 0, rgba.width, min(height, rgba.height)))
            if rgba.getchannel("A").getextrema() == (255, 255):
                rgba = art.screen_piece(rgba, radius=16)
            padded = Image.new("RGBA", (rgba.width + 96, rgba.height + 96))
            padded.alpha_composite(rgba, (48, 48))
            padded.save(shots_dir / f"{shot}.png")
        selected[file] = hashlib.sha256(source.read_bytes()).hexdigest()
    for job in shots.jobs(("readme",), shots_dir, work / "out"):
        if job[2].stem in selected:
            shots.run(job)
            shots.make(job[0], job[1]).save(masters / f"{job[2].stem}.png")
            print("Exported", job[2].name, flush=True)
    (work / "poster-sources.json").write_text(json.dumps(selected, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
