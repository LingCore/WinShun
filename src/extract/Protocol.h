#pragma once

// How the app talks to WinShunExtract.exe, the process that reads documents
// (see DocExtractor in src/core). Over a pipe: once started, the extractor
// says it is Ready; then one request at a time, the app sends a Request with
// a read-only handle to the file already duplicated into the extractor, and
// the extractor answers with a ResponseHeader followed by `payloadBytes` of
// doctext::serialize() output. Qt-free.

#include <cstdint>

namespace ws::extractproto {

inline constexpr std::uint32_t kRequestMagic = 0x51525857; // "WXRQ"
inline constexpr std::uint32_t kResponseMagic = 0x53525857; // "WXRS"
inline constexpr std::uint32_t kReadyMagic = 0x59525857; // "WXRY"

enum class Status : std::uint8_t {
    Ok, // text found
    NoText, // read, nothing to find (a scanned PDF, an empty sheet)
    Encrypted, // needs a password
    Unsupported, // not a format it reads (Word 95, a private WPS format)
    Broken, // not what its type says, or damaged
    Failed, // (the app's verdict) the extractor crashed, ran out of time or memory
};

struct Ready {
    std::uint32_t magic = kReadyMagic;
    std::uint32_t flags = 0; // kPdf
    static constexpr std::uint32_t kPdf = 1; // PDFium loaded
};

struct Request {
    std::uint32_t magic = kRequestMagic;
    std::uint32_t codePage = 0; // of legacy text with no code page of its own (GBK on Chinese Windows)
    std::uint64_t file = 0; // HANDLE, valid in the extractor
    std::uint64_t fileSize = 0;
    std::uint64_t maxText = 0; // bytes of UTF-8
};

struct ResponseHeader {
    std::uint32_t magic = kResponseMagic;
    Status status = Status::Broken;
    std::uint8_t reserved[3] = {};
    std::uint64_t payloadBytes = 0;
};

static_assert(sizeof(Ready) == 8);
static_assert(sizeof(Request) == 32);
static_assert(sizeof(ResponseHeader) == 16);

} // namespace ws::extractproto
