#pragma once

#include <QString>

#include <string>
#include <string_view>

// WTF-8 is UTF-8 that may also carry unpaired UTF-16 surrogates. NTFS file
// names are arbitrary UTF-16, so storing names as WTF-8 keeps every name
// round-trippable while costing roughly half the memory of UTF-16 for the
// mostly-ASCII names found on a typical disk.
namespace qf::wtf8 {

void append(std::string& out, std::u16string_view utf16);
std::string fromUtf16(std::u16string_view utf16);

void decodeAppend(std::u16string& out, std::string_view bytes);
QString toQString(std::string_view bytes);

inline std::u16string_view view(std::wstring_view s) noexcept
{
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    return {reinterpret_cast<const char16_t*>(s.data()), s.size()};
}

inline std::u16string_view view(const QString& s) noexcept
{
    return {reinterpret_cast<const char16_t*>(s.utf16()), static_cast<std::size_t>(s.size())};
}

inline std::wstring_view wview(std::u16string_view s) noexcept
{
    return {reinterpret_cast<const wchar_t*>(s.data()), s.size()};
}

} // namespace qf::wtf8
