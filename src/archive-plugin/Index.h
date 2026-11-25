#pragma once

// What the plugin knows of every archive: the paths of its members, kept in
// memory and in one file of its data folder, and searched by the words
// typed. Copies of one archive (a delivery kept in several folders, a
// backup of a backup) share one list of members. Plain C++ (no libarchive):
// tests/tst_archive.cpp builds it in again.

#include "Lister.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wsarchive {

// What an archive holds, as it was last read.
struct Contents {
    static constexpr std::uint64_t kFolder = std::uint64_t {1} << 63;
    static constexpr std::uint64_t kLocked = std::uint64_t {1} << 62;
    static constexpr std::uint64_t kSize = kLocked - 1;

    std::string names; // the members' paths, each followed by '\n'
    std::vector<std::uint64_t> sizes; // a member's size, kFolder and kLocked
    std::vector<std::uint32_t> times; // seconds since 1970 (0: unknown)

    std::size_t count() const noexcept { return sizes.size(); }
    void add(const Member& member);
    friend bool operator==(const Contents&, const Contents&) = default;
};

// One archive.
struct Entry {
    std::string path; // UTF-8, full
    std::uint64_t size = 0; // the file's size and time when it was read, as Win顺's
    std::int64_t modified = 0; // index had them: read again when they differ
    Outcome outcome = Outcome::NotArchive;
    std::shared_ptr<const Contents> contents; // never null in the index; shared with its copies

    std::size_t count() const noexcept { return contents ? contents->count() : 0; }
};

struct Hit {
    std::string archive; // its full path
    std::string member; // its path in the archive
    std::uint32_t index = 0; // among the archive's members
    std::uint64_t size = 0;
    std::int64_t modified = 0;
    bool folder = false;
    bool locked = false;
    int rank = 0; // 0: every word in the member's own name; 1: some only in its folders or the archive's name
};

struct Where {
    std::vector<std::string> folders; // only archives under these (any case); empty: everywhere
    std::vector<std::string> extensions; // only members of these types, lower case, no dot; empty: all
};

struct Found {
    std::vector<Hit> hits; // the best first
    bool more = false; // there were more than asked for
};

struct Totals {
    std::size_t archives = 0;
    std::size_t members = 0; // of all the archives, copies counted each
    std::size_t nameBytes = 0; // kept in memory: copies counted once
    std::size_t byOutcome[kOutcomes] = {};
};

// The words of a query, split at spaces; "quoted words" stay together.
std::vector<std::string> wordsOf(std::string_view text);

// Where `hay` holds `needle` (already lower case) whatever the case of its
// ASCII letters, from `from` on; npos if nowhere.
std::size_t findFolded(std::string_view hay, std::string_view needle, std::size_t from = 0) noexcept;
std::string folded(std::string_view text); // ASCII letters to lower case

class Index {
public:
    // What save() wrote. False when there is none or it is damaged or of
    // another version: the index is then empty and built again.
    bool load(const std::wstring& file);
    // To `file`, through a temporary file beside it: whole or not at all.
    bool save(const std::wstring& file) const;

    // The archive at `path` as it is kept, or null.
    std::shared_ptr<const Entry> find(const std::string& path) const;
    // `entry` (its contents not looked at) with `contents`; the bytes of
    // names this added to what is kept (0: the same as another archive's).
    std::size_t put(Entry entry, Contents contents);
    // What an archive of this name, size and time holds, when one that was
    // read whole is known: a copy of it need not be opened to be listed too.
    // Null when the size or the time is unknown, or there is none.
    std::shared_ptr<const Contents> copyOf(std::string_view path, std::uint64_t size, std::int64_t modified) const;
    // `entry` as a copy of an archive already read (copyOf()).
    void put(Entry entry, std::shared_ptr<const Contents> contents);
    // Forgets every archive whose path is not in `paths`; how many.
    std::size_t keepOnly(const std::unordered_set<std::string>& paths);
    void clear();

    // The members every word is in: in their path, or some of the words in
    // the archive's own name (but one at least in the member's path).
    Found search(const std::vector<std::string>& words, std::size_t limit, const Where& where = {}) const;
    Totals totals() const;
    bool changed() const noexcept { return m_changed.load(); } // since load() or save()

private:
    using Shared = std::shared_ptr<const Contents>;
    Shared pooled(Contents contents, std::size_t* added); // with m_mutex held for writing
    void insert(Entry entry, Shared contents); // with m_mutex held for writing

    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<const Entry>> m_entries; // by path
    std::unordered_multimap<std::uint64_t, std::weak_ptr<const Contents>> m_pool; // by a hash of the names
    std::unordered_map<std::string, std::weak_ptr<const Contents>> m_copies; // by name, size and time (copyKey)
    mutable std::atomic_bool m_changed = false;
};

} // namespace wsarchive
