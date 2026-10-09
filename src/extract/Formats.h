#pragma once

// The readers behind extract(), one per family of formats.

#include "Extract.h"
#include "Writer.h"

#include <string>
#include <string_view>

namespace ws::extract {

class Cfb;
class Zip;

Status extractOoxml(Zip& zip, Writer& out, doctext::DocText& doc); // docx, xlsx, pptx
Status extractDoc(Cfb& cfb, Writer& out); // Word 97–2003
Status extractXls(Cfb& cfb, Writer& out, unsigned codePage); // Excel 97–2003, 5.0/95
Status extractPpt(Cfb& cfb, Writer& out); // PowerPoint 97–2003
Status extractPdf(Source& source, Writer& out);
// A text file under a document's name: HTML or XML (an "xls" exported by a
// web system), or plain text (a "csv" saved as xls).
Status extractMarkup(std::string_view bytes, Writer& out, unsigned codePage);
Status extractPlain(std::string_view bytes, Writer& out, unsigned codePage);
bool looksLikeMarkup(std::string_view head); // <html, <?xml, <table...

// Single-byte and double-byte code pages to UTF-16 (MultiByteToWideChar),
// for formats that store text in one.
std::u16string decodeCodePage(std::string_view bytes, unsigned codePage);

} // namespace ws::extract
