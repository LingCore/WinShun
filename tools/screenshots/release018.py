"""Capture 0.18 with the existing isolated previews and square poster template.

Run --capture-only first. Add web/web-plugin-settings.png with
tools/verify_web_plugin.py and ocr/window.png with the OCR plugin's isolated
preview. The gallery uses the captured settings/p09-t1.png.
Capture shortcuts/ with --preview-results --shortcut-panel.
Inspect the raw renders, then use --export-only with the same --work.
"""
import release017
import shots


release017.SOURCES = {
    name: source for name, source in release017.SOURCES.items()
    if name not in ("onboarding", "foldermenu")
}
release017.SOURCES.update({
    "appearance": ("L-appearance", "settings/p01-t0.png", 1440),
    "droppie": ("L-droppie", "settings/p05-t0.png", None),
    "backup": ("L-backup", "settings/p07-t0.png", 1440),
    "web": ("L-web", "web/web-plugin-settings.png", None),
    "plugin-library": ("L-plugin-library", "settings/p09-t1.png", 1600),
    "ocr-search": ("L-ocr-search", "ocr/window.png", None),
    "shortcuts": ("L-shortcuts", "shortcuts/shortcut-panel-launcher.png", None),
})
shots.PAGES = tuple(
    (file, "L-web" if file == "07-web" else source, *rest)
    for file, source, *rest in shots.PAGES
)
shots.README_ONLY += (
    ("ocr-search", "L-ocr-search", "图片文字搜索，[离线识别]", "图片、扫描 PDF 和文档图片 · 增量索引 · 高亮预览"),
    ("shortcuts", "L-shortcuts", "快捷键[总览]", "搜索窗口查看当前操作 · 设置中查看全部快捷键"),
)


if __name__ == "__main__":
    release017.main()
