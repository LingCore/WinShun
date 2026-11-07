"""Capture 0.22.0 production previews and export the existing square posters.

Use a fresh --work for --capture-only, inspect the PNGs and reports, then run
--export-only with that directory. The retained work contains source images,
binary hashes and lossless poster masters. Preview profiles are isolated.
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
import release017
import shots


release017.SOURCES = {
    "panel": ("L-panel", "results/panel-gallery-menu.png", None),
    "clipboard-resize": ("L-clipboard-resize", "results/clipboard-gallery.png", None),
    "appearance": ("L-appearance", "settings/p01-t0.png", 1604),
    "health": ("L-health", "settings/p10-t0.png", 1440),
    "plugin-library": ("L-plugin-library", "settings/p09-t1.png", 1440),
    "foldermenu": ("L-foldermenu", "workflows/folder-direct.png", None),
    "capture": ("L-capture", "toolbar/toolbar-with-style.png", None),
    "query-history": ("L-query-history", "experience/search-history.png", None),
    "merged-search": ("L-merged-search", "experience/merged-scope-prefixes.png", None),
    "pure-search": ("L-pure-search", "pure-combined.png", None),
    "clipboard-groups": ("L-clipboard-groups", "workflows/clipboard-groups.png", None),
    "plugin-guide": ("L-plugin-guide", "experience-settings/plugin-readme.png", None),
    "open-windows": ("L-open-windows", "windows/open-window-results.png", None),
    "interface-font": ("L-interface-font", "fonts/font-picker.png", None),
    "double-tap": ("L-double-tap", "double-tap/double-tap-settings.png", None),
    "native-review": ("L-native-review", "plugin-review/native-confirm.png", None),
}

overrides = {
    "appearance": ("界面风格，[三种任选]", "精修原生 · 品牌色彩 · 轻拟物质感，浅色深色都有"),
    "capture": ("截图工具条，[两行排开]", "常用的一行贴着选区，其余在第二行 · 按住左侧三个点拖动"),
}
shots.README_ONLY = tuple(
    (entry[0], entry[1], *overrides[entry[0]]) if entry[0] in overrides else entry
    for entry in shots.README_ONLY
) + (
    ("health", "L-health", "运行状况，[一目了然]", "内存花在哪 · 正在做什么、还要多久 · 一键归还"),
    ("query-history", "L-query-history", "搜索词，[一键找回]", "时钟按钮查看历史 · Ctrl + 上下键翻阅 · 可删除和清空"),
    ("merged-search", "L-merged-search", "搜索范围，[合在一起]", "全部里直接按类型筛选 · 按住 Ctrl 查看类型关键词"),
    ("pure-search", "L-pure-search", "空输入，[只留搜索栏]", "输入后恢复结果与操作 · 纯净搜索模式按需开启"),
    ("clipboard-groups", "L-clipboard-groups", "剪贴板分组，[打字就找]", "Ctrl + G 查找分组 · Enter 进入 · Esc 返回原查询"),
    ("plugin-guide", "L-plugin-guide", "插件说明，[就在应用里]", "查看随包文档 · 选字复制 · 按 Esc 回到设置"),
    ("open-windows", "L-open-windows", "已经打开，[直接切换]", "按窗口标题或程序名搜索 · Enter 回到那个窗口"),
    ("interface-font", "L-interface-font", "界面字体，[自己选择]", "三种界面风格共用字体 · 字体列表可搜索"),
    ("double-tap", "L-double-tap", "双击修饰键，[动作自己定]", "搜索框、剪贴板或已启用插件动作 · 在快捷键设置中选择"),
    ("native-review", "L-native-review", "插件来源，[安装前看清]", "查看审核状态和发布来源 · 原生社区包逐次确认"),
)


def capture(exe, plugin, work):
    work.mkdir(parents=True)
    temporary = work / "temp"
    temporary.mkdir()
    environment = {**os.environ, "TEMP": str(temporary), "TMP": str(temporary),
                   "PYTHONDONTWRITEBYTECODE": "1"}
    demo = shots.HERE / "scenes/区域销售额.png"
    runs = (
        (exe, "results", "--preview-results", "1.5", ["--file-metadata", "--gallery-image", str(demo)]),
        (exe, "settings", "--preview-settings", "2", []),
        (exe, "workflows", "--preview-results", "2", ["--navigation-workflows"]),
        (exe, "experience", "--preview-results", "2", ["--search-experience"]),
        (exe, "experience-settings", "--preview-settings", "2", ["--search-experience-settings"]),
        (exe, "pure", "--preview-results", "2", ["--pure-search"]),
        (exe, "windows", "--preview-results", "2", ["--window-search-menu"]),
        (exe, "optimization", "--preview-settings", "2", ["--optimization-settings"]),
        (exe, "fonts", "--preview-settings", "2", ["--font-picker"]),
        (exe, "double-tap", "--preview-settings", "2", ["--double-tap-settings"]),
        (exe, "plugin-review", "--preview-settings", "2", ["--plugin-review-settings"]),
        (plugin, "toolbar", "--preview-capture-tools", "2", ["--app-dir", str(exe.parent), "--toolbar-drag"]),
    )
    try:
        for program, folder, flag, scale, extra in runs:
            print("Capturing", folder, flush=True)
            result = subprocess.run([str(program), flag, str(work / folder), "--scale", scale,
                                     "--renderer", "d3d11", *extra], env=environment, timeout=240)
            if result.returncode:
                log = work / folder / "preview.log"
                warnings = [line for line in log.read_text(encoding="utf-8").splitlines()
                            if line.startswith("WARNING:")] if log.is_file() else []
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
    finally:
        if temporary.resolve().parent != work.resolve():
            raise ValueError("Temporary profile path escaped the capture directory")
        shutil.rmtree(temporary)
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    (work / "capture.json").write_text(json.dumps({
        "exe": str(exe), "sha256": digest(exe), "capture": str(plugin),
        "capture_sha256": digest(plugin),
        "capture_app_sha256": digest(plugin.with_name("WinShunCaptureApp.dll")),
        "renderer": "d3d11", "profiles": "isolated production previews",
        "scale": {folder: scale for _, folder, _, scale, _ in runs},
    }, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--capture-only", action="store_true")
    modes.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    if args.capture_only:
        if not args.capture or args.work.exists():
            parser.error("--capture is required and --work must be a fresh directory")
        capture(args.exe.resolve(strict=True), args.capture.resolve(strict=True), args.work.resolve())
        return
    art.stacked([Image.open(args.work / "pure" / name).convert("RGBA")
                 for name in ("pure-empty.png", "pure-results.png")]).save(args.work / "pure-combined.png")
    sys.argv = [sys.argv[0], "--exe", str(args.exe), "--work", str(args.work), "--export-only"]
    release017.main()
    cover = ("cover", (str(args.work / "shots/L-merged-search.png"), "square", "letters"),
             shots.HERE.parent.parent / "docs/images/cover.jpg")
    shots.run(cover)
    shots.make(cover[0], cover[1]).save(args.work / "poster-masters/cover.png")


if __name__ == "__main__":
    main()
