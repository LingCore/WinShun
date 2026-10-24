#include "Encoding.h"

#include "Win.h"

#include <winternl.h>

#include <iterator>

namespace ws::cli {

namespace {

bool equalsIgnoringCase(std::wstring_view a, std::wstring_view b) noexcept
{
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// Code pages for which WideCharToMultiByte takes no flags and no default
// character (its documentation); UTF-8 neither.
bool flagsAllowed(UINT codePage) noexcept
{
    switch (codePage) {
    case CP_UTF8:
    case CP_UTF7:
    case 42: // symbol
    case 50220:
    case 50221:
    case 50222:
    case 50225:
    case 50227:
    case 50229:
    case 52936:
    case kGb18030CodePage:
        return false;
    default:
        return !(codePage >= 57002 && codePage <= 57011);
    }
}

void appendJsonEscape(std::string& out, wchar_t unit)
{
    static constexpr char kHex[] = "0123456789abcdef";
    out += "\\u";
    out += kHex[(unit >> 12) & 0xF];
    out += kHex[(unit >> 8) & 0xF];
    out += kHex[(unit >> 4) & 0xF];
    out += kHex[unit & 0xF];
}

} // namespace

std::string utf8(std::wstring_view text)
{
    if (text.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring wide(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::optional<EncodingChoice> encodingNamed(std::wstring_view name)
{
    if (equalsIgnoringCase(name, L"utf8") || equalsIgnoringCase(name, L"utf-8"))
        return EncodingChoice::Utf8;
    if (equalsIgnoringCase(name, L"utf16") || equalsIgnoringCase(name, L"utf-16") || equalsIgnoringCase(name, L"utf16le")
        || equalsIgnoringCase(name, L"utf-16le"))
        return EncodingChoice::Utf16;
    if (equalsIgnoringCase(name, L"ansi"))
        return EncodingChoice::Ansi;
    if (equalsIgnoringCase(name, L"oem"))
        return EncodingChoice::Oem;
    if (equalsIgnoringCase(name, L"console"))
        return EncodingChoice::Console;
    return std::nullopt;
}

UINT codePageOf(EncodingChoice choice, UINT consoleCp)
{
    switch (choice) {
    case EncodingChoice::Utf8:
        return CP_UTF8;
    case EncodingChoice::Utf16:
        return kUtf16CodePage;
    case EncodingChoice::Ansi:
        return ::GetACP();
    case EncodingChoice::Oem:
        return ::GetOEMCP();
    case EncodingChoice::Console:
        break;
    }
    return consoleCp != 0 && ::IsValidCodePage(consoleCp) ? consoleCp : CP_UTF8;
}

UINT redirectedCodePage(std::optional<EncodingChoice> chosen, bool parentIsPowerShell, UINT consoleCp)
{
    if (chosen)
        return codePageOf(*chosen, consoleCp);
    // PowerShell decodes what a program writes with the console's output code
    // page ([Console]::OutputEncoding), whether it hands it to a cmdlet or
    // writes it to a file: so does ws, as PowerShell's own output is encoded.
    return parentIsPowerShell ? codePageOf(EncodingChoice::Console, consoleCp) : CP_UTF8;
}

std::wstring parentImageName()
{
    PROCESS_BASIC_INFORMATION basic {};
    ULONG length = 0;
    if (::NtQueryInformationProcess(::GetCurrentProcess(), ProcessBasicInformation, &basic, sizeof basic, &length) < 0)
        return {}; // an NTSTATUS error
    // Reserved3 is InheritedFromUniqueProcessId.
    const auto parentId = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(basic.Reserved3));
    const Handle parent(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentId));
    if (!parent)
        return {};
    // The number may have been given to another process since the parent
    // went: the parent started before this one.
    FILETIME parentCreated {}, ownCreated {}, unused1 {}, unused2 {}, unused3 {};
    if (!::GetProcessTimes(parent.get(), &parentCreated, &unused1, &unused2, &unused3)
        || !::GetProcessTimes(::GetCurrentProcess(), &ownCreated, &unused1, &unused2, &unused3)
        || ::CompareFileTime(&parentCreated, &ownCreated) > 0)
        return {};
    wchar_t image[MAX_PATH * 2] {};
    DWORD size = static_cast<DWORD>(std::size(image));
    if (!::QueryFullProcessImageNameW(parent.get(), 0, image, &size))
        return {};
    const std::wstring_view path(image, size);
    const std::size_t slash = path.find_last_of(L'\\');
    return std::wstring(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
}

bool parentIsPowerShell()
{
    const std::wstring name = parentImageName();
    return equalsIgnoringCase(name, L"powershell.exe") || equalsIgnoringCase(name, L"pwsh.exe");
}

Encoder::Encoder(UINT codePage, bool jsonEscapes)
    : m_codePage(codePage)
    , m_jsonEscapes(jsonEscapes)
    , m_lossless(codePage == CP_UTF8 || codePage == kUtf16CodePage || codePage == kGb18030CodePage)
    , m_flagsAllowed(flagsAllowed(codePage))
{
}

std::size_t Encoder::append(std::string& out, std::wstring_view text) const
{
    if (text.empty())
        return 0;
    if (m_codePage == kUtf16CodePage) {
        out.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
        return 0;
    }
    const int units = static_cast<int>(text.size());
    if (m_lossless) {
        const int n = ::WideCharToMultiByte(m_codePage, 0, text.data(), units, nullptr, 0, nullptr, nullptr);
        const std::size_t at = out.size();
        out.resize(at + static_cast<std::size_t>(n));
        ::WideCharToMultiByte(m_codePage, 0, text.data(), units, out.data() + at, n, nullptr, nullptr);
        return 0;
    }
    // All at once; only text with a character the code page lacks goes
    // character by character.
    const std::size_t at = out.size();
    BOOL usedDefault = FALSE;
    const DWORD flags = m_flagsAllowed ? WC_NO_BEST_FIT_CHARS : 0;
    BOOL* used = m_flagsAllowed ? &usedDefault : nullptr;
    const int n = ::WideCharToMultiByte(m_codePage, flags, text.data(), units, nullptr, 0, nullptr, used);
    if (n > 0) {
        out.resize(at + static_cast<std::size_t>(n));
        usedDefault = FALSE;
        ::WideCharToMultiByte(m_codePage, flags, text.data(), units, out.data() + at, n, nullptr, used);
        bool whole = m_flagsAllowed ? !usedDefault : true;
        if (!m_flagsAllowed) {
            // No default-character report for these: read it back.
            const std::string_view bytes(out.data() + at, static_cast<std::size_t>(n));
            const int back = ::MultiByteToWideChar(m_codePage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            std::wstring again(static_cast<std::size_t>(back), L'\0');
            ::MultiByteToWideChar(m_codePage, 0, bytes.data(), static_cast<int>(bytes.size()), again.data(), back);
            whole = again == text;
        }
        if (whole)
            return 0;
    }
    out.resize(at);
    return appendSlowly(out, text);
}

bool Encoder::fits(std::wstring_view character, std::string& bytes) const
{
    char buffer[16];
    BOOL usedDefault = FALSE;
    const DWORD flags = m_flagsAllowed ? WC_NO_BEST_FIT_CHARS : 0;
    const int n = ::WideCharToMultiByte(m_codePage, flags, character.data(), static_cast<int>(character.size()), buffer,
        sizeof buffer, nullptr, m_flagsAllowed ? &usedDefault : nullptr);
    if (n <= 0)
        return false;
    if (m_flagsAllowed) {
        if (usedDefault)
            return false;
    } else {
        wchar_t back[8];
        const int m = ::MultiByteToWideChar(m_codePage, 0, buffer, n, back, static_cast<int>(std::size(back)));
        if (std::wstring_view(back, static_cast<std::size_t>(m > 0 ? m : 0)) != character)
            return false;
    }
    bytes.assign(buffer, static_cast<std::size_t>(n));
    return true;
}

std::size_t Encoder::appendSlowly(std::string& out, std::wstring_view text) const
{
    std::size_t lost = 0;
    std::string bytes;
    std::size_t runStart = 0; // ASCII and other plain characters are converted in runs
    const auto flushRun = [&](std::size_t end) {
        if (end > runStart) {
            const std::wstring_view run = text.substr(runStart, end - runStart);
            const int n = ::WideCharToMultiByte(m_codePage, 0, run.data(), static_cast<int>(run.size()), nullptr, 0, nullptr, nullptr);
            const std::size_t at = out.size();
            out.resize(at + static_cast<std::size_t>(n));
            ::WideCharToMultiByte(m_codePage, 0, run.data(), static_cast<int>(run.size()), out.data() + at, n, nullptr, nullptr);
        }
    };
    for (std::size_t i = 0; i < text.size();) {
        const bool pair = IS_HIGH_SURROGATE(text[i]) && i + 1 < text.size() && IS_LOW_SURROGATE(text[i + 1]);
        const std::size_t width = pair ? 2 : 1;
        const std::wstring_view character = text.substr(i, width);
        if (text[i] < 0x80) {
            i += width;
            continue; // part of the run
        }
        if (fits(character, bytes)) {
            flushRun(i);
            out += bytes;
        } else {
            flushRun(i);
            if (m_jsonEscapes) {
                for (const wchar_t unit : character)
                    appendJsonEscape(out, unit);
            } else {
                out += '?';
                ++lost;
            }
        }
        i += width;
        runStart = i;
    }
    flushRun(text.size());
    return lost;
}

} // namespace ws::cli
