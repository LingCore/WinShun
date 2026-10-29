#pragma once

#include "FileIndex.h"
#include "Query.h"
#include "SearchTypes.h"
#include "WorkerPool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ws {

// The file types listed first among names that match about as well
// (优先显示的文件类型, as Listary has it; forum #104, #116): programs and
// scripts, so that "curl" finds curl.exe before the folder curl and curl.h.
// A few extensions in a small fixed table: asked about every match of a
// search over millions of entries, it costs a length test and a compare.
class PreferredTypes {
public:
    PreferredTypes() = default;
    // Without the dot, any ASCII case; one longer than 16 bytes, or past
    // the 32nd, is left out.
    explicit PreferredTypes(const std::vector<std::string>& extensions);

    bool empty() const noexcept { return m_count == 0; }
    // `extension` without the dot, as a name has it (any ASCII case).
    bool has(std::string_view extension) const noexcept;
    // Whether a file of this name (UTF-8) is of one: what follows its last dot.
    bool hasName(std::string_view name) const noexcept;

    // How much more a match of such a file scores (NameSearch): as much as
    // a name that has the words at its start scores above one that has them
    // at the start of a later word (NameMatcher), so that "curlie.exe" goes
    // before "curl_notes.txt", but the names that match much better stay
    // before it.
    static constexpr int kBonus = 12;

private:
    static constexpr std::size_t kMax = 32;
    static constexpr std::size_t kLongest = 16;
    using Key = std::array<std::uint64_t, 2>; // the bytes folded, zero-padded
    static Key keyOf(std::string_view extension) noexcept;

    // Most extensions are ruled out by their length and first letter, one
    // bit each, before any key is compared.
    std::uint32_t m_lengths = 0; // bit n: one of them n bytes long
    std::array<std::uint64_t, 4> m_firsts {}; // bit c: one of them starts with byte c (folded)
    std::uint8_t m_count = 0;
    std::array<Key, kMax> m_keys {};
};

struct NameHit {
    EntryId id = kNoEntry;
    int score = 0;
    FileTime modified = 0;
    bool inside = false; // lies in NameSearchOptions::insideFirst (at any depth)
    FileSize size = kUnknownSize; // read when the search is by size, or filters by it
};

struct NameSearchOptions {
    std::size_t limit = 100;
    bool foldersOnly = false;
    RankBy rankBy = RankBy::Match;
    // The best `limit` files and the best `limit` folders, each apart: for a
    // list that puts one kind before the other (KindOrder). Not set for the
    // panel's column order (SortOrder): that is one list.
    bool kindsApart = false;
    // Besides the names: when written, how large, under which folders.
    EntryFilter filter;
    // Typed in a folder's window (GitHub #110): the hits that lie in this
    // folder, at any depth, go before all the others, each part in the
    // order asked for (NameHit::inside tells them apart), so that one pass
    // over the index lists the folder's matches first and the rest after
    // them. kNoEntry: one list.
    EntryId insideFirst = kNoEntry;
    // A column's order (the search panel) in place of the rank; Rank: rankBy.
    SortOrder order;
    // Ordered as not known when later than this (sortTime, trustedUntil):
    // the hits' `modified` is then 0, the caller shows the index's own.
    FileTime trustedUntil = 0xFFFF'FFFFu;
    // Files of these types score PreferredTypes::kBonus more; one named just
    // what was typed (NameMatcher::isExactName) also loses nothing for being
    // in a system or program folder (EntryFlag::LowPriority): that is where
    // programs are, C:\Windows\System32\curl.exe. Only for the best match
    // (RankBy::Match, SortKey::Rank): by time or by a column, left aside.
    PreferredTypes preferred;
};

struct NameSearchOutput {
    std::vector<NameHit> hits; // best first
    std::size_t totalMatches = 0;
    bool cancelled = false;
};

// Scans the whole index in parallel and keeps the best matches.
// Caller holds index.readLock().
NameSearchOutput searchNames(const FileIndex& index, const NameMatcher& matcher, const NameSearchOptions& options,
    WorkerPool& pool, const std::function<bool()>& isCancelled);

struct NewestOptions {
    std::size_t limit = 5;
    // The folders those files are in instead, each once (a folder's own time
    // changes with whatever comes and goes in it, a program's files too).
    bool folders = false;
    std::size_t perFolder = 2; // files: at most this many from one folder
    std::size_t perTree = 3; // ...and from one project or other tree of folders (F:\Project, Documents\Work)
    // Later than this is a clock gone wrong (a file from a camera or another
    // computer dated in the future): left out.
    FileTime notAfter = 0xFFFF'FFFFu;
    // Files: only those with one of these extensions (folded, without the
    // dot); empty: any.
    std::vector<std::string> extensions;
    // Files with these extensions (folded, without the dot) are left out,
    // and those with these names (folded wildcards: nameMatchesAny).
    std::vector<std::string> excludedExtensions;
    std::vector<std::string> excludedFileNames;
};

struct NewestOutput {
    std::vector<NameHit> hits; // newest first; a folder's `modified` is its newest file's
    bool cancelled = false;
};

// The files (or folders) written last that are the user's own work: not
// under system, program or tool folders (EntryFlag::LowPriority), not
// hidden, not what tools write as they go (isWorkFile). Caller holds
// index.readLock().
NewestOutput newestFiles(const FileIndex& index, const NewestOptions& options, WorkerPool& pool,
    const std::function<bool()>& isCancelled);

// A name that tools write as they go, not one a person works on: dot-names,
// Office's "~$" lock files, downloads under way, logs, compiler output.
bool isWorkFile(std::string_view name, bool isDir) noexcept;

// Where a path typed from some folder along the way leads ("tools/out/" ->
// every folder out in a folder tools): `names` (UTF-8, outermost first) one
// inside the other, each just so, ASCII case aside as NTFS has it. Folders
// alone for `foldersOnly`. At most `limit`, lowest ids first; none when
// cancelled. Caller holds index.readLock().
std::vector<EntryId> findRelativePath(const FileIndex& index, const std::vector<std::string>& names, bool foldersOnly,
    std::size_t limit, WorkerPool& pool, const std::function<bool()>& isCancelled);

} // namespace ws
