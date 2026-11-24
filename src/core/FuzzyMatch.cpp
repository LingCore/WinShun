// FZF-compatible Smith-Waterman V2 & linear V1 fuzzy subsequence matching.
//
// Algorithm and scoring constants adapted from junegunn/fzf:
//   Copyright (c) 2013-2025 Junegunn Choi (MIT License)
//   https://github.com/junegunn/fzf/blob/master/src/algo/algo.go

#include "FuzzyMatch.h"

#include "TextUtil.h"
#include "Wtf8.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace ws::fuzzy {

namespace {

constexpr int kScoreMatch = 16;
constexpr int kScoreGapStart = -3;
constexpr int kScoreGapExtension = -1;

constexpr int kBonusBoundary = kScoreMatch / 2;         // 8
constexpr int kBonusNonWord = kScoreMatch / 2;          // 8
constexpr int kBonusBoundaryWhite = kBonusBoundary + 2; // 10 (start of string or after space)
constexpr int kBonusBoundaryDelimiter = kBonusBoundary + 1; // 9 (after /, \, _, -, ., etc.)
constexpr int kBonusCamel123 = kBonusBoundary + kScoreGapExtension; // 7 (camelCase, letter<->digit, ASCII<->CJK)
constexpr int kBonusConsecutive = -(kScoreGapStart + kScoreGapExtension); // 4
constexpr int kBonusFirstCharMultiplier = 2;

constexpr std::size_t kMaxPatternV2 = 64;
constexpr std::size_t kMaxMatrixCells = 65536;
constexpr std::int32_t kNegInf = std::numeric_limits<std::int32_t>::min() / 4;

enum CharClass : std::uint8_t {
    White = 0,
    NonWord = 1,
    Delimiter = 2,
    Lower = 3,
    Upper = 4,
    Digit = 5,
    NonAscii = 6,
};

constexpr std::array<CharClass, 256> kAsciiClass = [] {
    std::array<CharClass, 256> table {};
    for (int i = 0; i < 256; ++i) {
        if (i >= 0x80) {
            table[i] = NonAscii;
        } else if (i == ' ' || i == '\t' || i == '\n' || i == '\r' || i == '\v' || i == '\f') {
            table[i] = White;
        } else if (i >= 'a' && i <= 'z') {
            table[i] = Lower;
        } else if (i >= 'A' && i <= 'Z') {
            table[i] = Upper;
        } else if (i >= '0' && i <= '9') {
            table[i] = Digit;
        } else if (i == '/' || i == '\\' || i == '_' || i == '-' || i == '.' || i == ',' || i == ':' || i == ';'
            || i == '|' || i == '(' || i == ')' || i == '[' || i == ']' || i == '{' || i == '}') {
            table[i] = Delimiter;
        } else {
            table[i] = NonWord;
        }
    }
    return table;
}();

constexpr std::array<std::array<std::int16_t, 7>, 7> kBonusMatrix = [] {
    std::array<std::array<std::int16_t, 7>, 7> m {};
    for (int prev = 0; prev < 7; ++prev) {
        for (int cur = 0; cur < 7; ++cur) {
            std::int16_t b = 0;
            if (cur >= Lower) {
                if (prev == White)
                    b = kBonusBoundaryWhite;
                else if (prev == Delimiter)
                    b = kBonusBoundaryDelimiter;
                else if (prev == NonWord)
                    b = kBonusBoundary;
                else if ((prev == Lower || prev == Digit) && cur == Upper)
                    b = kBonusCamel123;
                else if (prev != Digit && cur == Digit && prev != NonAscii)
                    b = kBonusCamel123;
                else if ((prev == NonAscii) != (cur == NonAscii))
                    b = kBonusCamel123;
            } else if (cur == NonWord || cur == Delimiter) {
                b = kBonusNonWord;
            } else if (cur == White) {
                b = kBonusBoundaryWhite;
            }
            m[prev][cur] = b;
        }
    }
    return m;
}();

inline CharClass charClassOf(char32_t cp) noexcept
{
    return cp < 0x80 ? kAsciiClass[static_cast<unsigned char>(cp)] : NonAscii;
}

inline char32_t foldCodePoint(char32_t cp) noexcept
{
    return cp >= U'A' && cp <= U'Z' ? cp + 32 : cp;
}

inline bool isPureAscii(std::string_view s) noexcept
{
    for (const char c : s) {
        if (static_cast<unsigned char>(c) >= 0x80)
            return false;
    }
    return true;
}

// Decodes one UTF-8/WTF-8 code point at `s[i]`, advancing `i`. Always makes
// forward progress even on malformed trailing bytes.
inline char32_t nextCodePoint(std::string_view s, std::size_t& i) noexcept
{
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        if ((b1 & 0xC0) == 0x80) {
            i += 2;
            return ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
        }
    } else if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        const auto b2 = static_cast<unsigned char>(s[i + 2]);
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            i += 3;
            return ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
        }
    } else if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        const auto b2 = static_cast<unsigned char>(s[i + 2]);
        const auto b3 = static_cast<unsigned char>(s[i + 3]);
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
            i += 4;
            return ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
        }
    }
    ++i;
    return b0;
}

