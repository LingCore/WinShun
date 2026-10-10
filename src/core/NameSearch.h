#pragma once

#include "FileIndex.h"
#include "Query.h"
#include "SearchTypes.h"
#include "WorkerPool.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace ws {

struct NameHit {
    EntryId id = kNoEntry;
    int score = 0;
    FileTime modified = 0;
};

struct NameSearchOptions {
    std::size_t limit = 100;
    bool foldersOnly = false;
    RankBy rankBy = RankBy::Match;
    // The best `limit` files and the best `limit` folders, each apart: for a
    // list that puts one kind before the other (KindOrder).
    bool kindsApart = false;
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

} // namespace ws
