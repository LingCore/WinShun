#pragma once

#include <QDateTime>
#include <QDir>
#include <QLockFile>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace ws {

// The folder the clipboard's display process (ClipboardDisplayReplica) keeps
// its settings and its window's state in, apart from the user's: a new one
// under %TEMP% at every start. The service ends that process together with
// its job, so it seldom gets to remove the folder itself. Hence the lock file
// in it, held as long as the process lives, and sweep(): each display process
// clears away what the ones before it left.
class DisplayProfile {
public:
    explicit DisplayProfile(const QString& root = QDir::tempPath());

    bool isValid() const { return m_lock && m_lock->isLocked(); }
    QString path() const { return m_folder.path(); }

    // Removes, under `root`, the folders of display processes that run no
    // more, and the empty "WinShun-XXXXXX" folders versions up to 0.23 left
    // beside them (a day old: other parts of Win顺 use such names for a
    // moment). Returns how many went. Any thread.
    static int sweep(const QString& root = QDir::tempPath(), const QDateTime& now = QDateTime::currentDateTimeUtc());

private:
    QTemporaryDir m_folder;
    std::unique_ptr<QLockFile> m_lock; // after m_folder: let go before the folder is removed
};

} // namespace ws
