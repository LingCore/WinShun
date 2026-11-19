"""Redo the settings posters for 0.39.0: the settings window's title bar has a
捐赠 button now, 收藏夹 has column headers to sort by (GitHub #397) and the
插件 pages are tiles, so every poster cut from a plain settings page is
rendered again.

Same route as release037.py (release036.py's capture and cuts): the release
build's own production QML and controllers render every settings page at 200%
offscreen, on a private preview profile and demo data, with a neutral
USERPROFILE and TEMP. 收藏夹 is pictured sorted by name, its caret on
(--favorites-by name). Put the release's WinShun folder at C:\\WinShun first (a
copy, or a junction: mklink /J C:\\WinShun <release>\\WinShun) and pass that
exe: the favorites and AI pages show the exe's own path.

Pages by position (pNN-tM, page NN and tab M from 0): p02 外观, p03 搜索范围,
p05 系统整合 (t0 任务栏), p06 收藏夹, p07 剪贴板 (t1 快捷键), p08 插件 (t1 插件库), p10 运行状况.

  python tools/screenshots/release039.py --exe C:/WinShun/WinShun.exe --work build/release039-shots --capture-only
  # look at <work>/settings/*.png, then
  python tools/screenshots/release039.py --exe C:/WinShun/WinShun.exe --work build/release039-shots --export-only

Posters whose pictures need other fixtures keep their earlier pictures (see
release037.py).
"""
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

import release036
import release037  # noqa: F401  (its README titles, cuts and sources)
import shots

# The favorites poster says what the page has now.
shots.README_ONLY = tuple(
    (entry[0], entry[1], "收藏夹，[点列头排好]", "名称、日期、类型、大小一键排序 · 拖文件按 ` 弹出圆盘")
    if entry[0] == "droppie" else entry
    for entry in shots.README_ONLY)

# release036's capture runs the whole settings preview; 收藏夹 is to be sorted.
# It also resolves --exe, which turns a C:\WinShun junction back into the
# release folder; start the exe by the path given so the pages show C:\WinShun.
_run = subprocess.run
_given_exe = None
if "--exe" in sys.argv[:-1]:
    _given_exe = sys.argv[sys.argv.index("--exe") + 1]


def _sorted_favorites(command, *args, **kwargs):
    if isinstance(command, list) and "--preview-settings" in command and "--favorites-by" not in command:
        command = command + ["--favorites-by", "name"]
        if _given_exe:
            command[0] = str(Path(_given_exe).absolute())
    return _run(command, *args, **kwargs)


subprocess.run = _sorted_favorites

if __name__ == "__main__":
    release036.main()