// Generic core over a sequence of folded elements `text[0..n-1]` with precomputed
// per-index bonuses `bonus[0..n-1]`, matching folded `pat[0..m-1]`.
// Returns {score, firstIdx, lastIdx} in element indices, plus optional element positions.
struct CoreMatch {
    int score = 0;
    std::size_t startIdx = 0;
    std::size_t endIdx = 0; // inclusive element index of last match
};

template <typename CharT>
std::optional<CoreMatch> matchV1Core(const CharT* text, std::size_t n, const std::int16_t* bonus,
    const CharT* pat, std::size_t m, std::vector<std::size_t>* outIndices)
{
    // Forward pass: find earliest subsequence end.
    std::size_t pidx = 0;
    std::size_t sidx = 0;
    std::size_t eidx = 0;
    bool started = false;
    for (std::size_t i = 0; i < n && pidx < m; ++i) {
        if (text[i] == pat[pidx]) {
            if (!started) {
                sidx = i;
                started = true;
            }
            if (++pidx == m) {
                eidx = i;
                break;
            }
        }
    }
    if (pidx < m)
        return std::nullopt;

    // Backward pass: tighten match window from eidx back to sidx.
    std::vector<std::size_t> pos(m);
    std::size_t k = m;
    for (std::size_t i = eidx + 1; i > sidx && k > 0;) {
        --i;
        if (text[i] == pat[k - 1])
            pos[--k] = i;
    }
    sidx = pos.front();

    // Score using the same fzf scoring rules as V2.
    int score = 0;
    int consecutive = 0;
    std::int16_t firstBonus = 0;
    for (std::size_t i = 0; i < m; ++i) {
        const std::size_t idx = pos[i];
        std::int16_t b = bonus[idx];
        if (i == 0) {
            score += kScoreMatch + b * kBonusFirstCharMultiplier;
            consecutive = 1;
            firstBonus = b;
        } else {
            const std::size_t gap = idx - pos[i - 1] - 1;
            if (gap == 0) {
                if (b >= kBonusBoundary && b > firstBonus) {
                    consecutive = 1;
                    firstBonus = b;
                } else {
                    b = std::max<std::int16_t>({b, static_cast<std::int16_t>(kBonusConsecutive), firstBonus});
                    ++consecutive;
                }
                score += kScoreMatch + b;
            } else {
                score += kScoreGapStart + static_cast<int>(gap - 1) * kScoreGapExtension;
                score += kScoreMatch + b;
                consecutive = 1;
                firstBonus = b;
            }
        }
    }
    (void)consecutive;
    if (outIndices)
        *outIndices = std::move(pos);
    return CoreMatch {score, sidx, eidx};
}

