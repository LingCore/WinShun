// The settings page's side of the command line ws (src/app/CommandLine,
// McpClients): the PATH as text, the analysis of where ws is, writing the
// user's PATH in the registry (in keys of the test's own, never the real
// ones), and Claude Code's Skill installed through a stand-in ws.exe.
#include "CommandLine.h"
#include "McpClients.h"
#include "KnownFolderOverride.h"
#include "platform/Integrity.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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
        // Only the entry as it was written goes (the last such one); another spelling of the same
        // folder is the user's. The rest stays as written: %VAR%, quotes, empty entries.
        const QString path = u"%SystemRoot%\\system32;C:\\wstest\\app\\bin;;\"C:\\Other Tools\";c:\\wstest\\APP\\bin\\"_s;
        QCOMPARE(cli::withoutExactEntry(path, kBin), u"%SystemRoot%\\system32;;\"C:\\Other Tools\";c:\\wstest\\APP\\bin\\"_s);
        QVERIFY(cli::hasExactEntry(path, kBin));
        QVERIFY(!cli::hasExactEntry(path, u"c:\\wstest\\app\\bin"_s)); // not the same text
        QCOMPARE(cli::withoutExactEntry(u"C:\\A;"_s + kBin, kBin), u"C:\\A"_s);
        QCOMPARE(cli::withoutExactEntry(u"C:\\A;"_s + kBin + u";"_s, kBin), u"C:\\A;"_s); // the closing ";" is kept
        QCOMPARE(cli::withoutExactEntry(kBin, kBin), QString());
        const QString without = u"C:\\A;;C:\\B;"_s;
        QCOMPARE(cli::withoutExactEntry(without, kBin), without); // nothing to take out: not a byte changed
        QCOMPARE(cli::withoutExactEntry(without, {}), without);
        // Two of them: the last (ours is appended at the end), the other stays.
        QCOMPARE(cli::withoutExactEntry(kBin + u";C:\\A;"_s + kBin, kBin), kBin + u";C:\\A"_s);
    }

    void aFolderWithASemicolon()
    {
        // A ';' inside quotes is part of the entry, as Windows reads a PATH.
        const QString odd = u"C:\\wstest\\a;b\\bin"_s;
        QCOMPARE(cli::entryOf(odd), u"\"C:\\wstest\\a;b\\bin\""_s);
        QCOMPARE(cli::entryOf(kBin), kBin); // quotes only when needed
        QCOMPARE(cli::entryOf(u"C:/wstest/app/bin"_s), kBin); // native separators
        const QString path = u"C:\\A;\"C:\\wstest\\a;b\\bin\";C:\\B;"_s;
        QCOMPARE(cli::splitPath(path), (QStringList {u"C:\\A"_s, u"\"C:\\wstest\\a;b\\bin\""_s, u"C:\\B"_s, QString()}));
        QCOMPARE(cli::splitPath(path).join(u';'), path); // joined again: not a byte changed
        QCOMPARE(cli::splitPath({}), QStringList {QString()});
        QVERIFY(cli::hasEntry(path, odd));
        QVERIFY(!cli::hasEntry(path, u"C:\\wstest\\a"_s));
        QVERIFY(!cli::hasEntry(path, u"b\\bin"_s));
        QCOMPARE(cli::withEntry(path, odd), path); // recognised: not appended again
        QCOMPARE(cli::withEntry(u"C:\\A"_s, odd), u"C:\\A;\"C:\\wstest\\a;b\\bin\""_s);
        // And in the analysis: on the PATH, found where the two halves are not folders of their own.
        const cli::Status s = cli::analyse(odd, sources({}, path), disk({odd + u"\\ws.exe"_s}));
        QCOMPARE(s.where, cli::Status::Where::User);
    }

    void scoopShimIsNotInTheWay()
    {
        // Scoop shims ws.exe into <root>\shims, which it puts on the user's PATH: that is ws working.
        const QString bin = u"C:\\wstest\\scoop\\apps\\winshun\\current\\bin"_s;
        const QString shim = u"C:\\wstest\\scoop\\shims\\ws.exe"_s;
        const cli::Status s = cli::analyse(bin, sources({}, u"C:\\wstest\\scoop\\shims"_s), disk({bin + u"\\ws.exe"_s, shim}));
        QVERIFY(s.shadow.isEmpty());
        QVERIFY(s.onPath());
        QVERIFY(!s.removable); // not WinShun's own PATH entry
        // Another ws in front is still reported.
        const cli::Status other = cli::analyse(bin, sources({}, u"C:\\wstest\\u1"_s), disk({bin + u"\\ws.exe"_s, u"C:\\wstest\\u1\\ws.exe"_s}));
        QVERIFY(!other.shadow.isEmpty());
        // A path that is not in a Scoop install: the shim rule does not apply.
        const cli::Status plain = cli::analyse(kBin, sources({}, u"C:\\wstest\\scoop\\shims"_s), disk({kBin + u"\\ws.exe"_s, shim}));
        QVERIFY(!plain.shadow.isEmpty());
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
            // The user's own spelling of the folder is not the entry WinShun wrote.
            const cli::Status other = cli::analyse(kBin, sources({}, u"c:\\wstest\\APP\\bin\\"_s, kBin), disk({exe}));
            QCOMPARE(other.where, cli::Status::Where::User);
            QVERIFY(!other.removable);
            // A note of another folder is not about this one.
            QVERIFY(!cli::analyse(kBin, sources({}, kBin, u"C:\\wstest\\other\\bin"_s), disk({exe})).removable);
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

    // ---- a PATH that is not what it should be: left exactly as it is ----

    static DWORD writeBinary(const cli::Locations& where, const wchar_t* name, const QByteArray& bytes, DWORD type = REG_BINARY)
    {
        HKEY key = nullptr;
        LSTATUS status = ::RegCreateKeyExW(where.userRoot, where.userKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS)
            return static_cast<DWORD>(status);
        status = ::RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(bytes.constData()), static_cast<DWORD>(bytes.size()));
        ::RegCloseKey(key);
        return static_cast<DWORD>(status);
    }

    // The value as stored, of any type: "<type>:<bytes in hex>".
    static QByteArray storedValue(const cli::Locations& where, const wchar_t* name)
    {
        DWORD type = 0;
        DWORD size = 0;
        if (::RegGetValueW(where.userRoot, where.userKey.c_str(), name, RRF_RT_ANY | RRF_NOEXPAND, &type, nullptr, &size) != ERROR_SUCCESS)
            return QByteArray("<absent>");
        QByteArray bytes(static_cast<qsizetype>(size), '\0');
        ::RegGetValueW(where.userRoot, where.userKey.c_str(), name, RRF_RT_ANY | RRF_NOEXPAND, &type, bytes.data(), &size);
        return QByteArray::number(type) + ':' + bytes.toHex();
    }

    static bool keyExists(HKEY root, const std::wstring& key)
    {
        HKEY handle = nullptr;
        if (::RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS)
            return false;
        ::RegCloseKey(handle);
        return true;
    }

    void aPathOfAnotherTypeIsLeftAlone_data()
    {
        QTest::addColumn<quint32>("type");
        QTest::newRow("REG_BINARY") << quint32(REG_BINARY);
        QTest::newRow("REG_DWORD") << quint32(REG_DWORD);
        QTest::newRow("REG_MULTI_SZ") << quint32(REG_MULTI_SZ);
    }

    void aPathOfAnotherTypeIsLeftAlone()
    {
        QFETCH(quint32, type);
        const QByteArray bytes("\x01\x02\x03\x04", 4);
        QCOMPARE(writeBinary(m_where, L"Path", bytes, type), DWORD(0));
        const QByteArray before = storedValue(m_where, L"Path");
        QVERIFY(before != "<absent>");

        // Adding: the error back, the PATH not a byte different, nothing noted.
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(ERROR_UNSUPPORTED_TYPE));
        QCOMPARE(storedValue(m_where, L"Path"), before);
        QVERIFY(!keyExists(m_where.recordRoot, m_where.recordKey));
        QVERIFY(!cli::readValue(m_where.userRoot, m_where.userKey, L"Path").present);
        QCOMPARE(cli::readValue(m_where.userRoot, m_where.userKey, L"Path").error, DWORD(ERROR_UNSUPPORTED_TYPE));

        // Looking: the error is carried, not read as an empty PATH.
        const cli::Sources read = cli::readSources(m_where);
        QCOMPARE(read.pathError, DWORD(ERROR_UNSUPPORTED_TYPE));
        QVERIFY(read.userPath.isEmpty());
        QCOMPARE(cli::analyse(kBin, read, disk({})).pathError, DWORD(ERROR_UNSUPPORTED_TYPE));
        QCOMPARE(cli::toVariant(cli::analyse(kBin, read, disk({}))).value(u"pathError"_s).toUInt(), uint(ERROR_UNSUPPORTED_TYPE));

        // Taking out: with a note of ours, the error back, PATH and note untouched.
        QCOMPARE(cli::writeValue(m_where.recordRoot, m_where.recordKey, L"CliPathEntry", kBin, REG_SZ), DWORD(0));
        QCOMPARE(cli::removeFromUserPath(kBin, m_where), DWORD(ERROR_UNSUPPORTED_TYPE));
        QCOMPARE(storedValue(m_where, L"Path"), before);
        QCOMPARE(cli::readSources(m_where).recorded, kBin);
    }

    void aSystemPathOfAnotherTypeIsAnErrorToo()
    {
        // The user's list is fine; the system's cannot be read: adding goes on (the worst is a duplicate),
        // and the look says the list was not read.
        HKEY key = nullptr;
        QCOMPARE(::RegCreateKeyExW(m_where.systemRoot, m_where.systemKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr), LSTATUS(ERROR_SUCCESS));
        const DWORD value = 7;
        QCOMPARE(::RegSetValueExW(key, L"Path", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value), LSTATUS(ERROR_SUCCESS));
        ::RegCloseKey(key);
        QCOMPARE(cli::readSources(m_where).pathError, DWORD(ERROR_UNSUPPORTED_TYPE));
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), kBin);
    }

    void aValueThatGrowsWhileItIsRead()
    {
        // The size asked, then the value grows before it is read: the read starts over
        // and gives the whole of it (it used to be taken for "not there").
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", u"C:\\wstest\\short"_s, REG_EXPAND_SZ), DWORD(0));
        const QString grown = u"C:\\wstest\\short;"_s + QString(500, u'x');
        int calls = 0;
        const cli::RawValue value = cli::readValue(m_where.userRoot, m_where.userKey, L"Path", [&] {
            if (++calls == 1)
                cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", grown, REG_EXPAND_SZ);
        });
        QVERIFY(value.present);
        QCOMPARE(value.error, DWORD(0));
        QCOMPARE(value.text, grown);
        QCOMPARE(calls, 2);

        // One that never stops growing is an error, not a short or empty text.
        int growth = 0;
        const cli::RawValue endless = cli::readValue(m_where.userRoot, m_where.userKey, L"Path", [&] {
            cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", grown + QString(++growth * 100, u'y'), REG_EXPAND_SZ);
        });
        QVERIFY(!endless.present);
        QCOMPARE(endless.error, DWORD(ERROR_MORE_DATA));
    }

    void aMissingValueIsAbsentNotAnError()
    {
        const cli::RawValue none = cli::readValue(m_where.userRoot, m_where.userKey, L"Path"); // not even the key
        QVERIFY(!none.present);
        QCOMPARE(none.error, DWORD(0));
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Other", u"x"_s, REG_SZ), DWORD(0));
        const cli::RawValue noPath = cli::readValue(m_where.userRoot, m_where.userKey, L"Path"); // the key, not the value
        QVERIFY(!noPath.present);
        QCOMPARE(noPath.error, DWORD(0));
        QCOMPARE(cli::readSources(m_where).pathError, DWORD(0));
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), kBin);
        QCOMPARE(cli::readValue(m_where.userRoot, m_where.userKey, L"Other").text, u"x"_s); // the neighbour untouched
    }

    // ---- what is written, and what is taken out again ----

    void aFolderWithASemicolonOnTheUserPath()
    {
        const QString odd = u"C:\\wstest\\a;b\\bin"_s;
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", u"C:\\wstest\\Tools"_s, REG_EXPAND_SZ), DWORD(0));
        QCOMPARE(cli::addToUserPath(odd, m_where), DWORD(0));
        const QString quoted = u"\"C:\\wstest\\a;b\\bin\""_s;
        QCOMPARE(userPath(), u"C:\\wstest\\Tools;"_s + quoted);
        QCOMPARE(cli::readSources(m_where).recorded, quoted);
        // Again, and again: recognised, not appended once more.
        QCOMPARE(cli::addToUserPath(odd, m_where), DWORD(0));
        QCOMPARE(cli::addToUserPath(odd, m_where), DWORD(0));
        QCOMPARE(userPath(), u"C:\\wstest\\Tools;"_s + quoted);
        const cli::Status s = cli::analyse(odd, cli::readSources(m_where), disk({odd + u"\\ws.exe"_s}));
        QCOMPARE(s.where, cli::Status::Where::User);
        QVERIFY(s.removable);
        QCOMPARE(cli::removeFromUserPath(odd, m_where), DWORD(0));
        QCOMPARE(userPath(), u"C:\\wstest\\Tools"_s);
    }

    void onlyTheEntryWeWroteIsTakenOut()
    {
        // The user added their own spelling of the folder too (after ours): it stays.
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        const QString both = kBin + u";C:\\wstest\\Tools;c:\\wstest\\APP\\bin\\"_s;
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", both, REG_EXPAND_SZ), DWORD(0));
        QCOMPARE(cli::removeFromUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), u"C:\\wstest\\Tools;c:\\wstest\\APP\\bin\\"_s);
        QVERIFY(cli::readSources(m_where).recorded.isEmpty());

        // Only the user's spelling there: not ours, the PATH stays, and the stale note goes.
        QCOMPARE(cli::writeValue(m_where.recordRoot, m_where.recordKey, L"CliPathEntry", kBin, REG_SZ), DWORD(0));
        QCOMPARE(cli::removeFromUserPath(kBin, m_where), DWORD(ERROR_NOT_FOUND));
        QCOMPARE(userPath(), u"C:\\wstest\\Tools;c:\\wstest\\APP\\bin\\"_s);
    }

    void theSystemPathNamesItAlready()
    {
        // An all-users install put it on the system's list: no second entry on the user's, no note.
        QCOMPARE(cli::writeValue(m_where.systemRoot, m_where.systemKey, L"Path", u"C:\\wstest\\sys;"_s + kBin, REG_EXPAND_SZ), DWORD(0));
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", u"C:\\wstest\\Tools"_s, REG_EXPAND_SZ), DWORD(0));
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), u"C:\\wstest\\Tools"_s);
        QVERIFY(cli::readSources(m_where).recorded.isEmpty());
    }

    void aCopyThatMovedTakesItsOldEntryAlong()
    {
        const QString oldBin = u"D:\\old place\\WinShun\\bin"_s;
        const QString original = u"C:\\wstest\\Tools"_s;
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", original, REG_EXPAND_SZ), DWORD(0));
        QCOMPARE(cli::addToUserPath(oldBin, m_where), DWORD(0));
        QCOMPARE(userPath(), original + u';' + oldBin);
        // The folder moved; the page of the new place enables it: the old entry goes, the new comes, the note follows.
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), original + u';' + kBin);
        QCOMPARE(cli::readSources(m_where).recorded, kBin);
        QCOMPARE(cli::removeFromUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), original);

        // The old entry gone already (the user took it out): just the new one.
        QCOMPARE(cli::addToUserPath(oldBin, m_where), DWORD(0));
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", original, REG_EXPAND_SZ), DWORD(0));
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), original + u';' + kBin);

        // The old entry also the user's own spelling of something else: only the exact text goes.
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", original + u";d:\\OLD PLACE\\winshun\\bin"_s, REG_EXPAND_SZ), DWORD(0));
        QCOMPARE(cli::writeValue(m_where.recordRoot, m_where.recordKey, L"CliPathEntry", oldBin, REG_SZ), DWORD(0));
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(userPath(), original + u";d:\\OLD PLACE\\winshun\\bin;"_s + kBin);
    }

    void aNoteThatCannotBeKeptStopsEverything()
    {
        // The key of the note cannot be made (a name over 255 characters): the PATH is not touched.
        m_where.recordKey = (m_root + u"\\Record\\"_s + QString(300, u'n')).toStdWString();
        const QString original = u"C:\\wstest\\Tools"_s;
        QCOMPARE(cli::writeValue(m_where.userRoot, m_where.userKey, L"Path", original, REG_EXPAND_SZ), DWORD(0));
        QVERIFY(cli::addToUserPath(kBin, m_where) != DWORD(0));
        QCOMPARE(userPath(), original);
    }

    void theKeyOfTheNoteGoesWhenItIsEmpty()
    {
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QVERIFY(keyExists(m_where.recordRoot, m_where.recordKey));
        QCOMPARE(cli::removeFromUserPath(kBin, m_where), DWORD(0));
        QVERIFY(!keyExists(m_where.recordRoot, m_where.recordKey));

        // With something else in it, it stays, and so does the something.
        QCOMPARE(cli::addToUserPath(kBin, m_where), DWORD(0));
        QCOMPARE(cli::writeValue(m_where.recordRoot, m_where.recordKey, L"Other", u"keep"_s, REG_SZ), DWORD(0));
        QCOMPARE(cli::removeFromUserPath(kBin, m_where), DWORD(0));
        QVERIFY(keyExists(m_where.recordRoot, m_where.recordKey));
        QCOMPARE(cli::readValue(m_where.recordRoot, m_where.recordKey, L"Other").text, u"keep"_s);
        QVERIFY(cli::readSources(m_where).recorded.isEmpty());

        // With a subkey in it, too.
        QVERIFY(!cli::deleteKeyIfEmpty(m_where.recordRoot, m_where.recordKey));
        QVERIFY(cli::deleteValue(m_where.recordRoot, m_where.recordKey, L"Other"));
        QCOMPARE(cli::writeValue(m_where.recordRoot, m_where.recordKey + L"\\child", L"x", u"1"_s, REG_SZ), DWORD(0));
        QVERIFY(!cli::deleteKeyIfEmpty(m_where.recordRoot, m_where.recordKey));
        QVERIFY(keyExists(m_where.recordRoot, m_where.recordKey));
        QVERIFY(cli::deleteValue(m_where.recordRoot, m_where.recordKey, L"never there")); // nothing to delete is fine
    }

    void shortNamesAreExpandedOnlyWhereTheyCouldBe()
    {
        // A name with a '~' is asked of the file system (a real folder with an 8.3 alias, if the volume keeps them).
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString longName = QDir::toNativeSeparators(temporary.filePath(u"A Folder With A Long Name"_s));
        QVERIFY(QDir().mkpath(longName));
        wchar_t shortForm[MAX_PATH] {};
        const DWORD n = ::GetShortPathNameW(longName.toStdWString().c_str(), shortForm, MAX_PATH);
        if (n > 0 && n < MAX_PATH && QString::fromWCharArray(shortForm).contains(u'~')) {
            QCOMPARE(cli::normalized(QString::fromWCharArray(shortForm)).toLower(), cli::normalized(longName).toLower());
            QVERIFY(cli::sameFolder(QString::fromWCharArray(shortForm), longName));
        } else {
            qInfo("this volume keeps no 8.3 names: the expansion itself is not tried");
        }
        // Any other entry never reaches the file system. One on a share nobody answers on would
        // stall for many seconds in GetLongPathName (192.0.2.1 is reserved for documentation).
        QElapsedTimer timer;
        timer.start();
        const QString share = u"\\\\192.0.2.1\\share\\tools\\bin"_s;
        QCOMPARE(cli::normalized(share), share);
        QVERIFY(!cli::hasEntry(u"C:\\A;"_s + share, kBin));
        QVERIFY(cli::hasEntry(u"C:\\A;"_s + share, share));
        QVERIFY2(timer.elapsed() < 2000, "a PATH entry on a share was asked of the file system");
    }

    void clientPathsFollowWindowsFolders()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString profile = temporary.filePath(u"迁移后的用户目录"_s);
        const QString roaming = temporary.filePath(u"迁移后的 Roaming"_s);
        const QString local = temporary.filePath(u"迁移后的 Local"_s);
        KnownFolderOverride folders({{&FOLDERID_Profile, profile}, {&FOLDERID_RoamingAppData, roaming}, {&FOLDERID_LocalAppData, local}});
        QVERIFY(folders.installed());
        const QString oldProfile = qEnvironmentVariable("USERPROFILE");
        const QString oldRoaming = qEnvironmentVariable("APPDATA");
        const QString oldConfig = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
        const auto restore = qScopeGuard([&] {
            setEnvironment(L"USERPROFILE", oldProfile);
            setEnvironment(L"APPDATA", oldRoaming);
            setEnvironment(L"CLAUDE_CONFIG_DIR", oldConfig);
        });
        setEnvironment(L"USERPROFILE", {});
        setEnvironment(L"APPDATA", {});
        setEnvironment(L"CLAUDE_CONFIG_DIR", {});
        QCOMPARE(ws::mcpclients::skillPath(), QDir::toNativeSeparators(profile + u"/.claude/skills/winshun/SKILL.md"_s));
        const std::pair<QString, QString> clients[] {
            {u"cursor"_s, profile + u"/.cursor/mcp.json"_s},
            {u"claude-desktop"_s, roaming + u"/Claude/claude_desktop_config.json"_s},
            {u"vscode"_s, roaming + u"/Code/User/mcp.json"_s},
        };
        for (const auto& [id, path] : clients) {
            // Validate the destination before invoking a writer, including
            // when this regression is run against an older implementation.
            const auto detected = ws::mcpclients::detect();
            const auto detectedClient = std::ranges::find(detected, id, &ws::mcpclients::Client::id);
            QVERIFY(detectedClient != detected.end());
            QCOMPARE(detectedClient->configPath, QDir::toNativeSeparators(path));
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile original(path);
            QVERIFY(original.open(QIODevice::WriteOnly));
            const QByteArray contents = R"({"keep":true,"mcpServers":{"other":{"command":"keep.exe"}}})";
            QCOMPARE(original.write(contents), contents.size());
            original.close();
            bool completed = false;
            ws::mcpclients::connect(id, this, [&](const ws::mcpclients::Result& result) {
                QVERIFY2(result.ok, qPrintable(result.message));
                completed = true;
            });
            QVERIFY(completed);
            QVERIFY(original.open(QIODevice::ReadOnly));
            const QJsonObject saved = QJsonDocument::fromJson(original.readAll()).object();
            QVERIFY(saved.value(u"keep"_s).toBool());
            QCOMPARE(saved.value(u"mcpServers"_s).toObject().value(u"other"_s).toObject().value(u"command"_s).toString(), u"keep.exe"_s);
            const QString servers = id == u"vscode" ? u"servers"_s : u"mcpServers"_s;
            QCOMPARE(saved.value(servers).toObject().value(u"winshun"_s).toObject().value(u"command"_s).toString(), ws::mcpclients::relayPath());
            for (const auto& client : ws::mcpclients::detect()) {
                if (client.id == id) {
                    QCOMPARE(client.configPath, QDir::toNativeSeparators(path));
                    QVERIFY(client.connected);
                }
            }
        }
        setEnvironment(L"USERPROFILE", u"X:\\stale-profile"_s);
        setEnvironment(L"APPDATA", u"X:\\stale-appdata"_s);
        QCOMPARE(ws::mcpclients::skillPath(), QDir::toNativeSeparators(profile + u"/.claude/skills/winshun/SKILL.md"_s));
        for (const QString& relative : {u"relative-config"_s, u"C:claude"_s, u"\\claude"_s, u"/claude"_s}) {
            setEnvironment(L"CLAUDE_CONFIG_DIR", relative);
            QVERIFY2(ws::mcpclients::skillPath().isEmpty(), qPrintable(relative));
        }
        const QString custom = temporary.filePath(u"custom-claude"_s);
        setEnvironment(L"CLAUDE_CONFIG_DIR", custom);
        QCOMPARE(ws::mcpclients::skillPath(), QDir::toNativeSeparators(custom + u"/skills/winshun/SKILL.md"_s));
        setEnvironment(L"CLAUDE_CONFIG_DIR", {});
        for (auto& folder : folders.folders)
            folder.path.clear();
        for (const auto& [id, path] : clients) {
            bool rejected = false;
            ws::mcpclients::connect(id, this, [&](const ws::mcpclients::Result& result) { rejected = !result.ok; });
            QVERIFY(rejected);
        }
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

        // Whether it is there is judged from where ws put it, as it printed: a CLAUDE_CONFIG_DIR
        // that this program sees differently (set or changed after it started, say) does not
        // move the row's file.
        setEnvironment(L"CLAUDE_CONFIG_DIR", QDir::toNativeSeparators(temporary.filePath(u"elsewhere"_s)));
        QCOMPARE(ws::mcpclients::skillPath(), skill);
        QVERIFY(claudeCode().skillInstalled);
        // And the file deleted afterwards is seen at the next look.
        QVERIFY(QFile::remove(skill));
        QVERIFY(!claudeCode().skillInstalled);
        QCOMPARE(claudeCode().skillPath, skill);

        // No ws.exe: said at once, nothing run.
        auto missing = install({});
        QVERIFY(missing->done);
        QVERIFY(!missing->result.ok);
        QVERIFY(missing->result.message.contains(u"ws.exe"_s));
    }
};

QTEST_MAIN(TestCommandLine)
#include "tst_commandline.moc"
