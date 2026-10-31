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
class DropPieHelper : public QObject {
    Q_DECLARE_TR_FUNCTIONS(DropPieHelper)
public:
    using Notify = std::function<void(const QString&, const QString&)>;
    DropPieHelper(Favorites* favorites, Notify notify, QObject* parent = nullptr);
    ~DropPieHelper() override;
    void update(const Settings& settings);
    QJsonObject state() const;
private:
    bool snapshot();
    void poll();
    Favorites* m_favorites;
    Notify m_notify;
    QTemporaryDir m_profile;
    win32::UniqueHandle m_process;
    QTimer m_poll;
    QElapsedTimer m_starting;
    bool m_reported = false;
};

int runDropPieHelper(const QStringList& arguments);

} // namespace ws
