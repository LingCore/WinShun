"""Exercise the real OCR plugin with generated documents and isolated profiles.

Creates its own fixtures under --output; never opens or edits user documents.
Requires Pillow, python-docx, python-pptx, openpyxl and reportlab in the test
environment only. The shipped plugin has no Python dependency.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from PIL import Image, ImageDraw, ImageFont
from docx import Document
from docx.shared import Inches
from openpyxl import Workbook
from openpyxl.drawing.image import Image as SheetImage
from pptx import Presentation
from pptx.util import Inches as PptInches
from reportlab.pdfgen import canvas


def fixtures(folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    picture = Image.new("RGB", (1200, 480), "white")
    draw = ImageDraw.Draw(picture)
    font = ImageFont.truetype(str(Path(os.environ["WINDIR"]) / "Fonts/msyh.ttc"), 56)
    draw.text((56, 45), "年度报销规定", font=font, fill="#202020")
    draw.text((56, 150), "INVOICE 731905", font=font, fill="#202020")
    draw.text((56, 255), "差旅费用 123.45 元", font=font, fill="#202020")
    image = folder / "01-invoice.png"
    picture.save(image)
    shutil.copyfile(image, folder / "02-copy.png")
    picture.save(folder / "09-multipage.tiff", save_all=True, append_images=[picture])

    document = Document()
    document.add_heading("Travel expenses", 0)
    document.add_paragraph("This paragraph is native text. The invoice below is an image.")
    document.add_picture(str(image), width=Inches(6))
    document.sections[0].header.paragraphs[0].add_run().add_picture(str(image), width=Inches(2))
    document.save(folder / "03-expenses.docx")

    presentation = Presentation()
    for number in range(12):
        slide = presentation.slides.add_slide(presentation.slide_layouts[6])
        if number == 11:
            slide.shapes.add_picture(str(image), PptInches(0.5), PptInches(1), width=PptInches(8))
    presentation.save(folder / "04-expenses.pptx")

    workbook = Workbook()
    workbook.active.title = "差旅费用"
    workbook.active["A1"] = "Native spreadsheet text"
    workbook.active.add_image(SheetImage(str(image)), "B3")
    workbook.save(folder / "05-expenses.xlsx")

    pdf = canvas.Canvas(str(folder / "06-scanned-and-mixed.pdf"))
    pdf.drawImage(str(image), 40, 400, width=510, height=204)
    pdf.showPage()
    pdf.drawString(40, 740, "This page includes native text AND an invoice image.")
    pdf.drawImage(str(image), 40, 400, width=510, height=204)
    pdf.save()

    (folder / "07-broken.pdf").write_bytes(b"not a PDF")
    (folder / "08-legacy.doc").write_bytes(b"legacy fixture, explicitly unsupported")
    excluded = folder / "excluded"
    excluded.mkdir(exist_ok=True)
    shutil.copyfile(image, excluded / "excluded-invoice.png")


def main() -> None:
    import ctypes
    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    native_scale = ctypes.windll.user32.GetDpiForSystem() / 96
    parser = argparse.ArgumentParser()
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--app-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    plugin, app_dir, output = args.plugin.resolve(), args.app_dir.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sources = output / "fixtures"
    fixtures(sources)
    before = {str(p.relative_to(sources)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in sources.rglob("*") if p.is_file()}
    reports = {}
    for theme, locale, scale, width, height, software in [
        ("light", "zh", "1", 1080, 720, False),
        ("dark", "zh", "1.5", 1080, 720, False),
        ("light", "en", "2", 1080, 720, False),
        ("dark", "en", "1.5", 820, 540, False),
        ("light", "zh", "1.5", 1080, 720, True),
    ]:
        name = f"{theme}-{locale}-{scale}-{width}"
        if software:
            name += "-software"
        env = os.environ.copy()
        env["QT_FORCE_STDERR_LOGGING"] = "1"
        env.pop("QT_SCREEN_SCALE_FACTORS", None)
        env["QT_SCALE_FACTOR"] = str(float(scale) / native_scale)
        destination = output / name
        command = [str(plugin), "--preview-ocr-search", "--app-dir", str(app_dir),
                   "--data-dir", str(output / "profile"), "--folder", str(sources),
                   "--exclude-folder", str(sources / "excluded"), "--query", "报销",
                   "--output", str(destination), "--index", "--theme", theme, "--locale", locale,
                   "--width", str(width), "--height", str(height)]
        if software:
            command.append("--software")
        result = subprocess.run(command, env=env, capture_output=True, timeout=150)
        destination.mkdir(parents=True, exist_ok=True)
        (destination / "runtime.log").write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"{name}: exit {result.returncode}: {result.stderr.decode('utf-8', 'replace')}")
        report = json.loads((destination / "report.json").read_text(encoding="utf-8"))
        assert report["screenshot"], name
        assert abs(report["devicePixelRatio"] - float(scale)) < 0.01, (name, report["devicePixelRatio"])
        assert (report["width"], report["height"]) == (width, height), name
        for control in report["controls"].values():
            assert control["x"] >= 0 and control["y"] >= 0, (name, control)
            assert control["x"] + control["width"] <= width + 1, (name, control)
            assert control["y"] + control["height"] <= height + 1, (name, control)
        assert report["controls"]["ocrPreviewImage"]["width"] >= 180, (name, "preview too small")
        assert report["controls"]["ocrPreviewImage"]["height"] >= 70, (name, "preview too small")
        assert len(report["results"]) == 7, (name, report)
        assert report["preview"]["boxes"], (name, "missing highlight")
        assert not report["preview"]["loading"]
        assert not report["preview"].get("error"), report["preview"]
        assert report["statistics"].get("failed") == 1, report["statistics"]
        assert report["statistics"].get("unsupported") == 1, report["statistics"]
        assert report["statistics"]["assets"] < report["statistics"]["pages"], report["statistics"]
        assert all("excluded" not in hit["path"] for hit in report["results"])
        assert report["statistics"]["bytes"] < 4 * 1024 * 1024, report["statistics"]
        warnings = (result.stdout + result.stderr).decode("utf-8", "replace")
        assert "qrc:/" not in warnings and "ReferenceError" not in warnings, warnings
        reports[name] = report
    after = {str(p.relative_to(sources)): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in sources.rglob("*") if p.is_file()}
    assert before == after, "A source file changed during read-only indexing"
    summary = {"plugin_sha256": hashlib.sha256(plugin.read_bytes()).hexdigest(),
               "dll_sha256": hashlib.sha256(plugin.with_name("WinShunOcrSearchApp.dll").read_bytes()).hexdigest(),
               "source_files_unchanged": True, "runs": reports}
    (output / "verification.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({name: {"results": len(r["results"]), "statistics": r["statistics"]}
                      for name, r in reports.items()}, ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
