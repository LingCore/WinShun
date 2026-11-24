"""Shared offscreen preview capture and square brand poster export helpers.

Used by release poster scripts (e.g. release044.py) so each release only declares
its RUNS, SOURCES and POSTERS without chaining imports across older release scripts.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True

from PIL import Image

import art
import shots

KNOWN_FRAME_WARNING = "Window frame: the window has a new native handle"


def capture_previews(
    exe: Path,
    work: Path,
    runs,
    *,
    only=None,
    scale: str = "2",
    known_warnings=(KNOWN_FRAME_WARNING,),
    timeout: int = 600,
) -> None:
    """Run isolated WinShun preview/probe commands into a fresh work directory."""
    exe = exe.resolve(strict=True)
    work = work.resolve()
    if work.exists():
        raise FileExistsError(f"--work must be a fresh directory: {work}")
    selected_runs = tuple(run for run in runs if not only or run[0] in only)
    work.mkdir(parents=True)
    temporary = work / "temp"
    temporary.mkdir()
    environment = {
        **os.environ,
        "TEMP": str(temporary),
        "TMP": str(temporary),
        "QT_FONT_DPI": "96",
        "PYTHONDONTWRITEBYTECODE": "1",
        "__COMPAT_LAYER": "RunAsInvoker",
    }
    try:
        for folder, flag, extra in selected_runs:
            print("Capturing", folder, flush=True)
            result = subprocess.run(
                [str(exe), flag, str(work / folder), "--scale", str(scale), "--renderer", "d3d11", *extra],
                env=environment,
                timeout=timeout,
            )
            print(f"{folder}: exit {result.returncode} (2 = warnings, 4 = a check failed)", flush=True)
            if result.returncode:
                log_path = work / folder / "preview.log"
                lines = log_path.read_text(encoding="utf-8").splitlines() if log_path.is_file() else []
                warnings = [line for line in lines if line.startswith("WARNING:")]
                if (
                    result.returncode != 2
                    or not warnings
                    or any(not any(k in line for k in known_warnings) for line in warnings)
                ):
                    result.check_returncode()
                print(f"Retained {len(warnings)} known warnings in {folder}/preview.log", flush=True)
    finally:
        if temporary.resolve().parent != work.resolve():
            raise ValueError("Temporary profile path escaped the capture directory")
        shutil.rmtree(temporary)
    (work / "capture.json").write_text(
        json.dumps(
            {
                "exe": str(exe),
                "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                "renderer": "d3d11",
                "profiles": "isolated production previews",
                "scale": scale,
                "QT_FONT_DPI": 96,
            },
            indent=2,
        ),
        encoding="utf-8",
    )


def export_posters(work: Path, sources: dict, posters: tuple) -> None:
    """Wrap raw window renders with Windows 11 corners/shadow padding and export brand posters."""
    work = work.resolve()
    shots_dir = work / "shots"
    masters = work / "poster-masters"
    shots_dir.mkdir(exist_ok=True)
    masters.mkdir(exist_ok=True)

    selected = {}
    for file_stem, (shot_name, raw_rel, height) in sources.items():
        source = work / raw_rel
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
            padded.save(shots_dir / f"{shot_name}.png")
        selected[file_stem] = hashlib.sha256(source.read_bytes()).hexdigest()

    original_readme_only = shots.README_ONLY
    try:
        shots.README_ONLY = tuple(entry for entry in original_readme_only if entry[0] not in sources) + tuple(posters)
        for job in shots.jobs(("readme",), shots_dir, work / "out"):
            if job[2].stem in selected:
                shots.run(job)
                shots.make(job[0], job[1]).save(masters / f"{job[2].stem}.png")
                print("Exported", job[2].name, flush=True)
    finally:
        shots.README_ONLY = original_readme_only

    (work / "poster-sources.json").write_text(json.dumps(selected, indent=2), encoding="utf-8")
