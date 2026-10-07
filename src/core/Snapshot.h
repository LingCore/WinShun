#pragma once

#include "Crawler.h"
#include "FileIndex.h"

#include <QString>

#include <memory>
#include <optional>
#include <vector>

// On-disk copy of the index so the launcher is searchable instantly after a
// restart. Deleted slots are dropped (the file is always compact).
namespace qf::snapshot {

// Streams the index to disk through a small buffer and replaces the file
// atomically. Caller holds index.readLock(). `journals` is parallel to
// `volumes` (or empty): where each volume's change journal had been read to.
// `rules` are the ones the index was built with.
bool save(const FileIndex& index, const std::vector<VolumeInfo>& volumes,
    const std::vector<JournalPosition>& journals, const CrawlRules& rules, const QString& filePath);

struct Contents {
    std::unique_ptr<FileIndex> index;
    std::vector<VolumeInfo> volumes; // as saved: root and serial number
    std::vector<JournalPosition> journals; // parallel to `volumes`
    std::optional<CrawlRules> rules; // what it was built with; older files do not say
};

// Nullopt when the file is missing, corrupt or from an unknown version. The
// caller keeps the volumes that are still there (see IndexService).
std::optional<Contents> load(const QString& filePath);

} // namespace qf::snapshot
