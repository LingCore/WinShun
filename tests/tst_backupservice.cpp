// The backup service (src/app/Backup) as the cloud backup plugin uses it
// through Win顺's tools backup_export and backup_restore (GitHub #276): a
// backup made for it into a file, not made again while nothing changed, and
// a backup taken back: checked, what there is now saved first, staged for
// the restart. On a profile in a temporary folder; the dialogs are not built in.

#include "Backup.h"
#include "BackupFiles.h"
#include "Settings.h"
#include "platform/Shell.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace ws;
using namespace Qt::StringLiterals;

// What Backup's buttons use and this test does not (Shell.cpp is not built in).
namespace ws::shell {
void open(const QString&, bool, std::function<void(bool)>) {}
QString pickFile(HWND, const QString&, const std::vector<std::pair<QString, QString>>&) { return {}; }
QString pickSaveFile(HWND, const QString&, const QString&, const QString&, const QString&) { return {}; }
} // namespace ws::shell

namespace {

void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class TestBackupService : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;

    Backup::Exported exported(Backup& backup, const QString& file, const QString& unless)
    {
        std::optional<Backup::Exported> result;
        backup.exportFor(file, unless, [&](const Backup::Exported& e) { result = e; });
        if (!QTest::qWaitFor([&] { return result.has_value(); }))
            return {u"no answer"_s};
        return *result;
    }

    struct Answer {
        QJsonObject result;
        QString error;
    };
    Answer tool(Backup& backup, bool restore, const QString& dataDir, const QJsonObject& arguments)
    {
        std::optional<Answer> answer;
        const auto done = [&](const QJsonObject& result, const QString& error) { answer = Answer {result, error}; };
        if (restore)
            backup.restoreTool(dataDir, arguments, done);
        else
            backup.exportTool(dataDir, arguments, done);
        if (!QTest::qWaitFor([&] { return answer.has_value(); }))
            return {{}, u"no answer"_s};
        return *answer;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        Settings::useProfile(m_tmp.filePath(u"profile"_s));
        QVERIFY(QDir::fromNativeSeparators(Settings::filePath()).startsWith(QDir::fromNativeSeparators(m_tmp.path()), Qt::CaseInsensitive));
        writeFile(Settings::filePath(), "[Launcher]\nHotkey=Alt+Space\n[Backup]\nFolder=D:/mine\n");
        writeFile(Settings::dataDir() + u"/favorites.json"_s, "{\"favorites\":[]}");
    }

    void exportsForAPlugin()
    {
        Backup backup;
        QSignalSpy changed(&backup, &Backup::changed);
        const QString file = m_tmp.filePath(u"plugin.data/upload.zip"_s);
        QDir().mkpath(QFileInfo(file).path());

        const Backup::Exported first = exported(backup, file, {});
        QVERIFY2(first.problem.isEmpty(), qPrintable(first.problem));
        QVERIFY(first.written);
        QVERIFY(!first.hash.isEmpty());
        QVERIFY(first.when.isValid());
        QCOMPARE(backupfiles::hashOf(file), first.hash); // a backup of Win顺's
        // As 导出备份: without this computer's backup folder.
        const QString unpacked = m_tmp.filePath(u"unpacked"_s);
        QCOMPARE(backupfiles::unpack(file, unpacked), QString());
        const QByteArray ini = readFile(unpacked + u"/WinShun.ini"_s);
        QVERIFY(ini.contains("Alt+Space"));
        QVERIFY(!ini.contains("D:/mine"));

        // What the plugin has already: nothing is written.
        QVERIFY(QFile::remove(file));
        const Backup::Exported same = exported(backup, file, first.hash);
        QVERIFY(same.problem.isEmpty());
        QVERIFY(!same.written);
        QCOMPARE(same.hash, first.hash);
        QVERIFY(!QFile::exists(file));

        // Changed since: written, with another hash.
        writeFile(Settings::dataDir() + u"/favorites.json"_s, "{\"favorites\":[{\"path\":\"D:/work\"}]}");
        const Backup::Exported next = exported(backup, file, first.hash);
        QVERIFY(next.problem.isEmpty());
        QVERIFY(next.written);
        QVERIFY(next.hash != first.hash);
        QCOMPARE(backupfiles::hashOf(file), next.hash);

        // Nowhere to write it: said, not thrown away silently.
        const Backup::Exported nowhere = exported(backup, m_tmp.filePath(u"missing/folder/upload.zip"_s), {});
        QVERIFY(!nowhere.problem.isEmpty());
        QVERIFY(!nowhere.written);

        // The settings page hears nothing of it, and the folder gets no backup.
        QCOMPARE(changed.count(), 0);
        QVERIFY(!backup.busy());
        QVERIFY(!QFileInfo::exists(Settings::dataDir() + u"/backups"_s));
    }

