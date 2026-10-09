#pragma once

#include "DocText.h"
#include "Protocol.h"
#include "Source.h"

#include <cstdint>

namespace ws::extract {

using extractproto::Status;

struct Options {
    std::size_t maxText = 16u << 20; // bytes of UTF-8 text; the rest is cut off
    unsigned codePage = 0; // of legacy text with no code page of its own; 0: the system's
    std::uint64_t unpackBudget = 1ull << 30; // bytes taken out of a zip archive in all
};

// The text of a document. What it is comes from its first bytes, not its
// name: a .wps may be an OOXML or a Word 97 file, a .xls an HTML table.
// Reads Office Open XML (docx, xlsx, pptx and their macro and template
// kinds), Word 97–2003, Excel 97–2003 (and 5.0/95), PowerPoint 97–2003,
// PDF, and HTML or plain text under a document's name. Runs untrusted
// parsers on untrusted input: the app calls it only in WinShunExtract.exe,
// a sandboxed process (DocExtractor).
Status extract(Source& source, const Options& options, doctext::DocText& out);

const char* statusName(Status status) noexcept;

// Loads PDFium (pdfium.dll next to the program) if it is not yet: false if
// it cannot be, and PDFs come back Unsupported.
bool loadPdfium();

} // namespace ws::extract
