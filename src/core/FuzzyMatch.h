#pragma once

#include <QList>
#include <QStringView>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ws::fuzzy {

// Standalone FZF-compatible subsequence matcher (Smith-Waterman V2 with linear
// V1 fallback for very large inputs). Completely independent of WinShun's core
// query syntax so callers can opt in explicitly (fz:/fuzzy:) or via FuzzyMode.
//
// Scoring algorithm adapted from junegunn/fzf (MIT License):
//   https://github.com/junegunn/fzf/blob/master/src/algo/algo.go

struct Match {
    int score = 0;
    std::size_t start = 0; // first matched UTF-8 byte offset in text
    std::size_t end = 0;   // one past last matched UTF-8 byte sequence in text
};

// Reusable per-thread scratch storage so hot search loops do zero allocations.
struct Slab {
    std::vector<std::int32_t> h;
    std::vector<std::int16_t> c;
    std::vector<std::int16_t> bonus;
    std::vector<std::uint8_t> folded;
    std::vector<std::int32_t> first;
    // Unicode code-point buffers (only populated for patterns with non-ASCII).
    std::vector<char32_t> textCp;
    std::vector<std::size_t> textByteStart;
    std::vector<std::size_t> textByteEnd;
    std::vector<char32_t> patCp;
};

Slab& threadSlab() noexcept;

// Fast O(n) subsequence check on UTF-8 text against an ASCII-folded UTF-8
// pattern. Used as a cheap pre-filter before running Smith-Waterman V2.
bool isSubsequence(std::string_view text, std::string_view foldedPattern) noexcept;

// Number of UTF-8 code points in `s` (safe on arbitrary WTF-8/UTF-8 bytes).
std::size_t codePointCount(std::string_view s) noexcept;

// Whether a folded query term is eligible for automatic fuzzy matching
// (FuzzyMode::Fallback / FuzzyMode::On): avoids 1-2 character noise and
// pure-punctuation queries. Explicit fz:/fuzzy: terms bypass this check.
bool isEligibleAutoTerm(std::string_view foldedTerm, std::size_t minCodePoints = 3) noexcept;

// Smith-Waterman V2 (falling back to V1 for huge inputs). `foldedPattern` must
// already be ASCII-folded (text::foldAscii). Returns std::nullopt on no match.
std::optional<Match> match(std::string_view text, std::string_view foldedPattern, Slab* slab = nullptr);

// Same as match(), plus exact sorted UTF-8 byte start offsets of each matched
// code point (for discrete UI highlighting). Only called on visible rows.
std::optional<Match> matchPositions(std::string_view text, std::string_view foldedPattern,
    std::vector<std::size_t>& bytePositions, Slab* slab = nullptr);

// Quality gate for automatic fuzzy matching (Fallback / On): filters out
// heavily scattered low-quality matches while keeping tight abbreviations
// (rpt -> Report.pdf, nsrch -> NameSearch.cpp, vcode -> Visual Studio Code).
bool passesAutoThreshold(int rawScore, std::size_t patternChars) noexcept;

// Maps raw FZF score + name coverage onto a length-independent [1..100] scale
// that harmonizes with NameSearch's static signals (LowPriority, Hidden, depth).
int normalizedScore(int rawScore, std::size_t patternChars, std::size_t textChars) noexcept;

// Computes merged UTF-16 (start, length) highlight ranges for QString display
// (elide::highlighted / elide::shortenedHtml). Empty on no match.
QList<std::pair<qsizetype, qsizetype>> highlightRanges(QStringView text, QStringView pattern);

} // namespace ws::fuzzy
