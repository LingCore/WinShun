// The settings page's side of the command line ws (src/app/CommandLine,
// McpClients): the PATH as text, the analysis of where ws is, writing the
// user's PATH in the registry (in keys of the test's own, never the real
// ones), and Claude Code's Skill installed through a stand-in ws.exe.
#include "CommandLine.h"
#include "McpClients.h"
#include "platform/Integrity.h"

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <windows.h>

#include <cstdlib>
#include <functional>
#include <memory>

using namespace Qt::StringLiterals;
namespace cli = ws::cli;

namespace {

const QString kBin = u"C:\\wstest\\app\\bin"_s;

// A made-up disk: the files in it, compared without regard to case.
std::function<bool(const QString&)> disk(const QStringList& files)
{
    QSet<QString> lower;
    for (const QString& file : files)
        lower.insert(file.toLower());
    return [lower](const QString& file) { return lower.contains(file.toLower()); };
}

// A variable of this process with a value in any language (qputenv takes the
// ANSI code page); empty removes it. _wputenv_s, not SetEnvironmentVariableW:
// Qt reads the C runtime's copy, which only the former updates (the child
// processes get the operating system's, which both do).
void setEnvironment(const wchar_t* name, const QString& value)
{
    _wputenv_s(name, value.toStdWString().c_str());
}

cli::Sources sources(const QString& system, const QString& user, const QString& recorded = {})
{
    return {system, user, {u".COM"_s, u".EXE"_s, u".CMD"_s, u".PS1"_s}, recorded};
}

} // namespace

class TestCommandLine : public QObject {
    Q_OBJECT

    cli::Locations m_where; // throwaway keys, made in init() and deleted in cleanup()
    QString m_root;

    QString userPath() const { return cli::readValue(m_where.userRoot, m_where.userKey, L"Path").text; }
    DWORD userType() const { return cli::readValue(m_where.userRoot, m_where.userKey, L"Path").type; }

private slots:
    void init()
    {
        m_root = u"Software\\WinShunTest\\"_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_where.systemRoot = HKEY_CURRENT_USER;
        m_where.systemKey = (m_root + u"\\System"_s).toStdWString();
        m_where.userRoot = HKEY_CURRENT_USER;
        m_where.userKey = (m_root + u"\\User"_s).toStdWString();
        m_where.recordRoot = HKEY_CURRENT_USER;
        m_where.recordKey = (m_root + u"\\Record"_s).toStdWString();
    }

    void cleanup()
    {
        ::RegDeleteTreeW(HKEY_CURRENT_USER, m_root.toStdWString().c_str());
        ::RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\WinShunTest"); // when it is empty
    }

    void normalizing()
    {
        qputenv("WS_TEST_TOOLS", "C:\\wstest\\Tools");
        QCOMPARE(cli::normalized(u"\"C:/wstest/Tools/bin/\""_s), u"C:\\wstest\\Tools\\bin"_s);
        QCOMPARE(cli::normalized(u"  %WS_TEST_TOOLS%\\bin  "_s), u"C:\\wstest\\Tools\\bin"_s);
        QCOMPARE(cli::normalized(u"\\\\server\\share\\"_s), u"\\\\server\\share"_s);
        QCOMPARE(cli::normalized(u"C:\\"_s), u"C:\\"_s);
        QVERIFY(cli::normalized(u""_s).isEmpty());
        QVERIFY(cli::sameFolder(u"C:\\wstest\\Tools\\BIN\\"_s, u"c:/wstest/tools/bin"_s));
        QVERIFY(cli::sameFolder(u"%WS_TEST_TOOLS%\\bin"_s, u"C:\\wstest\\tools\\bin"_s));
        QVERIFY(!cli::sameFolder(u"C:\\wstest\\Tools\\bin2"_s, u"C:\\wstest\\Tools\\bin"_s));
        QVERIFY(!cli::sameFolder({}, {})); // nothing is not a folder
        QVERIFY(!cli::sameFolder(u"\"\""_s, {}));
    }

