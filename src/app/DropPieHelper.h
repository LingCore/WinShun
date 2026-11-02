#pragma once

#include "Win32Util.h"

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QJsonObject>
#include <QObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QStringList>

#include <functional>

namespace ws {

class Favorites;
struct Settings;

// An elevated launcher cannot receive Explorer's OLE drops. Keep only the
// pie and its input listener in a process with the desktop user's token.
// It is started again whenever it is not running: once the desktop shell is
// there (an elevated autostart at logon comes first), and after it exits.
class DropPieHelper : public QObject {
    Q_DECLARE_TR_FUNCTIONS(DropPieHelper)
public:
    // A notification; `retry`, when given, is what clicking it does.
    using Notify = std::function<void(const QString& title, const QString& text, std::function<void()> retry)>;
    DropPieHelper(Favorites* favorites, Notify notify, QObject* parent = nullptr);
    ~DropPieHelper() override;
    void update(const Settings& settings);
    void retry(); // now, not at the next try: the shell is back (TaskbarCreated), or 重试
    QJsonObject state() const;
private:
    enum class Failure { Start, Exit, NoAnswer };
    bool snapshot();
    void start();
    void poll();
    void failed(Failure kind, const QString& detail, unsigned long code);
    Favorites* m_favorites;
    Notify m_notify;
    QTemporaryDir m_profile;
    win32::UniqueHandle m_process;
    QTimer m_poll; // while it runs
    QTimer m_retry; // the next start
    QElapsedTimer m_starting;
    qint64 m_shellSince = 0; // when the desktop shell started (ms since the epoch)
    int m_failures = 0; // in a row
    int m_noShell = 0; // WINSHUN_TEST_PIE_NO_SHELL: lookups still to fail
    QString m_failure; // the last one, for the log and the diagnostics
    bool m_ready = false;
    bool m_reported = false; // this run of failures shown
};

int runDropPieHelper(const QStringList& arguments);

} // namespace ws
