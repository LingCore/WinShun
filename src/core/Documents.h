#pragma once

#include <QStringList>

#include <algorithm>
#include <array>
#include <string_view>

namespace ws {

// The document formats a content search reads through WinShunExtract.exe
// (DocExtractor): Office Open XML, Office 97–2003, WPS's own names for the
// same formats, and PDF. Which of them a file really is comes from its first
// bytes (a .wps may be Word 97 or Office Open XML, a .xls an HTML table).
// Sorted, for binary search.
inline constexpr std::array<std::string_view, 28> kDocumentExtensions {"doc", "docm", "docx", "dot", "dotm", "dotx",
    "dps", "dpt", "et", "ett", "pdf", "pot", "potm", "potx", "pps", "ppsm", "ppsx", "ppt", "pptm", "pptx", "wps", "wpt",
    "xls", "xlsm", "xlsx", "xlt", "xltm", "xltx"};
static_assert(std::ranges::is_sorted(kDocumentExtensions));

inline bool isDocumentExtension(std::string_view lowercase) noexcept
{
    return std::ranges::binary_search(kDocumentExtensions, lowercase);
}

inline const QStringList& documentExtensions()
{
    static const QStringList list = [] {
        QStringList out;
        for (const std::string_view ext : kDocumentExtensions)
            out.append(QString::fromLatin1(ext.data(), static_cast<qsizetype>(ext.size())));
        return out;
    }();
    return list;
}

// Whether the file (by its name) is one of them.
inline bool isDocumentPath(std::wstring_view path) noexcept
{
    const std::size_t dot = path.find_last_of(L".\\/");
    if (dot == std::wstring_view::npos || path[dot] != L'.' || path.size() - dot - 1 > 4)
        return false;
    std::array<char, 4> ext {};
    const std::size_t n = path.size() - dot - 1;
    for (std::size_t i = 0; i < n; ++i) {
        const wchar_t c = path[dot + 1 + i];
        if (c >= 0x80)
            return false;
        ext[i] = static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c);
    }
    return isDocumentExtension({ext.data(), n});
}

} // namespace ws