    void addingAnEntry()
    {
        QCOMPARE(cli::withEntry(u"C:\\A;C:\\B"_s, kBin), u"C:\\A;C:\\B;"_s + kBin);
        QCOMPARE(cli::withEntry(u"C:\\A;"_s, kBin), u"C:\\A;"_s + kBin); // not ";;"
        QCOMPARE(cli::withEntry({}, kBin), kBin);
        // Already there, however it is written: untouched, byte for byte.
        qputenv("WS_TEST_APP", "C:\\wstest\\app");
        for (const QString& path : {u"C:\\A;C:\\WSTEST\\app\\bin\\"_s, u"\"c:/wstest/app/bin\";C:\\A"_s,
                 u"%WS_TEST_APP%\\bin;C:\\A"_s}) {
            QCOMPARE(cli::withEntry(path, kBin), path);
            QVERIFY(cli::hasEntry(path, kBin));
        }
        QVERIFY(!cli::hasEntry(u"C:\\wstest\\app\\bin2;C:\\wstest\\app"_s, kBin));
    }

    void takingEntriesOut()
    {
        // Every spelling of it goes; the rest stays as written: %VAR%, quotes, empty entries.
        const QString path = u"%SystemRoot%\\system32;C:\\wstest\\app\\bin;;\"C:\\Other Tools\";c:\\wstest\\APP\\bin\\"_s;
        QCOMPARE(cli::withoutEntry(path, kBin), u"%SystemRoot%\\system32;;\"C:\\Other Tools\""_s);
        QCOMPARE(cli::withoutEntry(u"C:\\A;"_s + kBin, kBin), u"C:\\A"_s);
        QCOMPARE(cli::withoutEntry(u"C:\\A;"_s + kBin + u";"_s, kBin), u"C:\\A;"_s); // the closing ";" is kept
        QCOMPARE(cli::withoutEntry(kBin, kBin), QString());
        const QString without = u"C:\\A;;C:\\B;"_s;
        QCOMPARE(cli::withoutEntry(without, kBin), without); // nothing to take out: not a byte changed
    }

    void analysing()
    {
        const QString exe = kBin + u"\\ws.exe"_s;
        {
            // In the system list (the installer's), found.
            const cli::Status s = cli::analyse(kBin, sources(u"C:\\wstest\\sys;"_s + kBin, u"C:\\wstest\\u1"_s), disk({exe}));
            QCOMPARE(s.exe, exe);
            QCOMPARE(s.where, cli::Status::Where::System);
            QVERIFY(s.onPath());
            QVERIFY(!s.removable);
            QVERIFY(s.shadow.isEmpty());
        }
        {
            // In the user's list, and WinShun put it there: it may take it out.
            const cli::Status s = cli::analyse(kBin, sources({}, u"C:\\wstest\\u1;"_s + kBin, kBin), disk({exe}));
            QCOMPARE(s.where, cli::Status::Where::User);
            QVERIFY(s.removable);
        }
        {
            // A note of the entry without the entry, or the entry without the note: nothing to take out.
            QVERIFY(!cli::analyse(kBin, sources({}, u"C:\\wstest\\u1"_s, kBin), disk({exe})).removable);
            QVERIFY(!cli::analyse(kBin, sources({}, kBin), disk({exe})).removable);
        }
        {
            // Not on the PATH; no ws elsewhere.
            const cli::Status s = cli::analyse(kBin, sources(u"C:\\wstest\\sys"_s, u"C:\\wstest\\u1"_s), disk({exe}));
            QVERIFY(!s.onPath());
            QCOMPARE(s.where, cli::Status::Where::None);
            QVERIFY(s.shadow.isEmpty());
        }
        {
            // Another ws in front: found by PATHEXT, in the system list before the user's.
            const QString old = u"C:\\wstest\\old\\ws.cmd"_s;
            const cli::Status s = cli::analyse(kBin, sources(u"C:\\wstest\\old"_s, kBin), disk({exe, old}));
            QCOMPARE(s.shadow, old);
            QCOMPARE(s.where, cli::Status::Where::User);
        }
        {
            // PowerShell's ws.ps1 counts too; only the first is named.
            const cli::Status s = cli::analyse(kBin, sources({}, u"C:\\wstest\\p;C:\\wstest\\q;"_s + kBin),
                disk({exe, u"C:\\wstest\\p\\ws.ps1"_s, u"C:\\wstest\\q\\ws.exe"_s}));
            QCOMPARE(s.shadow, u"C:\\wstest\\p\\ws.ps1"_s);
        }
        {
            // After ours, it cannot come first.
            const cli::Status s = cli::analyse(kBin, sources(kBin + u";C:\\wstest\\old"_s, {}), disk({exe, u"C:\\wstest\\old\\ws.exe"_s}));
            QVERIFY(s.shadow.isEmpty());
        }
        {
            // Not on the PATH yet: any other ws on it will be in front of ours.
            const cli::Status s = cli::analyse(kBin, sources(u"C:\\wstest\\old"_s, {}), disk({exe, u"C:\\wstest\\old\\ws.exe"_s}));
            QVERIFY(!s.onPath());
            QCOMPARE(s.shadow, u"C:\\wstest\\old\\ws.exe"_s);
        }
        {
            // A name that is not ws.
            const cli::Status s = cli::analyse(kBin, sources(u"C:\\wstest\\old"_s, kBin), disk({exe, u"C:\\wstest\\old\\wsl.exe"_s, u"C:\\wstest\\old\\ws.txt"_s}));
            QVERIFY(s.shadow.isEmpty());
        }
        {
            // No ws.exe where it should be.
            const cli::Status s = cli::analyse(kBin, sources(kBin, {}), disk({}));
            QVERIFY(s.exe.isEmpty());
            QVERIFY(s.onPath());
            QVERIFY(!cli::toVariant(s).value(u"exeFound"_s).toBool());
        }
    }

