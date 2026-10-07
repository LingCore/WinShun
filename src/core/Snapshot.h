#pragma once

#include "Crawler.h"
#include "FileIndex.h"

#include <QString>

#include <memory>
#include <vector>

// On-disk copy of the index so the launcher is searchable instantly after a
// restart. Deleted slots are dropped (the file is always compact).
namespace qf::snapshot {

// Streams the index to disk through a small buffer and replaces the file
// atomically. Caller holds index.readLock(). `journals` is parallel to
// `volumes` (or empty): where each volume's change journal had been read to.
bool save(const FileIndex& index, const std::vector<VolumeInfo>& volumes,
    const std::vector<JournalPosition>& journals, const QString& filePath);

// Returns nullptr when the file is missing, corrupt, from another version, or
// was taken from a different set of volumes. Fills `journals` (parallel to
// the volumes) when given.
std::unique_ptr<FileIndex> load(const QString& filePath, const std::vector<VolumeInfo>& expectedVolumes,
    std::vector<JournalPosition>* journals = nullptr);

} // namespace qf::snapshot
