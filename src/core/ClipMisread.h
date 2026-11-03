#pragma once

#include <QString>

#include <windows.h>

#include <vector>

namespace ws {

// Whether `candidate` is `previous` as a remote-desktop clipboard sync garbles
// it (GitHub #181, NetEase UU Remote): the text is encoded in the ANSI code
// page, and those bytes are read back as UTF-16LE ("2026年11月2日" in GBK
// becomes "〲㘲ㄱ싔젲"). A byte left over is either dropped or becomes
// the last character. Trailing NULs of the candidate are not part of it.
// ASCII text is garbled the same way ("ab" becomes U+6261), so it counts.
//
// `codePage` is the machine's ANSI code page unless a test asks for one.
inline bool isAnsiMisread(const QString& candidate, const QString& previous, UINT codePage = CP_ACP)
{
    constexpr qsizetype kMaxChars = 1024 * 1024; // a sync garbles what was copied just now: nothing huge
    if (previous.isEmpty() || previous.size() > kMaxChars || candidate.isEmpty() || candidate == previous)
        return false;
    qsizetype length = candidate.size();
    while (length > 0 && candidate.at(length - 1).unicode() == 0)
        --length;
    if (length == 0)
        return false;
    const int bytes = ::WideCharToMultiByte(codePage, 0, reinterpret_cast<LPCWCH>(previous.utf16()),
        static_cast<int>(previous.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return false;
    // Cheap size check first: half the bytes, or one more for an odd count.
    const qsizetype whole = bytes / 2;
    const bool odd = (bytes & 1) != 0;
    if (length != whole && !(odd && length == whole + 1))
        return false;
    std::vector<char> ansi(static_cast<std::size_t>(bytes));
    if (::WideCharToMultiByte(codePage, 0, reinterpret_cast<LPCWCH>(previous.utf16()),
            static_cast<int>(previous.size()), ansi.data(), bytes, nullptr, nullptr) != bytes)
        return false;
    for (qsizetype i = 0; i < whole; ++i) {
        const char16_t unit = static_cast<char16_t>(static_cast<unsigned char>(ansi[static_cast<std::size_t>(2 * i)])
            | (static_cast<unsigned char>(ansi[static_cast<std::size_t>(2 * i + 1)]) << 8));
        if (candidate.at(i).unicode() != unit)
            return false;
    }
    if (length == whole)
        return true; // the odd byte (if any) was dropped
    return candidate.at(whole).unicode() == static_cast<unsigned char>(ansi.back()); // ... or is the last character
}

} // namespace ws
