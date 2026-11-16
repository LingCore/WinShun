#pragma once

#include "Crawler.h"
#include "FileIndex.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace ws {

// Folders the index holds that Win顺's own searches leave out (GitHub #351).
// With the Everything compatibility on (Settings::everythingCompat), the
// folders excluded in 搜索范围 are indexed all the same, for the programs
// that ask Win顺 as they would ask Everything (Total Commander's folder
// sizes); only Win顺's own results go without them. The rules are those of
// a CrawlRules' excluded paths and names, as the Crawler applies them to
// the disks.
class HiddenFolders {
public:
    explicit HiddenFolders(CrawlRules rules);

    // What is known of the folders looked at, kept while one search looks at
    // many entries (siblings share their folders): one per thread and search.
    class Memo {
    private:
        friend class HiddenFolders;
        struct Slot {
            EntryId folder = kNoEntry;
            bool hidden = false;
        };
        static constexpr std::size_t kSlots = 1024;
        std::vector<Slot> m_slots; // made when first needed
    };

    // Whether `entry` is one of those folders or in one. Caller holds index.readLock().
    bool hides(const FileIndex& index, const EntryScan& entry, Memo& memo) const;
    // The same for a full path ("D:\a\b").
    bool hidesPath(std::wstring_view path) const { return m_rules.isExcludedPath(path); }

    const CrawlRules& rules() const noexcept { return m_rules.rules(); }

private:
    bool folderHidden(const FileIndex& index, EntryId folder, Memo& memo) const;
    // A rule for the folder itself (not for those it is in).
    bool ruleFor(const FileIndex& index, EntryId folder) const;
    // Whether a name (UTF-8) is the last part of a rule: a folder with any
    // other name is none, which spares building its path.
    bool mayBeNamed(std::string_view name) const;

    Crawler m_rules;
    std::vector<std::wstring> m_lastNames;
};

} // namespace ws
