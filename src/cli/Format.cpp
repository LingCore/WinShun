#include "Format.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>

namespace ws::cli {

std::wstring humanSize(long long bytes)
{
    if (bytes < 0)
        return L"-";
    if (bytes < 1024)
        return std::to_wstring(bytes) + L" B";
    static constexpr const wchar_t* kUnits[] = {L"KB", L"MB", L"GB", L"TB", L"PB"};
    double value = static_cast<double>(bytes) / 1024.0;
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t buffer[32];
    if (value < 9.95)
        std::swprintf(buffer, std::size(buffer), L"%.1f %ls", value, kUnits[unit]);
    else
        std::swprintf(buffer, std::size(buffer), L"%.0f %ls", value, kUnits[unit]);
    return buffer;
}

std::wstring shortTime(std::string_view iso)
{
    // YYYY-MM-DDTHH:MM…
    if (iso.size() < 16 || iso[4] != '-' || iso[7] != '-' || (iso[10] != 'T' && iso[10] != ' ') || iso[13] != ':')
        return iso.empty() ? L"-" : std::wstring(iso.begin(), iso.end());
    std::wstring out;
    out.reserve(16);
    for (std::size_t i = 0; i < 16; ++i)
        out += i == 10 ? L' ' : static_cast<wchar_t>(static_cast<unsigned char>(iso[i]));
    return out;
}

std::wstring thousands(long long value)
{
    std::wstring digits = std::to_wstring(value < 0 ? -value : value);
    std::wstring out;
    const std::size_t first = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    out.append(digits, 0, first);
    for (std::size_t i = first; i < digits.size(); i += 3) {
        out += L',';
        out.append(digits, i, 3);
    }
    return value < 0 ? L"-" + out : out;
}

std::size_t nameStart(std::wstring_view path) noexcept
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? 0 : slash + 1;
}

std::vector<std::wstring> queryTerms(std::wstring_view query)
{
    // The query's parts: quoted phrases whole, the rest split on spaces.
    std::vector<std::wstring> parts;
    std::wstring current;
    bool quoted = false;
    for (const wchar_t c : query) {
        if (c == L'"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && (c == L' ' || c == L'\t' || c == L'\x3000')) {
            if (!current.empty())
                parts.push_back(std::move(current));
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty())
        parts.push_back(std::move(current));

    std::vector<std::wstring> terms;
    for (std::wstring& part : parts) {
        if (part.starts_with(L'!'))
            continue; // excluded
        // ext:pdf and the like (a word of letters before the colon); "C:\x" is a path.
        if (const std::size_t colon = part.find(L':'); colon != std::wstring::npos && colon >= 2) {
            const bool word = std::all_of(part.begin(), part.begin() + static_cast<std::ptrdiff_t>(colon),
                [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); });
            if (word)
                continue;
        }
        // folder\name: the name is what is shown marked.
        std::wstring_view name = part;
        while (!name.empty() && (name.back() == L'\\' || name.back() == L'/'))
            name.remove_suffix(1);
        name = name.substr(nameStart(name));
        // *.pdf, a?c: the literal pieces.
        std::size_t from = 0;
        for (std::size_t i = 0; i <= name.size(); ++i) {
            if (i == name.size() || name[i] == L'*' || name[i] == L'?') {
                if (i > from)
                    terms.emplace_back(name.substr(from, i - from));
                from = i + 1;
            }
        }
    }
    std::ranges::sort(terms);
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    return terms;
}

std::wstring lowered(std::wstring_view text)
{
    if (text.empty())
        return {};
    std::wstring out(text.size(), L'\0');
    const int n = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), static_cast<int>(text.size()), out.data(),
        static_cast<int>(out.size()), nullptr, nullptr, 0);
    if (n != static_cast<int>(text.size()))
        return std::wstring(text);
    return out;
}

std::vector<std::pair<std::size_t, std::size_t>> findTerms(std::wstring_view text, std::span<const std::wstring> terms)
{
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    if (terms.empty() || text.empty())
        return ranges;
    const std::wstring haystack = lowered(text);
    for (const std::wstring& term : terms) {
        const std::wstring needle = lowered(term);
        if (needle.empty())
            continue;
        for (std::size_t at = haystack.find(needle); at != std::wstring::npos; at = haystack.find(needle, at + needle.size()))
            ranges.emplace_back(at, at + needle.size());
    }
    std::ranges::sort(ranges);
    std::vector<std::pair<std::size_t, std::size_t>> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, range.second);
        else
            merged.push_back(range);
    }
    return merged;
}

std::size_t displayWidth(std::wstring_view text) noexcept
{
    std::size_t width = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];
        if (IS_HIGH_SURROGATE(text[i]) && i + 1 < text.size() && IS_LOW_SURROGATE(text[i + 1])) {
            c = 0x10000 + ((static_cast<char32_t>(text[i]) - 0xD800) << 10) + (static_cast<char32_t>(text[i + 1]) - 0xDC00);
            ++i;
        }
        const bool wideCharacter = (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF)
            || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3)
            || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6)
            || (c >= 0x20000 && c <= 0x3FFFD);
        width += wideCharacter ? 2 : 1;
    }
    return width;
}

bool isUnder(std::wstring_view path, std::wstring_view folder) noexcept
{
    while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/'))
        folder.remove_suffix(1);
    if (folder.empty() || path.size() < folder.size())
        return false;
    if (::CompareStringOrdinal(path.data(), static_cast<int>(folder.size()), folder.data(), static_cast<int>(folder.size()), TRUE)
        != CSTR_EQUAL)
        return false;
    return path.size() == folder.size() || path[folder.size()] == L'\\' || path[folder.size()] == L'/';
}

} // namespace ws::cli