template <typename CharT>
std::optional<CoreMatch> matchV2Core(const CharT* text, std::size_t n, const std::int16_t* bonus,
    const CharT* pat, std::size_t m, Slab& slab, std::vector<std::size_t>* outIndices)
{
    if (m == 0 || m > n)
        return std::nullopt;

    // Phase 1: forward scan to locate first occurrence F[0..m-1].
    slab.first.resize(m);
    std::size_t pidx = 0;
    for (std::size_t i = 0; i < n && pidx < m; ++i) {
        if (text[i] == pat[pidx]) {
            slab.first[pidx] = static_cast<std::int32_t>(i);
            ++pidx;
        }
    }
    if (pidx < m)
        return std::nullopt;

    const std::size_t minIdx = static_cast<std::size_t>(slab.first[0]);
    // Reverse scan for last occurrence of pat[m - 1] to shrink DP width.
    std::size_t maxIdx = static_cast<std::size_t>(slab.first[m - 1]);
    for (std::size_t i = n; i-- > maxIdx;) {
        if (text[i] == pat[m - 1]) {
            maxIdx = i;
            break;
        }
    }

    // Fast path for single-character pattern (used by explicit fz:x).
    if (m == 1) {
        int bestScore = kNegInf;
        std::size_t bestIdx = minIdx;
        for (std::size_t i = minIdx; i <= maxIdx; ++i) {
            if (text[i] == pat[0]) {
                const int s = kScoreMatch + bonus[i] * kBonusFirstCharMultiplier;
                if (s > bestScore) {
                    bestScore = s;
                    bestIdx = i;
                }
            }
        }
        if (outIndices)
            *outIndices = {bestIdx};
        return CoreMatch {bestScore, bestIdx, bestIdx};
    }

    const std::size_t width = maxIdx - minIdx + 1;
    if (m > kMaxPatternV2 || width > kMaxMatrixCells / m)
        return matchV1Core(text, n, bonus, pat, m, outIndices);

    const std::size_t cells = m * width;
    slab.h.resize(cells);
    slab.c.resize(cells);
    std::int32_t* const H = slab.h.data();
    std::int16_t* const C = slab.c.data();

    // Row 0: matching pat[0].
    {
        std::int32_t leftScore = kNegInf;
        bool inGap = false;
        const CharT p0 = pat[0];
        for (std::size_t col = 0; col < width; ++col) {
            const std::size_t idx = minIdx + col;
            std::int32_t s1 = kNegInf;
            if (text[idx] == p0)
                s1 = kScoreMatch + bonus[idx] * kBonusFirstCharMultiplier;
            const std::int32_t s2
                = leftScore > kNegInf ? leftScore + (inGap ? kScoreGapExtension : kScoreGapStart) : kNegInf;
            if (s1 > kNegInf && (s2 == kNegInf || s1 >= s2)) {
                H[col] = s1;
                C[col] = 1;
                leftScore = s1;
                inGap = false;
            } else {
                H[col] = s2;
                C[col] = 0;
                leftScore = s2;
                inGap = true;
            }
        }
    }

    // Rows 1..m-1.
    std::int32_t bestScore = kNegInf;
    std::size_t bestCol = 0;
    for (std::size_t row = 1; row < m; ++row) {
        const std::size_t prevRowOff = (row - 1) * width;
        const std::size_t curRowOff = row * width;
        const std::size_t startCol = static_cast<std::size_t>(slab.first[row]) - minIdx;
        const CharT pchar = pat[row];
        const bool lastRow = (row + 1 == m);

        for (std::size_t col = 0; col < startCol; ++col) {
            H[curRowOff + col] = kNegInf;
            C[curRowOff + col] = 0;
        }

        std::int32_t leftScore = kNegInf;
        bool inGap = false;
        for (std::size_t col = startCol; col < width; ++col) {
            const std::size_t idx = minIdx + col;
            std::int32_t s1 = kNegInf;
            std::int16_t c1 = 0;
            if (text[idx] == pchar && col > 0) {
                const std::int32_t diag = H[prevRowOff + col - 1];
                if (diag > kNegInf) {
                    const std::int16_t prevConsec = C[prevRowOff + col - 1];
                    std::int16_t b = bonus[idx];
                    if (prevConsec > 0) {
                        const std::int16_t fb = bonus[idx - static_cast<std::size_t>(prevConsec)];
                        if (b >= kBonusBoundary && b > fb) {
                            c1 = 1;
                        } else {
                            b = std::max<std::int16_t>({b, static_cast<std::int16_t>(kBonusConsecutive), fb});
                            c1 = static_cast<std::int16_t>(prevConsec + 1);
                        }
                    } else {
                        c1 = 1;
                    }
                    s1 = diag + kScoreMatch + b;
                    if (lastRow && s1 > bestScore) {
                        bestScore = s1;
                        bestCol = col;
                    }
                }
            }
            const std::int32_t s2
                = leftScore > kNegInf ? leftScore + (inGap ? kScoreGapExtension : kScoreGapStart) : kNegInf;
            if (s1 > kNegInf && (s2 == kNegInf || s1 >= s2)) {
                H[curRowOff + col] = s1;
                C[curRowOff + col] = c1;
                leftScore = s1;
                inGap = false;
            } else {
                H[curRowOff + col] = s2;
                C[curRowOff + col] = 0;
                leftScore = s2;
                inGap = true;
            }
        }
    }

    if (bestScore <= kNegInf)
        return std::nullopt;

    // Backtrack from (m - 1, bestCol) to find exact start index (and positions if requested).
    std::size_t col = bestCol;
    std::size_t firstMatchIdx = minIdx;
    if (outIndices)
        outIndices->resize(m);

    for (std::size_t row = m; row-- > 0;) {
        const std::size_t rowOff = row * width;
        while (col > 0 && C[rowOff + col] == 0)
            --col;
        const std::size_t idx = minIdx + col;
        if (outIndices)
            (*outIndices)[row] = idx;
        if (row == 0)
            firstMatchIdx = idx;
        else if (col > 0)
            --col;
    }

    return CoreMatch {bestScore, firstMatchIdx, minIdx + bestCol};
}

