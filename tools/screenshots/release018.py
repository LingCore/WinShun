"""Capture 0.18 with the existing isolated previews and square poster template.

Run --capture-only first. Add web/web-plugin-settings.png with
tools/verify_web_plugin.py, plugin-library.png with
--preview-plugins, and ocr/window.png with the OCR plugin's isolated preview.
Inspect the raw renders, then use --export-only with the same --work.
"""
import release017
import shots


release017.SOURCES = {
    name: source for name, source in release017.SOURCES.items()
    if name not in ("onboarding", "foldermenu")
}
release017.SOURCES.update({
    "web": ("L-web", "web/web-plugin-settings.png", None),
    "plugin-library": ("L-plugin-library", "plugin-library.png", None),
    "ocr-search": ("L-ocr-search", "ocr/window.png", None),
})
shots.PAGES = tuple(
    (file, "L-web" if file == "07-web" else source, *rest)
    for file, source, *rest in shots.PAGES
)
shots.README_ONLY += (("ocr-search", "L-ocr-search", "图片文字搜索，[离线识别]", "图片、扫描 PDF 和文档图片 · 增量索引 · 高亮预览"),)


if __name__ == "__main__":
    release017.main()
