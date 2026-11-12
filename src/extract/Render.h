#pragma once

#include "Protocol.h"
#include "Source.h"

#include <cstdint>
#include <string>

namespace ws::extract {

using extractproto::Status;

// A file drawn as a picture, for the preview (GitHub #297): what it is comes
// from its first bytes. Reads
//  - PDF, and so Adobe Illustrator's .ai as saved with its PDF part (the
//    default): the first page, through PDFium, without the empty paper around
//    what is drawn when that is most of the page;
//  - Photoshop's .psd and .psb: the flattened picture stored after the
//    layers (8 or 16 bits; RGB, CMYK, grey, indexed).
// Untrusted parsers on untrusted input: the app calls it only in
// WinShunExtract.exe (DocExtractor::render).
struct Picture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fullWidth = 0; // of the page in points, or of the picture as stored
    std::uint32_t fullHeight = 0;
    std::string bgra; // width * height * 4, opaque, top row first
};

// `largest`: pixels on the longer side at most (64..8192).
Status render(Source& source, std::uint32_t largest, Picture& out);

Status renderPdf(Source& source, std::uint32_t largest, Picture& out); // Pdf.cpp
Status renderPsd(Source& source, std::uint32_t largest, Picture& out); // Psd.cpp

} // namespace ws::extract
