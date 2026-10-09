// PDF, through PDFium (pdfium.dll next to the program, loaded on first use).
// Each page becomes one line: PDF text comes as laid-out lines, and a
// sentence broken across two of them must still be found. Lines are joined
// with a space, or without one between Chinese, Japanese or Korean
// characters; the spaces PDFium puts between such characters (spread-out
// headings, justified text) are dropped too.

#include "Formats.h"

#include "fpdf_text.h"
#include "fpdfview.h"

#include <mutex>

namespace ws::extract {

bool loadPdfium()
{
    static const bool loaded = [] {
        // Delay-loaded: present only if pdfium.dll is next to the program.
        if (!::LoadLibraryExW(L"pdfium.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR))
            return false;
        FPDF_InitLibrary();
        return true;
    }();
    return loaded;
}

namespace {

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
    static std::mutex mutex; // PDFium is not thread-safe (only tools read more than one file at once)
    const std::lock_guard lock(mutex);
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

} // namespace ws::extract
