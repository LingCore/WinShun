"""Make the one poster 0.36.0 adds: skipping folders and files (docs/images/exclude.jpg).

Settings › 搜索范围 › 文件夹 has a new line under 不搜索的文件夹: a link to
名称和类型 for skipping single files by name or extension. The picture is the
page's 文件夹 tab, rendered by the release build's own production QML and
controllers:

  WinShun.exe --preview-settings <work>/settings --scale 2 --renderer d3d11

an offscreen, isolated render of every settings page at 200% (files
settings/pNN-tM.png, page NN and tab M counted from 0, so 搜索范围 is p02 and
its 文件夹 tab p02-t0), on a private preview profile and demo data; no running
Win顺 is touched and nothing of the user's data directory is read. The poster
comes from the same square brand template as the other README pictures
(logo, LingCore credit, coloured title, subtitle, background, footer).

The page lists this PC's own folders (the six known ones from the user's
profile) and %TEMP% among the excluded ones, so the preview process gets a
neutral environment: USERPROFILE = C:/Users/Public (Windows' own, with a
Desktop, Documents ... of its own), no OneDrive variables, TEMP = C:/Temp.
Only that process sees it; no setting, registry value or folder is changed.
What the run adds in C:/Temp (its private profile) and in
C:/Users/Public/AppData/Local (icon, shader and pipeline caches that go to
the profile folder) is removed again afterwards; --temp and --home name other
folders. The whole page is 3348 px tall at this scale and the new link line is
its last one, so the poster shows the window scrolled down to 排除目录 (see
scrolled_to_excluded): the render's own rows, cut where the page is plain.

  python tools/screenshots/release036.py --exe dist/release036/WinShun/WinShun.exe --work build/release036-shots --capture-only
  # look at <work>/settings/p02-t0.png, then
  python tools/screenshots/release036.py --exe dist/release036/WinShun/WinShun.exe --work build/release036-shots --export-only

--work must be a fresh directory when capturing (build/, not committed). The
export writes docs/images/exclude.jpg (1440 x 1440 JPEG, quality 96, no
chroma subsampling) and keeps a lossless master and the source hashes in
<work>/poster-masters and <work>/poster-sources.json.
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
import art
import shots

# Where the poster's window is cut from the 2x render of the whole page
# (1840 x 3348): the page is a long one and its new link line is at the very
# bottom, so the poster shows the window as it looks scrolled down to 排除目录.
# Each cut is a row that is plain background all the way across, so nothing
# is torn: the window's fixed top (title bar, page name, tabs) down to HEADER,
# the page's rows from FROM on (a gap between the folder list's card and the
# 排除目录 heading), the side list from the top down and its footer (the text
# about saving and the version) at the bottom, joined at NAV (the gap between
# the side list and the page: the same plain colour). All pixels are the
# render's own; nothing is drawn or retouched.
HEADER, FROM, FOOTER, NAV = 300, 2195, 160, 490


def scrolled_to_excluded(image):
    width, height = image.size
    rgb = image.convert("RGB")
    background = rgb.getpixel((width - 1, 0))

    def plain(box):
        return all(pixel == background for pixel in rgb.crop(box).getdata())

    checks = {
        "the row under the tabs": plain((NAV, HEADER, width, HEADER + 1)),
        "the row between the folder list and 排除目录": plain((NAV, FROM, width, FROM + 1)),
        "the gap between side list and page": plain((NAV - 10, 0, NAV + 10, height)),
        "the side list's end": plain((0, 1280, NAV, 1281)),
        "above the footer": plain((0, height - FOOTER, NAV, height - FOOTER + 1)),
    }
    bad = [name for name, ok in checks.items() if not ok]
    if bad:
        raise ValueError("the page's layout changed, choose the cuts again: " + ", ".join(bad))
    page_rows = height - FROM
    out = Image.new("RGB", (width, HEADER + page_rows), background)
    out.paste(rgb.crop((NAV, 0, width, HEADER)), (NAV, 0))
    out.paste(rgb.crop((NAV, FROM, width, height)), (NAV, HEADER))
    nav_rows = out.height - FOOTER
    out.paste(rgb.crop((0, 0, NAV, nav_rows)), (0, 0))
    out.paste(rgb.crop((0, height - FOOTER, NAV, height)), (0, nav_rows))
    return out.convert("RGBA")


# Poster file (docs/images/<file>.jpg) -> (shot name, raw render under --work,
# what to keep of it: a height from the top, or a function making the window).
SOURCES = {
    "exclude": ("L-exclude", "settings/p02-t0.png", scrolled_to_excluded),
}


def capture(exe, work, temp, home):
    work.mkdir(parents=True)
    temp = temp.resolve(strict=True)
    home = home.resolve(strict=True)
    for name in ("Desktop", "Documents", "Downloads", "Pictures", "Videos", "Music"):
        if not (home / name).is_dir():
            raise FileNotFoundError(home / name)
    # The page names this PC's own folders: the six known ones of the user (from
    # their profile, %USERPROFILE%), OneDrive's roots, and %TEMP% in the excluded
    # list. A poster for the README shows neutral ones: the profile folder of
    # the environment is another that exists (C:\Users\Public: Windows' own),
    # OneDrive's variables are left out, and TEMP is a plain folder (C:\Temp).
    # Only this process sees them; no setting, registry value or folder is changed.
    environment = {key: value for key, value in os.environ.items()
                   if key.upper() not in ("ONEDRIVE", "ONEDRIVECOMMERCIAL", "ONEDRIVECONSUMER")}
    environment.update({"USERPROFILE": str(home), "TEMP": str(temp), "TMP": str(temp), "PYTHONDONTWRITEBYTECODE": "1"})
    # Windows and the drivers write their caches (icon cache, D3D shader cache,
    # Qt's pipeline cache) to the profile's AppData\Local, which is now the
    # neutral profile's: what appears there is this run's and is removed again.
    scratch = [(temp, lambda name: name.startswith("WinShun-preview-")),  # the preview's private profile
               (home / "AppData" / "Local", lambda name: name in ("Microsoft", "NVIDIA", "WinShun", "D3DSCache"))]
    before = [{entry.name for entry in folder.iterdir()} if folder.is_dir() else set() for folder, _ in scratch]
    try:
        print("Capturing settings", flush=True)
        subprocess.run([str(exe), "--preview-settings", str(work / "settings"), "--scale", "2",
                        "--renderer", "d3d11"], env=environment, timeout=240).check_returncode()
    finally:
        # Only what the run added, directly inside these folders; never the folders themselves.
        for (folder, ours), known in zip(scratch, before):
            if not folder.is_dir():
                continue
            for entry in folder.iterdir():
                if entry.name not in known and ours(entry.name) and entry.is_dir():
                    if entry.resolve().parent != folder.resolve():
                        raise ValueError(f"{entry} is not directly in {folder}")
                    shutil.rmtree(entry, ignore_errors=True)
    (work / "capture.json").write_text(json.dumps({
        "exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "scale": 2, "renderer": "d3d11", "profiles": "isolated preview fixture; production UI and controllers",
        "temp": str(temp), "userprofile": str(home),
    }, indent=2), encoding="utf-8")


def export(work):
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
            if callable(height):
                rgba = height(rgba)
            elif height:
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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--temp", type=Path, default=Path("C:/Temp"),
                        help="the preview's TEMP, shown in the excluded folders (must exist)")
    parser.add_argument("--home", type=Path, default=Path("C:/Users/Public"),
                        help="the profile folder the page's known folders come from (needs Desktop, Documents, ...)")
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    work = args.work.resolve()
    if args.capture_only:
        if work.exists():
            parser.error("--work must be a fresh directory when capturing")
        capture(args.exe.resolve(strict=True), work, args.temp, args.home)
        return
    export(work)


if __name__ == "__main__":
    main()
