"""Export the posters 0.27.0 adds, from production previews of its package.

The raw pictures come from runs of the deployed WinShun.exe and the capture
plugin's WinShunCapture.exe (isolated temporary profiles, --scale 2
--renderer d3d11, QT_FONT_DPI=96) into one --work directory:

  cmd    WinShun.exe --preview-results <work>/cmd --search-experience
  pure   WinShun.exe --preview-results <work>/pure --pure-search
  size   WinShunCapture.exe --preview-capture-tools <work>/size --app-dir <WinShun folder> --selection-size

size-crop.png is size/size-panel.png cut to what is drawn on the fixture's
plain backdrop, with a margin of 70 px (the fixture is a whole screen).

QT_FONT_DPI=96: the previews turn Qt's high-DPI scaling off, so an icon,
sized in points, follows the primary display's font DPI; on a display that
reports more than 96 the icons come out too large in the pictures (the real
windows, with the scaling on, do not).

Three posters are new: a capture of a typed size, a command that finds files
with the words after its keyword, and the scope tabs as one button. Without a
poster: regular expressions in Content search, the archivers' extraction
windows (a real program's window), a library plugin's README (the fixture
lists sample plugins) and typing on the desktop.
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

import release017
import shots

SOURCES = {
    "capture-size": ("L-capture-size", "size-crop.png", None),
    "command-files": ("L-command-files", "cmd/command-files.png", None),
    "scope-button": ("L-scope-button", "pure/scope-key.png", None),
}

POSTERS = (
    ("capture-size", "L-capture-size", "截图，[指定大小]", "按 F 输入宽高，锁定比例，常用尺寸一点就选"),
    ("command-files", "L-command-files", "自己的命令，[先搜再开]", "关键词后面加几个字，搜到的文件回车就交给程序"),
    ("scope-button", "L-scope-button", "范围标签，[合成一个]", "点一下或按 Tab 切到下一个范围，搜索框更清爽"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    release017.SOURCES = SOURCES
    shots.README_ONLY = shots.README_ONLY + POSTERS
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()


if __name__ == "__main__":
    main()
