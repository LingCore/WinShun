#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <system_error>
#include <vector>

// The folder the index service keeps its files in (设置 → 高级 → 索引位置):
// the snapshot, index.bin, and the content index's segments, content\*.grams,
// and documents' texts, content\*.texts.
// Nothing else in the folder is ever touched. Paths take either separator.
namespace ws::indexfolder {

QString snapshotPath(const QString& folder); // folder/index.bin
QString contentPath(const QString& folder); // folder/content

bool hasIndex(const QString& folder); // a snapshot is there
std::uint64_t size(const QString& folder); // bytes of the index files in it
// Nothing in it but index files, or nothing at all (or it is not there yet):
// the index can go there as it is.
bool onlyIndexFiles(const QString& folder);

// The disk `folder` is on can be unplugged while Windows runs (外接), which
// the index cannot live with: its files stay open, so the disk cannot be
// ejected, and pulling it out under the mapped content index would bring Win顺
// down. That is a USB, SD or FireWire disk, whatever the plug (U 盘, 移动硬盘,
// Type-C), and one behind a controller that can be unplugged (Thunderbolt and
// USB4 enclosures). A disk on a fixed controller counts as internal even if
// it can be hot-plugged itself: SATA ports set up for hot-plugging list their
// internal disks under Safely Remove Hardware, and eSATA cannot be told from
// them. False when it cannot be told (a volume across several disks, say).
bool onExternalDisk(const QString& folder);

// The disk `folder` is on is a hard disk: it has to seek, so the content
// index's scattered reads cost milliseconds each, and seconds when it has to
// spin up first. True if any disk under the volume says so; nullopt when one
// cannot tell (a virtual disk, RAID).
std::optional<bool> onSpinningDisk(const QString& folder);

// Reads the files into the file cache, where the content index's mapped
// views find them (the same pages): at low I/O priority, so other programs'
// reads go first, and resting as long as each read took, so the disk is never
// taken over. Not in background mode, which would also mark the pages as the
// first to go. Stops once `cancelled` says so.
void readIntoCache(const std::vector<QString>& files, const std::function<bool()>& cancelled);

// Copies the index files of `from` to `to` (made if need be), in place of any
// there, and has them written through to the disk. `progress` hears of the
// bytes copied so far; false from it cancels (ERROR_REQUEST_ABORTED). On
// failure the copies are deleted again.
using Progress = std::function<bool(std::uint64_t done, std::uint64_t total)>;
std::error_code copy(const QString& from, const QString& to, const Progress& progress = {});

// Deletes the index files in `folder`, and content\ once it is empty. What
// cannot be deleted (still open) is left.
void remove(const QString& folder);

} // namespace ws::indexfolder