// Fast ASCII-pattern path directly over UTF-8 bytes. Because UTF-8 multi-byte
// sequences only use bytes >= 0x80, an ASCII pattern byte (< 0x80) can never
// spuriously match inside a multi-byte character.
std::optional<Match> matchAsciiPattern(
    std::string_view text, std::string_view foldedPattern, Slab& slab, std::vector<std::size_t>* bytePositions)
{
    const std::size_t n = text.size();
    const std::size_t m = foldedPattern.size();
    if (m == 0 || m > n)
        return std::nullopt;

    // Cheap O(n) pre-filter before populating folded/bonus buffers.
    if (!isSubsequence(text, foldedPattern))
        return std::nullopt;

    slab.folded.resize(n);
    slab.bonus.resize(n);
    CharClass prev = White;
    for (std::size_t i = 0; i < n; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        const CharClass cur = kAsciiClass[byte];
        slab.folded[i] = text::kFoldTable[byte];
        slab.bonus[i] = kBonusMatrix[prev][cur];
        prev = cur;
    }

    const auto* pat = reinterpret_cast<const std::uint8_t*>(foldedPattern.data());
    const auto core = matchV2Core(slab.folded.data(), n, slab.bonus.data(), pat, m, slab, bytePositions);
    if (!core)
        return std::nullopt;
    return Match {core->score, core->startIdx, core->endIdx + 1};
}

// General UTF-8/WTF-8 code-point path when the pattern contains non-ASCII.
std::optional<Match> matchUnicodePattern(
    std::string_view text, std::string_view foldedPattern, Slab& slab, std::vector<std::size_t>* bytePositions)
{
    slab.patCp.clear();
    for (std::size_t i = 0; i < foldedPattern.size();)
        slab.patCp.push_back(foldCodePoint(nextCodePoint(foldedPattern, i)));
    if (slab.patCp.empty())
        return std::nullopt;

    slab.textCp.clear();
    slab.textByteStart.clear();
    slab.textByteEnd.clear();
    slab.bonus.clear();
    CharClass prev = White;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t start = i;
        const char32_t raw = nextCodePoint(text, i);
        const CharClass cur = charClassOf(raw);
        slab.textCp.push_back(foldCodePoint(raw));
        slab.textByteStart.push_back(start);
        slab.textByteEnd.push_back(i);
        slab.bonus.push_back(kBonusMatrix[prev][cur]);
        prev = cur;
    }

    std::vector<std::size_t> cpIndices;
    std::vector<std::size_t>* indicesPtr = bytePositions ? &cpIndices : nullptr;
    const auto core = matchV2Core(
        slab.textCp.data(), slab.textCp.size(), slab.bonus.data(), slab.patCp.data(), slab.patCp.size(), slab, indicesPtr);
    if (!core)
        return std::nullopt;

    if (bytePositions) {
        bytePositions->resize(cpIndices.size());
        for (std::size_t i = 0; i < cpIndices.size(); ++i)
            (*bytePositions)[i] = slab.textByteStart[cpIndices[i]];
    }
    return Match {core->score, slab.textByteStart[core->startIdx], slab.textByteEnd[core->endIdx]};
}

} // namespace

Slab& threadSlab() noexcept
{
    thread_local Slab slab;
    return slab;
}

bool isSubsequence(std::string_view text, std::string_view foldedPattern) noexcept
{
    if (foldedPattern.empty())
        return true;
    if (foldedPattern.size() > text.size())
        return false;
    if (isPureAscii(foldedPattern)) {
        std::size_t pi = 0;
        const std::size_t m = foldedPattern.size();
        auto want = static_cast<unsigned char>(foldedPattern[0]);
        for (const char ch : text) {
            if (text::fold(ch) == want) {
                if (++pi == m)
                    return true;
                want = static_cast<unsigned char>(foldedPattern[pi]);
            }
        }
        return false;
    }
    std::size_t pi = 0;
    char32_t want = foldCodePoint(nextCodePoint(foldedPattern, pi));
    for (std::size_t ti = 0; ti < text.size();) {
        if (foldCodePoint(nextCodePoint(text, ti)) == want) {
            if (pi >= foldedPattern.size())
                return true;
            want = foldCodePoint(nextCodePoint(foldedPattern, pi));
        }
    }
    return false;
}

