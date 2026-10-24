#pragma once

// How values look for people: sizes, times, counts, the words of a query
// found in a name.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ws::cli {

// 1024-based, one decimal below 10: "0 B", "999 B", "1.5 KB", "12 KB", "3.4 GB".
std::wstring humanSize(long long bytes);

// "2026-10-24 13:45" from Win顺's ISO 8601 time ("2026-10-24T13:45:12+08:00",
// local already: the same computer); "-" for none.
std::wstring shortTime(std::string_view iso);

// 3312345 → "3,312,345".
std::wstring thousands(long long value);

// Where the name ends: the text after the last backslash (a root "C:\" is
// all folder).
std::size_t nameStart(std::wstring_view path) noexcept;

// The literal words of a Win顺 query, for marking them in what was found:
// operators (!word, ext:…) left out, a path's last part, wildcards split
// on. Pinyin matches have none to mark.
std::vector<std::wstring> queryTerms(std::wstring_view query);

// Where `terms` occur in `text`, any case, as sorted, merged [start, end).
std::vector<std::pair<std::size_t, std::size_t>> findTerms(std::wstring_view text, std::span<const std::wstring> terms);

// Lower case, the same length (for finding terms).
std::wstring lowered(std::wstring_view text);

// A path is `folder` or inside it (any case, whole names).
bool isUnder(std::wstring_view path, std::wstring_view folder) noexcept;

// Columns on a console: East Asian wide characters take two.
std::size_t displayWidth(std::wstring_view text) noexcept;

} // namespace ws::cli
