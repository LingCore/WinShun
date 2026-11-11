#include "DisplayProfile.h"

#include <QFileInfo>

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr QLatin1StringView kPrefix {"WinShun-display-"};
constexpr QLatin1StringView kLock {"alive.lock"};
constexpr auto kBeingMade = 60s; // a folder with no lock file yet
constexpr auto kLeftBehind = 24h; // an empty folder of an earlier version

// Stale only once its process is gone, however long that one has run.
std::unique_ptr<QLockFile> lockOf(const QString& folder)
{
    auto lock = std::make_unique<QLockFile>(folder + u'/' + kLock);
    lock->setStaleLockTime(0ms);
    return lock;
}

bool isPlainFolder(const QFileInfo& entry)
{
    return entry.isDir() && !entry.isSymLink() && !entry.isJunction();
}

} // namespace

DisplayProfile::DisplayProfile(const QString& root) : m_folder(root + u'/' + kPrefix + u"XXXXXX"_s)
{
    if (!m_folder.isValid()) return;
    m_lock = lockOf(m_folder.path());
    m_lock->tryLock();
}

int DisplayProfile::sweep(const QString& root, const QDateTime& now)
{
    int removed = 0;
    const QDir dir(root);
    const auto age = [&now](const QFileInfo& entry) { return std::chrono::seconds(entry.lastModified().secsTo(now)); };
    for (const QFileInfo& entry : dir.entryInfoList({kPrefix + u'*'}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!isPlainFolder(entry)) continue;
        if (!QFileInfo::exists(entry.filePath() + u'/' + kLock) && age(entry) < kBeingMade) continue;
        const auto lock = lockOf(entry.filePath());
        if (!lock->tryLock()) continue; // its process lives
        lock->unlock();
        removed += QDir(entry.filePath()).removeRecursively();
    }
    for (const QFileInfo& entry : dir.entryInfoList({u"WinShun-??????"_s}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (isPlainFolder(entry) && age(entry) >= kLeftBehind)
            removed += dir.rmdir(entry.fileName()); // an empty one only
    }
    return removed;
}

} // namespace ws
