#include "platform/Integrity.h"
#include "platform/Shell.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QTemporaryDir>
#include <QTest>

#include <tlhelp32.h>

#include <atomic>
#include <memory>

using namespace Qt::StringLiterals;

namespace {
QString parentProgram()
{
    const ws::win32::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry {sizeof entry};
    DWORD parent = 0;
    if (::Process32FirstW(snapshot.get(), &entry)) {
        do {
            if (entry.th32ProcessID == ::GetCurrentProcessId()) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (::Process32NextW(snapshot.get(), &entry));
    }
    const ws::win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent));
    wchar_t path[32768] {};
    DWORD size = static_cast<DWORD>(std::size(path));
    return process.valid() && ::QueryFullProcessImageNameW(process.get(), 0, path, &size)
        ? QString::fromWCharArray(path, size) : QString();
}
} // namespace

class TestShellLaunch : public QObject {
    Q_OBJECT
private slots:
    void customCommandViaExplorer()
    {
        if (!::GetShellWindow())
            QSKIP("No desktop shell in this test session");
        QTemporaryDir temporary(QDir::tempPath() + u"/WinShun launch-XXXXXX"_s);
        QVERIFY(temporary.isValid());
        // Explorer uses its own PATH, not CTest's Qt PATH. Stage only this
        // helper and its Qt DLLs so the test also works on a clean build.
        const QString helper = temporary.filePath(u"launch-probe.exe"_s);
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), helper));
        const QDir qtBin(QLibraryInfo::path(QLibraryInfo::BinariesPath));
        for (const auto* module : {"Core", "Gui", "Sql", "Test"}) {
            const QString dll = u"Qt6"_s + QString::fromLatin1(module)
                + (QLibraryInfo::isDebugBuild() ? u"d.dll"_s : u".dll"_s);
            QVERIFY(QFile::copy(qtBin.filePath(dll), temporary.filePath(dll)));
        }
        const QString working = temporary.filePath(u"工作 folder"_s);
        QVERIFY(QDir().mkpath(working));
        const QString output = temporary.filePath(u"result.json"_s);
        const QString query = u"中文 \"引号\" C:\\末尾\\ {query} %TEMP% & text"_s;
        const QString command = u'"' + helper
            + u"\" --probe \""_s + output + u"\" {query}"_s;
        const auto done = std::make_shared<std::atomic<int>>(-1);
        ws::shell::runCustom(command, working, query, [done](bool ok) { done->store(ok ? 1 : 0); });
        QTRY_VERIFY_WITH_TIMEOUT(done->load() >= 0, 10000);
        QCOMPARE(done->load(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output), 10000);
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject result = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY2(result.value(u"parent"_s).toString().endsWith(u"\\explorer.exe", Qt::CaseInsensitive),
            qPrintable(result.value(u"parent"_s).toString()));
        QVERIFY(!result.value(u"elevated"_s).toBool());
        QCOMPARE(QDir::cleanPath(result.value(u"cwd"_s).toString()), QDir::cleanPath(working));
        QCOMPARE(result.value(u"arguments"_s).toArray().last().toString(), query);
        qInfo() << "Launched by" << result.value(u"parent"_s).toString() << "with the requested arguments and folder";
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 4 && args[1] == u"--probe") {
        const QJsonObject result {
            {u"parent"_s, parentProgram()}, {u"cwd"_s, QDir::currentPath()},
            {u"elevated"_s, ws::integrity::processElevated()},
            {u"arguments"_s, QJsonArray::fromStringList(args)},
        };
        QFile file(args[2]);
        return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(result).toJson()) > 0 ? 0 : 1;
    }
    TestShellLaunch test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_shelllaunch.moc"