    // backup_export and backup_restore as a plugin calls them: only files in
    // its own data folder, and the answers it reads.
    void toolsKeepToTheDataFolder()
    {
        Backup backup;
        QSignalSpy restart(&backup, &Backup::restartRequested);
        const QString dataDir = QDir::toNativeSeparators(m_tmp.filePath(u"tools.data"_s));
        QDir().mkpath(dataDir);
        const QString inside = dataDir + u"\\upload.zip"_s;

        const Answer made = tool(backup, false, dataDir, {{u"file"_s, inside}});
        QVERIFY2(made.error.isEmpty(), qPrintable(made.error));
        QCOMPARE(made.result.value(u"written"_s).toBool(), true);
        const QString hash = made.result.value(u"hash"_s).toString();
        QCOMPARE(backupfiles::hashOf(inside), hash);
        QVERIFY(QDateTime::fromString(made.result.value(u"when"_s).toString(), Qt::ISODate).isValid());
        const Answer again = tool(backup, false, dataDir, {{u"file"_s, inside}, {u"unless"_s, hash}});
        QCOMPARE(again.result.value(u"written"_s).toBool(true), false);
        QCOMPARE(again.result.value(u"hash"_s).toString(), hash);

        // Anywhere else: refused, nothing written and nothing read.
        const QString outside = QDir::toNativeSeparators(m_tmp.filePath(u"elsewhere.zip"_s));
        QVERIFY(QFile::copy(inside, outside));
        for (const QString& file : {outside, dataDir + u"\\..\\elsewhere.zip"_s, u"upload.zip"_s, QString(),
                 QDir::toNativeSeparators(m_tmp.filePath(u"tools.data2/upload.zip"_s))}) {
            const Answer no = tool(backup, false, dataDir, {{u"file"_s, file}});
            QVERIFY2(no.error.contains(u"data folder"_s), qPrintable(file));
            const Answer neither = tool(backup, true, dataDir, {{u"file"_s, file}});
            QVERIFY2(neither.error.contains(u"data folder"_s), qPrintable(file));
        }
        QVERIFY(!QFileInfo::exists(m_tmp.filePath(u"tools.data2"_s)));
        QCOMPARE(restart.count(), 0);

        // What is no backup: the reason, no restart.
        writeFile(dataDir + u"/junk.zip"_s, "junk");
        const Answer junk = tool(backup, true, dataDir, {{u"file"_s, dataDir + u"\\junk.zip"_s}});
        QVERIFY(!junk.error.isEmpty());
        QVERIFY(junk.result.isEmpty());
        QCOMPARE(restart.count(), 0);
        QVERIFY(QTest::qWaitFor([&] { return !backup.busy(); }));

        // A backup of its own: Win顺 restarts with it.
        const Answer restored = tool(backup, true, dataDir, {{u"file"_s, inside}});
        QVERIFY2(restored.error.isEmpty(), qPrintable(restored.error));
        QCOMPARE(restored.result.value(u"restarting"_s).toBool(), true);
        QTRY_COMPARE(restart.count(), 1);
        Backup::applyRestored(); // as the next start would; the next test starts clean
        QVERIFY(QDir(Settings::dataDir() + u"/backups"_s).removeRecursively());
    }

    void restoresFromAPluginsFile()
    {
        Backup backup;
        QSignalSpy restart(&backup, &Backup::restartRequested);
        const QString file = m_tmp.filePath(u"plugin.data/restore.zip"_s);
        QVERIFY(exported(backup, file, {}).written);
        const QByteArray saved = readFile(Settings::filePath());

        // Not a backup: said, nothing staged, no restart.
        const QString junk = m_tmp.filePath(u"plugin.data/junk.zip"_s);
        writeFile(junk, "not a zip");
        std::optional<QString> answer;
        QVERIFY(backup.restoreFrom(junk, [&](const QString& problem) { answer = problem; }));
        // One at a time.
        QVERIFY(!backup.restoreFrom(file, [](const QString&) { QFAIL("the second one must not run"); }));
        QVERIFY(QTest::qWaitFor([&] { return answer.has_value(); }));
        QVERIFY(!answer->isEmpty());
        QCOMPARE(restart.count(), 0);
        QVERIFY(!QFileInfo::exists(Settings::dataDir() + u"/restore"_s));

        // The settings changed after the backup; the backup brings the old ones back.
        writeFile(Settings::filePath(), "[Launcher]\nHotkey=Ctrl+Space\n[Backup]\nFolder=D:/mine\n");
        answer.reset();
        QVERIFY(QTest::qWaitFor([&] { return !backup.busy(); }));
        QVERIFY(backup.restoreFrom(file, [&](const QString& problem) { answer = problem; }));
        QVERIFY(QTest::qWaitFor([&] { return answer.has_value(); }));
        QVERIFY2(answer->isEmpty(), qPrintable(*answer));
        QTRY_COMPARE(restart.count(), 1);
        // What there was is kept on this computer first.
        const QList<backupfiles::Found> kept = backupfiles::list(Settings::dataDir() + u"/backups"_s);
        QCOMPARE(kept.size(), 1);
        // Put in place by the next start, the locations of this computer kept.
        QVERIFY(QFileInfo(Settings::dataDir() + u"/restore"_s).isDir());
        Backup::applyRestored();
        const QByteArray now = readFile(Settings::filePath());
        QVERIFY2(now.contains("Alt+Space"), now.constData());
        QVERIFY2(now.contains("D:/mine") || now.contains("D:\\mine"), now.constData());
        QVERIFY(!QFileInfo::exists(Settings::dataDir() + u"/restore"_s));
        Q_UNUSED(saved);
    }
};

QTEST_MAIN(TestBackupService)
#include "tst_backupservice.moc"
