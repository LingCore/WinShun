"""Capture the 0.45.0 recording panel and export the poster this release adds.

New: four recording qualities, 15/30/60 frames a second and the cursor trail
(docs/images/recording-ultra.jpg). The shot is the screenshot plugin's real
recording panel, taken by its --preview-recording run: it opens its own demo
window, selects it as a real capture would, opens the options and picks 超清
(options-ultra.png), then records a few seconds of that window into the work
directory. --auto runs the steps by itself (without it the panel waits for
you); --trail starts with 鼠标拖尾 on, --fps 60 with 60 frames a second.

Run the plugin from the release deployment (its Qt files come from --app-dir),
into a fresh --work directory on a path without the project's folder name (the
recording lands there). The demo window and the capture overlay are on the
screen for a few seconds; leave the mouse alone meanwhile.

  python tools/screenshots/release045.py --exe <dist>/WinShun/library-plugins/winshun.capture/WinShunCapture.exe --app-dir <dist>/WinShun --work C:/wsposter045 --capture-only
  # look at <work>/rec/options-ultra.png, then
  python tools/screenshots/release045.py --exe ... --app-dir ... --work C:/wsposter045 --export-only
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

import poster_pipeline

SOURCES = {
    # Cut above 保存到: its row shows the work directory the preview records into.
    "recording-ultra": ("L-recording-ultra", "rec/options-ultra.png", 290),
}

POSTERS = (
    ("recording-ultra", "L-recording-ultra", "录屏更清楚，[超清 60 帧]", "四档画质，帧率 15 / 30 / 60，还能加鼠标拖尾"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--app-dir", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    if args.capture_only:
        if args.work.resolve().exists():
            parser.error("--work must be a fresh directory")
        if not args.app_dir:
            parser.error("--app-dir is needed to capture")
        runs = (("rec", "--preview-recording", ["--auto", "--trail", "--fps", "60", "--app-dir", str(args.app_dir.resolve())]),)
        poster_pipeline.capture_previews(args.exe, args.work, runs)
        return
    poster_pipeline.export_posters(args.work, SOURCES, POSTERS)


if __name__ == "__main__":
    main()
