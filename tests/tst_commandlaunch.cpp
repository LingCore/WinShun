#include "CommandLaunch.h"
#include "Favorites.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>

using namespace ws;
using namespace ws::commandlaunch;
using namespace Qt::StringLiterals;

namespace {

// Starts a plan as Shell.cpp's startHidden does (not elevated): created with
// CREATE_NO_WINDOW, and waited for here so that the tests can read what it did.
bool runAndWait(const Plan& plan, const QString& directory, DWORD* exitCode = nullptr)
{
    std::wstring line = plan.commandLine.toStdWString();
    STARTUPINFOW startup {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION started {};
    const std::wstring dir = directory.toStdWString();
    if (!::CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            dir.empty() ? nullptr : dir.c_str(), &startup, &started))
        return false;
    const bool finished = ::WaitForSingleObject(started.hProcess, 20000) == WAIT_OBJECT_0;
    if (exitCode && finished)
        ::GetExitCodeProcess(started.hProcess, exitCode);
    ::CloseHandle(started.hThread);
    ::CloseHandle(started.hProcess);
    return finished;
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeAll(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace

// How a custom command is started without a console window (GitHub #112):
// the kind of program, the command lines made for each, and, for real, what
// the programs see of their arguments.
class TestCommandLaunch : public QObject {
    Q_OBJECT

private slots:
    void kindsByExtensionAndSubsystem()
    {
        const Tools tools = systemTools();
        QVERIFY(QFile::exists(tools.cmd));
        QVERIFY(QFile::exists(tools.powershell));
        QCOMPARE(kindOf(tools.cmd), Kind::Console);
        QCOMPARE(kindOf(tools.powershell), Kind::Console);
        const QString notepad = resolveProgram(u"notepad"_s);
        QVERIFY(!notepad.isEmpty());
        QCOMPARE(kindOf(notepad), Kind::Gui);
        QCOMPARE(kindOf(u"C:\\x\\a.BAT"_s), Kind::Batch);
        QCOMPARE(kindOf(u"C:\\x\\a.cmd"_s), Kind::Batch);
        QCOMPARE(kindOf(u"C:\\x\\a.Ps1"_s), Kind::PowerShell);
        QCOMPARE(kindOf(u"C:\\x\\a.txt"_s), Kind::Other);
        QCOMPARE(kindOf(u"C:\\x\\a.lnk"_s), Kind::Other);
        QCOMPARE(kindOf(u"C:\\no\\such.exe"_s), Kind::Other); // unreadable: never hidden
    }

    void subsystemOfNotAnImage()
    {
        QTemporaryDir dir;
        const QString text = dir.filePath(u"fake.exe"_s);
        QVERIFY(writeAll(text, "MZ not a real image"));
        QCOMPARE(peSubsystem(text), 0);
        QCOMPARE(kindOf(text), Kind::Other);
        QVERIFY(writeAll(text, QByteArray()));
        QCOMPARE(peSubsystem(text), 0);
    }

    void resolvesLikeTheShell_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("found");
        QTest::newRow("exe without extension") << u"cmd"_s << true;
        QTest::newRow("exe with extension") << u"cmd.exe"_s << true;
        QTest::newRow("in System32") << u"%windir%\\System32\\cmd.exe"_s << true;
        QTest::newRow("path without extension") << u"%windir%\\System32\\cmd"_s << true;
        QTest::newRow("there is none") << u"winshun-no-such-program-112"_s << false;
        QTest::newRow("empty") << QString() << false;
    }
    void resolvesLikeTheShell()
    {
        QFETCH(QString, name);
        QFETCH(bool, found);
        const QString path = resolveProgram(name);
        QCOMPARE(!path.isEmpty(), found);
        if (found)
            QVERIFY(QFileInfo::exists(path));
    }

    void resolvesInTheWorkingFolderAndScripts()
    {
        QTemporaryDir dir;
        const QString folder = QDir::toNativeSeparators(dir.path());
        QVERIFY(writeAll(dir.filePath(u"tool.bat"_s), "@echo off\r\n"));
        QCOMPARE(QFileInfo(resolveProgram(u"tool"_s, folder)).fileName(), u"tool.bat"_s);
        QCOMPARE(QFileInfo(resolveProgram(u"tool.bat"_s, folder)).fileName(), u"tool.bat"_s);
        QVERIFY(resolveProgram(u"tool"_s, QString()).isEmpty());
        QVERIFY(!resolveProgram(folder + u"\\tool"_s).isEmpty()); // a path: .bat tried
    }

    void plansQuoteEachKind()
    {
        const Tools tools {u"C:\\Windows\\System32\\cmd.exe"_s, u"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"_s};
        const QString tool = u"C:\\My Tools\\tool.exe"_s;
        Plan p = silentPlan(tool, u"a \"b c\""_s, Kind::Console, tools);
        QVERIFY(p.ok);
        QCOMPARE(p.application, tool);
        QCOMPARE(p.commandLine, u"\"C:\\My Tools\\tool.exe\" a \"b c\""_s);
        QCOMPARE(silentPlan(tool, {}, Kind::Console, tools).commandLine, u"\"C:\\My Tools\\tool.exe\""_s);
        QCOMPARE(silentPlan(tool, u"   "_s, Kind::Console, tools).commandLine, u"\"C:\\My Tools\\tool.exe\""_s);

        p = silentPlan(u"C:\\My Tools\\go.bat"_s, u"x \"y z\""_s, Kind::Batch, tools);
        QVERIFY(p.ok);
        QCOMPARE(p.application, tools.cmd);
        QCOMPARE(p.commandLine,
            u"\"C:\\Windows\\System32\\cmd.exe\" /d /s /c \"\"C:\\My Tools\\go.bat\" x \"y z\"\""_s);

        p = silentPlan(u"C:\\My Tools\\go.ps1"_s, u"-Name 报告"_s, Kind::PowerShell, tools);
        QVERIFY(p.ok);
        QCOMPARE(p.application, tools.powershell);
        QCOMPARE(p.commandLine,
            u"\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive "
            u"-ExecutionPolicy Bypass -File \"C:\\My Tools\\go.ps1\" -Name 报告"_s);
    }

    void plansSplitTheProgramFromItsArguments()
    {
        const Tools tools {u"C:\\Windows\\System32\\cmd.exe"_s, u"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"_s};
        // What ShellExecute's "runas" is given for a script that is run as administrator, with its window.
        Plan p = silentPlan(u"C:\\My Tools\\go.bat"_s, u"x \"y z\""_s, Kind::Batch, tools, true);
        QCOMPARE(p.application, tools.cmd);
        QCOMPARE(p.arguments, u"/d /s /c \"\"C:\\My Tools\\go.bat\" x \"y z\"\""_s);
        QCOMPARE(p.commandLine, u"\"%1\" %2"_s.arg(tools.cmd, p.arguments));
        p = silentPlan(u"C:\\My Tools\\go.ps1"_s, {}, Kind::PowerShell, tools, true);
        QCOMPARE(p.arguments, u"-NoProfile -ExecutionPolicy Bypass -File \"C:\\My Tools\\go.ps1\""_s);
        QVERIFY(!p.arguments.contains(u"NonInteractive"_s)); // its window is there to be answered in
        p = silentPlan(u"C:\\My Tools\\go.ps1"_s, {}, Kind::PowerShell, tools, false);
        QVERIFY(p.arguments.contains(u"-NonInteractive"_s));
        p = silentPlan(u"C:\\t\\tool.exe"_s, u"/a"_s, Kind::Console, tools, true);
        QCOMPARE(p.application, u"C:\\t\\tool.exe"_s);
        QCOMPARE(p.arguments, u"/a"_s);
    }

    void windowedAndUnknownProgramsAreNotHidden()
    {
        const Tools tools = systemTools();
        QVERIFY(!silentPlan(u"C:\\Windows\\notepad.exe"_s, {}, Kind::Gui, tools).ok);
        QVERIFY(!silentPlan(u"C:\\a.txt"_s, {}, Kind::Other, tools).ok);
        QVERIFY(!silentPlan(u"C:\\a.bat"_s, {}, Kind::Batch, Tools {}).ok); // no cmd.exe found
        QVERIFY(!silentPlan(u"C:\\a.ps1"_s, {}, Kind::PowerShell, Tools {}).ok);
    }

    void wordsForCmd_data()
    {
        QTest::addColumn<QString>("words");
        QTest::addColumn<bool>("safe");
        QTest::newRow("plain") << u"hello world"_s << true;
        QTest::newRow("chinese and path") << u"报告 D:\\工作\\a.txt"_s << true;
        QTest::newRow("ampersand") << u"a & calc"_s << false;
        QTest::newRow("pipe") << u"a|b"_s << false;
        QTest::newRow("redirect") << u"a>b"_s << false;
        QTest::newRow("caret") << u"a^b"_s << false;
        QTest::newRow("percent") << u"%PATH%"_s << false;
        QTest::newRow("quote") << u"a\"b"_s << false;
        QTest::newRow("bang") << u"a!b"_s << false;
        QTest::newRow("newline") << u"a\nb"_s << false;
        QTest::newRow("nothing") << QString() << true;
    }
    void wordsForCmd()
    {
        QFETCH(QString, words);
        QFETCH(bool, safe);
        QCOMPARE(safeForCmd(words), safe);
    }

    // The plans, for real: a console program, a batch file and a script get
    // their arguments whole (spaces, Chinese, quotes) and the working folder.
    void consoleProgramGetsItsArguments()
    {
        QTemporaryDir dir;
        const QString out = QDir::toNativeSeparators(dir.filePath(u"out.txt"_s));
        const Tools tools = systemTools();
        // cmd.exe is a console program too: its own command line is the arguments.
        const Plan plan = silentPlan(tools.cmd, u"/d /c echo hi 报告 > \""_s + out + u'"', Kind::Console, tools);
        QVERIFY(plan.ok);
        DWORD code = 99;
        QVERIFY(runAndWait(plan, dir.path(), &code));
        QCOMPARE(code, DWORD(0));
        QVERIFY2(readAll(out).startsWith("hi "), readAll(out).constData());
    }

    void batchFileGetsArgumentsAndWorkingFolder()
    {
        QTemporaryDir root;
        QDir().mkpath(root.filePath(u"work dir 报告"_s));
        QDir().mkpath(root.filePath(u"my scripts"_s));
        const QString script = QDir::toNativeSeparators(root.filePath(u"my scripts/args 报告.bat"_s));
        const QString out = QDir::toNativeSeparators(root.filePath(u"out.txt"_s));
        // %~1 strips the quotes: what the script was given as its first argument, whole.
        QVERIFY(writeAll(script,
            (u"@echo off\r\nchcp 65001 >nul\r\n(echo [%~1]& echo [%~2]& cd) > \""_s + out + u"\"\r\nexit /b 7\r\n"_s).toUtf8()));
        const Plan plan = silentPlan(script, u"\"first arg 报告\" second"_s, Kind::Batch, systemTools());
        QVERIFY(plan.ok);
        DWORD code = 0;
        QVERIFY(runAndWait(plan, QDir::toNativeSeparators(root.filePath(u"work dir 报告"_s)), &code));
        QCOMPARE(code, DWORD(7));
        const QString text = QString::fromUtf8(readAll(out));
        QVERIFY2(text.contains(u"[first arg 报告]"_s), qPrintable(text));
        QVERIFY2(text.contains(u"[second]"_s), qPrintable(text));
        QVERIFY2(text.contains(u"work dir 报告"_s), qPrintable(text));
    }

    void scriptGetsArgumentsAndNeverWaitsForAnswers()
    {
        QTemporaryDir root;
        const QString script = QDir::toNativeSeparators(root.filePath(u"my script 报告.ps1"_s));
        const QString out = QDir::toNativeSeparators(root.filePath(u"out.txt"_s));
        QVERIFY(writeAll(script,
            (u"param($One, $Two)\r\n[IO.File]::WriteAllText('"_s + out + u"', \"[$One][$Two][\" + (Get-Location).Path + \"]\")\r\n"_s)
                .toUtf8()
                .prepend("\xEF\xBB\xBF"))); // with a BOM: Windows PowerShell 5.1 reads the Chinese name
        const Plan plan = silentPlan(script, u"\"one two 报告\" three"_s, Kind::PowerShell, systemTools());
        QVERIFY(plan.ok);
        DWORD code = 99;
        QVERIFY(runAndWait(plan, QDir::toNativeSeparators(root.path()), &code));
        QCOMPARE(code, DWORD(0));
        const QString text = QString::fromUtf8(readAll(out));
        QVERIFY2(text.contains(u"[one two 报告][three]["_s), qPrintable(text));
    }

    void commandFlagsTravelWithFavorites()
    {
        Favorite command {Favorite::Kind::Command, u"winshun-command:flags"_s, u"清理"_s, u"clean"_s, u"clean.bat"_s};
        // Neither: the file stays as an older copy reads it.
        QVERIFY(writeFavoritesFile({command}).contains("\"version\": 2"));
        QVERIFY(!writeFavoritesFile({command}).contains("silent"));

        command.silent = true;
        QByteArray written = writeFavoritesFile({command});
        QVERIFY(written.contains("\"version\": 4"));
        QVERIFY(written.contains("\"silent\": true"));
        QVERIFY(!written.contains("\"admin\""));
        auto loaded = readFavoritesFile(written);
        QVERIFY(loaded);
        QCOMPARE(loaded->favorites, FavoriteList {command});
        QVERIFY(loaded->favorites[0].silent);
        QVERIFY(!loaded->favorites[0].runAsAdmin);

        command.silent = false;
        command.runAsAdmin = true;
        written = writeFavoritesFile({command});
        QVERIFY(written.contains("\"admin\": true"));
        loaded = readFavoritesFile(written);
        QVERIFY(loaded);
        QVERIFY(loaded->favorites[0].runAsAdmin);
        QVERIFY(!loaded->favorites[0].silent);

        // Not true (a number, text) is no.
        loaded = readFavoritesFile(R"({"format": "winshun-favorites", "version": 4, "favorites": [
            {"kind": "command", "path": "winshun-command:x", "name": "x", "command": "a.exe", "silent": "yes", "admin": 1}]})");
        QVERIFY(loaded);
        QVERIFY(!loaded->favorites[0].silent);
        QVERIFY(!loaded->favorites[0].runAsAdmin);
    }

    void searchRowsCarryTheFlags()
    {
        Favorite command {Favorite::Kind::Command, u"winshun-command:row"_s, u"行"_s, u"row"_s, u"a.exe"_s};
        command.silent = true;
        command.runAsAdmin = true;
        SearchResult row;
        row.path = command.path;
        row.name = command.name;
        row.app = AppKind::Tool;
        row.target = command.command;
        row.silent = command.silent;
        row.runAsAdmin = command.runAsAdmin;
        const std::optional<Favorite> back = favoriteOf(row);
        QVERIFY(back);
        QVERIFY(back->silent);
        QVERIFY(back->runAsAdmin);
    }
};

QTEST_GUILESS_MAIN(TestCommandLaunch)
#include "tst_commandlaunch.moc"
