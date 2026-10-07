#pragma once

#include "FileIndex.h"
#include "Query.h"
#include "SearchTypes.h"
#include "WorkerPool.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace qf {

struct NameHit {
    EntryId id = kNoEntry;
    int score = 0;
};

struct NameSearchOutput {
    std::vector<NameHit> hits; // best first
    std::size_t totalMatches = 0;
    bool cancelled = false;
};

// Scans the whole index in parallel and keeps the `limit` best matches.
// Caller holds index.readLock().
NameSearchOutput searchNames(const FileIndex& index, const NameMatcher& matcher, Scope scope, std::size_t limit,
    WorkerPool& pool, const std::function<bool()>& isCancelled);

} // namespace qf
