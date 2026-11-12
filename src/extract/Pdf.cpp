// PDF, through PDFium (pdfium.dll in Win顺's folder, loaded on first use).
// Each page becomes one line: PDF text comes as laid-out lines, and a
// sentence broken across two of them must still be found. Lines are joined
// with a space, or without one between Chinese, Japanese or Korean
// characters; the spaces PDFium puts between such characters (spread-out
// headings, justified text) are dropped too.

#include "Formats.h"
#include "Render.h"

#include "fpdf_text.h"
#include "fpdfview.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace ws::extract {

bool loadPdfium()
{
    static const bool loaded = [] {
        // Delay-loaded, so a program starts without it. Looked for beside the
        // program (Win顺, WinShunExtract) and in the folders the process added
        // with AddDllDirectory: a library plugin lives in the user's plugins
        // folder, and its stub adds Win顺's. Never PATH or the current folder.
        if (!::LoadLibraryExW(
                L"pdfium.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS))
            return false;
        FPDF_InitLibrary();
        return true;
    }();
    return loaded;
}

namespace {

std::mutex g_pdfium; // PDFium is not thread-safe (only tools read more than one file at once)

void putPage(std::u16string_view text, Writer& out)
{
    bool gap = false; // whitespace or a line end since the last character
    bool softHyphen = false; // the line ended in a word broken with a hyphen
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            ++i;
        }
        if (c == 0xFFFE || c == 0x0002) {
            softHyphen = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xA0 || c == 0x3000) {
            if (!(softHyphen && (c == '\r' || c == '\n')))
                gap = true;
            continue;
        }
        if (gap && !(isCjk(out.last()) && isCjk(c)))
            out.space();
        gap = false;
        softHyphen = false;
        out.put(c);
    }
}

} // namespace

Status extractPdf(Source& source, Writer& out)
{
    const std::lock_guard lock(g_pdfium);
    if (!loadPdfium())
        return Status::Unsupported;
    if (source.size() > 0xFFFFFFFFu)
        return Status::Unsupported; // FPDF_FILEACCESS takes a 32-bit length

    FPDF_FILEACCESS access {};
    access.m_FileLen = static_cast<unsigned long>(source.size());
    access.m_GetBlock = [](void* param, unsigned long position, unsigned char* buffer, unsigned long size) -> int {
        return static_cast<Source*>(param)->readExact(position, buffer, size) ? 1 : 0;
    };
    access.m_Param = &source;
    FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(&access, nullptr);
    if (!doc) {
        const unsigned long error = FPDF_GetLastError();
        return error == FPDF_ERR_PASSWORD || error == FPDF_ERR_SECURITY ? Status::Encrypted : Status::Broken;
    }
    const int pages = FPDF_GetPageCount(doc);
    std::u16string text;
    for (int i = 0; i < pages && !out.full(); ++i) {
        FPDF_PAGE page = FPDF_LoadPage(doc, i);
        if (!page)
            continue;
        if (FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page)) {
            const int count = FPDFText_CountChars(textPage);
            if (count > 0) {
                text.resize(static_cast<std::size_t>(count) + 1);
                const int got
                    = FPDFText_GetText(textPage, 0, count, reinterpret_cast<unsigned short*>(text.data()));
                text.resize(got > 0 ? static_cast<std::size_t>(got - 1) : 0); // without the terminating zero
                out.place(doctext::PlaceKind::Page, static_cast<std::uint32_t>(i + 1));
                putPage(text, out);
                out.newline();
            }
            FPDFText_ClosePage(textPage);
        }
        FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(doc);
    return Status::Ok;
}

