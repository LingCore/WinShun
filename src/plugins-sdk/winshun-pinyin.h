// winshun-pinyin.h -- finding Chinese by pinyin, for a Win顺 plugin.
//
// An optional companion to winshun.h, kept out of it so that a plugin that
// does not match Chinese names pays nothing for the table: including this
// header adds about 195 KB of readings to the binary (measured with MSVC
// 2022 x64, /O2).
//
//     #include "winshun.h"
//     #include "winshun-pinyin.h"
//
//     const winshun::pinyin::Matcher m("xian");   // or "xa", "xianjt"
//     if (m.matches("西安交通大学")) { ... }
//
// The same rule the app itself uses (src/core/Pinyin.h): each Chinese
// character matches a non-empty prefix of one of its readings, so initials,
// whole syllables and a mix all work ("bg", "baogao", "baog", "bgao" all find
// 报告); every other character must match itself, ASCII case folded. Only
// U+4E00..U+9FFF is read; ü is written v, as pinyin keyboards do ("lv" for 绿).
//
// The table below is generated from pinyin-data
// (https://github.com/mozillazg/pinyin-data, MIT licence) by
// tools/make_sdk_pinyin.py, with the same readings as the app's
// src/core/PinyinTable.cpp. Do not edit it by hand; rerun the generator.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "winshun-pinyin-table.h" // the generated readings

namespace winshun::pinyin {

inline constexpr char32_t kFirstHan = 0x4E00;
inline constexpr char32_t kLastHan = 0x9FFF;

constexpr bool isHan(char32_t c) noexcept
{
    return c >= kFirstHan && c <= kLastHan;
}

namespace detail {

inline constexpr std::uint32_t kNoReading = 0xFFFFFFFFu;

// kPinyinSyllables as a view: it is a char array, and a view built from the
// literal alone would stop at the first NUL.
inline constexpr std::string_view kSyllablesView {kPinyinSyllables, sizeof kPinyinSyllables - 1};

// The syllable a 10-bit field names: 0 is none, otherwise index + 1.
inline std::string_view syllable(std::uint32_t number) noexcept
{
    // NUL separated, in the order the generator numbered them.
    std::uint32_t seen = 0;
    std::string_view rest = kSyllablesView;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\0');
        if (end == std::string_view::npos)
            return {};
        if (seen == number - 1)
            return rest.substr(0, end);
        rest.remove_prefix(end + 1);
        ++seen;
    }
    return {};
}

// The readings of a character, up to three, most common first. Reads the
// packed 8-hex-digit groups: 10 bits per reading, low first, 0 = none.
inline std::uint32_t packed(char32_t c) noexcept
{
    if (!isHan(c))
        return 0;
    const std::size_t index = static_cast<std::size_t>(c - kFirstHan);
    const std::size_t at = index * 8;
    if (at + 8 > kPinyinReadingsHex.size())
        return 0;
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        const char ch = kPinyinReadingsHex[at + i];
        const std::uint32_t digit = ch <= '9' ? static_cast<std::uint32_t>(ch - '0')
                                              : static_cast<std::uint32_t>(ch - 'A' + 10);
        value = (value << 4) | digit;
    }
    return value;
}

} // namespace detail

// Up to three readings of a character, without tones, most common first.
struct Readings {
    std::array<std::string_view, 3> items {};
    std::size_t count = 0;

    const std::string_view* begin() const noexcept { return items.data(); }
    const std::string_view* end() const noexcept { return items.data() + count; }
};

inline Readings readings(char32_t c) noexcept
{
    Readings r;
    if (!isHan(c))
        return r;
    for (std::uint32_t bits = detail::packed(c); bits != 0; bits >>= 10) {
        if (const std::uint32_t number = bits & 0x3FF)
            r.items[r.count++] = detail::syllable(number);
    }
    return r;
}

// Whether UTF-8 text contains a Chinese character.
inline bool hasHan(std::string_view s) noexcept
{
    // U+4E00..U+9FFF are three bytes in UTF-8, led by E4..E9.
    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0xE4 || b > 0xE9)
            continue;
        const char32_t c = ((b & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6)
            | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
        if (isHan(c))
            return true;
    }
    return false;
}

