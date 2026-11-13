"""Export the posters 0.26.0 adds, from production previews of its package.

The raw pictures come from four runs of the deployed WinShun.exe (isolated
temporary profiles, --scale 2 --renderer d3d11) into one --work directory:

  gallery   --preview-results <work>/gallery --file-metadata --gallery-image scenes/区域销售额.png
  apps      --preview-results <work>/apps --apps-grid
  dialogs   --preview-settings <work>/dialogs --dialog-rule-settings --fixture tests/fixtures/wpf-file-window.ps1
  settings  --preview-settings <work>/settings

Four posters are new: F2 rename and the coloured preview (owed since 0.25.0),
a program's own file window learnt in the settings, and the Apps tab. The Apps
tab's picture lists the apps installed on the PC it is taken on (--apps), so it
is exported only when asked for. Still without a poster: the cloud backup
plugin (needs a server and an account), clipboard notes (the fixture's one row
is a file that does not exist) and the panel's quick-filter chips.
"""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True

import release017
import shots

SOURCES = {
    "rename": ("L-rename", "gallery/rename-box.png", None),
    "preview-colors": ("L-preview-colors", "gallery/preview-markdown.png", None),
    "dialog-rules": ("L-dialog-rules", "dialogs/dialog-rule-learned.png", None),
}
APPS = {"apps-grid": ("L-apps-grid", "apps/apps-all-paged.png", None)}

POSTERS = (
    ("rename", "L-rename", "按 F2，[就地改名]", "扩展名前面的部分已选好，Enter 生效，Ctrl+Z 可撤销"),
    ("preview-colors", "L-preview-colors", "预览文档，[语法上色]", "代码、Markdown、JSON 按语法着色，搜索的词加粗标出"),
    ("dialog-rules", "L-dialog-rules", "自绘窗口，[教一次就会]", "程序自己画的选文件窗口，点一次“识别窗口”就能搜能转"),
    ("apps-grid", "L-apps-grid", "应用排成一屏，[一眼找到]", "常用、收藏、全部，方向键选，点星标收藏"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--apps", action="store_true", help="also the Apps tab's poster (it lists this PC's apps)")
    args = parser.parse_args()
    release017.SOURCES = {**SOURCES, **(APPS if args.apps else {})}
    shots.README_ONLY = shots.README_ONLY + tuple(p for p in POSTERS if p[0] in release017.SOURCES)
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()


if __name__ == "__main__":
    main()
