#include "Extract.h"

#include "Cfb.h"
#include "Formats.h"
#include "Zip.h"

#include <cstring>

namespace ws::extract {

namespace {

constexpr std::size_t kMaxTextFile = 64u << 20; // an HTML or text file under a document's name

bool isText(std::string_view head)
{
    if (head.size() >= 2
        && ((head[0] == '\xFF' && head[1] == '\xFE') || (head[0] == '\xFE' && head[1] == '\xFF')))
        return true; // UTF-16
    return std::memchr(head.data(), 0, head.size()) == nullptr;
}

} // namespace

const char* statusName(Status status) noexcept
{
    switch (status) {
    case Status::Ok:
        return "ok";
    case Status::NoText:
        return "no text";
    case Status::Encrypted:
        return "encrypted";
    case Status::Unsupported:
        return "unsupported";
    case Status::Broken:
        return "broken";
    case Status::Failed:
        return "failed";
    }
    return "?";
}

Status extract(Source& source, const Options& options, doctext::DocText& out)
{
    out = {};
    Writer writer(out, options.maxText);
    const unsigned codePage = options.codePage != 0 ? options.codePage : ::GetACP();
    char buffer[2048];
    const std::size_t got = source.read(0, buffer, sizeof buffer);
    const std::string_view head(buffer, got);

    Status status = Status::Unsupported;
    if (head.starts_with("PK\x03\x04")) {
        Zip zip(source, options.unpackBudget);
        status = zip.ok() ? extractOoxml(zip, writer, out) : Status::Broken;
    } else if (Cfb::isCfb(head)) {
        Cfb cfb(source);
        if (!cfb.ok())
            status = Status::Broken;
        else if (cfb.has(u"EncryptedPackage"))
            status = Status::Encrypted; // Office Open XML with a password
        else if (cfb.has(u"WordDocument"))
            status = extractDoc(cfb, writer);
        else if (cfb.has(u"Workbook") || cfb.has(u"Book"))
            status = extractXls(cfb, writer, codePage);
        else if (cfb.has(u"PowerPoint Document"))
            status = extractPpt(cfb, writer);
    } else if (head.substr(0, 1024).find("%PDF-") != std::string_view::npos) {
        status = extractPdf(source, writer);
    } else if (isText(head)) {
        std::string bytes(static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), kMaxTextFile)), '\0');
        bytes.resize(source.read(0, bytes.data(), bytes.size()));
        status = looksLikeMarkup(head) ? extractMarkup(bytes, writer, codePage) : extractPlain(bytes, writer, codePage);
    }
    writer.finish();
    // What was read before a part turned out broken still counts.
    if (!out.text.empty())
        return Status::Ok;
    if (status == Status::Ok)
        return Status::NoText;
    out = {};
    return status;
}

} // namespace ws::extract