// The first page as a picture. An Illustrator drawing is often small on a
// large artboard (a bag's pattern on a sheet of A1): the page is drawn small
// first to see where anything is, and when that is well inside it, only
// that part is drawn, as large as asked for.
Status renderPdf(Source& source, std::uint32_t largest, Picture& out)
{
    const std::lock_guard lock(g_pdfium);
    if (!loadPdfium())
        return Status::Unsupported;
    if (source.size() > 0xFFFFFFFFu)
        return Status::Unsupported;
    largest = std::clamp<std::uint32_t>(largest, 64, 8192);

    FPDF_FILEACCESS access {};
    access.m_FileLen = static_cast<unsigned long>(source.size());
    access.m_GetBlock = [](void* param, unsigned long position, unsigned char* buffer, unsigned long size) -> int {
        return static_cast<Source*>(param)->readExact(position, buffer, size) ? 1 : 0;
    };
    access.m_Param = &source;
    FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(&access, nullptr);
    if (!doc) {
        const unsigned long error = FPDF_GetLastError();
        return error == FPDF_ERR_PASSWORD || error == FPDF_ERR_SECURITY ? Status::Encrypted : Status::Broken;
    }
    Status status = Status::Broken;
    if (FPDF_PAGE page = FPDF_LoadPage(doc, 0)) {
        const double pageWidth = FPDF_GetPageWidthF(page);
        const double pageHeight = FPDF_GetPageHeightF(page);
        // The page at `scale` pixels a point, the part from (left, top) of
        // the size given: white paper, then what is drawn.
        const auto draw = [&](double scale, int left, int top, int width, int height, std::string& pixels) {
            FPDF_BITMAP bitmap = FPDFBitmap_Create(width, height, 0);
            if (!bitmap)
                return false;
            FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);
            FPDF_RenderPageBitmap(bitmap, page, -left, -top, std::max(1, static_cast<int>(std::lround(pageWidth * scale))),
                std::max(1, static_cast<int>(std::lround(pageHeight * scale))), 0, FPDF_ANNOT);
            const auto* from = static_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
            const int stride = FPDFBitmap_GetStride(bitmap);
            pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
            for (int y = 0; y < height; ++y) {
                auto* to = reinterpret_cast<unsigned char*>(pixels.data()) + static_cast<std::size_t>(y) * width * 4;
                std::copy_n(from + static_cast<std::size_t>(y) * stride, static_cast<std::size_t>(width) * 4, to);
                for (int x = 0; x < width; ++x)
                    to[x * 4 + 3] = 255;
            }
            FPDFBitmap_Destroy(bitmap);
            return true;
        };
        if (pageWidth >= 1 && pageHeight >= 1 && pageWidth <= 200000 && pageHeight <= 200000) {
            const double longer = std::max(pageWidth, pageHeight);
            // Where anything is, in the page drawn 400 pixels long.
            constexpr int kLook = 400;
            const double lookScale = kLook / longer;
            const int lookWidth = std::max(1, static_cast<int>(std::lround(pageWidth * lookScale)));
            const int lookHeight = std::max(1, static_cast<int>(std::lround(pageHeight * lookScale)));
            double left = 0; // the part to draw, in points
            double top = 0;
            double right = pageWidth;
            double bottom = pageHeight;
            std::string look;
            if (draw(lookScale, 0, 0, lookWidth, lookHeight, look)) {
                int x0 = lookWidth;
                int y0 = lookHeight;
                int x1 = -1;
                int y1 = -1;
                for (int y = 0; y < lookHeight; ++y) {
                    const auto* p = reinterpret_cast<const unsigned char*>(look.data()) + static_cast<std::size_t>(y) * lookWidth * 4;
                    for (int x = 0; x < lookWidth; ++x, p += 4) {
                        if (p[0] < 245 || p[1] < 245 || p[2] < 245) {
                            x0 = std::min(x0, x);
                            x1 = std::max(x1, x);
                            y0 = std::min(y0, y);
                            y1 = std::max(y1, y);
                        }
                    }
                }
                // Only when the paper left out is worth it: what is drawn
                // takes less than 70% of the page one way or the other.
                if (x1 >= x0 && (x1 - x0 + 1 < lookWidth * 7 / 10 || y1 - y0 + 1 < lookHeight * 7 / 10)) {
                    const int margin = std::max(4, std::max(x1 - x0, y1 - y0) / 25); // a little paper around it
                    left = std::max(0, x0 - margin) / lookScale;
                    top = std::max(0, y0 - margin) / lookScale;
                    right = std::min(lookWidth, x1 + 1 + margin) / lookScale;
                    bottom = std::min(lookHeight, y1 + 1 + margin) / lookScale;
                }
            }
            // As large as asked for, but no finer than 600 dots an inch.
            const double scale = std::min(largest / std::max(right - left, bottom - top), 600.0 / 72);
            const int width = std::clamp(static_cast<int>(std::lround((right - left) * scale)), 1, static_cast<int>(largest));
            const int height = std::clamp(static_cast<int>(std::lround((bottom - top) * scale)), 1, static_cast<int>(largest));
            if (draw(scale, static_cast<int>(std::lround(left * scale)), static_cast<int>(std::lround(top * scale)), width, height, out.bgra)) {
                out.width = static_cast<std::uint32_t>(width);
                out.height = static_cast<std::uint32_t>(height);
                out.fullWidth = static_cast<std::uint32_t>(std::lround(pageWidth));
                out.fullHeight = static_cast<std::uint32_t>(std::lround(pageHeight));
                status = Status::Ok;
            }
        }
        FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(doc);
    return status;
}

} // namespace ws::extract