    void readingTheRegistry()
    {
        QVERIFY(!cli::readValue(m_where.userRoot, m_where.userKey, L"Path").present);
        QCOMPARE(cli::writeValue(m_where.systemRoot, m_where.systemKey, L"Path", u"C:\\wstest\\sys"_s, REG_EXPAND_SZ), DWORD(0));
        QCOMPARE(cli::writeValue(m_where.systemRoot, m_where.systemKey, L"PATHEXT", u".EXE;.CMD"_s, REG_SZ), DWORD(0));
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", u"%USERPROFILE%\\bin"_s, REG_EXPAND_SZ), DWORD(0));
        cli::Sources read = cli::readSources(m_where);
        QCOMPARE(read.systemPath, u"C:\\wstest\\sys"_s);
        QCOMPARE(read.userPath, u"%USERPROFILE%\\bin"_s); // not expanded
        QCOMPARE(read.pathExt, (QStringList {u".EXE"_s, u".CMD"_s, u".PS1"_s})); // PowerShell's added
        QVERIFY(read.recorded.isEmpty());
        // The user's own PATHEXT replaces the system's.
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"PATHEXT", u".BAT"_s, REG_SZ), DWORD(0));
        QCOMPARE(cli::readSources(m_where).pathExt, (QStringList {u".BAT"_s, u".PS1"_s}));
    }

    void puttingItOnTheUserPath()
    {
        const QString original = u"%USERPROFILE%\\bin;C:\\wstest\\Tools\\;\"C:\\wstest\\Quoted Dir\""_s;
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", original, REG_EXPAND_SZ), DWORD(0));
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        // The old text exactly as it was (%USERPROFILE% not expanded), the type kept, ours at the end.
        QCOMPARE(userPath(), original + u';' + kBin);
        QCOMPARE(userType(), DWORD(REG_EXPAND_SZ));
        QCOMPARE(cli::readSources(m_where).recorded, kBin);
        // Again: no second entry.
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), original + u';' + kBin);

        // Taking out something else's note does nothing.
        QCOMPARE(cli::removeFromUserPath(u"C:\\wstest\\other\\bin"_s, m_where), DWORD(ERROR_NOT_FOUND));
        QCOMPARE(userPath(), original + u';' + kBin);
        // Its own: back to the original, and the note is gone.
        QCOMPARE(cli::removeFromUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), original);
        QCOMPARE(userType(), DWORD(REG_EXPAND_SZ));
        QVERIFY(cli::readSources(m_where).recorded.isEmpty());
        QCOMPARE(cli::removeFromUserPath(kBin, m_where), DWORD(ERROR_NOT_FOUND)); // nothing to do twice
    }

    void anEntryThatIsNotOurs()
    {
        // The installer (or the user) wrote it: adding it again changes nothing, and it is not
        // noted as WinShun's, so it cannot be taken out from the page.
        const QString path = u"C:\\wstest\\Tools;"_s + kBin;
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", path, REG_EXPAND_SZ), DWORD(0));
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), path);
        QVERIFY(cli::readSources(m_where).recorded.isEmpty());
        QCOMPARE(cli::removeFromUserPath(kBin, m_where), DWORD(ERROR_NOT_FOUND));
        QCOMPARE(userPath(), path);
    }

    void aPathThatIsNotThereOrNotExpandable()
    {
        // No PATH value at all: made, as REG_EXPAND_SZ like Windows' own.
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), kBin);
        QCOMPARE(userType(), DWORD(REG_EXPAND_SZ));
        // A plain REG_SZ stays one.
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", u"C:\\wstest\\Tools"_s, REG_SZ), DWORD(0));
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userType(), DWORD(REG_SZ));
        QCOMPARE(userPath(), u"C:\\wstest\\Tools;"_s + kBin);
        // The one entry out: an empty value, not a missing one.
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", kBin, REG_EXPAND_SZ), DWORD(0));
        QCOMPARE(cli::writeValue(m_where.recordRoot, m_where.recordKey, L"CliPathEntry", kBin, REG_SZ), DWORD(0));
        QCOMPARE(cli::removeFromUserPath(kBin, m_where), DWORD(0));
        QVERIFY(cli::readValue(m_where.userRoot, m_where.userKey, L"Path").present);
        QVERIFY(userPath().isEmpty());
    }

    void claudeCodesSkill()
    {
        if (ws::integrity::processElevated())
            QSKIP("Elevated, the program would not see this test's CLAUDE_CONFIG_DIR and write into the real Claude folder");
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString config = QDir::toNativeSeparators(temporary.filePath(u"配置 claude"_s));
        const QString before = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
        setEnvironment(L"CLAUDE_CONFIG_DIR", config);
        const auto restore = qScopeGuard([&] { setEnvironment(L"CLAUDE_CONFIG_DIR", before); });
        const QString skill = QDir::toNativeSeparators(config + u"/skills/winshun/SKILL.md"_s);
        QCOMPARE(ws::mcpclients::skillPath(), skill);
        const auto claudeCode = [] {
            for (const ws::mcpclients::Client& c : ws::mcpclients::detect()) {
                if (c.id == u"claude-code")
                    return c;
            }
            return ws::mcpclients::Client {};
        };
        QVERIFY(!claudeCode().skillInstalled);
        QCOMPARE(claudeCode().skillPath, skill);

        // ws.exe in a bin folder of its own, the stand-in.
        QVERIFY(QDir().mkpath(temporary.filePath(u"bin"_s)));
        const QString ws = QDir::toNativeSeparators(temporary.filePath(u"bin/ws.exe"_s));
        QVERIFY(QFile::copy(QStringLiteral(WINSHUN_FAKE_WS), ws));

        struct Pending {
            bool done = false;
            ws::mcpclients::Result result;
        };
        const auto install = [&](const QString& exe) {
            auto pending = std::make_shared<Pending>();
            ws::mcpclients::installSkill(exe, this, [pending](const ws::mcpclients::Result& r) {
                pending->result = r;
                pending->done = true;
            });
            return pending;
        };

        // Failing: its words, both streams, in UTF-8, and the code.
        setEnvironment(L"FAKE_WS_FAIL", u"没有写入权限"_s);
        setEnvironment(L"FAKE_WS_BOTH", u"1"_s);
        auto failing = install(ws);
        QTRY_VERIFY_WITH_TIMEOUT(failing->done, 15000);
        setEnvironment(L"FAKE_WS_FAIL", {});
        setEnvironment(L"FAKE_WS_BOTH", {});
        QVERIFY(!failing->result.ok);
        QVERIFY2(failing->result.message.contains(u"没有写入权限"_s), qPrintable(failing->result.message));
        QVERIFY2(failing->result.message.contains(u"to standard output first"_s), qPrintable(failing->result.message));
        QVERIFY2(failing->result.message.contains(u"退出码 7"_s), qPrintable(failing->result.message));
        QVERIFY(!claudeCode().skillInstalled);

        // Working: the file is there, in the folder the page looks in, and the message names it.
        auto working = install(ws);
        QTRY_VERIFY_WITH_TIMEOUT(working->done, 15000);
        QVERIFY2(working->result.ok, qPrintable(working->result.message));
        QVERIFY2(working->result.message.contains(skill), qPrintable(working->result.message));
        QVERIFY(QFile::exists(skill));
        QVERIFY(claudeCode().skillInstalled);

        // No ws.exe: said at once, nothing run.
        auto missing = install({});
        QVERIFY(missing->done);
        QVERIFY(!missing->result.ok);
        QVERIFY(missing->result.message.contains(u"ws.exe"_s));
    }
};

QTEST_MAIN(TestCommandLine)
#include "tst_commandline.moc"
