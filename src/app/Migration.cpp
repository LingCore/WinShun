#include "Migration.h"
#include "Settings.h"
#include "platform/Shell.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// What QuickFind's MessageWindow answered to: a second instance sent it
// commands this way.
void quitRunningQuickFind()
{
    const HWND target = ::FindWindowW(L"QuickFind.MessageWindow", nullptr);
    if (!target)
        return;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(target, &pid);
    const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    const QString command = u"quit"_s;
    COPYDATASTRUCT data {};
    data.dwData = 0x51464E44; // "QFND"
    data.cbData = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<void*>(static_cast<const void*>(command.utf16()));
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, &result);
    if (process) {
        ::WaitForSingleObject(process, 30'000); // it saves its index on the way out
        ::CloseHandle(process);
    }
}

// Moves the folder when the new one does not exist yet (it is left alone if it
// does), then renames the files inside that carried the old name.
bool moveFolder(const QString& from, const QString& to, const QStringList& renames)
{
    if (!QFileInfo(from).isDir() || QFileInfo::exists(to) || !QDir().rename(from, to))
        return false;
    QDir dir(to);
    for (const QString& name : renames) {
        if (dir.exists(name))
            dir.rename(name, QString(name).replace(u"QuickFind"_s, u"WinShun"_s));
    }
    return true;
}

} // namespace

void migrateFromQuickFind()
{
    const QString oldName = u"QuickFind"_s;
    const QString oldConfig
        = QFileInfo(QSettings(QSettings::IniFormat, QSettings::UserScope, oldName, oldName).fileName()).absolutePath();
    const QString oldData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u'/' + oldName;
    if (!QFileInfo(oldConfig).isDir() && !QFileInfo(oldData).isDir())
        return; // the usual case: nothing left of the old name

    quitRunningQuickFind();
    const QString newConfig = QFileInfo(Settings::filePath()).absolutePath();
    const QString newData = QDir::fromNativeSeparators(Settings::dataDir());
    moveFolder(oldConfig, newConfig, {u"QuickFind.ini"_s});
    if (moveFolder(oldData, newData, {u"QuickFind.log"_s, u"QuickFind.old.log"_s}))
        QDir(newData + u"/QuickFind"_s).removeRecursively(); // Qt's cache under the old name
    autostart::migrateFromQuickFind();
}

} // namespace ws
