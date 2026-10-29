"""Capture 0.16's changed UI with the real executable, then reuse the poster template.

python tools/screenshots/release016.py --exe C:/WinShun/WinShun.exe --work C:/wsrel016/shots
Use --capture-only to inspect raw renders first, or --export-only to recompose.
The preview modes use temporary profiles; they never stop the running WinShun.
Recording previews briefly show their own demo window and record only its client area.
--work keeps the 2x renders off a full drive; the posters still go to docs/images.
Paths the pictures show are neutral ones: the favorites list has the program's
own path (copy the release folder to C:/WinShun) and the temporary folder
(--temp, C:/Temp), and the recording panel its save folder (--videos, C:/录屏).
Pages that list this computer's own folders or shares (search scope, network
and WSL) and the plugin library's sample plugins are not taken from here.
"""

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess

from PIL import Image

import art
import shots

# Poster file (docs/images/<file>.jpg) -> (shot name, raw render under --work,
# the height to keep: the top of a whole page, as tall as the settings window).
SOURCES = {
    "panel": ("L-panel", "results/panel-gallery-menu.png", None),
    "clipboard-resize": ("L-clipboard-resize", "results/clipboard-gallery.png", None),
    "appearance": ("L-appearance", "settings/p01-t0.png", None),
    "droppie": ("L-droppie", "settings/p06-t0.png", None),
    "backup": ("L-backup", "settings/p08-t0.png", 1440), # no export made: no folder shown
    "file-types": ("L-file-types", "file-types/file-types-top.png", None),
    "recording": ("L-recording", "mp4/ready.png", None),
    "recording-gif": ("L-recording-gif", "gif/ready-gif.png", None),
    "capture": ("L-capture", "toolbar/toolbar-with-style.png", None),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--work", type=pathlib.Path, default=shots.WORK / "release016")
    parser.add_argument("--temp", type=pathlib.Path, default=pathlib.Path("C:/Temp"))
    parser.add_argument("--videos", type=pathlib.Path, default=pathlib.Path("C:/录屏"))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--capture-only", action="store_true")
    mode.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    demo = shots.HERE / "scenes/区域销售额.png"
    runs = (
        ("results", "--preview-results", ["--file-metadata", "--gallery-image", str(demo)]),
        ("settings", "--preview-settings", []),
        ("file-types", "--preview-settings", ["--file-types"]),
        ("toolbar", "--preview-capture-tools", ["--toolbar-drag"]),
        ("mp4", "--preview-recording", ["--auto"]),
        ("gif", "--preview-recording", ["--auto", "--gif", "--clicks"]),
    )
    if not args.export_only:
        args.temp.mkdir(parents=True, exist_ok=True)
        env = {**os.environ, "TEMP": str(args.temp), "TMP": str(args.temp)}
        for folder, flag, options in runs:
            print(f"Capturing {folder} with {exe}", flush=True)
            # A recording is saved where the preview writes, and the panel shows that folder.
            out = args.videos if flag == "--preview-recording" else work / folder
            if out == args.videos and out.exists():
                shutil.rmtree(out)
            result = subprocess.run([str(exe), flag, str(out), "--scale", "2",
                                     "--renderer", "d3d11", *options], env=env)
            if result.returncode:
                print(f"  {folder}: exit {result.returncode} (warnings; check the renders)", flush=True)
            if out == args.videos:
                shutil.rmtree(work / folder, ignore_errors=True)
                shutil.move(str(out), str(work / folder))
        (work / "capture.json").write_text(json.dumps({
            "exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
            "scale": 2, "renderer": "d3d11", "profiles": "temporary, demo entries only",
        }, indent=2), encoding="utf-8")
    if args.capture_only:
        return

    shots_dir = shots.WORK / "shots"
    masters = work / "poster-masters"
    masters.mkdir(exist_ok=True)
    for shot, raw, height in SOURCES.values():
        with Image.open(work / raw) as source:
            rgba = source.convert("RGBA")
            if height:
                rgba = rgba.crop((0, 0, rgba.width, min(height, rgba.height)))
            if rgba.getchannel("A").getextrema() == (255, 255):
                rgba = art.screen_piece(rgba, radius=16)
            padded = Image.new("RGBA", (rgba.width + 96, rgba.height + 96))
            padded.alpha_composite(rgba, (48, 48))
            padded.save(shots_dir / f"{shot}.png")
    for job in shots.jobs(("readme",), shots_dir, shots.WORK / "out"):
        if job[2].stem not in SOURCES:
            continue
        shots.run(job)
        print("Exported", job[2].name, flush=True)
    # Keep lossless template renders next to the native captures for later reuse.
    for file, (shot, _, _) in SOURCES.items():
        entry = next(job for job in shots.jobs(("readme",), shots_dir, shots.WORK / "out")
                     if job[2].stem == file)
        image = art.compose(Image.open(shots_dir / f"{shot}.png"), *entry[1][1:])
        image.save(masters / f"{file}.png")
    shutil.copy2(work / "capture.json", masters / "capture.json")


if __name__ == "__main__":
    main()
