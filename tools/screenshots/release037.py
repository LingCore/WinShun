"""Redo the settings posters for 0.37.0: the settings window has fewer, clearer
categories (常规, 快捷键, 外观, 搜索范围, 结果列表, 系统整合, 收藏夹, 剪贴板,
插件, AI 助手, 运行状况, 拾穗计划), so every poster cut from a plain settings
page is rendered again with the new side list.

Same route as release036.py (whose capture and cuts it reuses): the release
build's own production QML and controllers render every settings page at 200%
offscreen, on a private preview profile and demo data, with a neutral
USERPROFILE and TEMP. Copy the release's WinShun folder to C:\\WinShun first and
pass that exe: the favorites and AI pages show the exe's own path.

Pages by position (pNN-tM, page NN and tab M from 0): p02 外观, p03 搜索范围,
p05 系统整合 (t0 任务栏), p06 收藏夹, p07 剪贴板 (t1 快捷键), p08 插件 (t1 插件库), p10 运行状况.

  python tools/screenshots/release037.py --exe C:/WinShun/WinShun.exe --work build/release037-shots2 --capture-only
  # look at <work>/settings/*.png, then
  python tools/screenshots/release037.py --exe C:/WinShun/WinShun.exe --work build/release037-shots2 --export-only

Posters whose pictures need other fixtures (file types, quick filters, dialog
rules, double tap, fonts, network and WSL, backup, the settings search, plugin
guide and review, the AI page made by the agent route) keep their earlier pictures.
"""
import sys

sys.dont_write_bytecode = True

import release036
import shots

# Posters that earlier release scripts added to the README set (titles as there).
_known = {entry[0] for entry in shots.README_ONLY}
shots.README_ONLY = tuple(shots.README_ONLY) + tuple(entry for entry in (
    ("admin-paste", "L-admin-paste", "管理员程序里，[照样粘贴]", "索引服务替你按一次粘贴键 · CAD 和旧式对话框也能跳转"),
    ("taskbar-side", "L-taskbar-side", "任务栏搜索框，[位置你来定]", "自动避开任务栏图标，或者始终停在托盘旁"),
    ("health", "L-health", "运行状况，[一目了然]", "内存花在哪 · 正在做什么、还要多久 · 一键归还"),
) if entry[0] not in _known)
# The appearance poster's title since 0.23.0 (release023.py).
shots.README_ONLY = tuple(
    (entry[0], entry[1], "界面风格，[四种任选]", "精修原生 · 品牌色彩 · 轻拟物质感 · 液态玻璃") if entry[0] == "appearance" else entry
    for entry in shots.README_ONLY)

# The side list is two rows longer than in 0.36.0: it ends at 1298 (2x), so the
# poster keeps more of the page (from the gap at 2180) and checks row 1300.
release036.FROM = 2180
release036.SIDE_END = 1300

release036.SOURCES = {
    "appearance": ("L-appearance", "settings/p02-t0.png", 1556),
    "search-scope": ("L-search-scope", "settings/p03-t0.png", 1440),
    "exclude": ("L-exclude", "settings/p03-t0.png", release036.scrolled_to_excluded),
    "taskbar-side": ("L-taskbar-side", "settings/p05-t0.png", None),
    "droppie": ("L-droppie", "settings/p06-t0.png", None),
    "admin-paste": ("L-admin-paste", "settings/p07-t1.png", 1440),
    "plugin-library": ("L-plugin-library", "settings/p08-t1.png", 1440),
    "health": ("L-health", "settings/p10-t0.png", 1440),
}

if __name__ == "__main__":
    release036.main()
