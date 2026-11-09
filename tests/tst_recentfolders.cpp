// The folders used lately (RecentFolders): the shortcuts of Windows' Recent
// folder are listed with Win32 alone, newest first, so that no shortcut's
// target is touched while listing (a server gone: tens of seconds each).
#include "RecentFolders.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>

using namespace Qt::StringLiterals;
using namespace ws::recentfolders;

namespace {

bool writeWithTime(const QString& path, int minutesAgo)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write("stub") != 4)
        return false;
    file.close();
    FILETIME now {};
    ::GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t {now.dwLowDateTime, now.dwHighDateTime};
    t.QuadPart -= static_cast<ULONGLONG>(minutesAgo) * 60ull * 10'000'000ull;
    const FILETIME written {t.LowPart, t.HighPart};
    const HANDLE handle = ::CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = ::SetFileTime(handle, nullptr, nullptr, &written);
    ::CloseHandle(handle);
    return ok;
}

} // namespace

class TestRecentFolders : public QObject {
    Q_OBJECT

private slots:
    void shortcutsNewestFirstListsOnlyShortcuts()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeWithTime(dir.filePath(u"old.lnk"_s), 30));
        QVERIFY(writeWithTime(dir.filePath(u"new.lnk"_s), 1));
        QVERIFY(writeWithTime(dir.filePath(u"middle.LNK"_s), 10)); // the ending is not case sensitive
        QVERIFY(writeWithTime(dir.filePath(u"note.txt"_s), 0)); // not a shortcut, however new
        QVERIFY(writeWithTime(dir.filePath(u"trick.lnkx"_s), 0)); // FindFirstFile's 8.3 matching would take it
        QVERIFY(QDir(dir.path()).mkdir(u"folder.lnk"_s)); // a folder of that name is no shortcut
        const std::vector<QString> all = shortcutsNewestFirst(dir.path(), 10);
        QCOMPARE(all.size(), 3u);
        QCOMPARE(QFileInfo(all[0]).fileName(), u"new.lnk"_s);
        QCOMPARE(QFileInfo(all[1]).fileName(), u"middle.LNK"_s);
        QCOMPARE(QFileInfo(all[2]).fileName(), u"old.lnk"_s);
        QVERIFY(all[0].startsWith(QDir::toNativeSeparators(dir.path()))); // native, full paths
        QVERIFY(!all[0].contains(u'/'));
        const std::vector<QString> two = shortcutsNewestFirst(dir.path(), 2);
        QCOMPARE(two.size(), 2u);
        QCOMPARE(QFileInfo(two[1]).fileName(), u"middle.LNK"_s);
        QVERIFY(shortcutsNewestFirst(dir.path(), 0).empty());
        QVERIFY(shortcutsNewestFirst(dir.filePath(u"missing"_s), 10).empty());
    }
};

QTEST_GUILESS_MAIN(TestRecentFolders)
#include "tst_recentfolders.moc"
