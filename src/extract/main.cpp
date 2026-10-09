// WinShunExtract.exe: reads the text out of documents for WinShun's content
// index and content search. WinShun runs as administrator (it reads the
// MFT); document parsers are large and the documents untrusted, so they run
// here instead: started by WinShun (DocExtractor) with a restricted token at
// low integrity, in a job that caps its memory and lets it start nothing.
// It opens no files itself: each request comes with a handle to the file.
//
//   WinShunExtract.exe --pipe <handle>
//
// Requests and answers go over the pipe (Protocol.h), one at a time, until
// WinShun closes it.

#include "DocText.h"
#include "Extract.h"
#include "Protocol.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>

namespace {

bool readExact(HANDLE pipe, void* buffer, std::size_t n)
{
    auto* p = static_cast<char*>(buffer);
    while (n > 0) {
        DWORD got = 0;
        if (!::ReadFile(pipe, p, static_cast<DWORD>(std::min<std::size_t>(n, 1u << 20)), &got, nullptr) || got == 0)
            return false;
        p += got;
        n -= got;
    }
    return true;
}

bool writeExact(HANDLE pipe, const void* buffer, std::size_t n)
{
    const auto* p = static_cast<const char*>(buffer);
    while (n > 0) {
        DWORD put = 0;
        if (!::WriteFile(pipe, p, static_cast<DWORD>(std::min<std::size_t>(n, 1u << 20)), &put, nullptr) || put == 0)
            return false;
        p += put;
        n -= put;
    }
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    // A crash ends the process quietly: WinShun sees the pipe break.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    const wchar_t* arg = std::wcsstr(commandLine, L"--pipe ");
    if (!arg)
        return 2;
    const auto pipe = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::wcstoull(arg + 7, nullptr, 16)));

    ws::extractproto::Ready ready;
    if (ws::extract::loadPdfium())
        ready.flags |= ws::extractproto::Ready::kPdf;
    if (!writeExact(pipe, &ready, sizeof ready))
        return 0;

    for (;;) {
        ws::extractproto::Request request;
        if (!readExact(pipe, &request, sizeof request))
            return 0; // WinShun closed the pipe
        if (request.magic != ws::extractproto::kRequestMagic)
            return 3;
        const auto file = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(request.file));
        ws::extract::Options options;
        options.maxText = static_cast<std::size_t>(std::min<std::uint64_t>(request.maxText, 256u << 20));
        options.codePage = request.codePage;
        ws::doctext::DocText text;
        ws::extract::HandleSource source(file, request.fileSize);
        const ws::extractproto::Status status = ws::extract::extract(source, options, text);
        ::CloseHandle(file);

        const std::string payload = status == ws::extractproto::Status::Ok ? ws::doctext::serialize(text) : std::string();
        ws::extractproto::ResponseHeader header;
        header.status = status;
        header.payloadBytes = payload.size();
        if (!writeExact(pipe, &header, sizeof header) || !writeExact(pipe, payload.data(), payload.size()))
            return 0;
    }
}