std::size_t codePointCount(std::string_view s) noexcept
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size();) {
        nextCodePoint(s, i);
        ++count;
    }
    return count;
}

bool isEligibleAutoTerm(std::string_view foldedTerm, std::size_t minCodePoints) noexcept
{
    if (foldedTerm.empty())
        return false;
    std::size_t chars = 0;
    bool hasWordChar = false;
    for (std::size_t i = 0; i < foldedTerm.size();) {
        const char32_t cp = nextCodePoint(foldedTerm, i);
        ++chars;
        if (chars > kMaxPatternV2)
            return false;
        const CharClass cls = charClassOf(cp);
        if (cls >= Lower)
            hasWordChar = true;
    }
    return chars >= minCodePoints && hasWordChar;
}

std::optional<Match> match(std::string_view text, std::string_view foldedPattern, Slab* slab)
{
    if (foldedPattern.empty() || foldedPattern.size() > text.size())
        return std::nullopt;
    Slab& s = slab ? *slab : threadSlab();
    if (isPureAscii(foldedPattern))
        return matchAsciiPattern(text, foldedPattern, s, nullptr);
    return matchUnicodePattern(text, foldedPattern, s, nullptr);
}

std::optional<Match> matchPositions(
    std::string_view text, std::string_view foldedPattern, std::vector<std::size_t>& bytePositions, Slab* slab)
{
    bytePositions.clear();
    if (foldedPattern.empty() || foldedPattern.size() > text.size())
        return std::nullopt;
    Slab& s = slab ? *slab : threadSlab();
    if (isPureAscii(foldedPattern))
        return matchAsciiPattern(text, foldedPattern, s, &bytePositions);
    return matchUnicodePattern(text, foldedPattern, s, &bytePositions);
}

bool passesAutoThreshold(int rawScore, std::size_t patternChars) noexcept
{
    if (patternChars == 0)
        return false;
    return rawScore >= static_cast<int>(patternChars) * 14;
}

int normalizedScore(int rawScore, std::size_t patternChars, std::size_t textChars) noexcept
{
    if (patternChars == 0)
        return 1;
    const int maxRaw = static_cast<int>(patternChars) * (kScoreMatch + kBonusBoundaryWhite) + kBonusBoundaryWhite;
    const int quality = rawScore <= 0 ? 1 : std::clamp(rawScore * 80 / maxRaw, 1, 80);
    const std::size_t denom = std::max<std::size_t>(textChars, 1);
    const int coverage = static_cast<int>(20 * std::min(patternChars, denom) / denom);
    return quality + coverage;
}

QList<std::pair<qsizetype, qsizetype>> highlightRanges(QStringView text, QStringView pattern)
{
    QList<std::pair<qsizetype, qsizetype>> ranges;
    if (text.isEmpty() || pattern.isEmpty())
        return ranges;

    const std::string utf8Text = wtf8::fromUtf16(std::u16string_view(text.utf16(), static_cast<std::size_t>(text.size())));
    const std::string utf8Pat
        = text::foldAscii(wtf8::fromUtf16(std::u16string_view(pattern.utf16(), static_cast<std::size_t>(pattern.size()))));

    std::vector<std::size_t> bytePositions;
    if (!matchPositions(utf8Text, utf8Pat, bytePositions))
        return ranges;

    // Map UTF-8 byte offsets back to UTF-16 code unit (start, length) ranges,
    // merging consecutive matches into contiguous spans.
    std::size_t bytePos = 0;
    qsizetype utf16Pos = 0;
    std::size_t nextMatch = 0;
    while (bytePos < utf8Text.size() && nextMatch < bytePositions.size()) {
        const std::size_t startByte = bytePos;
        const char32_t cp = nextCodePoint(utf8Text, bytePos);
        const qsizetype units = cp > 0xFFFF ? 2 : 1;
        if (startByte == bytePositions[nextMatch]) {
            if (!ranges.isEmpty() && ranges.back().first + ranges.back().second == utf16Pos)
                ranges.back().second += units;
            else
                ranges.append({utf16Pos, units});
            ++nextMatch;
        }
        utf16Pos += units;
    }
    return ranges;
}

} // namespace ws::fuzzy
