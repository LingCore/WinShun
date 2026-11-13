#pragma once

#include "platform/ComWorker.h"
#include "platform/DialogRules.h"

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>

#include <windows.h>

#include <memory>
#include <utility>

namespace ws {

// dialogs.json in the data folder: the user's rules for file windows that
// programs draw themselves (platform/DialogRules, GitHub #272). Read at the
// start and again whenever the file changes, so a rule written in an editor
// works as soon as it is saved.
//
// And the way to a rule without writing one: learn() waits a few seconds for
// the user to bring that window to the front, looks at it, and adds a rule
// for it to the file, with every box and button it saw beside the rule for
// the case that it picked the wrong box.
class DialogRuleStore : public QObject {
    Q_OBJECT

public:
    // `folder`: the data folder.
    explicit DialogRuleStore(const QString& folder, QObject* parent = nullptr);

    QString filePath() const;

    // The file to open in the user's editor: one with an example in it is
    // made when there is none. Empty when it could not be made.
    QString ensureFile();

    // Counts down (status tells), then adds a rule for the window in front.
    void learn();
    bool learning() const { return m_countdown.isActive() || m_looking; }

    // Adds `seen.rule` to the file, in place of a rule for the same program,
    // class and title; false when the file could not be written.
    static bool add(const QString& path, const dialogrules::Seen& seen);

signals:
    // A line for the settings page; `done`: learn() is over.
    void status(const QString& text, bool done);
    // Other rules are in use now (the file was read again).
    void changed();

private:
    void reload();
    void look();

    const QString m_folder;
    QFileSystemWatcher m_watcher;
    QTimer m_reload; // single shot: editors save in several steps
    QTimer m_countdown;
    int m_left = 0;
    bool m_looking = false;
    std::pair<QDateTime, qint64> m_stamp {QDateTime(), -2}; // the file as last read: changed, size
    std::unique_ptr<ComWorker> m_worker; // made for the first look: UI Automation waits on the program
};

} // namespace ws