// Whether a folded query term is worth trying as pinyin: two or more ASCII
// letters and digits, at least one of them a letter. "ab" may be pinyin; "a"
// is not tried, so a single letter stays a plain substring search.
inline bool isPinyinTerm(std::string_view folded) noexcept
{
    if (folded.size() < 2 || folded.size() > 63)
        return false;
    bool letter = false;
    for (const char c : folded) {
        if (c >= 'a' && c <= 'z')
            letter = true;
        else if (c < '0' || c > '9')
            return false;
    }
    return letter;
}

// Matches one folded query term against text, reading Chinese characters as
// pinyin. Immutable: build one per term and reuse it.
class Matcher {
public:
    explicit Matcher(std::string_view foldedTerm)
    {
        if (!isPinyinTerm(foldedTerm))
            return;
        m_length = foldedTerm.size();
        m_first = foldedTerm[0];
        m_firstInitial = (m_first >= 'a' && m_first <= 'z') ? std::uint32_t {1} << (m_first - 'a') : 0;
        for (std::size_t p = 0; p < foldedTerm.size(); ++p)
            m_at[static_cast<unsigned char>(foldedTerm[p])] |= std::uint64_t {1} << p;
    }

    bool valid() const noexcept { return m_length > 0; }

    // Whether the term is found in the text at all.
    bool matches(std::string_view utf8) const noexcept { return findUtf8(utf8).has_value(); }

    // The leftmost match, as a byte span into `text`.
    std::optional<std::pair<std::size_t, std::size_t>> findUtf8(std::string_view text) const noexcept
    {
        if (m_length == 0)
            return std::nullopt;
        const std::uint64_t done = std::uint64_t {1} << m_length;
        std::size_t length = 0;
        for (std::size_t start = 0; start < text.size(); start += length) {
            const char32_t c = decodeAt(text, start, length);
            if (!canStart(c))
                continue;
            std::uint64_t states = step(c, 1);
            std::size_t i = start + length;
            while (states != 0 && !(states & done) && i < text.size()) {
                std::size_t l = 0;
                states = step(decodeAt(text, i, l), states);
                i += l;
            }
            if (states & done)
                return std::make_pair(start, i - start);
        }
        return std::nullopt;
    }

private:
    // One code point of UTF-8 at text[i]; its length in bytes in `length`.
    static char32_t decodeAt(std::string_view text, std::size_t i, std::size_t& length) noexcept
    {
        const auto b = static_cast<unsigned char>(text[i]);
        if (b < 0x80) {
            length = 1;
            return b;
        }
        const std::size_t len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        length = len < text.size() - i ? len : text.size() - i;
        char32_t c = len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
        for (std::size_t k = 1; k < length; ++k)
            c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        return c;
    }

    static unsigned char fold(char c) noexcept
    {
        return static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }

    bool canStart(char32_t c) const noexcept
    {
        if (c < 0x80)
            return fold(static_cast<char>(c)) == static_cast<unsigned char>(m_first);
        if (!isHan(c) || m_firstInitial == 0)
            return false;
        for (const std::string_view s : readings(c)) {
            if (s.empty())
                continue;
            if ((std::uint32_t {1} << (s[0] - 'a')) & m_firstInitial)
                return true;
        }
        return false;
    }

    // From the term positions reached so far, those reached after `c`.
    std::uint64_t step(char32_t c, std::uint64_t states) const noexcept
    {
        if (c < 0x80) {
            const auto folded = fold(static_cast<char>(c));
            return (states & m_at[folded]) << 1;
        }
        std::uint64_t next = 0;
        for (const std::string_view syllable : readings(c)) {
            // Every non-empty prefix of the syllable: after k letters, `t`
            // holds the positions reached. 'v' (ü) also accepts 'u'.
            std::uint64_t t = states;
            for (const char letter : syllable) {
                const auto l = static_cast<unsigned char>(letter);
                t = (t & (letter == 'v' ? m_at[l] | m_at['u'] : m_at[l])) << 1;
                if (t == 0)
                    break;
                next |= t;
            }
        }
        return next;
    }

    std::size_t m_length = 0; // 0: not a pinyin term
    char m_first = 0;
    std::uint32_t m_firstInitial = 0; // m_first as a bit, if a letter
    std::array<std::uint64_t, 128> m_at {}; // per ASCII character: positions in the term that hold it
};

} // namespace winshun::pinyin
