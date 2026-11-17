#pragma once

#include "FileIndex.h"

#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace ws {

// Caller holds a read lock. This is an index estimate, never a disk walk:
// links and offline placeholders cannot trigger downloads or network I/O.
inline std::optional<std::uint64_t> indexedFolderSize(const FileIndex& index, EntryId folder,
    const std::function<bool()>& cancelled = {})
{
    if (folder >= index.slotCount() || index.scan(folder).isDeleted() || !index.scan(folder).isDir()) return {};
    std::vector<EntryId> pending;
    if (const EntryId first = index.entry(folder).firstChild(); first != kNoEntry) pending.push_back(first);
    std::uint64_t bytes = 0;
    std::size_t visited = 0;
    bool anyKnown = false;
    bool anyUnknown = false;
    while (!pending.empty()) {
        if (cancelled && cancelled()) return {};
        const EntryId id = pending.back();
        pending.pop_back();
        if (id >= index.slotCount() || ++visited > index.slotCount()) return {}; // damaged links cannot loop
        const Entry entry = index.entry(id); // the whole of it: a walk down the folders reads their lists
        if (entry.nextSibling != kNoEntry) pending.push_back(entry.nextSibling);
        if (entry.isDeleted()) continue;
        if (entry.isDir()) {
            if (entry.firstChild() != kNoEntry) pending.push_back(entry.firstChild());
        } else {
            // A file the index has no size for (written since and still open,
            // or one that could not be read) is left out: on a disk in use there
            // is always one somewhere, and the whole folder would go unknown
            // (Total Commander's sizes over the Everything interface, GitHub #364).
            const FileSize size = index.size(id);
            if (size == kUnknownSize) {
                anyUnknown = true;
                continue;
            }
            anyKnown = true;
            const auto value = unpackSize(size);
            if (value > std::numeric_limits<std::uint64_t>::max() - bytes) return {};
            bytes += value;
        }
    }
    if (anyUnknown && !anyKnown) return {}; // nothing known is not empty
    return bytes;
}

} // namespace ws
