#pragma once

#include <QFileInfo>
#include <QString>
#include <QStringDecoder>

#include <windows.h>

#include <optional>
#include <string>

namespace ws {

// The text a program garbled by reading UTF-8 bytes in the ANSI code page
// ("tourbox预设" in GBK becomes "tourbox棰勮…", GitHub #346): back to what it
// was. None when the text is ASCII, does not go back into that code page
// whole, or its bytes are not UTF-8. Text in Chinese that was never garbled
// seldom passes, but may: take the answer only with other evidence (see
// ungarbledPath()).
//
// `codePage` is the machine's ANSI code page unless a test asks for one.
inline std::optional<QString> utf8ReadAsAnsi(const QString& text, UINT codePage = CP_ACP)
{
    bool ascii = true;
    for (const QChar c : text)
        ascii = ascii && c.unicode() < 0x80;
    if (ascii)
        return std::nullopt;
    const auto* wide = reinterpret_cast<LPCWCH>(text.utf16());
    const int length = static_cast<int>(text.size());
    BOOL lossy = FALSE;
    // UTF-8/7 code pages refuse the "default character used" flag: nothing to undo there.
    const int size = ::WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS, wide, length, nullptr, 0, nullptr, &lossy);
    if (size <= 0 || lossy)
        return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (::WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS, wide, length, bytes.data(), size, nullptr, &lossy) != size
        || lossy)
        return std::nullopt;
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString original = utf8.decode(QByteArrayView(bytes));
    if (utf8.hasError() || original == text)
        return std::nullopt;
    return original;
}

// A path as stored or handed over, or the one it was before something read
// its UTF-8 in the ANSI code page, when that one is on disk. Even when the
// garbled one is too: a backup made it (#346), nobody names a folder so.
inline QString ungarbledPath(const QString& path)
{
    if (const std::optional<QString> original = utf8ReadAsAnsi(path); original && QFileInfo::exists(*original))
        return *original;
    return path;
}

} // namespace ws
