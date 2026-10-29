// The plugin system (src/plugin): manifests, items, zero-code commands, the
// catalog's keywords, installing (from the 插件库's list too), and plugin
// processes for real -- started through the runner WinShunPluginHost.exe,
// played by plugin_fake.exe -- with the protocol, cancelling, timeouts,
// crashes and restarts, encodings, and (run elevated) the token, job and
// permissions a plugin process gets.

#include "Catalog.h"
#include "Gallery.h"
#include "Host.h"
#include "IndexService.h"
#include "Install.h"
#include "Items.h"
#include "Manifest.h"
#include "McpTools.h"
#include "Merge.h"
#include "OpenPolicy.h"
#include "Programs.h"
#include "Sandbox.h"
#include "SearchEngine.h"
#include "Secrets.h"
#include "Session.h"
#include "ToastPolicy.h"
#include "Tools.h"
#include "WebShortcut.h"
#include "ZeroCode.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <miniz.h>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

using namespace ws;
using namespace ws::plugin;
using namespace Qt::StringLiterals;

namespace {

const QString kFake = QString::fromUtf8(WINSHUN_PLUGIN_FAKE);
const QString kRunner = QString::fromUtf8(WINSHUN_PLUGIN_HOST);

QJsonObject parseObject(const QByteArray& json)
{
    return QJsonDocument::fromJson(json).object();
}

QByteArray toJson(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// A manifest for plugin_fake in one of its roles.
struct Spec {
    QString id = u"test.fake"_s;
    QString mode = u"resident"_s; // once | resident | "" (zero-code)
    QStringList args; // after the role
    QString role = u"--serve"_s;
    QStringList permissions;
    QStringList keywords = {u"fk"_s};
    QJsonObject runtimeExtra; // timeoutMs, debounceMs, idleExitSec, encoding, sandbox …
    QJsonObject commandExtra; // triggers, run, actions …
    QJsonArray commands; // instead of the one command
    QJsonArray settings; // the declared settings
};

QByteArray manifestFor(const Spec& s)
{
    QJsonObject m {{u"manifest"_s, 1}, {u"id"_s, s.id}, {u"name"_s, QJsonObject {{u"zh"_s, u"假插件"_s}, {u"en"_s, u"Fake"_s}}},
        {u"version"_s, u"1.0.0"_s}};
    if (!s.mode.isEmpty()) {
        QJsonObject runtime {{u"mode"_s, s.mode}, {u"program"_s, u"fake.exe"_s}};
        QJsonArray args {s.role};
        for (const QString& a : s.args)
            args.push_back(a);
        runtime.insert(u"args"_s, args);
        for (auto it = s.runtimeExtra.begin(); it != s.runtimeExtra.end(); ++it)
            runtime.insert(it.key(), it.value());
        m.insert(u"runtime"_s, runtime);
    }
    if (!s.permissions.isEmpty())
        m.insert(u"permissions"_s, QJsonArray::fromStringList(s.permissions));
    if (!s.settings.isEmpty())
        m.insert(u"settings"_s, s.settings);
    if (!s.commands.isEmpty())
        m.insert(u"commands"_s, s.commands);
    else {
        QJsonObject command {{u"id"_s, u"main"_s}, {u"title"_s, u"主命令"_s}, {u"keyword"_s, QJsonArray::fromStringList(s.keywords)}};
        for (auto it = s.commandExtra.begin(); it != s.commandExtra.end(); ++it)
            command.insert(it.key(), it.value());
        m.insert(u"commands"_s, QJsonArray {command});
    }
    return toJson(m);
}

struct Result {
    std::uint64_t queryId;
    QString plugin;
    QString command;
    Items items;
    bool partial;
    bool replace;
};

struct Fixture {
    QTemporaryDir dir;
    std::unique_ptr<Host> host;
    std::vector<Result> results;
    std::vector<std::pair<std::uint64_t, QString>> failures; // query id, why
    std::vector<std::uint64_t> finished;
    std::vector<quint32> startedPids;
    std::vector<quint32> exitCodes;
    std::vector<int> restarts;
    QStringList disabled;
    QStringList notices;
    QStringList progressTexts;
    struct Action {
        std::uint64_t requestId;
        Effects effects;
        bool ok;
        QString error;
    };
    std::vector<Action> actions;
    QStringList logs;

    Fixture()
    {
        Host::Options options;
        options.userPluginDir = dir.path() + u"/plugins"_s;
        options.dataRoot = dir.path() + u"/data"_s;
        options.logPath = dir.path() + u"/plugins.log"_s;
        options.runnerExe = kRunner;
        options.version = u"0.0-test"_s;
        host = std::make_unique<Host>(options);
        host->setBackoffScale(0.01); // 1, 5, 30 s -> 10, 50, 300 ms
        QObject::connect(host.get(), &Host::results,
            [this](std::uint64_t id, const QString& plugin, const QString& command, const Items& items, bool partial, bool replace) {
                results.push_back({id, plugin, command, items, partial, replace});
            });
        QObject::connect(host.get(), &Host::queryFailed, [this](std::uint64_t id, const QString&, const QString& why) {
            failures.emplace_back(id, why);
        });
        QObject::connect(host.get(), &Host::queryFinished, [this](std::uint64_t id, const QString&) { finished.push_back(id); });
        QObject::connect(host.get(), &Host::processStarted, [this](const QString&, quint32 pid) { startedPids.push_back(pid); });
        QObject::connect(host.get(), &Host::processExited, [this](const QString&, quint32 code) { exitCodes.push_back(code); });
        QObject::connect(host.get(), &Host::restartScheduled, [this](const QString&, int delay) { restarts.push_back(delay); });
        QObject::connect(host.get(), &Host::disabledAfterCrashes, [this](const QString& id) { disabled.push_back(id); });
        QObject::connect(host.get(), &Host::notice, [this](const QString&, const QString& text) { notices.push_back(text); });
        QObject::connect(host.get(), &Host::progress, [this](std::uint64_t, const QString&, const QString& text) { progressTexts.push_back(text); });
        QObject::connect(host.get(), &Host::actionDone, [this](std::uint64_t id, const QString&, const Effects& effects, bool ok, const QString& error) {
            actions.push_back({id, effects, ok, error});
        });
        QObject::connect(host.get(), &Host::logged, [this](const QString& line) { logs.push_back(line); });
    }

    // A plugin folder with plugin_fake.exe as fake.exe; enabled.
    QString install(const Spec& spec)
    {
        const QString folder = dir.path() + u"/plugins/"_s + spec.id;
        QDir().mkpath(folder);
        QFile::remove(folder + u"/fake.exe"_s);
        if (!QFile::copy(kFake, folder + u"/fake.exe"_s))
            return {};
        QFile manifest(folder + u"/plugin.json"_s);
        if (!manifest.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return {};
        manifest.write(manifestFor(spec));
        manifest.close();
        host->catalog().scan();
        host->setEnabled(spec.id, true);
        return QDir::toNativeSeparators(folder);
    }

    const Result* finalFor(std::uint64_t queryId) const
    {
        for (const Result& r : results)
            if (r.queryId == queryId && !r.partial)
                return &r;
        return nullptr;
    }
    bool waitFinal(std::uint64_t queryId, int ms = 8000)
    {
        return QTest::qWaitFor([&] { return finalFor(queryId) != nullptr; }, ms);
    }
    bool waitFailure(std::uint64_t queryId, int ms = 8000)
    {
        return QTest::qWaitFor([&] {
            for (const auto& [id, why] : failures)
                if (id == queryId)
                    return true;
            return false;
        }, ms);
    }
    QString failureOf(std::uint64_t queryId) const
    {
        for (const auto& [id, why] : failures)
            if (id == queryId)
                return why;
        return {};
    }
};

// The launcher's list as plugin::Merge sees it (ResultModel in the app).
struct ListSink : RowSink {
    SearchResults rows;
    int current = -1;
    int count() const override { return static_cast<int>(rows.size()); }
    const SearchResult& row(int index) const override { return rows[index]; }
    int currentRow() const override { return current; }
    void assign(SearchResults r) override { rows = std::move(r); }
    void append(const SearchResults& r) override { rows.append(r); }
    void insert(int index, const SearchResult& r) override { rows.insert(index, r); }
    void update(int index, const SearchResult& r) override { rows[index] = r; }
    void remove(int index) override { rows.removeAt(index); }
    QStringList names() const
    {
        QStringList out;
        for (const SearchResult& r : rows)
            out.append(r.name);
        return out;
    }
};

SearchResult appRow(const QString& name, int score, bool recent = false, bool exact = false)
{
    SearchResult r;
    r.name = name;
    r.path = u"shell:AppsFolder\\"_s + name;
    r.app = AppKind::Desktop;
    r.score = score;
    r.recent = recent;
    r.exact = exact;
    return r;
}

SearchResult fileRow(const QString& name, bool recent = false, bool exact = false)
{
    SearchResult r;
    r.name = name;
    r.path = u"C:\\files\\"_s + name;
    r.recent = recent;
    r.exact = exact;
    return r;
}

Item scoredItem(const QString& id, const QString& title, int score, const QString& subtitle = {})
{
    Item item;
    item.id = id;
    item.title = title;
    item.subtitle = subtitle;
    item.score = score;
    item.hasScore = true;
    return item;
}

// Three zero-code plugins (a, b, c with keywords aa, bb, cc) for the merge.
void installThree(Fixture& f)
{
    for (const QString& letter : {u"a"_s, u"b"_s, u"c"_s}) {
        Spec spec;
        spec.id = u"test."_s + letter;
        spec.mode.clear();
        spec.keywords = {letter + letter};
        spec.commandExtra = {{u"run"_s, QJsonObject {{u"items"_s, QJsonArray {QJsonObject {{u"title"_s, u"x"_s}}}}}}};
        QVERIFY(!f.install(spec).isEmpty());
    }
}

bool elevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    const bool result = sandbox::tokenElevated(token);
    ::CloseHandle(token);
    return result;
}

// The logon session of the desktop shell, as the plugins should have it.
QString explorerAuthId()
{
    DWORD pid = 0;
    if (const HWND shell = ::GetShellWindow())
        ::GetWindowThreadProcessId(shell, &pid);
    if (!pid)
        return {};
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    HANDLE token = nullptr;
    QString result;
    if (::OpenProcessToken(process, TOKEN_QUERY, &token)) {
        result = QString::fromStdString(sandbox::authenticationId(token));
        ::CloseHandle(token);
    }
    ::CloseHandle(process);
    return result;
}

bool processGone(quint32 pid, int ms)
{
    const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process)
        return true; // already gone
    const bool gone = ::WaitForSingleObject(process, static_cast<DWORD>(ms)) == WAIT_OBJECT_0;
    ::CloseHandle(process);
    return gone;
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

// A zip of the given entries (name -> bytes), written to `path`.
bool writeZip(const QString& path, const QList<std::pair<QString, QByteArray>>& entries)
{
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 64 * 1024))
        return false;
    bool ok = true;
    for (const auto& [name, bytes] : entries)
        ok = ok && mz_zip_writer_add_mem(&zip, name.toUtf8().constData(), bytes.constData(), static_cast<size_t>(bytes.size()), MZ_BEST_SPEED);
    void* buffer = nullptr;
    size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
    if (ok)
        ok = writeFile(path, QByteArray(static_cast<const char*>(buffer), static_cast<qsizetype>(size)));
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return ok;
}

QByteArray zeroManifest(const QString& id, const QString& version, const QString& extra = {})
{
    return (u"{\"manifest\":1,\"id\":\"%1\",\"name\":\"Zero\",\"version\":\"%2\",%3\"commands\":[{\"id\":\"go\",\"keyword\":\"go\",\"run\":{\"folder\":\"C:\\\\\"}}]}"_s
        .arg(id, version, extra))
        .toUtf8();
}

// Removes the secrets of a test plugin however the test ends.
struct SecretsCleanup {
    QString id;
    ~SecretsCleanup() { secrets::removeAll(id); }
};

// ---- finding Python: interpreters, uv's trampolines, venvs, the registry ----

// plugin_fake.exe copied to `path` (folders made): an interpreter, as far
// as finding Python goes.
bool copyFake(const QString& path)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile::remove(path);
    return QFile::copy(kFake, path);
}

// A trampoline as uv after 0.9.0 makes one: the kind (2: Python, 1: a
// script's) and the interpreter's path in RCDATA resources.
bool makeUvTrampoline(const QString& path, quint8 kind, const QString& target)
{
    if (!copyFake(path))
        return false;
    const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
    const HANDLE update = ::BeginUpdateResourceW(file.c_str(), FALSE);
    if (!update)
        return false;
    QByteArray utf8 = target.toUtf8();
    const bool ok = ::UpdateResourceW(update, MAKEINTRESOURCEW(10), L"UV_TRAMPOLINE_KIND", 0, &kind, 1)
        && ::UpdateResourceW(update, MAKEINTRESOURCEW(10), L"UV_PYTHON_PATH", 0, utf8.data(), DWORD(utf8.size()));
    return ::EndUpdateResourceW(update, !ok) && ok;
}

// One as uv up to 0.9.0 made them: the path, its length (u32, little
// endian) and "UVPY" ("UVSC": a script's) after the exe.
bool makeOldUvTrampoline(const QString& path, const QByteArray& magic, const QString& target)
{
    if (!copyFake(path))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::Append))
        return false;
    QByteArray tail = target.toUtf8();
    const quint32 length = qToLittleEndian(quint32(tail.size()));
    tail.append(reinterpret_cast<const char*>(&length), sizeof length);
    tail.append(magic);
    return file.write(tail) == tail.size();
}

// HKCU\Software\WinShunTest-<pid>: registrations made up for a test,
// removed however it ends.
struct ScratchKey {
    const QString path = u"Software\\WinShunTest-%1"_s.arg(::GetCurrentProcessId());
    ~ScratchKey() { ::RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(path.utf16())); }
    QString root() const { return u"HKEY_CURRENT_USER\\"_s + path; }
    bool set(const QString& key, const wchar_t* name, const QString& value) const
    {
        const QString full = path + u'\\' + key;
        return ::RegSetKeyValueW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(full.utf16()), name, REG_SZ, value.utf16(),
                   DWORD((value.size() + 1) * sizeof(char16_t)))
            == ERROR_SUCCESS;
    }
    bool exists(const QString& key) const
    {
        const QString full = path + u'\\' + key;
        HKEY handle = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(full.utf16()), 0, KEY_READ, &handle) != ERROR_SUCCESS)
            return false;
        ::RegCloseKey(handle);
        return true;
    }
};

// The Python the plugins get goes back to the lookup however the test ends.
struct PythonOverride {
    ~PythonOverride() { programs::setOverride(Program::Python, {}); }
};

} // namespace

class TestPlugin : public QObject {
    Q_OBJECT

private slots:
    // ---- manifest ------------------------------------------------------------
    void manifestComplete()
    {
        const QByteArray json = R"({
          "manifest": 1, "id": "winshun.kill", "name": {"zh": "结束进程", "en": "Kill process"}, "version": "1.0.0",
          "author": "LingCore", "homepage": "github.com/LingCore/WinShun", "license": "MIT", "icon": "icon.png",
          "minWinShun": "0.7.0",
          "runtime": {"mode": "resident", "program": "powershell", "script": "kill.ps1", "idleExitSec": 600, "memoryMB": 256},
          "permissions": ["processes", {"files:read": ["%LOCALAPPDATA%\\Google"]}],
          "commands": [{
            "id": "kill", "keyword": ["kill", "jc"], "title": {"zh": "结束进程", "en": "Kill process"},
            "hint": {"zh": "进程名或 PID", "en": "Process name or PID"},
            "actions": [{"id": "kill", "title": {"zh": "结束", "en": "Kill"}, "key": "Enter"},
                        {"id": "killTree", "title": "连同子进程", "key": "Ctrl+Enter"}],
            "triggers": [{"regex": "^\\d{2,6}$", "minLength": 2, "maxLength": 6}, {"push": {"minLength": 3, "maxItems": 2}}],
            "cache": {"seconds": 60},
            "ai": {"description": "List running processes"}
          }],
          "settings": [{"key": "showSystem", "type": "bool", "title": {"zh": "也列出系统进程", "en": "Include system"}, "default": false}],
          "ai": {"expose": true}
        })";
        QString error;
        const std::optional<Manifest> m = Manifest::parse(json, &error);
        QVERIFY2(m.has_value(), qPrintable(error));
        QCOMPARE(m->id, u"winshun.kill"_s);
        QCOMPARE(m->name.pick(u"zh"_s), u"结束进程"_s);
        QCOMPARE(m->name.pick(u"en"_s), u"Kill process"_s);
        QCOMPARE(m->version, u"1.0.0"_s);
        QCOMPARE(m->author, u"LingCore"_s);
        QCOMPARE(m->icon, u"icon.png"_s);
        QVERIFY(m->runtime.has_value());
        QCOMPARE(m->runtime->mode, RunMode::Resident);
        QCOMPARE(m->runtime->program, Program::PowerShell);
        QCOMPARE(m->runtime->script, u"kill.ps1"_s);
        QCOMPARE(m->runtime->memoryMB, 256);
        QCOMPARE(m->runtime->timeoutMs, 1500); // the resident default
        QCOMPARE(m->runtime->debounceMs, 0);
        QVERIFY(m->permissions.has(Permission::Processes));
        QVERIFY(m->permissions.has(Permission::FilesRead));
        QVERIFY(!m->permissions.has(Permission::ClipboardRead));
        QCOMPARE(m->permissions.readPaths, QStringList {u"%LOCALAPPDATA%\\Google"_s});
        QCOMPARE(m->commands.size(), 1);
        const Command& c = m->commands.first();
        QCOMPARE(c.keywords, (QStringList {u"kill"_s, u"jc"_s}));
        QCOMPARE(c.hint.pick(u"en"_s), u"Process name or PID"_s);
        QCOMPARE(c.actions.size(), 2);
        QCOMPARE(c.actions[1].title.pick(u"en"_s), u"连同子进程"_s); // a plain string serves both
        QCOMPARE(c.triggers.size(), 2);
        QCOMPARE(c.triggers[0].kind, Trigger::Kind::Regex);
        QCOMPARE(c.triggers[1].kind, Trigger::Kind::Push);
        QCOMPARE(c.triggers[1].maxItems, 2);
        QCOMPARE(c.cacheSeconds, 60);
        QVERIFY(!c.zeroCode());
        QCOMPARE(m->settings.size(), 1);
        QCOMPARE(m->settings[0].type, u"bool"_s);
        QVERIFY(m->aiExpose);
    }

    void manifestMinimal()
    {
        QString error;
        // (No "//" inside raw strings in this file: moc takes it for a comment and skips the class.)
        const auto m = Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","commands":[{"id":"c","run":{"url":"x.com/?q=%s"}}]})", &error);
        QVERIFY2(m.has_value(), qPrintable(error));
        QVERIFY(!m->runtime);
        QCOMPARE(m->commands[0].run.kind, Run::Kind::Url);
        QCOMPARE(m->commands[0].run.url, u"https://x.com/?q=%s"_s); // normalized as [WebSearch] entries are
        QVERIFY(m->commands[0].zeroCode());
        // Once defaults.
        const auto once = Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","runtime":{"mode":"once","program":"python"},"commands":[{"id":"c"}]})", &error);
        QVERIFY2(once.has_value(), qPrintable(error));
        QCOMPARE(once->runtime->timeoutMs, 3000);
        QCOMPARE(once->runtime->debounceMs, 120);
        QCOMPARE(once->runtime->script, u"main.py"_s);
    }

    void manifestMissingRequired_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("expect");
        QTest::newRow("manifest") << u"manifest"_s << u"manifest"_s;
        QTest::newRow("id") << u"id"_s << u"id"_s;
        QTest::newRow("name") << u"name"_s << u"name"_s;
        QTest::newRow("version") << u"version"_s << u"version"_s;
        QTest::newRow("commands") << u"commands"_s << u"commands"_s;
    }
    void manifestMissingRequired()
    {
        QFETCH(QString, field);
        QFETCH(QString, expect);
        QJsonObject o = parseObject(R"({"manifest":1,"id":"a.b","name":"X","version":"1","commands":[{"id":"c","run":{"folder":"C:\\"}}]})");
        o.remove(field);
        QString error;
        QVERIFY(!Manifest::parse(toJson(o), &error));
        QVERIFY2(error.contains(expect), qPrintable(error));
    }

    void manifestErrors()
    {
        QString error;
        QVERIFY(!Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","commands":[{"id":"c","keyword":"a b","run":{"folder":"C:\\"}}]})", &error));
        QVERIFY2(error.contains(u"commands[0].keyword 含空格"_s), qPrintable(error));
        QVERIFY(!Manifest::parse(R"({"manifest":2,"id":"a.b","name":"X","version":"1","commands":[{"id":"c","run":{"folder":"C:\\"}}]})", &error));
        QVERIFY2(error.contains(u"升级"_s), qPrintable(error));
        QVERIFY(!Manifest::parse(R"({"manifest":1,"id":"A B","name":"X","version":"1","commands":[{"id":"c","run":{"folder":"C:\\"}}]})", &error));
        QVERIFY2(error.contains(u"id"_s), qPrintable(error));
        // A command with nothing to run it.
        QVERIFY(!Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","commands":[{"id":"c"}]})", &error));
        QVERIFY2(error.contains(u"runtime"_s), qPrintable(error));
        // Unknown permissions are refused (a newer Win顺 may know them).
        QVERIFY(!Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","permissions":["magic"],"commands":[{"id":"c","run":{"folder":"C:\\"}}]})", &error));
        QVERIFY2(error.contains(u"magic"_s), qPrintable(error));
        // Not JSON.
        QVERIFY(!Manifest::parse("{", &error));
        QVERIFY2(error.contains(u"JSON"_s), qPrintable(error));
        // An SVG icon: the installed Qt cannot draw one, and the message says what to use.
        QVERIFY(!Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","icon":"icon.svg","commands":[{"id":"c","run":{"folder":"C:\\"}}]})", &error));
        QVERIFY2(error.contains(u"PNG"_s), qPrintable(error));
        QVERIFY(!Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","commands":[{"id":"c","icon":"c.SVG","run":{"folder":"C:\\"}}]})", &error));
        QVERIFY2(error.contains(u"commands[0].icon"_s) && error.contains(u"PNG"_s), qPrintable(error));
    }

    void manifestUnknownFieldsIgnored()
    {
        QString error;
        const auto m = Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","future":{"x":1},"commands":[{"id":"c","run":{"folder":"C:\\"},"newThing":true}]})", &error);
        QVERIFY2(m.has_value(), qPrintable(error));
    }

    void manifestProgramEscape()
    {
        QString error;
        for (const char* program : {"..\\evil.exe", "C:\\evil.exe", "/evil.exe", "sub\\..\\..\\evil.exe"}) {
            const QByteArray json = toJson({{u"manifest"_s, 1}, {u"id"_s, u"a.b"_s}, {u"name"_s, u"X"_s}, {u"version"_s, u"1"_s},
                {u"runtime"_s, QJsonObject {{u"mode"_s, u"once"_s}, {u"program"_s, QString::fromLatin1(program)}}},
                {u"commands"_s, QJsonArray {QJsonObject {{u"id"_s, u"c"_s}}}}});
            QVERIFY2(!Manifest::parse(json, &error), program);
            QVERIFY2(error.contains(u"runtime.program"_s), qPrintable(error));
        }
        QVERIFY(!Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","runtime":{"mode":"once","program":"python","script":"..\\x.py"},"commands":[{"id":"c"}]})", &error));
        QVERIFY2(error.contains(u"runtime.script"_s), qPrintable(error));
        QVERIFY(!Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","runtime":{"mode":"once","program":"python","env":{"WS_QUERY":"x"}},"commands":[{"id":"c"}]})", &error));
        QVERIFY2(error.contains(u"WS_"_s), qPrintable(error));
        const auto ok = Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","runtime":{"mode":"once","program":"bin\\tool.exe"},"commands":[{"id":"c"}]})", &error);
        QVERIFY2(ok.has_value(), qPrintable(error));
        QCOMPARE(ok->runtime->program, Program::Exe);
        QCOMPARE(ok->runtime->programPath, u"bin\\tool.exe"_s);
    }

    void textPick()
    {
        const Text both {u"中"_s, u"en"_s};
        QCOMPARE(both.pick(u"zh"_s), u"中"_s);
        QCOMPARE(both.pick(u"en"_s), u"en"_s);
        QCOMPARE(both.pick(u"en-US"_s), u"en"_s);
        QCOMPARE((Text {u"中"_s, {}}).pick(u"en"_s), u"中"_s);
        QCOMPARE((Text {{}, u"en"_s}).pick(u"zh"_s), u"en"_s);
    }

    void versionCompare()
    {
        QVERIFY(compareVersions(u"1.2.0", u"1.10") < 0);
        QVERIFY(compareVersions(u"1.10", u"1.2.0") > 0);
        QCOMPARE(compareVersions(u"1.2", u"1.2.0"), 0);
        QVERIFY(compareVersions(u"1.0.0-beta", u"1.0.0") < 0);
        QVERIFY(compareVersions(u"2", u"1.9.9") > 0);
    }

    // ---- finding Python ------------------------------------------------------
    // python.exe itself, never a launcher that starts it as a child: the
    // job of a plugin without the processes permission forbids the child.
    void pythonVenvHome()
    {
        using programs::venvHome;
        QCOMPARE(venvHome("home = C:\\Python312\ninclude-system-site-packages = false\nversion = 3.12.4\n"), u"C:\\Python312"_s);
        QCOMPARE(venvHome("version = 3.12.14\r\nhome = D:\\uv\\python\\cpython-3.12-windows-x86_64-none\r\n"),
            u"D:\\uv\\python\\cpython-3.12-windows-x86_64-none"_s);
        QCOMPARE(venvHome("\xEF\xBB\xBFhome=E:\\x\n"), u"E:\\x"_s);
        QCOMPARE(venvHome("  HOME  =   C:\\Program Files\\Python 3.12  \n"), u"C:\\Program Files\\Python 3.12"_s);
        QCOMPARE(venvHome("homeless = C:\\no\nhome = C:\\yes"), u"C:\\yes"_s);
        QCOMPARE(venvHome("home = C:\\用户\\张三\\py\n"), u"C:\\用户\\张三\\py"_s);
        QCOMPARE(venvHome("= C:\\no\nversion = 3.12\n"), QString());
        QCOMPARE(venvHome(""), QString());
    }

    void pythonUvTrampolineTail()
    {
        const auto tail = [](const QByteArray& path, const QByteArray& magic, qsizetype claimed = -1) {
            const quint32 length = qToLittleEndian(quint32(claimed < 0 ? path.size() : claimed));
            return QByteArray("MZ the trampoline's code ") + path + QByteArray(reinterpret_cast<const char*>(&length), sizeof length) + magic;
        };
        const QString target = u"C:\\Users\\张三\\AppData\\Roaming\\uv\\python\\cpython-3.12-windows-x86_64-none\\python.exe"_s;
        std::optional<programs::UvTrampoline> t = programs::uvTrampolineTail(tail(target.toUtf8(), "UVPY"));
        QVERIFY(t);
        QVERIFY(t->kind == programs::UvTrampoline::Kind::Python);
        QCOMPARE(t->target, target);
        t = programs::uvTrampolineTail(tail("C:\\tools\\python.exe", "UVSC"));
        QVERIFY(t);
        QVERIFY(t->kind == programs::UvTrampoline::Kind::Script);
        QCOMPARE(t->target, u"C:\\tools\\python.exe"_s);
        QVERIFY(!programs::uvTrampolineTail(tail("C:\\x.exe", "UVXX")));
        QVERIFY(!programs::uvTrampolineTail(tail("C:\\x.exe", "UVPY", 0)));
        QVERIFY(!programs::uvTrampolineTail(tail("C:\\x.exe", "UVPY", 4096))); // longer than what is there
        QVERIFY(!programs::uvTrampolineTail("UVPY"));
        QVERIFY(!programs::uvTrampolineTail(QByteArrayView()));
    }

    // uv's bin folder (`uv python install --default` puts it on PATH): a
    // python.exe that starts the managed interpreter as a child.
    void pythonUvTrampolines()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString root = QDir::toNativeSeparators(temp.path());
        const QString real = root + u"\\python\\cpython-3.12.14-windows-x86_64-none\\python.exe"_s;
        QVERIFY(copyFake(real));
        QVERIFY(!programs::uvTrampoline(real));
        QString why;
        std::optional<programs::Python> python = programs::resolvePython(real, &why);
        QVERIFY2(python, qPrintable(why));
        QCOMPARE(python->exe, real);
        QVERIFY(python->venvLauncher.isEmpty());

        // An absolute target, and one from the trampoline's folder.
        const QString bin = root + u"\\bin\\python.exe"_s;
        QVERIFY(makeUvTrampoline(bin, 2, real));
        std::optional<programs::UvTrampoline> t = programs::uvTrampoline(bin);
        QVERIFY(t);
        QVERIFY(t->kind == programs::UvTrampoline::Kind::Python);
        QCOMPARE(t->target, real);
        python = programs::resolvePython(bin, &why);
        QVERIFY2(python, qPrintable(why));
        QCOMPARE(python->exe, real);
        QVERIFY(python->venvLauncher.isEmpty());
        const QString relative = root + u"\\bin\\python3.exe"_s;
        QVERIFY(makeUvTrampoline(relative, 2, u"..\\python\\cpython-3.12.14-windows-x86_64-none\\python.exe"_s));
        python = programs::resolvePython(relative, &why);
        QVERIFY2(python, qPrintable(why));
        QCOMPARE(python->exe, real);

        // The older kind, the path after the exe.
        const QString old = root + u"\\old\\python.exe"_s;
        QVERIFY(makeOldUvTrampoline(old, "UVPY", real));
        t = programs::uvTrampoline(old);
        QVERIFY(t);
        QVERIFY(t->kind == programs::UvTrampoline::Kind::Python);
        QCOMPARE(t->target, real);
        python = programs::resolvePython(old, &why);
        QVERIFY2(python, qPrintable(why));
        QCOMPARE(python->exe, real);

        // Leading nowhere, and saying why: a script's launcher, an
        // interpreter uninstalled since, a loop.
        const QString script = root + u"\\bin\\black.exe"_s;
        QVERIFY(makeUvTrampoline(script, 1, real));
        QVERIFY(!programs::resolvePython(script, &why));
        QVERIFY2(why.contains(script) && why.contains(u"脚本"_s), qPrintable(why));
        const QString gone = root + u"\\python\\cpython-3.11.9-windows-x86_64-none\\python.exe"_s;
        const QString stale = root + u"\\bin\\python3.11.exe"_s;
        QVERIFY(makeUvTrampoline(stale, 2, gone));
        QVERIFY(!programs::resolvePython(stale, &why));
        QVERIFY2(why.contains(gone) && why.contains(u"uv python install"_s), qPrintable(why));
        const QString loop = root + u"\\loop\\python.exe"_s;
        QVERIFY(makeUvTrampoline(loop, 2, u"python.exe"_s));
        QVERIFY(!programs::resolvePython(loop, &why));
        QVERIFY2(why.contains(u"太多"_s), qPrintable(why));
        // A kind a newer uv may bring.
        const QString newer = root + u"\\newer\\python.exe"_s;
        QVERIFY(makeUvTrampoline(newer, 3, real));
        t = programs::uvTrampoline(newer);
        QVERIFY(t);
        QVERIFY(t->kind == programs::UvTrampoline::Kind::Unknown);
        QVERIFY(!programs::resolvePython(newer, &why));
        QVERIFY2(why.contains(u"不认得"_s), qPrintable(why));
    }

    // A venv's python.exe -- CPython's venv launcher, or uv's trampoline --
    // starts home\python.exe as a child: that one is started instead, told
    // the venv the way the launcher tells it.
    void pythonVenvs()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString root = QDir::toNativeSeparators(temp.path());
        const QString base = root + u"\\base\\python.exe"_s;
        QVERIFY(copyFake(base));
        QString why;

        const QString venv = root + u"\\venv\\Scripts\\python.exe"_s;
        QVERIFY(copyFake(venv));
        QVERIFY(writeFile(root + u"\\venv\\pyvenv.cfg"_s,
            (u"home = "_s + root + u"\\base\r\ninclude-system-site-packages = false\r\nversion = 3.12.14\r\n"_s).toUtf8()));
        std::optional<programs::Python> python = programs::resolvePython(venv, &why);
        QVERIFY2(python, qPrintable(why));
        QCOMPARE(python->exe, base);
        QCOMPARE(python->venvLauncher, venv);

        const QString uvVenv = root + u"\\.venv\\Scripts\\python.exe"_s;
        QVERIFY(makeUvTrampoline(uvVenv, 2, base));
        QVERIFY(writeFile(root + u"\\.venv\\pyvenv.cfg"_s,
            (u"home = "_s + root + u"\\base\nimplementation = CPython\nuv = 0.9.5\nversion_info = 3.12.14\ninclude-system-site-packages = false\n"_s)
                .toUtf8()));
        python = programs::resolvePython(uvVenv, &why);
        QVERIFY2(python, qPrintable(why));
        QCOMPARE(python->exe, base);
        QCOMPARE(python->venvLauncher, uvVenv);

        // pyvenv.cfg beside python.exe counts too (getpath looks there next).
        const QString flat = root + u"\\flat\\python.exe"_s;
        QVERIFY(copyFake(flat));
        QVERIFY(writeFile(root + u"\\flat\\pyvenv.cfg"_s, (u"home = "_s + root + u"\\base\n"_s).toUtf8()));
        python = programs::resolvePython(flat, &why);
        QVERIFY2(python, qPrintable(why));
        QCOMPARE(python->exe, base);
        QCOMPARE(python->venvLauncher, flat);

        const QString broken = root + u"\\broken\\Scripts\\python.exe"_s;
        QVERIFY(copyFake(broken));
        QVERIFY(writeFile(root + u"\\broken\\pyvenv.cfg"_s, (u"home = "_s + root + u"\\nothing\n"_s).toUtf8()));
        QVERIFY(!programs::resolvePython(broken, &why));
        QVERIFY2(why.contains(broken) && why.contains(u"home 里没有 python.exe"_s), qPrintable(why));
        QVERIFY(writeFile(root + u"\\broken\\pyvenv.cfg"_s, "version = 3.12.14\n"));
        QVERIFY(!programs::resolvePython(broken, &why));
        QVERIFY2(why.contains(u"没写 home"_s), qPrintable(why));
        // One that cannot be read (another program holds it) says so.
        QVERIFY(writeFile(root + u"\\broken\\pyvenv.cfg"_s, (u"home = "_s + root + u"\\base\n"_s).toUtf8()));
        const std::wstring cfg = (root + u"\\broken\\pyvenv.cfg"_s).toStdWString();
        const HANDLE held = ::CreateFileW(cfg.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(held != INVALID_HANDLE_VALUE);
        const bool resolved = programs::resolvePython(broken, &why).has_value();
        ::CloseHandle(held);
        QVERIFY(!resolved);
        QVERIFY2(why.contains(u"读不了"_s), qPrintable(why));
    }

    void pythonRefusals()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString root = QDir::toNativeSeparators(temp.path());
        QString why;
        QVERIFY(!programs::resolvePython(root + u"\\none\\python.exe"_s, &why));
        QVERIFY2(why.contains(u"不存在"_s), qPrintable(why));
        QVERIFY(QDir().mkpath(root + u"\\folder\\python.exe"_s));
        QVERIFY(!programs::resolvePython(root + u"\\folder\\python.exe"_s, &why));
        QVERIFY2(why.contains(u"文件夹"_s), qPrintable(why));
        QVERIFY(writeFile(root + u"\\shims\\python.bat"_s, "@python3.12.exe %*\r\n"));
        QVERIFY(!programs::resolvePython(root + u"\\shims\\python.bat"_s, &why));
        QVERIFY2(why.contains(u"不是 exe"_s), qPrintable(why));
        QVERIFY(copyFake(root + u"\\Microsoft\\WindowsApps\\python.exe"_s));
        QVERIFY(!programs::resolvePython(root + u"\\Microsoft\\WindowsApps\\python.exe"_s, &why));
        QVERIFY2(why.contains(u"Microsoft Store"_s), qPrintable(why));
        QVERIFY(copyFake(root + u"\\launcher\\py.exe"_s));
        QVERIFY(!programs::resolvePython(root + u"\\launcher\\py.exe"_s, &why));
        QVERIFY2(why.contains(u"启动器"_s), qPrintable(why));
    }

    // PEP 514: python.org's (PythonCore), uv's (Astral), Anaconda's …
    void pythonRegistry()
    {
        const ScratchKey scratch;
        const QString root = u"C:\\py"_s;
        const QString py = u"Python\\"_s;
        // python.org's older installers wrote only the folder.
        QVERIFY(scratch.set(py + u"PythonCore\\3.10\\InstallPath"_s, nullptr, root + u"\\Python310\\"_s));
        QVERIFY(scratch.set(py + u"PythonCore\\3.10"_s, L"SysVersion", u"3.10"_s));
        QVERIFY(scratch.set(py + u"PythonCore\\3.10"_s, L"Version", u"3.10.11"_s));
        QVERIFY(scratch.set(py + u"PythonCore\\3.13\\InstallPath"_s, L"ExecutablePath", root + u"\\Python313\\python.exe"_s));
        QVERIFY(scratch.set(py + u"PythonCore\\2.7\\InstallPath"_s, nullptr, root + u"\\Python27"_s));
        // What uv writes (without --no-registry).
        const QString uv = root + u"\\uv\\cpython-3.12.14-windows-x86_64-none"_s;
        QVERIFY(scratch.set(py + u"Astral\\CPython3.12.14\\InstallPath"_s, nullptr, uv));
        QVERIFY(scratch.set(py + u"Astral\\CPython3.12.14\\InstallPath"_s, L"ExecutablePath", uv + u"\\python.exe"_s));
        QVERIFY(scratch.set(py + u"Astral\\CPython3.12.14"_s, L"SysVersion", u"3.12"_s));
        QVERIFY(scratch.set(py + u"Astral\\CPython3.12.14"_s, L"Version", u"3.12.14"_s));
        // Anaconda's Version is its own.
        QVERIFY(scratch.set(py + u"ContinuumAnalytics\\Anaconda39-64\\InstallPath"_s, L"ExecutablePath", root + u"\\anaconda3\\python.exe"_s));
        QVERIFY(scratch.set(py + u"ContinuumAnalytics\\Anaconda39-64"_s, L"SysVersion", u"3.9"_s));
        QVERIFY(scratch.set(py + u"ContinuumAnalytics\\Anaconda39-64"_s, L"Version", u"2022.10"_s));
        // py.exe's own; entries that run nothing (no ExecutablePath: only
        // PythonCore's folder counts).
        QVERIFY(scratch.set(py + u"PyLauncher\\3\\InstallPath"_s, L"ExecutablePath", root + u"\\py.exe"_s));
        QVERIFY(scratch.set(py + u"Astral\\CPython3.11.9"_s, L"SysVersion", u"3.11"_s));
        QVERIFY(scratch.set(py + u"Astral\\CPython3.11.8\\InstallPath"_s, nullptr, root + u"\\uv\\old"_s));

        const QList<programs::RegisteredPython> all = programs::registeredPythons(scratch.root() + u"\\Python"_s);
        QStringList seen;
        for (const programs::RegisteredPython& r : all)
            seen.push_back(r.company + u'/' + r.tag + u'=' + r.version.toString() + u' ' + r.exe);
        seen.sort();
        QCOMPARE(seen, (QStringList {
            u"Astral/CPython3.12.14=3.12.14 "_s + uv + u"\\python.exe"_s,
            u"ContinuumAnalytics/Anaconda39-64=3.9 "_s + root + u"\\anaconda3\\python.exe"_s,
            u"PythonCore/2.7=2.7 "_s + root + u"\\Python27\\python.exe"_s,
            u"PythonCore/3.10=3.10.11 "_s + root + u"\\Python310\\python.exe"_s,
            u"PythonCore/3.13=3.13 "_s + root + u"\\Python313\\python.exe"_s,
        }));
        // python.org's first, as py.exe has it, then the others; the newest first in each.
        QStringList order;
        for (const programs::RegisteredPython& r : programs::ranked(all))
            order.push_back(r.company + u'/' + r.tag);
        QCOMPARE(order, (QStringList {u"PythonCore/3.13"_s, u"PythonCore/3.10"_s, u"Astral/CPython3.12.14"_s, u"ContinuumAnalytics/Anaconda39-64"_s}));
        // Reading creates nothing.
        QVERIFY(!scratch.exists(py + u"Astral\\CPython3.11.9\\InstallPath"_s));
        QVERIFY(programs::registeredPythons(scratch.root() + u"\\Missing\\Python"_s).isEmpty());
        QVERIFY(!scratch.exists(u"Missing"_s));
    }

    void pythonLaunchFor()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString root = QDir::toNativeSeparators(temp.path());
        const QString base = root + u"\\base\\python.exe"_s;
        QVERIFY(copyFake(base));
        const QString venv = root + u"\\.venv\\Scripts\\python.exe"_s;
        QVERIFY(makeUvTrampoline(venv, 2, base));
        QVERIFY(writeFile(root + u"\\.venv\\pyvenv.cfg"_s, (u"home = "_s + root + u"\\base\n"_s).toUtf8()));
        const QString bin = root + u"\\bin\\python.exe"_s;
        QVERIFY(makeUvTrampoline(bin, 2, base));
        const QString plugin = root + u"\\plugin"_s;
        QVERIFY(writeFile(plugin + u"\\main.py"_s, "print('{}')\n"));
        Runtime runtime;
        runtime.program = Program::Python;
        runtime.script = u"main.py"_s;
        const PythonOverride reset;
        QString error;

        programs::setOverride(Program::Python, venv);
        std::optional<programs::Launch> launch = programs::launchFor(runtime, plugin, &error);
        QVERIFY2(launch, qPrintable(error));
        QCOMPARE(launch->exe, base);
        QCOMPARE(launch->args, (QStringList {u"-u"_s, u"-X"_s, u"utf8"_s, plugin + u"\\main.py"_s}));
        QVERIFY(launch->env.contains(std::pair {u"__PYVENV_LAUNCHER__"_s, venv}));
        QVERIFY(launch->env.contains(std::pair {u"PYTHONIOENCODING"_s, u"utf-8"_s}));
        QCOMPARE(programs::find(Program::Python), base);

        // The venv made again over another Python: looked up again.
        const QString base2 = root + u"\\base2\\python.exe"_s;
        QVERIFY(copyFake(base2));
        QVERIFY(writeFile(root + u"\\.venv\\pyvenv.cfg"_s, (u"home = "_s + root + u"\\base2\n"_s).toUtf8()));
        {
            QFile cfg(root + u"\\.venv\\pyvenv.cfg"_s);
            QVERIFY(cfg.open(QIODevice::ReadWrite));
            QVERIFY(cfg.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime));
        }
        launch = programs::launchFor(runtime, plugin, &error);
        QVERIFY2(launch, qPrintable(error));
        QCOMPARE(launch->exe, base2);

        programs::setOverride(Program::Python, bin);
        launch = programs::launchFor(runtime, plugin, &error);
        QVERIFY2(launch, qPrintable(error));
        QCOMPARE(launch->exe, base);
        QVERIFY(std::none_of(launch->env.cbegin(), launch->env.cend(), [](const auto& e) { return e.first == u"__PYVENV_LAUNCHER__"_s; }));

        // The card says why.
        const QString py = root + u"\\launcher\\py.exe"_s;
        QVERIFY(copyFake(py));
        programs::setOverride(Program::Python, py);
        QVERIFY(!programs::launchFor(runtime, plugin, &error));
        QVERIFY2(error.contains(py) && error.contains(u"启动器"_s), qPrintable(error));
        QString hint;
        QVERIFY(programs::find(Program::Python, &hint).isEmpty());
        QCOMPARE(hint, error);
        programs::setOverride(Program::Python, root + u"\\gone\\python.exe"_s);
        QVERIFY(!programs::launchFor(runtime, plugin, &error));
        QCOMPARE(error, u"找不到 %1"_s.arg(root + u"\\gone\\python.exe"_s));
    }

    // ---- items ---------------------------------------------------------------
    void itemsMapping()
    {
        const Item item = parseItem(parseObject(R"({"id":"pid-1","title":"notepad.exe","subtitle":"PID 1","subtitleAway":"Up, then Enter","icon":"file:C:\\Windows\\notepad.exe",
            "score":180,"match":"notepad 记事本","autocomplete":"kill notepad","open":{"path":"C:\\x.txt","args":"-a"},"copy":"1","preview":{"text":"p"},
            "actions":[{"id":"kill","title":{"zh":"结束","en":"Kill"},"key":"Enter"}],"valid":false,"data":{"pid":1}})"));
        QCOMPARE(item.id, u"pid-1"_s);
        QCOMPARE(item.title, u"notepad.exe"_s);
        QCOMPARE(item.subtitle, u"PID 1"_s);
        QCOMPARE(item.subtitleAway, u"Up, then Enter"_s);
        QCOMPARE(item.icon.kind, IconSpec::Kind::File);
        QCOMPARE(item.icon.value, u"C:\\Windows\\notepad.exe"_s);
        QCOMPARE(item.score, 100); // clamped
        QVERIFY(item.hasScore);
        QCOMPARE(item.match, u"notepad 记事本"_s);
        QCOMPARE(item.autocomplete, u"kill notepad"_s);
        QCOMPARE(item.open.kind, Open::Kind::Path);
        QCOMPARE(item.open.target, u"C:\\x.txt"_s);
        QCOMPARE(item.open.args, u"-a"_s);
        QCOMPARE(item.copy, u"1"_s);
        QCOMPARE(item.previewText, u"p"_s);
        QCOMPARE(item.actions.size(), 1);
        QCOMPARE(item.actions[0].title.pick(u"zh"_s), u"结束"_s);
        QVERIFY(!item.valid);
        QCOMPARE(item.data.toObject().value(u"pid"_s).toInt(), 1);
        // Defaults.
        const Item plain = parseItem(parseObject(R"({"title":"t","open":{"url":"mailto:a@b.c"}})"));
        QCOMPARE(plain.score, 50);
        QVERIFY(!plain.hasScore);
        QVERIFY(plain.valid);
        QCOMPARE(plain.open.kind, Open::Kind::Url);
        QCOMPARE(parseItem(parseObject(R"({"title":"t","score":-5})")).score, 0);
        // Back to JSON for the action request.
        const QJsonObject back = itemToJson(item);
        QCOMPARE(back.value(u"id"_s).toString(), u"pid-1"_s);
        QCOMPARE(back.value(u"data"_s).toObject().value(u"pid"_s).toInt(), 1);
        QCOMPARE(back.value(u"open"_s).toObject().value(u"path"_s).toString(), u"C:\\x.txt"_s);
        QCOMPARE(back.value(u"subtitleAway"_s).toString(), u"Up, then Enter"_s);
        QVERIFY(!itemToJson(plain).contains(u"subtitleAway"_s));
    }

    void itemMetadata()
    {
        const Item item = parseItem(parseObject(R"({"title":"report.xlsx","modified":"2026-01-15T08:00:00Z","created":1700000000,"size":0})"));
        QCOMPARE(item.modified, std::uint32_t(QDateTime::fromString(u"2026-01-15T08:00:00Z"_s, Qt::ISODate).toSecsSinceEpoch()));
        QCOMPARE(item.created, std::uint32_t(1700000000));
        QCOMPARE(item.size, qint64(0));
        QCOMPARE(parseItem(itemToJson(item)), item);
        const Item invalid = parseItem(parseObject(R"({"title":"bad","modified":"invalid","created":-1,"size":-1})"));
        QCOMPARE(invalid.modified, std::uint32_t(0));
        QCOMPARE(invalid.created, std::uint32_t(0));
        QCOMPARE(invalid.size, qint64(-1));
        for (const auto* json : {R"({"title":"fraction","size":1.5})", R"({"title":"string","size":"100"})", R"({"title":"huge","size":1e30})"})
            QCOMPARE(parseItem(parseObject(json)).size, qint64(-1));
    }

    void itemsIcons()
    {
        QCOMPARE(parseIcon(u"ext:pdf"_s).kind, IconSpec::Kind::Ext);
        QCOMPARE(parseIcon(u"ext:pdf"_s).value, u"pdf"_s);
        QCOMPARE(parseIcon(u"app:Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"_s).kind, IconSpec::Kind::App);
        QCOMPARE(parseIcon(u"glyph:U+E8E5"_s).kind, IconSpec::Kind::Glyph);
        QCOMPARE(parseIcon(u"color:#ff8800"_s).kind, IconSpec::Kind::Color);
        QCOMPARE(parseIcon(u"color:#ff8800"_s).value, u"#ff8800"_s);
        QCOMPARE(parseIcon(u"icon.png"_s).kind, IconSpec::Kind::PluginFile);
        QCOMPARE(parseIcon(u"icons/a.svg"_s).kind, IconSpec::Kind::None); // no SVG support in the installed Qt
        QCOMPARE(parseIcon(u"data:image/png;base64,iVBORw0KGgo="_s).kind, IconSpec::Kind::Data);
        QCOMPARE(parseIcon(u"data:text/plain,hi"_s).kind, IconSpec::Kind::None);
        // A site's icon: only its home page is kept, never the path.
        QCOMPARE(parseIcon(u"site:https://github.com/LingCore/WinShun?tab=1"_s),
            (IconSpec {IconSpec::Kind::Site, u"https://github.com/"_s}));
        QCOMPARE(parseIcon(u"site:http://192.168.1.2:5244/dav/a"_s).value, u"http://192.168.1.2:5244/"_s);
        QCOMPARE(parseIcon(u"site:file:///C:/a.html"_s).kind, IconSpec::Kind::None);
        QCOMPARE(parseIcon(u"site:github.com"_s).kind, IconSpec::Kind::None);
        QCOMPARE(parseIcon(u"..\\icon.png"_s).kind, IconSpec::Kind::None); // leaves the folder
        QCOMPARE(parseIcon(u"C:\\icon.png"_s).kind, IconSpec::Kind::None);
        QCOMPARE(parseIcon(u"readme.txt"_s).kind, IconSpec::Kind::None);
        QCOMPARE(parseIcon(QString()).kind, IconSpec::Kind::None);
    }

    void itemsOutputForms()
    {
        // JSON object, bare array, plain text.
        QCOMPARE(parseOutput(R"({"items":[{"title":"a"}],"rerun":2,"cache":60})").items.size(), 1);
        QCOMPARE(parseOutput(R"({"items":[{"title":"a"}],"rerun":2,"cache":60})").rerun, 2.0);
        QCOMPARE(parseOutput(R"({"items":[{"title":"a"}],"rerun":2,"cache":60})").cacheSeconds, 60);
        QCOMPARE(parseOutput(R"([{"title":"a"},{"title":"b"},{"nope":1}])").items.size(), 2);
        const QueryResult text = parseOutput("first\r\n\n第二行\n");
        QCOMPARE(text.items.size(), 2);
        QCOMPARE(text.items[0].title, u"first"_s);
        QCOMPARE(text.items[0].copy, u"first"_s);
        QCOMPARE(text.items[1].title, u"第二行"_s);
        // Broken JSON is text too.
        QCOMPARE(parseOutput("{not json").items.size(), 1);
        // More than 200: cut.
        QJsonArray many;
        for (int i = 0; i < 250; ++i)
            many.push_back(QJsonObject {{u"title"_s, QString::number(i)}});
        const QueryResult cut = parseResult(many);
        QCOMPARE(cut.items.size(), kMaxItems);
        QVERIFY(cut.truncated);
    }

    void effects()
    {
        const Effects e = parseEffects(parseObject(R"({"effects":[{"type":"notify","text":"done"},{"type":"open","url":"mailto:a@b.c"},{"type":"setQuery","text":"x"},{"type":"copy","text":"c"},{"type":"replace","items":[{"title":"r"}]},{"type":"keep"},{"type":"bogus"}]})"));
        QCOMPARE(e.size(), 6);
        QCOMPARE(e[0].kind, Effect::Kind::Notify);
        QCOMPARE(e[0].text, u"done"_s);
        QCOMPARE(e[1].kind, Effect::Kind::Open);
        QCOMPARE(e[1].open.kind, Open::Kind::Url);
        QCOMPARE(e[2].kind, Effect::Kind::SetQuery);
        QCOMPARE(e[3].kind, Effect::Kind::Copy);
        QCOMPARE(e[4].kind, Effect::Kind::Replace);
        QCOMPARE(e[4].items.size(), 1);
        QCOMPARE(e[5].kind, Effect::Kind::Keep);
        const Effects none = parseEffects(QJsonValue());
        QCOMPARE(none.size(), 1);
        QCOMPARE(none[0].kind, Effect::Kind::Hide);
    }

    // ---- zero-code -------------------------------------------------------------
    void zeroCodeUrlEncoding()
    {
        Run run;
        run.kind = Run::Kind::Url;
        run.url = u"https://github.com/search?q=%s"_s;
        WebShortcut web {u"gh"_s, u"GitHub"_s, run.url, {}};
        for (const QString& words : {u"Win顺 launcher"_s, u"a&b=c"_s, u"100%"_s})
            QCOMPARE(zeroCodeUrl(run, words), web.searchUrl(words));
        QCOMPARE(zeroCodeUrl(run, {}), web.homeUrl());
        QCOMPARE(zeroCodeUrl(run, {}), u"https://github.com/"_s);
        run.home = u"https://github.com/LingCore"_s;
        QCOMPARE(zeroCodeUrl(run, {}), run.home);
        QCOMPARE(zeroCodeUrl(run, u"x"_s), u"https://github.com/search?q=x"_s);
    }

    void zeroCodeRuns()
    {
        Fixture f;
        Spec spec;
        spec.id = u"test.zero"_s;
        spec.mode.clear();
        spec.commands = QJsonArray {
            QJsonObject {{u"id"_s, u"gh"_s}, {u"keyword"_s, u"gh"_s}, {u"title"_s, u"GitHub"_s}, {u"run"_s, QJsonObject {{u"url"_s, u"https://github.com/search?q=%s"_s}}}},
            QJsonObject {{u"id"_s, u"code"_s}, {u"keyword"_s, u"code"_s}, {u"title"_s, u"VS Code"_s},
                {u"run"_s, QJsonObject {{u"program"_s, u"%SystemRoot%\\notepad.exe"_s}, {u"args"_s, u"\"%s\""_s}}}},
            QJsonObject {{u"id"_s, u"dl"_s}, {u"keyword"_s, u"dl"_s}, {u"title"_s, u"下载"_s}, {u"run"_s, QJsonObject {{u"folder"_s, u"%USERPROFILE%\\Downloads"_s}}}},
            QJsonObject {{u"id"_s, u"pdf"_s}, {u"keyword"_s, u"pdf"_s}, {u"title"_s, u"PDF"_s},
                {u"run"_s, QJsonObject {{u"search"_s, QJsonObject {{u"query"_s, u"ext:pdf %s"_s}, {u"scope"_s, u"files"_s}}}}}},
            QJsonObject {{u"id"_s, u"ssh"_s}, {u"keyword"_s, u"ssh"_s}, {u"title"_s, u"SSH"_s},
                {u"run"_s, QJsonObject {{u"items"_s, QJsonArray {QJsonObject {{u"title"_s, u"nas"_s}, {u"subtitle"_s, u"192.168.1.10"_s}, {u"copy"_s, u"ssh nas"_s}},
                                                          QJsonObject {{u"title"_s, u"服务器"_s}, {u"match"_s, u"server fuwuqi"_s}}}}}}},
        };
        QVERIFY(!f.install(spec).isEmpty());
        QVERIFY(f.host->query(u"gh Win顺"_s, Scope::All, 1));
        const Result* r = f.finalFor(1);
        QVERIFY(r);
        QCOMPARE(r->command, u"gh"_s);
        QCOMPARE(r->items.size(), 1);
        QCOMPARE(r->items[0].open.kind, Open::Kind::Url);
        QCOMPARE(r->items[0].open.target, u"https://github.com/search?q=Win%E9%A1%BA"_s);
        QVERIFY(f.host->query(u"code a b"_s, Scope::All, 2));
        r = f.finalFor(2);
        QVERIFY(r);
        QCOMPARE(r->items[0].open.kind, Open::Kind::Path);
        QVERIFY(r->items[0].open.target.endsWith(u"\\notepad.exe"_s, Qt::CaseInsensitive));
        QVERIFY(!r->items[0].open.target.contains(u'%'));
        QCOMPARE(r->items[0].open.args, u"\"a b\""_s);
        QVERIFY(!f.host->query(u"dl"_s, Scope::All, 11)); // the keyword alone is a name to search for (the hint row offers the command)
        QVERIFY(f.host->query(u"dl "_s, Scope::All, 3));
        r = f.finalFor(3);
        QVERIFY(r);
        QVERIFY(r->items[0].open.target.endsWith(u"\\Downloads"_s));
        QVERIFY(f.host->query(u"pdf 报告"_s, Scope::All, 4));
        r = f.finalFor(4);
        QVERIFY(r);
        QCOMPARE(r->items[0].searchQuery, u"ext:pdf 报告"_s);
        QCOMPARE(r->items[0].searchScope, u"files"_s);
        QVERIFY(f.host->query(u"ssh "_s, Scope::All, 5));
        QCOMPARE(f.finalFor(5)->items.size(), 2);
        QVERIFY(f.host->query(u"ssh fuwu"_s, Scope::All, 6));
        QCOMPARE(f.finalFor(6)->items.size(), 1);
        QCOMPARE(f.finalFor(6)->items[0].title, u"服务器"_s);
        QVERIFY(f.host->query(u"ssh nas"_s, Scope::All, 7));
        QCOMPARE(f.finalFor(7)->items[0].copy, u"ssh nas"_s);
        // Not in 全部, and not a keyword.
        QVERIFY(!f.host->query(u"gh x"_s, Scope::Files, 8));
        QVERIFY(!f.host->query(u"ghx"_s, Scope::All, 9));
        QVERIFY(!f.host->query(u"xgh a"_s, Scope::All, 10));
        // Zero-code actions need no process.
        f.host->action(1, u"test.zero"_s, u"gh"_s, {}, f.finalFor(1)->items[0], u"Win顺"_s);
        QCOMPARE(f.actions.size(), 1);
        QVERIFY(f.actions[0].ok);
        QCOMPARE(f.actions[0].effects[0].kind, Effect::Kind::Hide);
    }

    // ---- keywords ------------------------------------------------------------
    void keywordConflicts()
    {
        Fixture f;
        Spec a;
        a.id = u"test.a"_s;
        a.mode.clear();
        a.commands = QJsonArray {QJsonObject {{u"id"_s, u"one"_s}, {u"keyword"_s, QJsonArray {u"gh"_s, u"aa"_s}}, {u"run"_s, QJsonObject {{u"url"_s, u"https://a.com/?q=%s"_s}}}}};
        Spec b;
        b.id = u"test.b"_s;
        b.mode.clear();
        b.commands = QJsonArray {QJsonObject {{u"id"_s, u"two"_s}, {u"keyword"_s, QJsonArray {u"AA"_s, u"bb"_s}}, {u"run"_s, QJsonObject {{u"url"_s, u"https://b.com/?q=%s"_s}}}}};
        QVERIFY(!f.install(a).isEmpty());
        QVERIFY(!f.install(b).isEmpty());
        Catalog& catalog = f.host->catalog();
        // Two defaults alike: neither works.
        QVERIFY(catalog.match(u"aa x"_s).empty());
        QCOMPARE(catalog.conflicts().size(), 2u);
        QCOMPARE(catalog.keywordsOf(u"test.a"_s, u"one"_s), QStringList {u"gh"_s});
        QCOMPARE(catalog.keywordsOf(u"test.b"_s, u"two"_s), QStringList {u"bb"_s});
        // [WebSearch] wins: gh is GitHub's.
        f.host->setWebKeywords({{u"GH"_s, u"GitHub"_s}});
        QVERIFY(catalog.match(u"gh x"_s).empty());
        bool webConflict = false;
        for (const KeywordConflict& c : catalog.conflicts())
            webConflict = webConflict || (c.keyword == u"gh" && c.takenBy == u"web:GitHub");
        QVERIFY(webConflict);
        // A favorite's keyword wins too (after the web's).
        f.host->setFavoriteKeywords({{u"BB"_s, u"工作文档"_s}, {u"gh"_s, u"GitHub 文件夹"_s}});
        QVERIFY(catalog.match(u"bb x"_s).empty());
        bool favoriteConflict = false;
        for (const KeywordConflict& c : catalog.conflicts()) {
            favoriteConflict = favoriteConflict || (c.keyword == u"bb" && c.takenBy == u"fav:工作文档");
            QVERIFY(c.takenBy != u"fav:GitHub 文件夹"); // gh is the web's
        }
        QVERIFY(favoriteConflict);
        f.host->setFavoriteKeywords({}); // the favorite gone: bb the plugin's again
        QCOMPARE(catalog.keywordsOf(u"test.b"_s, u"two"_s), QStringList {u"bb"_s});
        // The user's keyword wins over a default, and replaces the plugin's own.
        f.host->setKeywordOverrides({{u"test.b/two"_s, u"aa"_s}});
        std::vector<Match> matches = catalog.match(u"AA hello  world"_s);
        QCOMPARE(matches.size(), 1u);
        QCOMPARE(matches[0].entry->id, u"test.b"_s);
        QCOMPARE(matches[0].kind, Match::Kind::Keyword);
        QCOMPARE(matches[0].keyword, u"AA"_s);
        QCOMPARE(matches[0].text, u"hello  world"_s);
        QVERIFY(catalog.match(u"bb x"_s).empty()); // replaced by the override
        // The keyword with a space after it and nothing else, typed with any
        // case; alone it is a name to search for (hint() offers the command).
        matches = catalog.match(u"Aa "_s);
        QCOMPARE(matches.size(), 1u);
        QVERIFY(matches[0].text.isEmpty());
        QVERIFY(catalog.match(u"Aa"_s).empty());
        QVERIFY(catalog.hint(u"Aa"_s).has_value());
        QCOMPARE(catalog.hint(u"Aa"_s)->entry->id, u"test.b"_s);
        QVERIFY(catalog.match(u"aax"_s).empty());
        QVERIFY(!catalog.hint(u"aax"_s));
        // Disabled: gone from the index, and its keyword falls back to the other.
        f.host->setEnabled(u"test.b"_s, false);
        QCOMPARE(catalog.match(u"aa "_s).size(), 1u);
        QCOMPARE(catalog.match(u"aa "_s)[0].entry->id, u"test.a"_s);
        f.host->setEnabled(u"test.b"_s, true);
        QCOMPARE(catalog.match(u"aa "_s)[0].entry->id, u"test.b"_s);
        // An override round trip: the user's keyword, then none, then the manifest's again.
        f.host->setKeywordOverrides({{u"test.b/two"_s, u"zz"_s}});
        QCOMPARE(catalog.keywordsOf(u"test.b"_s, u"two"_s), QStringList {u"zz"_s});
        QCOMPARE(catalog.match(u"zz q"_s).size(), 1u);
        QVERIFY(catalog.match(u"bb q"_s).empty());
        f.host->setKeywordOverrides({{u"test.b/two"_s, QString()}}); // the user took the keyword away
        QVERIFY(catalog.keywordsOf(u"test.b"_s, u"two"_s).isEmpty());
        QVERIFY(catalog.match(u"zz q"_s).empty());
        f.host->setKeywordOverrides({});
        QCOMPARE(catalog.keywordsOf(u"test.b"_s, u"two"_s), QStringList {u"bb"_s}); // AA: a's default too, both set aside again
        QCOMPARE(catalog.match(u"bb q"_s).size(), 1u);
    }

    // The space after a list command's keyword opens it; the keyword alone
    // searches names, with the command offered as a row (Merge::placeHint).
    // An instant command's keyword works alone.
    void keywordNeedsSpace()
    {
        Fixture f;
        Spec spec;
        spec.id = u"test.kw"_s;
        spec.mode.clear();
        spec.commands = QJsonArray {
            QJsonObject {{u"id"_s, u"kill"_s}, {u"keyword"_s, u"kill"_s}, {u"title"_s, QJsonObject {{u"zh"_s, u"结束进程"_s}, {u"en"_s, u"Kill"_s}}},
                {u"run"_s, QJsonObject {{u"items"_s, QJsonArray {QJsonObject {{u"title"_s, u"x"_s}}}}}}},
            QJsonObject {{u"id"_s, u"empty"_s}, {u"keyword"_s, u"empty"_s}, {u"title"_s, u"清空"_s}, {u"mode"_s, u"instant"_s},
                {u"run"_s, QJsonObject {{u"items"_s, QJsonArray {QJsonObject {{u"title"_s, u"y"_s}}}}}}},
        };
        QVERIFY(!f.install(spec).isEmpty());
        const Catalog& catalog = f.host->catalog();
        QVERIFY(catalog.match(u"kill"_s).empty());
        QVERIFY(catalog.match(u"  kill"_s).empty());
        QCOMPARE(catalog.match(u"kill "_s).size(), 1u);
        QCOMPARE(catalog.match(u"  kill note"_s).size(), 1u);
        QCOMPARE(catalog.match(u"kill note"_s)[0].text, u"note"_s);
        QCOMPARE(catalog.match(u"empty"_s).size(), 1u); // instant: alone is enough
        QCOMPARE(catalog.match(u"EMPTY "_s).size(), 1u);
        QVERIFY(catalog.hint(u"kill"_s).has_value());
        QVERIFY(!catalog.hint(u"kill "_s)); // in the command already
        QVERIFY(!catalog.hint(u"empty"_s)); // instant: no hint, the command itself
        QVERIFY(!catalog.hint(u"kil"_s));
        QVERIFY(!f.host->query(u"kill"_s, Scope::All, 1)); // nobody answers the bare keyword
        // hints(): the exact keyword, then the keywords a word of two or
        // more characters begins (an instant command's too), not a letter's.
        std::vector<Match> hints = catalog.hints(u"kill"_s);
        QCOMPARE(hints.size(), 1u);
        QVERIFY(!hints[0].partial);
        hints = catalog.hints(u"kil"_s);
        QCOMPARE(hints.size(), 1u);
        QVERIFY(hints[0].partial);
        QCOMPARE(hints[0].keyword, u"kill"_s);
        hints = catalog.hints(u"EM"_s);
        QCOMPARE(hints.size(), 1u);
        QCOMPARE(hints[0].command->id, u"empty"_s);
        QVERIFY(catalog.hints(u"k"_s).empty());
        QVERIFY(catalog.hints(u"kil "_s).empty()); // a space: a name with a word to come
        QVERIFY(catalog.hints(u"x"_s).empty());

        // The hint row of the keyword typed alone: first, even before the
        // rows opened lately (its words promise Enter and Tab), and the
        // keyboard brought back to it (betterFirstRow).
        ListSink sink;
        sink.current = 0;
        Merge merge(sink, catalog);
        merge.setHintText(u"按空格进入"_s);
        merge.setPrefixHintText(u"按 Tab 补全"_s);
        merge.begin(2, u"kill"_s);
        QVERIFY(!merge.keywordMode());
        sink.assign({fileRow(u"kill.txt"_s, true), appRow(u"Killer"_s, 60), fileRow(u"kill.log"_s)});
        merge.namesShown();
        QCOMPARE(sink.names(), (QStringList {u"kill — 结束进程"_s, u"kill.txt"_s, u"Killer"_s, u"kill.log"_s}));
        QVERIFY(isHintRow(sink.rows[0]));
        QVERIFY(merge.takeBetterFirstRow());
        QCOMPARE(sink.rows[0].plugin->setQuery, u"kill "_s); // Enter
        QCOMPARE(sink.rows[0].plugin->autocomplete, u"kill "_s); // Tab
        QCOMPARE(sink.rows[0].plugin->subtitle, u"按空格进入"_s);
        merge.namesShown(); // the names shown again: still one hint row
        QCOMPARE(sink.count(), 4);
        QVERIFY(!merge.takeBetterFirstRow());
        // With none at all it is the list.
        sink.assign({});
        merge.namesShown();
        QCOMPARE(sink.names(), (QStringList {u"kill — 结束进程"_s}));
        // A word the keyword begins with: a row after the recent rows and
        // the apps, before the files, scoring low (not first, not current).
        merge.begin(3, u"kil"_s);
        sink.assign({fileRow(u"kil.txt"_s, true), appRow(u"Killer"_s, 60), fileRow(u"kil.log"_s)});
        merge.namesShown();
        QCOMPARE(sink.names(), (QStringList {u"kil.txt"_s, u"Killer"_s, u"kill — 结束进程"_s, u"kil.log"_s}));
        QVERIFY(isHintRow(sink.rows[2]));
        QCOMPARE(sink.rows[2].plugin->autocomplete, u"kill "_s);
        QCOMPARE(sink.rows[2].plugin->subtitle, u"按 Tab 补全"_s);
        QVERIFY(!merge.takeBetterFirstRow());
        merge.begin(3, u"kil"_s); // files only: the row leads, but the keyboard stays on the first file
        sink.assign({fileRow(u"kil.log"_s)});
        merge.namesShown();
        QCOMPARE(sink.names(), (QStringList {u"kill — 结束进程"_s, u"kil.log"_s}));
        QVERIFY(!merge.takeBetterFirstRow());
        merge.begin(3, u"em"_s); // an instant command's keyword works alone: no space
        sink.assign({});
        merge.namesShown();
        QCOMPARE(sink.rows[0].plugin->autocomplete, u"empty"_s);
        // Not for other words, nor in keyword mode.
        merge.begin(3, u"kix"_s);
        sink.assign({appRow(u"Killer"_s, 60)});
        merge.namesShown();
        QCOMPARE(sink.count(), 1);
        merge.begin(4, u"kill "_s, u"test.kw"_s, u"kill"_s);
        QVERIFY(merge.keywordMode());
        sink.assign({});
        merge.namesShown();
        QCOMPARE(sink.count(), 0);
        // A pushed answer does not count the hint row against its quota, nor remove it.
        Spec pusher;
        pusher.id = u"test.push"_s;
        pusher.mode.clear();
        pusher.commands = QJsonArray {QJsonObject {{u"id"_s, u"p"_s}, {u"title"_s, u"推送"_s},
            {u"triggers"_s, QJsonArray {QJsonObject {{u"push"_s, QJsonObject {{u"minLength"_s, 2}}}}}},
            {u"run"_s, QJsonObject {{u"items"_s, QJsonArray {QJsonObject {{u"title"_s, u"pushed"_s}}}}}}}};
        QVERIFY(!f.install(pusher).isEmpty());
        merge.begin(5, u"kill"_s);
        sink.assign({appRow(u"Killer"_s, 60)});
        merge.namesShown();
        merge.results(5, u"test.push"_s, u"p"_s, {scoredItem(u"1"_s, u"p1"_s, 70), scoredItem(u"2"_s, u"p2"_s, 70), scoredItem(u"3"_s, u"p3"_s, 70)}, false, true);
        QCOMPARE(sink.count(), 5); // the hint, three pushed (the plugin's quota), the app
        QVERIFY(isHintRow(sink.rows[0]));
    }

    void triggersRegexAndPush()
    {
        Fixture f;
        Spec spec;
        spec.id = u"test.trig"_s;
        spec.mode.clear();
        spec.commands = QJsonArray {
            QJsonObject {{u"id"_s, u"color"_s}, {u"title"_s, u"颜色"_s}, {u"triggers"_s, QJsonArray {QJsonObject {{u"regex"_s, u"^#([0-9a-fA-F]{6})$"_s}, {u"minLength"_s, 7}, {u"maxLength"_s, 7}}}},
                {u"run"_s, QJsonObject {{u"items"_s, QJsonArray {QJsonObject {{u"title"_s, u"swatch"_s}}}}}}},
            QJsonObject {{u"id"_s, u"calc"_s}, {u"title"_s, u"计算"_s}, {u"keyword"_s, u"="_s}, {u"triggers"_s, QJsonArray {QJsonObject {{u"push"_s, QJsonObject {{u"minLength"_s, 3}, {u"maxItems"_s, 2}}}}}},
                {u"run"_s, QJsonObject {{u"items"_s, QJsonArray {QJsonObject {{u"title"_s, u"pushed"_s}}}}}}},
        };
        QVERIFY(!f.install(spec).isEmpty());
        std::vector<Match> matches = f.host->catalog().match(u"#ff8800"_s);
        QCOMPARE(matches.size(), 2u); // the regex, and the push (7 >= 3)
        QCOMPARE(matches[0].kind, Match::Kind::Regex);
        QCOMPARE(matches[0].captures, QStringList {u"ff8800"_s});
        QCOMPARE(matches[1].kind, Match::Kind::Push);
        QCOMPARE(matches[1].maxItems, 2);
        QVERIFY(f.host->catalog().match(u"#ff88"_s).empty() == false); // push only (5 chars)
        QCOMPARE(f.host->catalog().match(u"#ff88"_s)[0].kind, Match::Kind::Push);
        QVERIFY(f.host->catalog().match(u"ab"_s).empty()); // too short for the push
        // In keyword mode nothing is pushed.
        matches = f.host->catalog().match(u"= 1+2"_s);
        QCOMPARE(matches.size(), 1u);
        QCOMPARE(matches[0].kind, Match::Kind::Keyword);
    }

    // ---- merging into the launcher's list (Merge.h) ---------------------------
    void mergeToRow()
    {
        Fixture f;
        Spec spec;
        spec.id = u"test.row"_s;
        spec.mode.clear();
        spec.commandExtra = {{u"icon"_s, u"glyph:U+E711"_s}, {u"run"_s, QJsonObject {{u"items"_s, QJsonArray {QJsonObject {{u"title"_s, u"x"_s}}}}}},
            {u"actions"_s, QJsonArray {QJsonObject {{u"id"_s, u"kill"_s}, {u"title"_s, QJsonObject {{u"zh"_s, u"结束"_s}, {u"en"_s, u"Kill"_s}}}, {u"key"_s, u"Enter"_s}},
                                 QJsonObject {{u"id"_s, u"tree"_s}, {u"title"_s, u"连同子进程"_s}}}}};
        QVERIFY(!f.install(spec).isEmpty());
        const plugin::Entry* entry = f.host->catalog().find(u"test.row"_s);
        QVERIFY(entry && entry->manifest);
        const Command& command = entry->manifest->commands.first();
        Item item = parseItem(parseObject(R"({"title": "notepad", "subtitle": "PID 12", "subtitleAway": "Up, then Enter", "copy": "12", "autocomplete": "fk note",
            "open": {"path": "C:\\a.exe", "args": "-x"}, "valid": false, "data": {"pid": 12}})"));
        SearchResult row = toRow(*entry, command, item, 4, u"en"_s);
        QVERIFY(row.isPlugin());
        QVERIFY(row.isApp());
        QVERIFY(!row.isPlace());
        QCOMPARE(row.name, u"notepad"_s);
        QCOMPARE(row.score, 50);
        QCOMPARE(row.path, u"winshun-plugin:test.row/main/#4"_s); // no id: its place in the answer
        QVERIFY(isPluginPath(row.path));
        QCOMPARE(row.plugin->itemId, u"#4"_s);
        QCOMPARE(row.plugin->source, u"Fake"_s); // the plugin's name in the interface's language
        QCOMPARE(row.plugin->subtitle, u"PID 12"_s);
        QCOMPARE(row.plugin->subtitleAway, u"Up, then Enter"_s); // the line while the keyboard is elsewhere
        QCOMPARE(row.plugin->icon, u"glyph:U+E711"_s); // the command's, the item having none
        QCOMPARE(row.plugin->copy, u"12"_s);
        QCOMPARE(row.plugin->autocomplete, u"fk note"_s);
        QCOMPARE(row.plugin->openKind, PluginRow::OpenKind::Path);
        QCOMPARE(row.plugin->openTarget, u"C:\\a.exe"_s);
        QCOMPARE(row.plugin->openArgs, u"-x"_s);
        QVERIFY(!row.plugin->valid);
        QCOMPARE(row.plugin->actions.size(), 2); // the command's
        QCOMPARE(row.plugin->actions[0].title, u"Kill"_s);
        QCOMPARE(row.plugin->actions[1].title, u"连同子进程"_s);
        QCOMPARE(row.plugin->item.value(u"data"_s).toObject().value(u"pid"_s).toInt(), 12);
        // Its own icon, id and actions win; the plugin's icon when the command has none.
        item.id = u"p1"_s;
        item.icon = parseIcon(u"color:#ff8800"_s);
        item.actions = {Action {u"own"_s, Text {u"自己的"_s, {}}, {}}};
        row = toRow(*entry, command, item, 0, u"zh"_s);
        QCOMPARE(row.path, u"winshun-plugin:test.row/main/p1"_s);
        QCOMPARE(row.plugin->icon, u"color:#ff8800"_s);
        QCOMPARE(row.plugin->actions.size(), 1);
        QCOMPARE(row.plugin->actions[0].title, u"自己的"_s);
        QCOMPARE(row.plugin->source, u"假插件"_s);
        // Equal by what they say, not by which object says it.
        SearchResult again = toRow(*entry, command, item, 0, u"zh"_s);
        QVERIFY(row == again);
        again.plugin.row = std::make_shared<PluginRow>(*again.plugin.row);
        QVERIFY(row == again);
    }

    void mergePushQuotaAndOrder()
    {
        Fixture f;
        installThree(f);
        ListSink sink;
        Merge merge(sink, f.host->catalog());
        merge.begin(1, u"ab"_s);
        QVERIFY(!merge.keywordMode());
        // Nothing goes in before the names are shown.
        merge.results(1, u"test.a"_s, u"main"_s, {scoredItem(u"1"_s, u"a90"_s, 90)}, false, true);
        QCOMPARE(sink.count(), 0);
        sink.assign({appRow(u"app60"_s, 60, false, true), appRow(u"app40"_s, 40), fileRow(u"f1"_s), fileRow(u"f2"_s)});
        merge.namesShown();
        QCOMPARE(sink.names(), (QStringList {u"app60"_s, u"a90"_s, u"app40"_s, u"f1"_s, u"f2"_s}));
        // Answering again: by score among the apps, after the exact name
        // (never above it), before the files; three rows at most, and the
        // rest are to be seen by the keyword.
        merge.results(1, u"test.a"_s, u"main"_s,
            {scoredItem(u"1"_s, u"a90"_s, 90), scoredItem(u"2"_s, u"a30"_s, 30), scoredItem(u"3"_s, u"a70"_s, 70),
                scoredItem(u"4"_s, u"a10"_s, 10), scoredItem(u"5"_s, u"a5"_s, 5)},
            false, true);
        QCOMPARE(sink.names(), (QStringList {u"app60"_s, u"a90"_s, u"a70"_s, u"app40"_s, u"a30"_s, u"f1"_s, u"f2"_s}));
        QCOMPARE(sink.rows[1].plugin->subtitle, u"（aa 查看全部）"_s);
        QCOMPARE(merge.pluginRows(), 3);
        // Another plugin: three more; the third gets nothing (six in all).
        merge.results(1, u"test.b"_s, u"main"_s,
            {scoredItem(u"1"_s, u"b80"_s, 80), scoredItem(u"2"_s, u"b20"_s, 20), scoredItem(u"3"_s, u"b50"_s, 50), scoredItem(u"4"_s, u"b1"_s, 1)},
            false, true);
        QCOMPARE(sink.names(),
            (QStringList {u"app60"_s, u"a90"_s, u"b80"_s, u"a70"_s, u"b50"_s, u"app40"_s, u"a30"_s, u"b20"_s, u"f1"_s, u"f2"_s}));
        merge.results(1, u"test.c"_s, u"main"_s, {scoredItem(u"1"_s, u"c99"_s, 99)}, false, true);
        QCOMPARE(merge.pluginRows(), 6);
        QVERIFY(!sink.names().contains(u"c99"_s));
        QVERIFY(!merge.takeBetterFirstRow());
    }

    void mergeBelowCurrentRow()
    {
        Fixture f;
        installThree(f);
        ListSink sink;
        Merge merge(sink, f.host->catalog());
        // The keyboard on the third row: nothing goes in above it (or in it).
        merge.begin(1, u"ab"_s);
        sink.assign({appRow(u"app1"_s, 10), appRow(u"app2"_s, 10), appRow(u"app3"_s, 10), fileRow(u"f1"_s)});
        sink.current = 2;
        merge.namesShown();
        merge.results(1, u"test.a"_s, u"main"_s, {scoredItem(u"1"_s, u"a99"_s, 99)}, false, true);
        QCOMPARE(sink.names(), (QStringList {u"app1"_s, u"app2"_s, u"app3"_s, u"a99"_s, u"f1"_s}));
        QVERIFY(!merge.takeBetterFirstRow());
        // On the first row, a better one takes its place, and the launcher is told.
        merge.begin(2, u"ab"_s);
        sink.assign({appRow(u"app1"_s, 10), fileRow(u"f1"_s)});
        sink.current = 0;
        merge.namesShown();
        merge.results(2, u"test.a"_s, u"main"_s, {scoredItem(u"1"_s, u"a99"_s, 99)}, false, true);
        QCOMPARE(sink.names(), (QStringList {u"a99"_s, u"app1"_s, u"f1"_s}));
        QVERIFY(merge.takeBetterFirstRow());
        QVERIFY(!merge.takeBetterFirstRow());
        // Recent rows keep their place too, wherever they are.
        merge.begin(3, u"ab"_s);
        sink.assign({appRow(u"app1"_s, 10), fileRow(u"recent"_s, true), fileRow(u"f1"_s)});
        sink.current = 0;
        merge.namesShown();
        merge.results(3, u"test.a"_s, u"main"_s, {scoredItem(u"1"_s, u"a99"_s, 99)}, false, true);
        QCOMPARE(sink.names(), (QStringList {u"app1"_s, u"recent"_s, u"a99"_s, u"f1"_s}));
        QVERIFY(!merge.takeBetterFirstRow());
        // And favorites.
        merge.begin(5, u"ab"_s);
        SearchResult favorite = fileRow(u"kept"_s);
        favorite.favorite = true;
        sink.assign({favorite, fileRow(u"f1"_s)});
        sink.current = 0;
        merge.namesShown();
        merge.results(5, u"test.a"_s, u"main"_s, {scoredItem(u"1"_s, u"a99"_s, 99)}, false, true);
        QCOMPARE(sink.names(), (QStringList {u"kept"_s, u"a99"_s, u"f1"_s}));
        QVERIFY(!merge.takeBetterFirstRow());
        // Files named just what was typed do not (there may be dozens): the
        // row goes before them, past the apps that match better.
        merge.begin(4, u"ab"_s);
        sink.assign({appRow(u"app1"_s, 10), fileRow(u"ab.txt"_s, false, true), fileRow(u"ab.md"_s, false, true)});
        sink.current = -1;
        merge.namesShown();
        merge.results(4, u"test.a"_s, u"main"_s, {scoredItem(u"1"_s, u"a5"_s, 5)}, false, true);
        QCOMPARE(sink.names(), (QStringList {u"app1"_s, u"a5"_s, u"ab.txt"_s, u"ab.md"_s}));
    }

    void mergeUpdatesInPlace()
    {
        Fixture f;
        installThree(f);
        ListSink sink;
        Merge merge(sink, f.host->catalog());
        merge.begin(1, u"ab"_s);
        sink.assign({appRow(u"app1"_s, 10), fileRow(u"f1"_s)});
        merge.namesShown();
        merge.results(1, u"test.a"_s, u"main"_s,
            {scoredItem(u"x"_s, u"ax"_s, 90, u"one"_s), scoredItem(u"y"_s, u"ay"_s, 80), scoredItem(u"z"_s, u"az"_s, 70)}, false, true);
        QCOMPARE(sink.names(), (QStringList {u"ax"_s, u"ay"_s, u"az"_s, u"app1"_s, u"f1"_s}));
        // The same ids again (a rerun): rewritten where they are, whatever
        // their new scores; the one gone goes; the new one goes in by score
        // against the rows as they are then (above the one now scoring 5).
        merge.results(1, u"test.a"_s, u"main"_s,
            {scoredItem(u"z"_s, u"az2"_s, 95), scoredItem(u"x"_s, u"ax2"_s, 5, u"two"_s), scoredItem(u"n"_s, u"an"_s, 50)}, false, true);
        QCOMPARE(sink.names(), (QStringList {u"an"_s, u"ax2"_s, u"az2"_s, u"app1"_s, u"f1"_s}));
        QCOMPARE(sink.rows[1].plugin->subtitle, u"two"_s);
        QCOMPARE(sink.rows[1].path, u"winshun-plugin:test.a/main/x"_s);
        QCOMPARE(merge.pluginRows(), 3);
    }

    void mergePartialAndOldQuery()
    {
        Fixture f;
        installThree(f);
        ListSink sink;
        Merge merge(sink, f.host->catalog());
        merge.begin(1, u"ab"_s);
        sink.assign({appRow(u"app1"_s, 10)});
        merge.namesShown();
        // Push mode: partial batches add up and go in with the final answer.
        merge.results(1, u"test.a"_s, u"main"_s, {scoredItem(u"1"_s, u"a1"_s, 60)}, true, false);
        QCOMPARE(sink.count(), 1);
        merge.results(1, u"test.a"_s, u"main"_s, {scoredItem(u"2"_s, u"a2"_s, 70)}, true, false);
        QCOMPARE(sink.count(), 1);
        merge.results(1, u"test.a"_s, u"main"_s, {scoredItem(u"3"_s, u"a3"_s, 65)}, false, false);
        QCOMPARE(sink.names(), (QStringList {u"a2"_s, u"a3"_s, u"a1"_s, u"app1"_s}));
        // An answer to an earlier query is dropped; one with the final batch
        // replacing the partial ones counts the final batch alone.
        merge.begin(2, u"abc"_s);
        sink.assign({appRow(u"app1"_s, 10)});
        merge.namesShown();
        merge.results(1, u"test.a"_s, u"main"_s, {scoredItem(u"9"_s, u"old"_s, 99)}, false, true);
        QCOMPARE(sink.count(), 1);
        merge.results(2, u"test.b"_s, u"main"_s, {scoredItem(u"1"_s, u"b1"_s, 60)}, true, false);
        merge.results(2, u"test.b"_s, u"main"_s, {scoredItem(u"2"_s, u"b2"_s, 70)}, false, true);
        QCOMPARE(sink.names(), (QStringList {u"b2"_s, u"app1"_s}));
    }

    void mergeKeywordMode()
    {
        Fixture f;
        installThree(f);
        ListSink sink;
        sink.assign({appRow(u"old"_s, 10), fileRow(u"older"_s)});
        Merge merge(sink, f.host->catalog());
        merge.begin(1, u"aa x"_s, u"test.a"_s, u"main"_s);
        QVERIFY(merge.keywordMode());
        QVERIFY(merge.namesReady()); // no name search to wait for
        // The first batch replaces the rows whatever it says; later partial
        // batches are appended; the final answer replaces the lot. Another
        // plugin's answer (a regex trigger) is not the list's.
        merge.results(1, u"test.a"_s, u"main"_s, {scoredItem(u"1"_s, u"p1"_s, 50)}, true, false);
        QCOMPARE(sink.names(), QStringList {u"p1"_s});
        merge.results(1, u"test.a"_s, u"main"_s, {scoredItem(u"2"_s, u"p2"_s, 50)}, true, false);
        QCOMPARE(sink.names(), (QStringList {u"p1"_s, u"p2"_s}));
        merge.results(1, u"test.b"_s, u"main"_s, {scoredItem(u"1"_s, u"b1"_s, 99)}, false, true);
        QCOMPARE(sink.names(), (QStringList {u"p1"_s, u"p2"_s}));
        Items final;
        for (int i = 0; i < 10; ++i)
            final.push_back(scoredItem(QString::number(i), u"f%1"_s.arg(i), 50)); // no quota in keyword mode
        merge.results(1, u"test.a"_s, u"main"_s, final, false, true);
        QCOMPARE(sink.count(), 10);
        QCOMPARE(sink.rows[0].name, u"f0"_s); // in the plugin's order (every score alike)
        QVERIFY(sink.rows[0].plugin->subtitle.isEmpty()); // no "查看全部" here
        // The order given, unless some item has a score.
        Items some {scoredItem(u"a"_s, u"low"_s, 10), scoredItem(u"b"_s, u"high"_s, 90)};
        some[0].hasScore = false;
        some[0].score = 50;
        merge.results(1, u"test.a"_s, u"main"_s, some, false, true);
        QCOMPARE(sink.names(), (QStringList {u"high"_s, u"low"_s}));
        // An empty answer empties the list.
        merge.results(1, u"test.a"_s, u"main"_s, {}, false, true);
        QCOMPARE(sink.count(), 0);
        // A replace effect: the items as the list.
        merge.replace(u"test.a"_s, u"main"_s, {scoredItem(u"r"_s, u"second level"_s, 50)});
        QCOMPARE(sink.names(), QStringList {u"second level"_s});
    }

    // ---- installing, removing, secrets (Install.h, Secrets.h) ------------------
    void installFromFolder()
    {
        Fixture f;
        const QString userDir = f.dir.path() + u"/plugins"_s;
        const QString source = f.dir.path() + u"/src/anything"_s;
        QVERIFY(writeFile(source + u"/plugin.json"_s, zeroManifest(u"test.inst"_s, u"1.0.0"_s)));
        QVERIFY(writeFile(source + u"/README.md"_s, "# hi"));
        QVERIFY(writeFile(source + u"/sub/data.txt"_s, "x"));
        std::optional<install::Candidate> c = install::inspect(source);
        QVERIFY(c.has_value());
        QVERIFY2(c->manifest.has_value(), qPrintable(c->error));
        QVERIFY(!c->archive);
        QCOMPARE(c->manifest->id, u"test.inst"_s);
        QString error;
        const QString placed = install::place(*c, userDir, &error);
        QVERIFY2(!placed.isEmpty(), qPrintable(error));
        QVERIFY(placed.endsWith(u"\\test.inst"_s));
        QVERIFY(QFileInfo::exists(userDir + u"/test.inst/plugin.json"_s));
        QVERIFY(QFileInfo::exists(userDir + u"/test.inst/sub/data.txt"_s));
        QVERIFY(QFileInfo::exists(source + u"/plugin.json"_s)); // copied, not moved
        f.host->catalog().scan();
        QVERIFY(f.host->catalog().find(u"test.inst"_s));
        // Not a plugin: the manifest's own words come back.
        QVERIFY(writeFile(f.dir.path() + u"/bad/plugin.json"_s, R"({"manifest":1,"id":"test.bad","name":"B","version":"1","commands":[{"id":"c","keyword":"a b","run":{"folder":"C:\\"}}]})"));
        c = install::inspect(f.dir.path() + u"/bad"_s);
        QVERIFY(c.has_value() && !c->manifest);
        QVERIFY2(c->error.contains(u"keyword 含空格"_s), qPrintable(c->error));
        QVERIFY(install::place(*c, userDir, &error).isEmpty());
        QVERIFY(writeFile(f.dir.path() + u"/empty/readme.txt"_s, "nothing"));
        c = install::inspect(f.dir.path() + u"/empty"_s);
        QVERIFY(c.has_value() && !c->manifest);
        QVERIFY2(c->error.contains(u"plugin.json"_s), qPrintable(c->error));
        QVERIFY(!install::inspect(f.dir.path() + u"/missing"_s).has_value());
        c = install::inspect(f.dir.path() + u"/empty/readme.txt"_s);
        QVERIFY(c.has_value() && !c->manifest); // a file that is no archive
    }

    void installFromZip()
    {
        Fixture f;
        const QString userDir = f.dir.path() + u"/plugins"_s;
        // The manifest at the root.
        const QString flat = f.dir.path() + u"/flat.wsplugin"_s;
        QVERIFY(writeZip(flat, {{u"plugin.json"_s, zeroManifest(u"test.zip"_s, u"1.0.0"_s)}, {u"icon.png"_s, "png"}, {u"lib/a.txt"_s, "a"}}));
        std::optional<install::Candidate> c = install::inspect(flat);
        QVERIFY(c.has_value());
        QVERIFY2(c->manifest.has_value(), qPrintable(c->error));
        QVERIFY(c->archive);
        QString error;
        QString placed = install::place(*c, userDir, &error);
        QVERIFY2(!placed.isEmpty(), qPrintable(error));
        install::discard(*c);
        QVERIFY(QFileInfo::exists(userDir + u"/test.zip/lib/a.txt"_s));
        QVERIFY(!QFileInfo::exists(c->dir)); // the temporary folder went
        // In one folder, as a zipped folder has it; a newer version replaces the old.
        const QString nested = f.dir.path() + u"/nested.zip"_s;
        QVERIFY(writeZip(nested, {{u"test.zip-2/plugin.json"_s, zeroManifest(u"test.zip"_s, u"1.1.0"_s)}, {u"test.zip-2/new.txt"_s, "n"}}));
        c = install::inspect(nested);
        QVERIFY(c.has_value());
        QVERIFY2(c->manifest.has_value(), qPrintable(c->error));
        QCOMPARE(compareVersions(c->manifest->version, u"1.0.0"_s) > 0, true);
        placed = install::place(*c, userDir, &error);
        QVERIFY2(!placed.isEmpty(), qPrintable(error));
        install::discard(*c);
        QVERIFY(QFileInfo::exists(userDir + u"/test.zip/new.txt"_s));
        QVERIFY(!QFileInfo::exists(userDir + u"/test.zip/lib/a.txt"_s)); // the old files went with the old version
        QVERIFY(!QFileInfo::exists(userDir + u"/test.zip.old"_s));
        f.host->catalog().scan();
        QCOMPARE(f.host->catalog().find(u"test.zip"_s)->manifest->version, u"1.1.0"_s);
        // Refused: a path leaving the folder, no manifest, not a zip.
        const QString slip = f.dir.path() + u"/slip.zip"_s;
        QVERIFY(writeZip(slip, {{u"plugin.json"_s, zeroManifest(u"test.slip"_s, u"1"_s)}, {u"../evil.txt"_s, "e"}}));
        c = install::inspect(slip);
        QVERIFY(c.has_value() && !c->manifest);
        QVERIFY2(c->error.contains(u"越出"_s), qPrintable(c->error));
        const QString none = f.dir.path() + u"/none.zip"_s;
        QVERIFY(writeZip(none, {{u"a/readme.txt"_s, "r"}}));
        c = install::inspect(none);
        QVERIFY(c.has_value() && !c->manifest);
        QVERIFY2(c->error.contains(u"plugin.json"_s), qPrintable(c->error));
        QVERIFY(writeFile(f.dir.path() + u"/junk.zip"_s, "not a zip"));
        c = install::inspect(f.dir.path() + u"/junk.zip"_s);
        QVERIFY(c.has_value() && !c->manifest);
        QVERIFY2(c->error.contains(u"zip"_s), qPrintable(c->error));
    }

    // ---- the plugin library (Gallery.h) ------------------------------------------
    void galleryListStatesAndPackage()
    {
        namespace gallery = ws::plugin::gallery;
        QTemporaryDir dir;
        const QString root = dir.path();
        // A package, and a list beside it naming it by a relative address.
        const QString package = root + u"/list/test.lib-1.2.0.wsplugin"_s;
        QVERIFY(QDir().mkpath(root + u"/list"_s));
        QVERIFY(writeZip(package, {{u"plugin.json"_s, zeroManifest(u"test.lib"_s, u"1.2.0"_s)}}));
        QFile file(package);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        file.close();
        const QString sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        const auto entry = [&](const QString& id, const QString& url, const QString& hash, qint64 size, const QString& minWinShun = {}) {
            return QJsonObject {{u"id"_s, id}, {u"name"_s, QJsonObject {{u"zh"_s, u"库"_s}, {u"en"_s, u"Lib"_s}}}, {u"version"_s, u"1.2.0"_s},
                {u"minWinShun"_s, minWinShun}, {u"keywords"_s, QJsonArray {u"lib"_s}},
                {u"permissions"_s, QJsonArray {u"network"_s, QJsonObject {{u"files:read"_s, QJsonArray {u"x"_s}}}}},
                {u"runtime"_s, QJsonObject {{u"mode"_s, u"once"_s}, {u"program"_s, u"python"_s}}}, {u"url"_s, url}, {u"sha256"_s, hash},
                {u"size"_s, static_cast<double>(size)}};
        };
        const QJsonArray entries {
            entry(u"test.lib"_s, u"test.lib-1.2.0.wsplugin"_s, sha.toUpper(), bytes.size()),
            entry(u"test.new"_s, package, sha, bytes.size(), u"9.0"_s),
            entry(u"Bad Id"_s, package, sha, bytes.size()), // left out: the id
            entry(u"test.nohash"_s, package, QString(), bytes.size()), // left out: no SHA-256
            entry(u"test.lib"_s, package, sha, bytes.size()), // left out: the same id again
        };
        const QByteArray json = QJsonDocument(QJsonObject {{u"schema"_s, 1}, {u"generated"_s, u"2026-10-23T00:00:00Z"_s}, {u"plugins"_s, entries}}).toJson();
        const QString listPath = root + u"/list/index.json"_s;
        QString error;
        const std::optional<gallery::Index> index = gallery::parse(json, listPath, &error);
        QVERIFY2(index.has_value(), qPrintable(error));
        QCOMPARE(index->items.size(), 2u);
        QCOMPARE(index->skipped, 3);
        const gallery::Item* lib = index->find(u"test.lib"_s);
        QVERIFY(lib);
        QCOMPARE(lib->name.pick(u"en"_s), u"Lib"_s);
        QCOMPARE(lib->sha256, sha); // lower-cased
        QCOMPARE(lib->url, QDir::toNativeSeparators(package)); // beside the list
        QCOMPARE(lib->permissions, (QStringList {u"network"_s, u"files:read"_s}));
        QVERIFY(lib->program == Program::Python);
        QVERIFY(!gallery::parse("[]", listPath, &error));
        QVERIFY(!gallery::parse(R"({"schema":2,"plugins":[]})", listPath, &error));
        // From the web: a relative address is taken from the list's; anything but https is left out.
        const QJsonArray webEntries {
            entry(u"test.web"_s, u"test.web-1.wsplugin"_s, sha, 1),
            entry(u"test.http"_s, u"http:"_s + u"//example.com/a.wsplugin"_s, sha, 1),
            entry(u"test.file"_s, package, sha, 1),
        };
        const std::optional<gallery::Index> web = gallery::parse(QJsonDocument(QJsonObject {{u"schema"_s, 1}, {u"plugins"_s, webEntries}}).toJson(),
            u"https:"_s + u"//example.com/dl/index.json"_s, &error);
        QVERIFY2(web.has_value(), qPrintable(error));
        QCOMPARE(web->items.size(), 1u);
        QCOMPARE(web->items[0].url, u"https:"_s + u"//example.com/dl/test.web-1.wsplugin"_s);
        QCOMPARE(web->skipped, 2);

        // What each is here.
        const QString userDir = root + u"/user"_s;
        const QString bundledDir = root + u"/bundled"_s;
        Catalog catalog(userDir, bundledDir);
        const QString app = u"0.12.0"_s;
        const auto stateNow = [&](const gallery::Item& item) {
            catalog.scan();
            return gallery::stateOf(item, gallery::localOf(catalog, bundledDir, item.id), app);
        };
        gallery::Item asks = *lib; // the same, asking for a newer Win顺
        asks.minWinShun = u"9.0"_s;
        QVERIFY(stateNow(*lib) == gallery::State::Install);
        QVERIFY(stateNow(asks) == gallery::State::NeedsNewerApp);
        QVERIFY(writeFile(bundledDir + u"/test.lib/plugin.json"_s, zeroManifest(u"test.lib"_s, u"1.0.0"_s)));
        QVERIFY(stateNow(*lib) == gallery::State::Bundled);
        QVERIFY(stateNow(asks) == gallery::State::Bundled); // updated with Win顺
        QVERIFY(writeFile(userDir + u"/test.lib/plugin.json"_s, zeroManifest(u"test.lib"_s, u"1.0.0"_s)));
        QVERIFY(stateNow(*lib) == gallery::State::Update);
        QVERIFY(stateNow(asks) == gallery::State::NeedsNewerApp);
        QVERIFY(writeFile(userDir + u"/test.lib/plugin.json"_s, zeroManifest(u"test.lib"_s, u"1.2.0"_s)));
        QVERIFY(stateNow(*lib) == gallery::State::Installed);
        QVERIFY(writeFile(userDir + u"/test.lib/plugin.json"_s, zeroManifest(u"test.lib"_s, u"1.3.0"_s)));
        QVERIFY(stateNow(asks) == gallery::State::Installed); // newer than the list's: nothing to do

        // The package: fetched from its file in pieces, checked, saved, looked at by the install.
        int reports = 0;
        gallery::Fetched f = gallery::fetch(lib->url, lib->size + 1, {}, [&](qint64) { return ++reports > 0; });
        QCOMPARE(f.status, 200);
        QCOMPARE(f.body, bytes);
        QVERIFY(reports >= 1);
        QVERIFY(gallery::check(*lib, f.body) == gallery::Problem::None);
        QByteArray changed = f.body;
        changed[changed.size() / 2] = static_cast<char>(changed[changed.size() / 2] ^ 1);
        QVERIFY(gallery::check(*lib, changed) == gallery::Problem::HashMismatch);
        QVERIFY(gallery::check(*lib, f.body + "x") == gallery::Problem::SizeMismatch);
        gallery::Item huge = *lib;
        huge.size = gallery::kMaxPackageBytes + 1;
        QVERIFY(gallery::check(huge, f.body) == gallery::Problem::TooBig);
        QCOMPARE(gallery::fetch(lib->url, 10, {}).body.size(), 10); // cut at maxBytes
        const gallery::Fetched stopped = gallery::fetch(lib->url, 0, {}, [](qint64) { return false; });
        QCOMPARE(stopped.status, 0);
        QCOMPARE(stopped.error, static_cast<unsigned long>(ERROR_CANCELLED));
        QCOMPARE(gallery::fetch(root + u"/missing.wsplugin"_s, 0, {}).error, static_cast<unsigned long>(ERROR_FILE_NOT_FOUND));
        QCOMPARE(gallery::fetch(web->items[0].url, 0, {}).status, 0); // a web address and no fetcher
        const QString saved = gallery::saveTemporary(*lib, f.body, &error);
        QVERIFY2(!saved.isEmpty(), qPrintable(error));
        QVERIFY(saved.endsWith(u".wsplugin"_s));
        std::optional<install::Candidate> candidate = install::inspect(saved);
        QVERIFY(candidate.has_value());
        QVERIFY2(candidate->manifest.has_value(), qPrintable(candidate->error));
        QCOMPARE(candidate->manifest->id, u"test.lib"_s);
        install::discard(*candidate);
        QVERIFY(QFile::remove(saved));
    }

    void uninstallRemovesEverything()
    {
        Fixture f;
        const QString userDir = f.dir.path() + u"/plugins"_s;
        const QString dataRoot = f.dir.path() + u"/data"_s;
        const SecretsCleanup cleanup {u"test.gone"_s};
        QVERIFY(writeFile(userDir + u"/test.gone/plugin.json"_s, zeroManifest(u"test.gone"_s, u"1"_s)));
        QVERIFY(writeFile(dataRoot + u"/test.gone.data/cache.bin"_s, "c"));
        QVERIFY(secrets::write(u"test.gone"_s, u"token"_s, u"s3cret"_s));
        QCOMPARE(secrets::keysOf(u"test.gone"_s), QStringList {u"token"_s});
        // The plugin alone: its data stays.
        QVERIFY2(install::remove(userDir, dataRoot, u"test.gone"_s, false).isEmpty(), "remove");
        QVERIFY(!QFileInfo::exists(userDir + u"/test.gone"_s));
        QVERIFY(QFileInfo::exists(dataRoot + u"/test.gone.data/cache.bin"_s));
        // With its data, and the secrets as the settings page removes them.
        QVERIFY(install::remove(userDir, dataRoot, u"test.gone"_s, true).isEmpty());
        QVERIFY(!QFileInfo::exists(dataRoot + u"/test.gone.data"_s));
        QCOMPARE(secrets::removeAll(u"test.gone"_s), 1);
        QVERIFY(secrets::keysOf(u"test.gone"_s).isEmpty());
        QVERIFY(!secrets::read(u"test.gone"_s, u"token"_s).has_value());
        QVERIFY(!install::remove(userDir, dataRoot, u"../x"_s, true).isEmpty()); // never outside the folder
    }

    void secretsStayOutOfTheIni()
    {
        const SecretsCleanup cleanup {u"test.secret"_s};
        QString error;
        const std::optional<Manifest> m = Manifest::parse(R"({"manifest":1,"id":"test.secret","name":"S","version":"1",
            "settings":[{"key":"apiKey","type":"password","title":"Key"},{"key":"showAll","type":"bool","title":"All","default":false},{"key":"limit","type":"number","title":"N"}],
            "commands":[{"id":"c","keyword":"sc","run":{"folder":"C:\\"}}]})", &error);
        QVERIFY2(m.has_value(), qPrintable(error));
        QList<std::pair<QString, QString>> ini {{u"apiKey"_s, u"leaked"_s}}; // a line a hand put there before: dropped
        // A password: into the Credential Manager, never into the INI lines.
        QVERIFY(secrets::store(u"test.secret"_s, *m, u"apiKey"_s, u"sk-123 密钥"_s, ini));
        QVERIFY(ini.isEmpty());
        QCOMPARE(secrets::read(u"test.secret"_s, u"apiKey"_s).value_or(QString()), u"sk-123 密钥"_s);
        QCOMPARE(secrets::keysOf(u"test.secret"_s), QStringList {u"apiKey"_s});
        // The others: INI text, typed by the manifest when the plugin reads them.
        QVERIFY(secrets::store(u"test.secret"_s, *m, u"showAll"_s, u"true"_s, ini));
        QVERIFY(secrets::store(u"test.secret"_s, *m, u"limit"_s, u"20"_s, ini));
        QVERIFY(!secrets::store(u"test.secret"_s, *m, u"limit"_s, u"20"_s, ini)); // unchanged
        QCOMPARE(ini.size(), 2);
        const QJsonObject settings = secrets::settingsOf(u"test.secret"_s, *m, ini);
        QCOMPARE(settings.value(u"apiKey"_s).toString(), u"sk-123 密钥"_s);
        QCOMPARE(settings.value(u"showAll"_s).toBool(), true);
        QCOMPARE(settings.value(u"limit"_s).toDouble(), 20.0);
        // The host tells the plugin, but never the log or the trace.
        Fixture f;
        Spec spec;
        spec.id = u"test.secret"_s;
        spec.keywords = {u"sc"_s};
        spec.args = {u"--chatty"_s};
        spec.settings = QJsonArray {QJsonObject {{u"key"_s, u"apiKey"_s}, {u"type"_s, u"password"_s}, {u"title"_s, u"Key"_s}}};
        QVERIFY(!f.install(spec).isEmpty());
        f.host->setDeveloperMode(true);
        QStringList traces;
        QObject::connect(f.host.get(), &Host::traced, [&](const QString&, const QString&, const QString& summary, const QString& detail, int) {
            traces.push_back(summary + u'\n' + detail);
        });
        f.host->setPluginSettings(u"test.secret"_s, settings);
        QVERIFY(f.host->query(u"sc x"_s, Scope::All, 1));
        QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
        QVERIFY(!traces.isEmpty());
        for (const QString& t : traces)
            QVERIFY2(!t.contains(u"sk-123"_s), qPrintable(t));
        QVERIFY(std::any_of(traces.cbegin(), traces.cend(), [](const QString& t) { return t.contains(u"initialize"_s) && t.contains(u"••••"_s); }));
        for (const QString& line : f.logs)
            QVERIFY2(!line.contains(u"sk-123"_s), qPrintable(line));
        // Removing an empty value removes the secret.
        QVERIFY(!secrets::store(u"test.secret"_s, *m, u"apiKey"_s, QString(), ini));
        QVERIFY(!secrets::read(u"test.secret"_s, u"apiKey"_s).has_value());
        QVERIFY(install::unconfirmed(*m, {}).isEmpty()); // asks for no permission
        f.host->shutdown();
    }

    void unconfirmedPermissions()
    {
        QString error;
        const std::optional<Manifest> m = Manifest::parse(R"({"manifest":1,"id":"a.b","name":"X","version":"1","permissions":["processes","network"],"commands":[{"id":"c","run":{"folder":"C:\\"}}]})", &error);
        QVERIFY2(m.has_value(), qPrintable(error));
        QCOMPARE(install::unconfirmed(*m, {}), (QStringList {u"processes"_s, u"network"_s}));
        QCOMPARE(install::unconfirmed(*m, {u"processes"_s}), QStringList {u"network"_s});
        QVERIFY(install::unconfirmed(*m, {u"network"_s, u"PROCESSES"_s}).isEmpty());
    }

    // A newer version asking for more permissions than the user said yes to
    // (Catalog::setConfirmed): still switched on, but not enabled() -- out of
    // the keywords, the result menu and the host -- until they say yes to the
    // new ones too. The bundled ones are trusted; the same or fewer change
    // nothing.
    void consentForNewPermissions()
    {
        using Confirmed = QHash<QString, QStringList>;
        const QString none = u"none"_s; // Confirmed= of a version asking for nothing (kConfirmedNone)
        QTemporaryDir dir;
        const QString userDir = dir.path() + u"/user"_s;
        const QString bundledDir = dir.path() + u"/bundled"_s;
        // A zero-code command "c" with a keyword and a `files` trigger.
        const auto manifestOf = [](const QString& id, const QString& version, const QString& keyword, const QStringList& permissions) {
            const QJsonObject command {{u"id"_s, u"c"_s}, {u"keyword"_s, keyword}, {u"run"_s, QJsonObject {{u"folder"_s, u"C:\\"_s}}},
                {u"triggers"_s, QJsonArray {QJsonObject {{u"files"_s, QJsonObject {{u"kind"_s, u"any"_s}}}}}}};
            QJsonObject m {{u"manifest"_s, 1}, {u"id"_s, id}, {u"name"_s, u"Ask"_s}, {u"version"_s, version}, {u"commands"_s, QJsonArray {command}}};
            if (!permissions.isEmpty())
                m.insert(u"permissions"_s, QJsonArray::fromStringList(permissions));
            return toJson(m);
        };
        QVERIFY(writeFile(userDir + u"/test.ask/plugin.json"_s, manifestOf(u"test.ask"_s, u"1.0.0"_s, u"ak"_s, {})));
        QVERIFY(writeFile(bundledDir + u"/test.came/plugin.json"_s, manifestOf(u"test.came"_s, u"1.0.0"_s, u"cm"_s, {u"processes"_s})));
        QVERIFY(writeFile(userDir + u"/test.free/plugin.json"_s, manifestOf(u"test.free"_s, u"1.0.0"_s, u"fr"_s, {u"network"_s})));
        Catalog catalog(userDir, bundledDir);
        catalog.scan();
        QCOMPARE(catalog.entries().size(), 3u);
        for (const plugin::Entry& e : catalog.entries())
            QVERIFY2(e.usable(), qPrintable(e.error));
        catalog.setEnabled(QSet<QString> {u"test.ask"_s, u"test.came"_s, u"test.free"_s});
        const auto fileIds = [&] {
            QStringList out;
            for (const Catalog::FileCommand& c : catalog.fileCommands({u"C:\\x.txt"_s}, {false}))
                out.push_back(c.entry->id);
            return out;
        };
        // Before the settings say anything (tests, wsbench, the preview): nothing waits.
        QVERIFY(catalog.enabled(u"test.free"_s));
        QCOMPARE(catalog.match(u"fr x"_s).size(), 1u);
        int changes = 0;
        QObject::connect(&catalog, &Catalog::changed, [&] { ++changes; });
        catalog.setConfirmed(Confirmed {{u"test.ask"_s, QStringList {none}}, {u"test.came"_s, QStringList {none}}});
        QCOMPARE(changes, 1);
        // Said yes to what it asks for: on as before, by its keyword and on the menu.
        QVERIFY(catalog.enabled(u"test.ask"_s));
        QVERIFY(catalog.awaitingConsent(u"test.ask"_s).isEmpty());
        QCOMPARE(catalog.match(u"ak x"_s).size(), 1u);
        // Bundled: it comes with Win顺 and is trusted as it comes.
        QVERIFY(catalog.enabled(u"test.came"_s));
        QVERIFY(catalog.awaitingConsent(u"test.came"_s).isEmpty());
        // Turned on by a hand, never said yes to: it waits.
        QVERIFY(catalog.switchedOn(u"test.free"_s));
        QVERIFY(!catalog.enabled(u"test.free"_s));
        QCOMPARE(catalog.awaitingConsent(u"test.free"_s), QStringList {u"network"_s});
        QVERIFY(catalog.match(u"fr x"_s).empty());
        QCOMPARE(fileIds(), (QStringList {u"test.ask"_s, u"test.came"_s}));
        // The same again: nothing changes.
        catalog.setConfirmed(Confirmed {{u"test.ask"_s, QStringList {none}}, {u"test.came"_s, QStringList {none}}});
        QCOMPARE(changes, 1);

        // A newer version asking for more: it waits, out of the index and the menu.
        QVERIFY(writeFile(userDir + u"/test.ask/plugin.json"_s, manifestOf(u"test.ask"_s, u"2.0.0"_s, u"ak"_s, {u"processes"_s, u"network"_s})));
        catalog.scan();
        QVERIFY(catalog.switchedOn(u"test.ask"_s));
        QVERIFY(!catalog.enabled(u"test.ask"_s));
        QCOMPARE(catalog.awaitingConsent(u"test.ask"_s), (QStringList {u"processes"_s, u"network"_s}));
        QVERIFY(catalog.match(u"ak x"_s).empty());
        QVERIFY(catalog.hints(u"ak"_s).empty());
        QVERIFY(catalog.keywordsOf(u"test.ask"_s, u"c"_s).isEmpty());
        QCOMPARE(fileIds(), QStringList {u"test.came"_s});
        QVERIFY(catalog.enabled(u"test.came"_s)); // the others go on
        // Yes to one of them: it waits for the other.
        QVERIFY(catalog.setConfirmed(Confirmed {{u"test.ask"_s, QStringList {u"processes"_s}}, {u"test.came"_s, QStringList {none}}}).isEmpty());
        QCOMPARE(catalog.awaitingConsent(u"test.ask"_s), QStringList {u"network"_s});
        QVERIFY(!catalog.enabled(u"test.ask"_s));
        // Yes to both (as a hand may write them): on again.
        const int before = changes;
        // It says who stopped waiting: the app clears such a plugin's crash state.
        QCOMPARE(catalog.setConfirmed(Confirmed {{u"test.ask"_s, QStringList {u"network"_s, u"PROCESSES"_s}}, {u"test.came"_s, QStringList {none}}}),
            QStringList {u"test.ask"_s});
        QCOMPARE(changes, before + 1);
        QVERIFY(catalog.enabled(u"test.ask"_s));
        QCOMPARE(catalog.match(u"ak x"_s).size(), 1u);
        QCOMPARE(fileIds(), (QStringList {u"test.ask"_s, u"test.came"_s}));
        // Fewer: nothing to ask.
        QVERIFY(writeFile(userDir + u"/test.ask/plugin.json"_s, manifestOf(u"test.ask"_s, u"3.0.0"_s, u"ak"_s, {u"network"_s})));
        catalog.scan();
        QVERIFY(catalog.enabled(u"test.ask"_s));
        QCOMPARE(catalog.match(u"ak x"_s).size(), 1u);
        // Off: nothing waits (turning it on asks first, on the settings page).
        catalog.setEnabled(u"test.free"_s, false);
        QVERIFY(catalog.awaitingConsent(u"test.free"_s).isEmpty());
        QVERIFY(!catalog.enabled(u"test.free"_s));
        // The user's own copy over a bundled one is theirs: it asks like any other.
        QVERIFY(writeFile(userDir + u"/test.came/plugin.json"_s, manifestOf(u"test.came"_s, u"1.0.1"_s, u"cm"_s, {u"processes"_s})));
        catalog.scan();
        QVERIFY(catalog.find(u"test.came"_s)->source == plugin::Entry::Source::User);
        QCOMPARE(catalog.awaitingConsent(u"test.came"_s), QStringList {u"processes"_s});
        QVERIFY(!catalog.enabled(u"test.came"_s));

        // Through the host: the resident process ends, and the plugin answers
        // no query, runs no action, no AI call, until the user says yes; then
        // it starts again.
        bool called = false; // before the host: a reply it kept would outlive them otherwise
        bool calledOk = true;
        QString calledError;
        Fixture f;
        Spec spec; // test.fake, fk, asking for nothing
        const QString folder = f.install(spec);
        QVERIFY(!folder.isEmpty());
        Catalog& hosted = f.host->catalog();
        hosted.setConfirmed(Confirmed {{spec.id, QStringList {none}}});
        QVERIFY(f.host->query(u"fk one"_s, Scope::All, 1));
        QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
        QCOMPARE(f.startedPids.size(), 1u);
        Spec newer = spec;
        newer.permissions = {u"notifications"_s};
        QVERIFY(writeFile(folder + u"/plugin.json"_s, manifestFor(newer)));
        hosted.scan();
        QCOMPARE(hosted.awaitingConsent(spec.id), QStringList {u"notifications"_s});
        QVERIFY(processGone(f.startedPids[0], 5000));
        QCOMPARE(f.host->state(spec.id).status, Host::State::Status::Off);
        QVERIFY(!f.host->query(u"fk two"_s, Scope::All, 2));
        f.host->callCommand(spec.id, u"main"_s, u"x"_s, 5, [&](bool ok, const Items&, const QString& error) {
            called = true;
            calledOk = ok;
            calledError = error;
        });
        QVERIFY(called); // at once
        QVERIFY(!calledOk);
        QVERIFY2(calledError.contains(u"权限"_s), qPrintable(calledError));
        f.host->action(7, spec.id, u"main"_s, QString(), QJsonObject {}, u"x"_s);
        QVERIFY(QTest::qWaitFor([&] { return !f.actions.empty(); }, 3000));
        QVERIFY(!f.actions[0].ok);
        QVERIFY2(f.actions[0].error.contains(u"权限"_s), qPrintable(f.actions[0].error));
        QCOMPARE(f.startedPids.size(), 1u); // nothing started it again
        QCOMPARE(hosted.setConfirmed(Confirmed {{spec.id, QStringList {u"notifications"_s}}}), QStringList {spec.id});
        QVERIFY(hosted.enabled(spec.id));
        QVERIFY(f.host->query(u"fk three"_s, Scope::All, 3));
        QVERIFY2(f.waitFinal(3), qPrintable(f.logs.join(u'\n')));
        QCOMPARE(f.finalFor(3)->items[0].title, u"serve: three"_s);
        QCOMPARE(f.startedPids.size(), 2u);
        f.host->shutdown();
    }

    // The plugins' tools stand on their own (plugin::Tools): with the AI
    // switch off -- no McpServer, no AI policy -- search_files answers.
    void toolsWithoutMcp()
    {
        Fixture f;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(f.dir.path() + u"/index"_s, options);
        mcp::Services services;
        services.index = &service;
        services.version = u"0.0-test"_s;
        Tools tools(std::move(services), nullptr, nullptr);
        mcp::Policy aiPolicy; // what the AI page would say: everything off
        aiPolicy.allowOpen = false;
        aiPolicy.allowClipboard = false;
        aiPolicy.excludedPaths = {u"C:\\hidden"_s};
        tools.setPolicy(aiPolicy);
        QCOMPARE(tools.host().known(u"search_files"_s), mcp::ToolHost::Known::Yes);
        QCOMPARE(tools.host().known(u"open_item"_s), mcp::ToolHost::Known::Off); // no shell here (wsbench's case), not the AI's switch; the app gives it one
        QVERIFY(tools.host().policy().excludedPaths.isEmpty()); // the plugins run as the user: nothing hidden
        f.host->setToolBridge(tools.bridge());
        Spec allowed;
        allowed.id = u"test.tools"_s;
        allowed.keywords = {u"tl"_s};
        allowed.args = {u"--tool"_s, u"search_files"_s};
        allowed.permissions = {u"winshun:search"_s};
        QVERIFY(!f.install(allowed).isEmpty());
        QVERIFY(f.host->query(u"tl x"_s, Scope::All, 1));
        QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
        QVERIFY2(f.finalFor(1)->items[0].subtitle.startsWith(u"tool ok "_s), qPrintable(f.finalFor(1)->items[0].subtitle));
        // Still by the manifest's permissions.
        Spec denied;
        denied.id = u"test.toolsno"_s;
        denied.keywords = {u"tn"_s};
        denied.args = {u"--tool"_s, u"search_files"_s};
        QVERIFY(!f.install(denied).isEmpty());
        QVERIFY(f.host->query(u"tn x"_s, Scope::All, 2));
        QVERIFY2(f.waitFinal(2), qPrintable(f.logs.join(u'\n')));
        QCOMPARE(f.finalFor(2)->items[0].subtitle, u"tool error %1"_s.arg(kPermissionDenied));
        f.host->shutdown();
    }

    void developerModeReload()
    {
        Fixture f;
        Spec spec;
        spec.id = u"test.dev"_s;
        spec.keywords = {u"dv"_s};
        QVERIFY(!f.install(spec).isEmpty());
        f.host->catalog().setWatching(true);
        f.host->setDeveloperMode(true);
        QStringList reloaded;
        QObject::connect(f.host.get(), &Host::reloaded, [&](const QString& id) { reloaded.push_back(id); });
        QVERIFY(f.host->query(u"dv x"_s, Scope::All, 1));
        QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
        const quint32 pid = f.host->state(u"test.dev"_s).pid;
        QVERIFY(pid != 0);
        // A script changes: the process is restarted, and the host says so.
        QVERIFY(writeFile(f.dir.path() + u"/plugins/test.dev/main.ps1"_s, "# edited"));
        QVERIFY(QTest::qWaitFor([&] { return !reloaded.isEmpty(); }, 5000));
        QCOMPARE(reloaded, QStringList {u"test.dev"_s});
        QVERIFY(QTest::qWaitFor([&] { return f.host->state(u"test.dev"_s).pid != 0 && f.host->state(u"test.dev"_s).pid != pid; }, 8000));
        QVERIFY(processGone(pid, 2000));
        QVERIFY(f.host->query(u"dv y"_s, Scope::All, 2));
        QVERIFY2(f.waitFinal(2), qPrintable(f.logs.join(u'\n')));
        // Off: a change is nobody's business.
        f.host->setDeveloperMode(false);
        QVERIFY(writeFile(f.dir.path() + u"/plugins/test.dev/main.ps1"_s, "# edited again"));
        QTest::qWait(800);
        QCOMPARE(reloaded.size(), 1);
        // reload(): the process comes back with the same id, the catalog reads again.
        const quint32 before = f.host->state(u"test.dev"_s).pid;
        f.host->reload(u"test.dev"_s);
        QVERIFY(QTest::qWaitFor([&] { return f.host->state(u"test.dev"_s).pid != 0 && f.host->state(u"test.dev"_s).pid != before; }, 8000));
        // stop(): gone until asked for.
        f.host->stop(u"test.dev"_s);
        QVERIFY(QTest::qWaitFor([&] { return f.host->state(u"test.dev"_s).status == Host::State::Status::Ready; }, 5000));
        QCOMPARE(f.host->state(u"test.dev"_s).pid, 0u);
        f.host->shutdown();
    }

    // ---- the protocol, through the runner ------------------------------------
    void protocolQuery()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--chatty"_s};
        QVERIFY(!f.install(spec).isEmpty());
        QVERIFY(f.host->query(u"fk 你好 world"_s, Scope::All, 1));
        QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
        const Result* r = f.finalFor(1);
        QCOMPARE(r->plugin, u"test.fake"_s);
        QCOMPARE(r->command, u"main"_s);
        QCOMPARE(r->items.size(), 1);
        QCOMPARE(r->items[0].title, u"serve: 你好 world"_s);
        QCOMPARE(r->items[0].subtitle, u"main/keyword"_s);
        QCOMPARE(r->items[0].score, 80);
        QVERIFY(r->replace);
        QCOMPARE(f.finished, std::vector<std::uint64_t> {1});
        QCOMPARE(f.notices, QStringList {u"hello from fake 你好"_s});
        QCOMPARE(f.progressTexts, QStringList {u"working 正在查"_s});
        QVERIFY(f.logs.join(u'\n').contains(u"[info] query 你好 world"_s));
        QCOMPARE(f.startedPids.size(), 1u);
        QCOMPARE(f.host->state(u"test.fake"_s).status, Host::State::Status::Running);
        QCOMPARE(f.host->state(u"test.fake"_s).pid, f.startedPids[0]);
        QVERIFY(f.host->runnerReady());
        QCOMPARE(f.host->tokenSource(), elevated() ? Runner::TokenSource::Explorer : Runner::TokenSource::Plain);
        // A second query reuses the process.
        QVERIFY(f.host->query(u"fk again"_s, Scope::All, 2));
        QVERIFY(f.waitFinal(2));
        QCOMPARE(f.finalFor(2)->items[0].title, u"serve: again"_s);
        QCOMPARE(f.startedPids.size(), 1u);
        // Settings reach it.
        f.host->setPluginSettings(u"test.fake"_s, {{u"showSystem"_s, true}});
        QVERIFY(f.host->query(u"fk s"_s, Scope::All, 3));
        QVERIFY(f.waitFinal(3));
        QCOMPARE(f.finalFor(3)->items[0].data.toObject().value(u"settings"_s).toObject().value(u"showSystem"_s).toBool(), true);
        // An action.
        f.host->action(7, u"test.fake"_s, u"main"_s, u"kill"_s, f.finalFor(3)->items[0], u"s"_s);
        QVERIFY(QTest::qWaitFor([&] { return !f.actions.empty(); }, 5000));
        QVERIFY(f.actions[0].ok);
        QCOMPARE(f.actions[0].effects.size(), 2);
        QCOMPARE(f.actions[0].effects[0].kind, Effect::Kind::Notify);
        QCOMPARE(f.actions[0].effects[0].text, u"did kill on serve: s"_s);
        QCOMPARE(f.actions[0].effects[1].kind, Effect::Kind::Hide);
        // Shutdown: the process and the runner go.
        const quint32 pid = f.startedPids[0];
        const quint32 runnerPid = static_cast<quint32>(f.host->runnerFacts().value(u"pid"_s).toDouble());
        QVERIFY(runnerPid != 0);
        f.host->shutdown();
        QVERIFY(processGone(pid, 3000));
        QVERIFY(processGone(runnerPid, 3000));
    }

    void protocolPartial()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--partial"_s};
        QVERIFY(!f.install(spec).isEmpty());
        QVERIFY(f.host->query(u"fk p"_s, Scope::All, 1));
        QVERIFY(f.waitFinal(1));
        QCOMPARE(f.results.size(), 3u);
        QVERIFY(f.results[0].partial);
        QVERIFY(!f.results[0].replace);
        QCOMPARE(f.results[0].items[0].title, u"partial 0: p"_s);
        QVERIFY(f.results[1].partial);
        QCOMPARE(f.results[1].items[0].title, u"partial 1: p"_s);
        QVERIFY(!f.results[2].partial);
        QCOMPARE(f.results[2].items[0].title, u"serve: p"_s);
    }

    void protocolError()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--error"_s};
        QVERIFY(!f.install(spec).isEmpty());
        QVERIFY(f.host->query(u"fk e"_s, Scope::All, 1));
        QVERIFY(f.waitFailure(1));
        QCOMPARE(f.failureOf(1), u"cannot do that 做不到"_s);
        QVERIFY(f.results.empty());
    }

    void protocolToolCall()
    {
        // Win顺's tools behind the bridge: a real ToolHost on an empty index.
        Fixture f;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(f.dir.path() + u"/index"_s, options);
        SearchEngine engine(&service);
        mcp::Services services;
        services.index = &service;
        services.engine = &engine;
        services.version = u"0.0-test"_s;
        mcp::ToolHost toolHost(std::move(services));
        toolHost.setPolicy({});
        f.host->setToolBridge(toolBridge(&toolHost));

        // No permission: refused with kPermissionDenied.
        Spec denied;
        denied.id = u"test.denied"_s;
        denied.keywords = {u"dn"_s};
        denied.args = {u"--tool"_s, u"search_files"_s};
        QVERIFY(!f.install(denied).isEmpty());
        QVERIFY(f.host->query(u"dn x"_s, Scope::All, 1));
        QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
        QCOMPARE(f.finalFor(1)->items[0].subtitle, u"tool error %1"_s.arg(kPermissionDenied));
        // With it: the tool answers.
        Spec allowed;
        allowed.id = u"test.allowed"_s;
        allowed.keywords = {u"al"_s};
        allowed.args = {u"--tool"_s, u"search_files"_s};
        allowed.permissions = {u"winshun:search"_s};
        QVERIFY(!f.install(allowed).isEmpty());
        QVERIFY(f.host->query(u"al x"_s, Scope::All, 2));
        QVERIFY2(f.waitFinal(2), qPrintable(f.logs.join(u'\n')));
        QVERIFY2(f.finalFor(2)->items[0].subtitle.startsWith(u"tool ok "_s), qPrintable(f.finalFor(2)->items[0].subtitle));
        QVERIFY(f.finalFor(2)->items[0].subtitle.contains(u"\"items\":[]"_s));
        // index_status needs no permission; a tool its switch keeps off is refused.
        Spec status;
        status.id = u"test.status"_s;
        status.keywords = {u"st"_s};
        status.args = {u"--tool"_s, u"index_status"_s};
        QVERIFY(!f.install(status).isEmpty());
        QVERIFY(f.host->query(u"st x"_s, Scope::All, 3));
        QVERIFY(f.waitFinal(3));
        QVERIFY2(f.finalFor(3)->items[0].subtitle.startsWith(u"tool ok "_s), qPrintable(f.finalFor(3)->items[0].subtitle));
        Spec off;
        off.id = u"test.off"_s;
        off.keywords = {u"of"_s};
        off.args = {u"--tool"_s, u"open_item"_s};
        off.permissions = {u"winshun:open"_s};
        QVERIFY(!f.install(off).isEmpty());
        QVERIFY(f.host->query(u"of x"_s, Scope::All, 4));
        QVERIFY(f.waitFinal(4));
        QCOMPARE(f.finalFor(4)->items[0].subtitle, u"tool error %1"_s.arg(jsonrpc::kUnavailable));
        // Without a bridge at all.
        f.host->setToolBridge({});
        QVERIFY(f.host->query(u"st y"_s, Scope::All, 5));
        QVERIFY(f.waitFinal(5));
        QCOMPARE(f.finalFor(5)->items[0].subtitle, u"tool error %1"_s.arg(jsonrpc::kUnavailable));
        f.host->shutdown();
    }

    // ---- rerun, toasts, files (GitHub #113) ----------------------------------

    void manifestListenAndNotifications()
    {
        const QByteArray json = R"({"manifest": 1, "id": "a.listen", "name": "X", "version": "1.0.0",
          "runtime": {"mode": "resident", "program": "x.exe"},
          "permissions": ["network", {"network:listen": ["TCP 53317-53320", "UDP 53317"]}, "notifications"],
          "commands": [{"id": "send", "keyword": "x", "title": "X",
            "triggers": [{"files": {"kind": "any", "max": 1000, "title": {"zh": "用局域网发送…", "en": "Send over LAN…"}}}]}]})";
        QString error;
        const std::optional<Manifest> m = Manifest::parse(json, &error);
        QVERIFY2(m.has_value(), qPrintable(error));
        QVERIFY(m->permissions.has(Permission::NetworkListen));
        QVERIFY(m->permissions.has(Permission::Notifications));
        QVERIFY(m->permissions.has(Permission::Network));
        QCOMPARE(m->permissions.listens, (QStringList {u"TCP 53317-53320"_s, u"UDP 53317"_s}));
        QVERIFY(m->permissions.names().contains(u"network:listen"_s));
        QVERIFY(m->permissions.names().contains(u"notifications"_s));
        const Trigger& t = m->commands.first().triggers.first();
        QCOMPARE(t.kind, Trigger::Kind::Files);
        QCOMPARE(t.fileKind, u"any"_s);
        QCOMPARE(t.maxFiles, 1000);
        QCOMPARE(t.title.pick(u"zh"_s), u"用局域网发送…"_s);
        QCOMPARE(t.title.pick(u"en"_s), u"Send over LAN…"_s);
        // The one that ships parses, as the user's Win顺 reads it. (__FILE__
        // is UTF-8 here; QFINDTESTDATA does not find this folder's Chinese name.)
        const QString shippedPath = QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(u"../plugins/winshun.localsend/plugin.json"_s);
        QVERIFY2(QFileInfo::exists(shippedPath), qPrintable(shippedPath));
        QFile file(shippedPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const std::optional<Manifest> shipped = Manifest::parse(file.readAll(), &error);
        QVERIFY2(shipped.has_value(), qPrintable(error));
        QCOMPARE(shipped->id, u"winshun.localsend"_s);
        QVERIFY(shipped->resident());
        QVERIFY(shipped->permissions.has(Permission::NetworkListen) && shipped->permissions.has(Permission::Notifications));
        QCOMPARE(shipped->permissions.listens, (QStringList {u"TCP 53317-53320"_s, u"UDP 53317"_s}));
        QCOMPARE(shipped->commands.first().keywords, (QStringList {u"传"_s, u"ls"_s}));
        QCOMPARE(shipped->commands.first().triggers.first().title.pick(u"zh"_s), u"用局域网发送…"_s);
    }

    // A resident plugin's `rerun`: the same query sent again (no sooner
    // than kRerunMin); a new query takes over, a cancel ends it.
    void rerunResident()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--rerun"_s, u"0.1"_s}; // below the floor: 0.5 s
        QVERIFY(!f.install(spec).isEmpty());
        QElapsedTimer clock;
        clock.start();
        std::vector<std::pair<std::uint64_t, qint64>> answers; // query id, when
        QObject::connect(f.host.get(), &Host::results, [&](std::uint64_t id, const QString&, const QString&, const Items&, bool partial, bool) {
            if (!partial)
                answers.emplace_back(id, clock.elapsed());
        });
        const auto answersFor = [&](std::uint64_t id) {
            return std::count_if(answers.begin(), answers.end(), [id](const auto& a) { return a.first == id; });
        };
        QVERIFY(f.host->query(u"fk r"_s, Scope::All, 1));
        QVERIFY2(QTest::qWaitFor([&] { return answers.size() >= 3; }, 6000), qPrintable(f.logs.join(u'\n')));
        for (std::size_t i = 1; i < 3; ++i) {
            QCOMPARE(answers[i].first, std::uint64_t(1)); // the same query, asked again
            const qint64 gap = answers[i].second - answers[i - 1].second;
            QVERIFY2(gap >= 450, qPrintable(QString::number(gap)));
        }
        QVERIFY(f.results.back().items[0].data.toObject().value(u"n"_s).toInt() >= 3); // asked, not an old answer again
        QVERIFY(f.host->query(u"fk s"_s, Scope::All, 2));
        QVERIFY(f.waitFinal(2));
        const auto before = answersFor(1);
        QVERIFY(QTest::qWaitFor([&] { return answersFor(2) >= 2; }, 4000));
        QCOMPARE(answersFor(1), before);
        f.host->cancel(2); // the launcher closed
        const std::size_t now = answers.size();
        QTest::qWait(1200);
        QCOMPARE(answers.size(), now);
    }

    // A rerun answered too late once: asked again as before, so a list that
    // changes by itself (传's progress) does not freeze until the next key.
    void rerunAfterATimeout()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--rerun"_s, u"0.5"_s, u"--late"_s, u"2"_s};
        spec.runtimeExtra = {{u"timeoutMs"_s, 300}};
        QVERIFY(!f.install(spec).isEmpty());
        int answers = 0;
        QObject::connect(f.host.get(), &Host::results, [&](std::uint64_t id, const QString&, const QString&, const Items&, bool partial, bool) {
            if (!partial && id == 1)
                ++answers;
        });
        QVERIFY(f.host->query(u"fk r"_s, Scope::All, 1));
        QVERIFY(f.waitFailure(1, 4000)); // the second asking timed out
        QVERIFY2(QTest::qWaitFor([&] { return answers >= 2; }, 4000), qPrintable(f.logs.join(u'\n')));
        f.host->cancel(1);
    }

    // A once plugin's `rerun`: run again for the query (a second at least).
    void rerunOnce()
    {
        Fixture f;
        Spec spec;
        spec.mode = u"once"_s;
        spec.role = u"--once"_s;
        spec.args = {u"--rerun"_s, u"0.2"_s}; // below a once plugin's floor: 1 s
        spec.runtimeExtra = {{u"debounceMs"_s, 0}};
        QVERIFY(!f.install(spec).isEmpty());
        QElapsedTimer clock;
        clock.start();
        std::vector<qint64> answers;
        QObject::connect(f.host.get(), &Host::results, [&](std::uint64_t id, const QString&, const QString&, const Items&, bool partial, bool) {
            if (id == 1 && !partial)
                answers.push_back(clock.elapsed());
        });
        QVERIFY(f.host->query(u"fk o"_s, Scope::All, 1));
        QVERIFY2(QTest::qWaitFor([&] { return answers.size() >= 2; }, 8000), qPrintable(f.logs.join(u'\n')));
        QVERIFY2(answers[1] - answers[0] >= 950, qPrintable(QString::number(answers[1] - answers[0])));
        f.host->cancel(1);
        const std::size_t now = answers.size();
        QTest::qWait(1600);
        QCOMPARE(answers.size(), now);
    }

    // winshun/refresh (GitHub #113: a request came while 传 is open): the
    // query shown is asked again now, not when its `rerun` (30 s) is up;
    // then the rerun goes on as it asked.
    void refreshAsksAgainAtOnce()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--rerun"_s, u"30"_s, u"--refresh"_s, u"200"_s};
        QVERIFY(!f.install(spec).isEmpty());
        QElapsedTimer clock;
        clock.start();
        std::vector<qint64> answers;
        QObject::connect(f.host.get(), &Host::results, [&](std::uint64_t id, const QString&, const QString&, const Items&, bool partial, bool) {
            if (id == 1 && !partial)
                answers.push_back(clock.elapsed());
        });
        QVERIFY(f.host->query(u"fk r"_s, Scope::All, 1));
        QVERIFY2(QTest::qWaitFor([&] { return answers.size() >= 2; }, 6000), qPrintable(f.logs.join(u'\n')));
        const qint64 gap = answers[1] - answers[0];
        QVERIFY2(gap >= 100 && gap < 3000, qPrintable(QString::number(gap))); // after the refresh (200 ms), long before 30 s
        QCOMPARE(f.results.back().items[0].data.toObject().value(u"n"_s).toInt(), 2); // asked, not an old answer again
        QTest::qWait(800);
        QCOMPARE(answers.size(), std::size_t(2)); // back to its own 30 s
        f.host->cancel(1);
    }

    // A refresh while the query is being answered (the answer may be from
    // before the change; the first query too): asked again right after it.
    void refreshWhileAnswering()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--rerun"_s, u"30"_s, u"--refresh"_s, u"0"_s, u"--refresh-first"_s};
        QVERIFY(!f.install(spec).isEmpty());
        QElapsedTimer clock;
        clock.start();
        std::vector<qint64> answers;
        QObject::connect(f.host.get(), &Host::results, [&](std::uint64_t id, const QString&, const QString&, const Items&, bool partial, bool) {
            if (id == 1 && !partial)
                answers.push_back(clock.elapsed());
        });
        QVERIFY(f.host->query(u"fk r"_s, Scope::All, 1));
        QVERIFY2(QTest::qWaitFor([&] { return answers.size() >= 2; }, 6000), qPrintable(f.logs.join(u'\n')));
        QVERIFY2(answers[1] - answers[0] < 3000, qPrintable(QString::number(answers[1] - answers[0])));
        QCOMPARE(f.results.back().items[0].data.toObject().value(u"n"_s).toInt(), 2);
        QTest::qWait(800);
        QCOMPARE(answers.size(), std::size_t(2));
        f.host->cancel(1);
    }

    // Many refreshes at once: one more query (two when they straddle it),
    // not one each.
    void refreshBurst()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--rerun"_s, u"30"_s, u"--refresh"_s, u"100"_s, u"--refresh-burst"_s, u"8"_s};
        QVERIFY(!f.install(spec).isEmpty());
        int answers = 0;
        QObject::connect(f.host.get(), &Host::results, [&](std::uint64_t id, const QString&, const QString&, const Items&, bool partial, bool) {
            if (id == 1 && !partial)
                ++answers;
        });
        QVERIFY(f.host->query(u"fk r"_s, Scope::All, 1));
        QVERIFY2(QTest::qWaitFor([&] { return answers >= 2; }, 6000), qPrintable(f.logs.join(u'\n')));
        QTest::qWait(1000);
        QVERIFY2(answers <= 3, qPrintable(QString::number(answers)));
        // Queries the plugin got, not answers kept: a query sent per refresh
        // would drop the answers before it (n would be about 9).
        const int asked = f.results.back().items[0].data.toObject().value(u"n"_s).toInt();
        QVERIFY2(asked >= 2 && asked <= 3, qPrintable(QString::number(asked)));
        f.host->cancel(1);
    }

    // Otherwise a refresh does nothing: the answer asked for no `rerun`,
    // or the launcher closed (cancelled) before it came.
    void refreshIgnored()
    {
        const auto finals = [](const Fixture& f) {
            return static_cast<int>(std::count_if(f.results.begin(), f.results.end(), [](const Result& r) { return r.queryId == 1 && !r.partial; }));
        };
        {
            Fixture f;
            Spec spec;
            spec.args = {u"--refresh"_s, u"100"_s};
            QVERIFY(!f.install(spec).isEmpty());
            QVERIFY(f.host->query(u"fk r"_s, Scope::All, 1));
            QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
            QTest::qWait(900);
            QCOMPARE(finals(f), 1);
        }
        {
            Fixture f;
            Spec spec;
            spec.args = {u"--rerun"_s, u"30"_s, u"--refresh"_s, u"300"_s};
            QVERIFY(!f.install(spec).isEmpty());
            QVERIFY(f.host->query(u"fk r"_s, Scope::All, 1));
            QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
            f.host->cancel(1); // before the refresh comes
            QTest::qWait(900);
            QCOMPARE(finals(f), 1);
        }
    }

    // Toasts (winshun/notify with "toast"): with the notifications
    // permission, at most one per kToastGapMs (the newest waiting wins),
    // the words capped, a click's effects parsed; nothing of them logged.
    void toasts()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--toast"_s};
        spec.permissions = {u"notifications"_s, u"clipboard:write"_s};
        QVERIFY(!f.install(spec).isEmpty());
        struct Toast {
            QString plugin, title, text;
            Effects click;
            qint64 at;
        };
        std::vector<Toast> toasts;
        QElapsedTimer clock;
        clock.start();
        QObject::connect(f.host.get(), &Host::toastRequested, [&](const QString& plugin, const QString& title, const QString& text, const Effects& click) {
            toasts.push_back({plugin, title, text, click, clock.elapsed()});
        });
        QVERIFY(f.host->query(u"fk a"_s, Scope::All, 1));
        QVERIFY2(QTest::qWaitFor([&] { return toasts.size() == 1; }, 6000), qPrintable(f.logs.join(u'\n')));
        QCOMPARE(toasts[0].plugin, u"test.fake"_s);
        QCOMPARE(toasts[0].title, u"插件「假插件」：t:a"_s); // its name first, marked as the user's (GitHub #113)
        QCOMPARE(toasts[0].text, u"body a"_s);
        // Of copy, two setQuery, a link, a folder and a notify: the copy (it
        // has clipboard:write), its own list and the folder.
        QCOMPARE(toasts[0].click.size(), 3);
        QCOMPARE(toasts[0].click[0].kind, Effect::Kind::Copy);
        QCOMPARE(toasts[0].click[0].text, u"a"_s);
        QCOMPARE(toasts[0].click[1].kind, Effect::Kind::SetQuery);
        QCOMPARE(toasts[0].click[1].text, u"fk a"_s);
        QCOMPARE(toasts[0].click[2].kind, Effect::Kind::Open);
        QCOMPARE(toasts[0].click[2].open.target, u"C:\\Windows"_s);
        QVERIFY(f.notices.isEmpty()); // shown as a toast, not in the launcher
        // Two more at once: the first waits out the gap, the second takes its place.
        QVERIFY(f.host->query(u"fk b"_s, Scope::All, 2));
        QVERIFY(f.waitFinal(2));
        QVERIFY(f.host->query(u"fk c"_s, Scope::All, 3));
        QVERIFY(f.waitFinal(3));
        QVERIFY(QTest::qWaitFor([&] { return toasts.size() == 2; }, 5000));
        QCOMPARE(toasts[1].title, u"插件「假插件」：t:c"_s);
        const qint64 gap = toasts[1].at - toasts[0].at;
        QVERIFY2(gap >= Host::kToastGapMs - 100, qPrintable(QString::number(gap)));
        QTest::qWait(600);
        QCOMPARE(toasts.size(), std::size_t(2)); // b never came
        // Long words are cut (63 with the name, and 200, with the "…"), never
        // inside a pair of surrogates.
        const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
        QVERIFY(f.host->query(u"fk "_s + QString(51, u'x') + emoji + QString(300, u'y'), Scope::All, 4));
        QVERIFY(QTest::qWaitFor([&] { return toasts.size() == 3; }, 5000));
        QCOMPARE(toasts[2].title, u"插件「假插件」：t:"_s + QString(51, u'x') + u'…'); // 62 kept would have cut the pair
        QCOMPARE(toasts[2].text.size(), 200);
        QVERIFY(toasts[2].text.endsWith(u'…'));
        QVERIFY(!f.logs.join(u'\n').contains(u"body"_s)); // the words stay out of the log
    }

    // Without clipboard:write a toast's click copies nothing; its own list
    // and the local folder stay.
    void toastClicksWithoutClipboard()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--toast"_s};
        spec.permissions = {u"notifications"_s};
        QVERIFY(!f.install(spec).isEmpty());
        Effects click;
        int toasts = 0;
        QObject::connect(f.host.get(), &Host::toastRequested, [&](const QString&, const QString&, const QString&, const Effects& c) {
            click = c;
            ++toasts;
        });
        QVERIFY(f.host->query(u"fk a"_s, Scope::All, 1));
        QVERIFY2(QTest::qWaitFor([&] { return toasts == 1; }, 6000), qPrintable(f.logs.join(u'\n')));
        QCOMPARE(click.size(), 2);
        QCOMPARE(click[0].kind, Effect::Kind::SetQuery);
        QCOMPARE(click[1].kind, Effect::Kind::Open);
        QCOMPARE(f.logs.filter(u"系统通知的点击只留下"_s).size(), 1);
    }

    // ToastPolicy: the name first and no direction tricks in the words; a
    // click keeps only its own list, a plain local path and a copy it may
    // make, one of each.
    void toastPolicy()
    {
        const auto ch = [](char32_t c) { return QString(QStringView(QChar::fromUcs4(c))); };
        constexpr ToastFrom bundled = ToastFrom::Bundled;
        constexpr ToastFrom user = ToastFrom::User;
        QCOMPARE(toastTitle(bundled, u"winshun.localsend"_s, u"局域网传输"_s, u"收到了 3 个文件"_s, u"zh"_s), u"局域网传输：收到了 3 个文件"_s);
        QCOMPARE(toastTitle(bundled, u"winshun.localsend"_s, u"LAN transfer"_s, u"Got 3 files"_s, u"en"_s), u"LAN transfer: Got 3 files"_s);
        QCOMPARE(toastTitle(bundled, u"winshun.localsend"_s, u"局域网传输"_s, u"  "_s, u"zh"_s), u"局域网传输"_s);
        // One the user put in is marked, whatever it calls itself.
        QCOMPARE(toastTitle(user, u"test.fake"_s, u"Win顺"_s, u"已在后台运行"_s, u"zh"_s), u"插件「Win顺」：已在后台运行"_s);
        QCOMPARE(toastTitle(user, u"test.fake"_s, u"Win"_s + ch(0x0456) + u"n顺"_s, u"t"_s, u"en"_s), u"Plugin “Win"_s + ch(0x0456) + u"n顺”: t"_s);
        // A right-to-left override would show "Win顺" backwards-looking words
        // as something else; control characters and line breaks go too.
        QCOMPARE(toastTitle(user, u"test.fake"_s, u"假"_s + ch(0x202E) + u"插件"_s, u"Win顺"_s + ch(0x2066) + u" 更新\n可用"_s + ch(0x200B), u"zh"_s),
            u"插件「假插件」：Win顺 更新 可用"_s);
        QCOMPARE(toastWords(u"一\n二"_s + ch(0x202E) + u"三"_s, false), u"一\n二三"_s);
        // The joiners of an emoji stay; tags, fillers, variation selectors and soft hyphens go.
        const QString family = ch(0x1F468) + ch(0x200D) + ch(0x1F469);
        QCOMPARE(toastWords(family), family);
        QCOMPARE(toastWords(u"假"_s + ch(0xE0041) + ch(0x3164) + ch(0xFE0F) + ch(0x00AD) + u"插件"_s), u"假插件"_s);
        // The name is cut first (24 with the mark), so the plugin's words still show.
        QCOMPARE(toastTitle(bundled, u"p.long"_s, QString(40, u'n'), u"t"_s, u"en"_s), QString(23, u'n') + u"…: t"_s);
        QCOMPARE(toastName(user, u"p.long"_s, QString(40, u'n'), u"en"_s), u"Plugin “"_s + QString(14, u'n') + u"…”"_s);
        QCOMPARE(toastName(user, u"p.long"_s, QString(40, u'n'), u"zh"_s), u"插件「"_s + QString(19, u'n') + u"…」"_s);
        // Cut to 63 (a tray title's room) with the "…", a pair of surrogates kept whole.
        const QString emoji = ch(0x1F600);
        QCOMPARE(toastTitle(bundled, u"test.fake"_s, u"假插件"_s, QString(57, u'x') + emoji + u"tail"_s, u"zh"_s),
            u"假插件："_s + QString(57, u'x') + u'…');
        QCOMPARE(toastTitle(user, u"test.fake"_s, u"假插件"_s, QString(100, u'x'), u"zh"_s).size(), kToastTitleMost);

        // A name that shows nothing: its id, marked.
        const QStringList blanks {QString(), u" "_s, ch(0x200B), ch(0x202E), QString(5, QChar(0x3164)), ch(0x115F) + ch(0x1160),
            ch(0x2800), ch(0x3000) + ch(0x3000), ch(0x0301) + ch(0x0301), ch(0xFE0F)};
        for (const QString& blank : blanks) {
            QCOMPARE(toastName(user, u"test.fake"_s, blank, u"zh"_s), u"插件「test.fake」"_s);
            QCOMPARE(toastName(bundled, u"test.fake"_s, blank, u"zh"_s), u"插件「test.fake」"_s);
            QCOMPARE(toastTitle(user, u"test.fake"_s, blank, u"t"_s, u"en"_s), u"Plugin “test.fake”: t"_s);
        }
        // Fillers before a real name go and the name shows; marks that draw
        // nothing fill the room kept, and what is kept shows nothing.
        QCOMPARE(toastName(user, u"test.fake"_s, QString(23, QChar(0x3164)) + u"假插件"_s, u"zh"_s), u"插件「假插件」"_s);
        QCOMPARE(toastName(user, u"test.fake"_s, QString(30, QChar(0x0301)) + u"假插件"_s, u"zh"_s), u"插件「test.fake」"_s);
        QCOMPARE(toastName(user, QString(40, u'p'), QString(), u"en"_s), u"Plugin “"_s + QString(14, u'p') + u"…”"_s); // the id cut too

        QVERIFY(plainLocalPath(u"C:\\Users\\a\\Downloads"));
        QVERIFY(plainLocalPath(u"d:/相册"));
        QVERIFY(!plainLocalPath(u"\\\\server\\share"));
        QVERIFY(!plainLocalPath(u"\\\\?\\C:\\x"));
        QVERIFY(!plainLocalPath(u"https://example.com/"));
        QVERIFY(!plainLocalPath(u"file:///C:/x"));
        QVERIFY(!plainLocalPath(u"shell:startup"));
        QVERIFY(!plainLocalPath(u"notepad.exe"));
        QVERIFY(!plainLocalPath(u"C:\\a.txt:payload.exe"));
        QVERIFY(!plainLocalPath(u"C:"));
        QVERIFY(!plainLocalPath(u"c:relative"));

        const auto effect = [](Effect::Kind kind, const QString& text = {}) {
            Effect e;
            e.kind = kind;
            e.text = text;
            return e;
        };
        const auto open = [](Open::Kind kind, const QString& target, const QString& args = {}) {
            Effect e;
            e.kind = Effect::Kind::Open;
            e.open.kind = kind;
            e.open.target = target;
            e.open.args = args;
            return e;
        };
        const Effects asked {effect(Effect::Kind::SetQuery, u"传 "_s), effect(Effect::Kind::SetQuery, u"别的 "_s),
            effect(Effect::Kind::SetQuery, u" "_s), effect(Effect::Kind::Copy, u"x"_s), effect(Effect::Kind::Notify, u"n"_s),
            effect(Effect::Kind::Hide), open(Open::Kind::Url, u"https://example.com/"_s), open(Open::Kind::Path, u"C:\\x\\y"_s),
            open(Open::Kind::Path, u"C:\\Windows\\notepad.exe"_s, u"/a"_s), open(Open::Kind::Path, u"\\\\nas\\d"_s),
            // Second ones of each: dropped, a click does one of each at most.
            effect(Effect::Kind::SetQuery, u"传 b"_s), open(Open::Kind::Path, u"D:\\z"_s), effect(Effect::Kind::Copy, u"y"_s)};
        const auto own = [](const QString& q) { return q.startsWith(u"传 "_s); };
        Effects kept = toastClick(asked, own, false);
        QCOMPARE(kept.size(), 2);
        QCOMPARE(kept[0].text, u"传 "_s);
        QCOMPARE(kept[1].open.target, u"C:\\x\\y"_s);
        kept = toastClick(asked, own, true);
        QCOMPARE(kept.size(), 3);
        QCOMPARE(kept[1].kind, Effect::Kind::Copy);
        QCOMPARE(kept[1].text, u"x"_s);
        QVERIFY(toastClick(asked, {}, false).size() == 1); // no keyword check: no setQuery
    }

    void toastsNeedThePermission()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--toast"_s};
        QVERIFY(!f.install(spec).isEmpty());
        int toasts = 0;
        QObject::connect(f.host.get(), &Host::toastRequested, [&] { ++toasts; });
        QVERIFY(f.host->query(u"fk a"_s, Scope::All, 1));
        QVERIFY(f.waitFinal(1));
        QVERIFY(f.host->query(u"fk b"_s, Scope::All, 2));
        QVERIFY(f.waitFinal(2));
        QVERIFY(QTest::qWaitFor([&] { return f.notices.size() == 2; }, 3000));
        QCOMPARE(f.notices, (QStringList {u"body a"_s, u"body b"_s})); // in the launcher instead
        QCOMPARE(toasts, 0);
        QCOMPARE(f.logs.filter(u"notifications"_s).size(), 1); // said once
    }

    // A `files` trigger: on the result menu for the files it takes
    // (Catalog::fileCommands), and the action gets them (resident: trigger
    // "files" and the list; once: WS_TRIGGER and WS_FILES).
    void filesTrigger()
    {
        Fixture f;
        Spec spec;
        spec.commandExtra = {{u"triggers"_s, QJsonArray {QJsonObject {{u"files"_s,
            QJsonObject {{u"kind"_s, u"any"_s}, {u"max"_s, 3}, {u"title"_s, QJsonObject {{u"zh"_s, u"用假插件发送…"_s}, {u"en"_s, u"Send with Fake…"_s}}}}}}}}};
        QVERIFY(!f.install(spec).isEmpty());
        Spec photos;
        photos.id = u"test.photos"_s;
        photos.keywords = {u"ph"_s};
        photos.commandExtra = {{u"triggers"_s, QJsonArray {QJsonObject {{u"files"_s,
            QJsonObject {{u"kind"_s, u"file"_s}, {u"extensions"_s, QJsonArray {u".JPG"_s, u"png"_s}}}}}}}};
        QVERIFY(!f.install(photos).isEmpty());
        const auto commands = [&](const QStringList& paths, const QList<bool>& folders) {
            QStringList out;
            for (const Catalog::FileCommand& c : f.host->catalog().fileCommands(paths, folders))
                out.push_back(c.entry->id + u'/' + c.command->id + u'/' + c.trigger->title.pick(u"zh"_s));
            return out;
        };
        const QString fake = u"test.fake/main/用假插件发送…"_s;
        const QString photo = u"test.photos/main/"_s; // no title: the menu uses the command's
        QCOMPARE(commands({u"C:\\a.jpg"_s}, {false}), (QStringList {fake, photo}));
        QCOMPARE(commands({u"C:\\a.jpg"_s, u"C:\\b.txt"_s}, {false, false}), QStringList {fake}); // a .txt is no photo
        QCOMPARE(commands({u"C:\\a.jpg"_s, u"C:\\相册"_s}, {false, true}), QStringList {fake}); // files only
        QCOMPARE(commands({u"C:\\1.png"_s, u"C:\\2.png"_s, u"C:\\3.png"_s, u"C:\\4.png"_s}, {false, false, false, false}),
            QStringList {photo}); // more than 3
        QVERIFY(commands({}, {}).isEmpty());
        QVERIFY(commands({u"C:\\a.jpg"_s}, {false, false}).isEmpty()); // sizes disagree
        f.host->setEnabled(u"test.photos"_s, false);
        QCOMPARE(commands({u"C:\\a.jpg"_s}, {false}), QStringList {fake});
        // A menu's choice is looked up at the click by who it was, never by
        // its place (GitHub #113): with fake off and photos on, the menu's
        // first entry (fake's) would have sent the files to photos.
        const auto now = f.host->catalog().fileCommands({u"C:\\a.jpg"_s}, {false});
        QVERIFY(Catalog::hasFileCommand(now, u"test.fake", u"main"));
        QVERIFY(!Catalog::hasFileCommand(now, u"test.photos", u"main"));
        QVERIFY(!Catalog::hasFileCommand(now, u"test.fake", u"other"));
        f.host->setEnabled(u"test.photos"_s, true);
        f.host->setEnabled(u"test.fake"_s, false);
        const auto swapped = f.host->catalog().fileCommands({u"C:\\a.jpg"_s}, {false});
        QCOMPARE(swapped.size(), std::size_t(1));
        QCOMPARE(swapped[0].entry->id, u"test.photos"_s);
        QVERIFY(!Catalog::hasFileCommand(swapped, u"test.fake", u"main"));
        f.host->setEnabled(u"test.fake"_s, true);
        f.host->setEnabled(u"test.photos"_s, false);
        QCOMPARE(commands({u"C:\\a.jpg"_s}, {false}), QStringList {fake});

        f.host->action(5, u"test.fake"_s, u"main"_s, u"files"_s, QJsonObject {}, QString(), {u"C:\\a.jpg"_s, u"D:\\相册 1"_s});
        QVERIFY(QTest::qWaitFor([&] { return !f.actions.empty(); }, 8000));
        QVERIFY2(f.actions[0].ok, qPrintable(f.actions[0].error));
        QCOMPARE(f.actions[0].requestId, std::uint64_t(5));
        QCOMPARE(f.actions[0].effects[0].text, u"files files: C:\\a.jpg D:\\相册 1"_s);
        // Off: nothing runs, the menu said so.
        f.host->action(6, u"test.photos"_s, u"main"_s, u"files"_s, QJsonObject {}, QString(), {u"C:\\a.jpg"_s});
        QVERIFY(QTest::qWaitFor([&] { return f.actions.size() == 2; }, 3000));
        QVERIFY(!f.actions[1].ok);
        f.host->shutdown();
    }

    void filesTriggerOnce()
    {
        Fixture f;
        Spec spec;
        spec.mode = u"once"_s;
        spec.role = u"--once"_s;
        spec.args = {u"--action"_s};
        spec.commandExtra = {{u"triggers"_s, QJsonArray {QJsonObject {{u"files"_s, QJsonObject {{u"kind"_s, u"any"_s}}}}}}};
        QVERIFY(!f.install(spec).isEmpty());
        const QStringList files {u"C:\\a b.txt"_s, u"C:\\中文\\c.txt"_s};
        f.host->action(6, u"test.fake"_s, u"main"_s, u"files"_s, QJsonObject {}, QString(), files);
        QVERIFY2(QTest::qWaitFor([&] { return !f.actions.empty(); }, 8000), qPrintable(f.logs.join(u'\n')));
        QVERIFY2(f.actions[0].ok, qPrintable(f.actions[0].error));
        const QString said = f.actions[0].effects[0].text;
        QVERIFY2(said.startsWith(u"files files: "_s), qPrintable(said));
        QCOMPARE(QJsonDocument::fromJson(said.mid(13).toUtf8()).array(), QJsonArray::fromStringList(files));
        // WS_FILES is one environment variable: too many files are refused up front.
        QStringList many;
        for (int i = 0; i < 400; ++i)
            many.push_back(u"C:\\"_s + QString(100, u'f') + QString::number(i));
        f.host->action(7, u"test.fake"_s, u"main"_s, u"files"_s, QJsonObject {}, QString(), many);
        QVERIFY(QTest::qWaitFor([&] { return f.actions.size() == 2; }, 3000));
        QVERIFY(!f.actions[1].ok);
        QCOMPARE(f.actions[1].error, u"选的文件太多了，少选一些再试"_s);
    }

    // ---- cancelling and timing out ---------------------------------------------
    void cancelDropsTheOldQuery()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--slow"_s, u"150"_s};
        QVERIFY(!f.install(spec).isEmpty());
        QVERIFY(f.host->query(u"fk first"_s, Scope::All, 1));
        // Once initialized the first query is in flight (the plugin sleeps on it).
        QVERIFY(QTest::qWaitFor([&] { return f.host->state(u"test.fake"_s).status == Host::State::Status::Running; }, 5000));
        QTest::qWait(10);
        QVERIFY(f.host->query(u"fk second"_s, Scope::All, 2));
        QVERIFY(f.waitFinal(2));
        QVERIFY(!f.finalFor(1));
        QCOMPARE(f.finalFor(2)->items[0].title, u"serve: second"_s);
        // The plugin heard the cancel (it counts them).
        QVERIFY(f.host->query(u"fk third"_s, Scope::All, 3));
        QVERIFY(f.waitFinal(3));
        QCOMPARE(f.finalFor(3)->items[0].data.toObject().value(u"cancelled"_s).toInt(), 1);
        QVERIFY(!f.finalFor(1));
        QCOMPARE(f.finished, (std::vector<std::uint64_t> {2, 3}));
    }

    void timeoutsRestartAHungPlugin()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--hang"_s};
        spec.runtimeExtra = {{u"timeoutMs"_s, 200}};
        QVERIFY(!f.install(spec).isEmpty());
        for (std::uint64_t id = 1; id <= 3; ++id) {
            QVERIFY(f.host->query(u"fk %1"_s.arg(id), Scope::All, id));
            QVERIFY(f.waitFailure(id, 3000));
            QCOMPARE(f.failureOf(id), u"没有响应"_s);
        }
        // The third timeout ends it; the next query brings it back (after
        // the backoff) in a new process.
        QVERIFY(QTest::qWaitFor([&] { return !f.exitCodes.empty(); }, 5000));
        QCOMPARE(f.startedPids.size(), 1u);
        QVERIFY(f.host->query(u"fk 4"_s, Scope::All, 4));
        QVERIFY(QTest::qWaitFor([&] { return f.startedPids.size() == 2; }, 5000));
        QVERIFY(f.startedPids[1] != f.startedPids[0]);
        QVERIFY(processGone(f.startedPids[0], 2000));
        QCOMPARE(f.restarts, std::vector<int> {1000});
    }

    void onceNewKeyKillsTheOldProcess()
    {
        Fixture f;
        Spec spec;
        spec.mode = u"once"_s;
        spec.role = u"--once"_s;
        spec.args = {u"--slow"_s, u"3000"_s};
        spec.runtimeExtra = {{u"debounceMs"_s, 0}, {u"timeoutMs"_s, 5000}};
        QVERIFY(!f.install(spec).isEmpty());
        QVERIFY(f.host->query(u"fk one"_s, Scope::All, 1));
        QVERIFY(QTest::qWaitFor([&] { return f.startedPids.size() == 1; }, 5000));
        const quint32 first = f.startedPids[0];
        QVERIFY(f.host->query(u"fk two"_s, Scope::All, 2));
        QVERIFY2(processGone(first, 200), "the old process must be gone within 200 ms");
        QVERIFY(f.waitFinal(2, 10000));
        QVERIFY(!f.finalFor(1));
        QCOMPARE(f.finalFor(2)->items[0].title, u"once: two"_s);
        QCOMPARE(f.finalFor(2)->items[0].subtitle, u"main/keyword"_s);
        QCOMPARE(f.startedPids.size(), 2u);
    }

    void onceDebounce()
    {
        Fixture f;
        Spec spec;
        spec.mode = u"once"_s;
        spec.role = u"--once"_s;
        spec.runtimeExtra = {{u"debounceMs"_s, 100}};
        QVERIFY(!f.install(spec).isEmpty());
        for (std::uint64_t id = 1; id <= 3; ++id) {
            QVERIFY(f.host->query(u"fk k%1"_s.arg(id), Scope::All, id));
            QTest::qWait(20);
        }
        QVERIFY(f.waitFinal(3));
        QCOMPARE(f.startedPids.size(), 1u); // one process for three keys
        QCOMPARE(f.results.size(), 1u);
        QCOMPARE(f.finalFor(3)->items[0].title, u"once: k3"_s);
        QVERIFY(f.logs.join(u'\n').contains(u"stderr: once plugin answered 已回答"_s));
        // Many items are cut at 200; a failing exit is a failure with its stderr.
        Spec many = spec;
        many.id = u"test.many"_s;
        many.keywords = {u"mn"_s};
        many.args = {u"--n"_s, u"300"_s};
        QVERIFY(!f.install(many).isEmpty());
        QVERIFY(f.host->query(u"mn x"_s, Scope::All, 4));
        QVERIFY(f.waitFinal(4));
        QCOMPARE(f.finalFor(4)->items.size(), kMaxItems);
        Spec failing = spec;
        failing.id = u"test.failing"_s;
        failing.keywords = {u"fl"_s};
        failing.args = {u"--fail"_s};
        QVERIFY(!f.install(failing).isEmpty());
        QVERIFY(f.host->query(u"fl x"_s, Scope::All, 5));
        QVERIFY(f.waitFailure(5));
        QVERIFY2(f.failureOf(5).contains(u"退出码 3"_s) && f.failureOf(5).contains(u"出错了"_s), qPrintable(f.failureOf(5)));
        // Plain text output.
        Spec text = spec;
        text.id = u"test.text"_s;
        text.keywords = {u"tx"_s};
        text.args = {u"--text"_s};
        QVERIFY(!f.install(text).isEmpty());
        QVERIFY(f.host->query(u"tx 中"_s, Scope::All, 6));
        QVERIFY(f.waitFinal(6));
        QCOMPARE(f.finalFor(6)->items.size(), 2);
        QCOMPARE(f.finalFor(6)->items[1].title, u"第二行 中"_s);
        QCOMPARE(f.finalFor(6)->items[1].copy, u"第二行 中"_s);
        // An action of a once plugin starts a process too.
        Spec acting = spec;
        acting.id = u"test.acting"_s;
        acting.keywords = {u"ac"_s};
        acting.args = {u"--action"_s};
        QVERIFY(!f.install(acting).isEmpty());
        Item item;
        item.title = u"目标"_s;
        f.host->action(9, u"test.acting"_s, u"main"_s, u"open"_s, item, u"x"_s);
        QVERIFY(QTest::qWaitFor([&] { return !f.actions.empty(); }, 5000));
        QVERIFY2(f.actions[0].ok, qPrintable(f.actions[0].error));
        QCOMPARE(f.actions[0].effects[0].text, u"did open on 目标"_s);
    }

    void residentQueryFilters()
    {
        Fixture f;
        QVERIFY(!f.install(Spec {}).isEmpty());
        const QJsonObject filters {{u"modifiedFrom"_s, u"2026-01-01T00:00:00Z"_s}, {u"minSize"_s, 4096},
            {u"extensions"_s, QJsonArray {u"pdf"_s}}, {u"folder"_s, u"C:\\Docs"_s}};
        QVERIFY(f.host->query(u"fk invoice"_s, Scope::All, 1, filters));
        QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
        QCOMPARE(f.finalFor(1)->items[0].data.toObject().value(u"filters"_s).toObject(), filters);
    }

    void onceArgvAndEnvironment()
    {
        Fixture f;
        Spec spec;
        spec.mode = u"once"_s;
        spec.role = u"--once"_s;
        spec.args = {u"--echo-env"_s, u"a b"_s, u"\"quoted\""_s, u"C:\\path\\"_s, u"back\\slash\\\"q"_s, u"中文参数"_s, u""_s};
        spec.runtimeExtra = {{u"debounceMs"_s, 0}, {u"env"_s, QJsonObject {{u"FAKE_EXTRA"_s, u"额外"_s}}}};
        spec.commandExtra = {{u"triggers"_s, QJsonArray {QJsonObject {{u"regex"_s, u"^(\\d+)\\+(\\d+)$"_s}}}}};
        const QString folder = f.install(spec);
        QVERIFY(!folder.isEmpty());
        ::SetEnvironmentVariableW(L"PLUGIN_TEST_PARENT_MARK", L"1");
        const QJsonObject filters {{u"minSize"_s, 0}, {u"maxSize"_s, 8192}};
        QVERIFY(f.host->query(u"fk 查询 words"_s, Scope::All, 1, filters));
        QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
        QJsonObject data = f.finalFor(1)->items[0].data.toObject();
        const QJsonArray argv = data.value(u"argv"_s).toArray();
        QCOMPARE(argv.size(), 7);
        QCOMPARE(argv[0].toString(), u"--echo-env"_s);
        QCOMPARE(argv[1].toString(), u"a b"_s);
        QCOMPARE(argv[2].toString(), u"\"quoted\""_s);
        QCOMPARE(argv[3].toString(), u"C:\\path\\"_s);
        QCOMPARE(argv[4].toString(), u"back\\slash\\\"q"_s);
        QCOMPARE(argv[5].toString(), u"中文参数"_s);
        QCOMPARE(argv[6].toString(), u""_s);
        QCOMPARE(data.value(u"WS_QUERY"_s).toString(), u"查询 words"_s);
        QCOMPARE(data.value(u"WS_TEXT"_s).toString(), u"fk 查询 words"_s);
        QCOMPARE(data.value(u"WS_COMMAND"_s).toString(), u"main"_s);
        QCOMPARE(data.value(u"WS_TRIGGER"_s).toString(), u"keyword"_s);
        QCOMPARE(data.value(u"WS_KEYWORD"_s).toString(), u"fk"_s);
        QCOMPARE(data.value(u"WS_PLUGIN_DIR"_s).toString(), folder);
        QVERIFY(data.value(u"WS_DATA_DIR"_s).toString().endsWith(u"\\test.fake.data"_s));
        QVERIFY(QFileInfo::exists(data.value(u"WS_DATA_DIR"_s).toString()));
        QCOMPARE(data.value(u"WS_LOCALE"_s).toString(), u"zh"_s);
        QCOMPARE(data.value(u"WS_VERSION"_s).toString(), u"0.0-test"_s);
        QCOMPARE(data.value(u"WS_SETTINGS"_s).toString(), u"{}"_s);
        QCOMPARE(QJsonDocument::fromJson(data.value(u"WS_FILTERS"_s).toString().toUtf8()).object(), filters);
        QCOMPARE(data.value(u"FAKE_EXTRA"_s).toString(), u"额外"_s);
        QVERIFY(data.value(u"PYTHONIOENCODING"_s).toString().isEmpty()); // an exe, not python
        // The regex trigger: the whole text, and the groups.
        QVERIFY(f.host->query(u"12+34"_s, Scope::All, 2));
        QVERIFY(f.waitFinal(2));
        data = f.finalFor(2)->items[0].data.toObject();
        QCOMPARE(data.value(u"WS_TRIGGER"_s).toString(), u"regex"_s);
        QCOMPARE(data.value(u"WS_QUERY"_s).toString(), u"12+34"_s);
        QCOMPARE(data.value(u"WS_MATCH"_s).toString(), u"[\"12\",\"34\"]"_s);
    }

    // A Python plugin with uv's venv python.exe chosen, started for real:
    // the venv's interpreter runs in the job's one process (plugin_fake
    // plays it), told which venv it is.
    void pythonVenvThroughTheRunner()
    {
        Fixture f;
        const QString root = QDir::toNativeSeparators(f.dir.path());
        const QString base = root + u"\\uv\\cpython-3.12-windows-x86_64-none\\python.exe"_s;
        QVERIFY(copyFake(base));
        const QString venv = root + u"\\.venv\\Scripts\\python.exe"_s;
        QVERIFY(makeUvTrampoline(venv, 2, base));
        QVERIFY(writeFile(root + u"\\.venv\\pyvenv.cfg"_s, (u"home = "_s + root + u"\\uv\\cpython-3.12-windows-x86_64-none\n"_s).toUtf8()));
        const PythonOverride reset;
        programs::setOverride(Program::Python, venv);
        Spec spec;
        spec.mode = u"once"_s;
        spec.role = u"--once"_s;
        spec.args = {u"--echo-env"_s};
        spec.runtimeExtra = {{u"program"_s, u"python"_s}, {u"script"_s, u"main.py"_s}, {u"debounceMs"_s, 0}};
        const QString folder = f.install(spec);
        QVERIFY(!folder.isEmpty());
        QVERIFY(writeFile(folder + u"\\main.py"_s, "# plugin_fake plays python.exe\n"));
        QVERIFY(f.host->query(u"fk x"_s, Scope::All, 1));
        QVERIFY2(f.waitFinal(1), qPrintable(f.failureOf(1) + u'\n' + f.logs.join(u'\n')));
        const QJsonObject data = f.finalFor(1)->items[0].data.toObject();
        QCOMPARE(data.value(u"self"_s).toString().toLower(), base.toLower());
        QCOMPARE(data.value(u"__PYVENV_LAUNCHER__"_s).toString(), venv);
        QCOMPARE(data.value(u"PYTHONIOENCODING"_s).toString(), u"utf-8"_s);
        QCOMPARE(data.value(u"lead"_s).toArray(), (QJsonArray {u"-u"_s, u"-X"_s, u"utf8"_s, folder + u"\\main.py"_s}));
        QCOMPARE(data.value(u"argv"_s).toArray(), QJsonArray {u"--echo-env"_s});
        QCOMPARE(data.value(u"WS_QUERY"_s).toString(), u"x"_s);
    }

    void overlongLine()
    {
        Fixture f;
        Spec spec;
        spec.mode = u"once"_s;
        spec.role = u"--once"_s;
        spec.args = {u"--long"_s, u"17000000"_s};
        spec.runtimeExtra = {{u"debounceMs"_s, 0}, {u"timeoutMs"_s, 10000}};
        QVERIFY(!f.install(spec).isEmpty());
        QVERIFY(f.host->query(u"fk x"_s, Scope::All, 1));
        QVERIFY(f.waitFailure(1, 20000));
        QVERIFY2(f.failureOf(1).contains(u"16 MB"_s), qPrintable(f.failureOf(1)));
    }

    // ---- crashes ---------------------------------------------------------------
    void crashBackoffThenDisabled()
    {
        Fixture f;
        Spec spec;
        spec.args = {u"--crash"_s, u"1"_s};
        QVERIFY(!f.install(spec).isEmpty());
        QVERIFY(f.host->query(u"fk 1"_s, Scope::All, 1));
        QVERIFY(QTest::qWaitFor([&] { return f.restarts.size() == 1; }, 5000));
        QCOMPARE(f.restarts[0], 1000);
        QCOMPARE(f.failureOf(1), u"插件进程退出了"_s);
        QCOMPARE(f.host->state(u"test.fake"_s).status, Host::State::Status::Backoff);
        // A query during the backoff waits for the restart; the restart crashes again.
        QVERIFY(f.host->query(u"fk 2"_s, Scope::All, 2));
        QVERIFY(QTest::qWaitFor([&] { return f.restarts.size() == 2; }, 5000));
        QCOMPARE(f.restarts[1], 5000);
        QVERIFY(f.host->query(u"fk 3"_s, Scope::All, 3));
        QVERIFY(QTest::qWaitFor([&] { return !f.disabled.empty(); }, 5000));
        QCOMPARE(f.disabled, QStringList {u"test.fake"_s});
        QCOMPARE(f.restarts.size(), 2u);
        QCOMPARE(f.exitCodes, (std::vector<quint32> {3, 3, 3}));
        QCOMPARE(f.host->state(u"test.fake"_s).status, Host::State::Status::Crashed);
        QCOMPARE(f.host->state(u"test.fake"_s).crashes, 3);
        QVERIFY(f.logs.join(u'\n').contains(u"故意崩溃"_s));
        // Off: a query does nothing; switching it on again clears the slate.
        QVERIFY(!f.host->query(u"fk 4"_s, Scope::All, 4));
        f.host->setEnabled(u"test.fake"_s, true);
        QCOMPARE(f.host->state(u"test.fake"_s).status, Host::State::Status::Ready);
        QCOMPARE(Host::backoffDelayMs(0), 1000);
        QCOMPARE(Host::backoffDelayMs(1), 5000);
        QCOMPARE(Host::backoffDelayMs(2), 30000);
        QCOMPARE(Host::backoffDelayMs(5), 30000);
    }

    void runnerCrashRecovers()
    {
        // The runner dies under a query: the plugin dies with it (its job),
        // the query fails, and after the backoff the next query brings a
        // new runner and a new plugin process, without another keystroke.
        Fixture f;
        Spec spec;
        spec.args = {u"--slow"_s, u"400"_s};
        QVERIFY(!f.install(spec).isEmpty());
        QVERIFY(f.host->query(u"fk 1"_s, Scope::All, 1));
        QVERIFY(f.waitFinal(1));
        const quint32 runnerPid = static_cast<quint32>(f.host->runnerFacts().value(u"pid"_s).toDouble());
        const quint32 pluginPid = f.startedPids[0];
        QVERIFY(runnerPid != 0 && pluginPid != 0);
        QVERIFY(f.host->query(u"fk 2"_s, Scope::All, 2));
        QTest::qWait(50);
        const HANDLE runner = ::OpenProcess(PROCESS_TERMINATE, FALSE, runnerPid);
        QVERIFY(runner != nullptr);
        QVERIFY(::TerminateProcess(runner, 9));
        ::CloseHandle(runner);
        QVERIFY2(processGone(pluginPid, 3000), "the plugin must die with the runner (its job)");
        QVERIFY(f.waitFailure(2));
        QVERIFY(!f.finalFor(2));
        QVERIFY(QTest::qWaitFor([&] { return !f.restarts.empty(); }, 3000));
        QVERIFY(f.host->query(u"fk 3"_s, Scope::All, 3));
        QVERIFY2(f.waitFinal(3, 10000), qPrintable(f.logs.join(u'\n')));
        QCOMPARE(f.finalFor(3)->items[0].title, u"serve: 3"_s);
        QCOMPARE(f.startedPids.size(), 2u);
        QVERIFY(static_cast<quint32>(f.host->runnerFacts().value(u"pid"_s).toDouble()) != runnerPid);
        QCOMPARE(f.host->tokenSource(), elevated() ? Runner::TokenSource::Explorer : Runner::TokenSource::Plain);
    }

    void idleExit()
    {
        Fixture f;
        Spec spec;
        spec.runtimeExtra = {{u"idleExitSec"_s, 1}};
        QVERIFY(!f.install(spec).isEmpty());
        QVERIFY(f.host->query(u"fk x"_s, Scope::All, 1));
        QVERIFY(f.waitFinal(1));
        QVERIFY(QTest::qWaitFor([&] { return f.exitCodes.size() == 1; }, 5000));
        QCOMPARE(f.exitCodes[0], 0u); // it answered shutdown and left by itself
        QVERIFY(f.restarts.empty());
        QCOMPARE(f.host->state(u"test.fake"_s).status, Host::State::Status::Ready);
        // Needed again: back.
        QVERIFY(f.host->query(u"fk y"_s, Scope::All, 2));
        QVERIFY(f.waitFinal(2));
        QCOMPARE(f.startedPids.size(), 2u);
    }

    void residentCap()
    {
        Fixture f;
        // Two resident plugins, a cap of one: the older goes.
        Host::Options options = f.host->options();
        options.maxResident = 1;
        f.host = std::make_unique<Host>(options);
        f.host->setBackoffScale(0.01);
        std::vector<quint32> pids;
        QObject::connect(f.host.get(), &Host::processStarted, [&](const QString&, quint32 pid) { pids.push_back(pid); });
        int exits = 0;
        QObject::connect(f.host.get(), &Host::processExited, [&](const QString&, quint32) { ++exits; });
        Spec a;
        a.id = u"test.a"_s;
        a.keywords = {u"aa"_s};
        Spec b;
        b.id = u"test.b"_s;
        b.keywords = {u"bb"_s};
        QVERIFY(!f.install(a).isEmpty());
        QVERIFY(!f.install(b).isEmpty());
        QVERIFY(f.host->query(u"aa x"_s, Scope::All, 1));
        QVERIFY(QTest::qWaitFor([&] { return f.host->state(u"test.a"_s).status == Host::State::Status::Running && f.host->state(u"test.a"_s).pid != 0; }, 5000));
        QTest::qWait(50);
        QVERIFY(f.host->query(u"bb x"_s, Scope::All, 2));
        QVERIFY(QTest::qWaitFor([&] { return exits == 1 && pids.size() == 2; }, 5000));
        QVERIFY(processGone(pids[0], 3000));
        QCOMPARE(f.host->state(u"test.a"_s).status, Host::State::Status::Ready);
        QVERIFY(QTest::qWaitFor([&] { return f.host->state(u"test.b"_s).status == Host::State::Status::Running; }, 5000));
    }

    // ---- encodings -------------------------------------------------------------
    void encodingOem()
    {
        // The plugin writes "中文结果 Résumé <query>" in the OEM code page (as
        // Windows PowerShell 5.1 does when redirected). What survives depends
        // on that code page: all of it on a Chinese system (936), the Latin
        // part on an English one (437, where the Chinese becomes "?"); the
        // expectation is computed the same way.
        const QString written = u"中文结果 Résumé 查"_s;
        const std::wstring wide = written.toStdWString();
        const int bytes = ::WideCharToMultiByte(CP_OEMCP, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string oem(static_cast<std::size_t>(bytes), '\0');
        ::WideCharToMultiByte(CP_OEMCP, 0, wide.data(), static_cast<int>(wide.size()), oem.data(), bytes, nullptr, nullptr);
        const int chars = ::MultiByteToWideChar(CP_OEMCP, 0, oem.data(), bytes, nullptr, 0);
        std::wstring back(static_cast<std::size_t>(chars), L'\0');
        ::MultiByteToWideChar(CP_OEMCP, 0, oem.data(), bytes, back.data(), chars);
        const QString expected = QString::fromStdWString(back);
        qInfo() << "OEM code page" << ::GetOEMCP() << "gives back" << expected;
        QVERIFY(expected.contains(u"Résumé"_s));

        Fixture f;
        Spec spec;
        spec.mode = u"once"_s;
        spec.role = u"--once"_s;
        spec.args = {u"--gbk"_s};
        spec.runtimeExtra = {{u"debounceMs"_s, 0}, {u"encoding"_s, u"oem"_s}};
        QVERIFY(!f.install(spec).isEmpty());
        QVERIFY(f.host->query(u"fk 查"_s, Scope::All, 1));
        QVERIFY(f.waitFinal(1));
        QCOMPARE(f.finalFor(1)->items[0].title, expected);
        // Without `oem`, the same bytes are taken as UTF-8 and come out wrong.
        Spec wrong = spec;
        wrong.id = u"test.wrong"_s;
        wrong.keywords = {u"wr"_s};
        wrong.runtimeExtra = {{u"debounceMs"_s, 0}};
        QVERIFY(!f.install(wrong).isEmpty());
        QVERIFY(f.host->query(u"wr 查"_s, Scope::All, 2));
        QVERIFY(f.waitFinal(2));
        QVERIFY(f.finalFor(2)->items[0].title != expected);
        // UTF-8 is the default and round-trips Chinese.
        Spec utf = spec;
        utf.id = u"test.utf"_s;
        utf.keywords = {u"ut"_s};
        utf.args.clear();
        utf.runtimeExtra = {{u"debounceMs"_s, 0}};
        QVERIFY(!f.install(utf).isEmpty());
        QVERIFY(f.host->query(u"ut 中文查询"_s, Scope::All, 3));
        QVERIFY(f.waitFinal(3));
        QCOMPARE(f.finalFor(3)->items[0].title, u"once: 中文查询"_s);
    }

    // ---- the job and the permissions ---------------------------------------------
    // The facts a --probe plugin reports about its process. Run without
    // elevation, these are what the runner does itself; elevated, they
    // show the plugin never got Win顺's rights.
    void jobsAndPermissions()
    {
        if (!elevated())
            QSKIP("needs elevation: run tst_plugin as administrator for this group");
        Fixture f;
        ::SetEnvironmentVariableW(L"PLUGIN_TEST_PARENT_MARK", L"1");
        const QString explorerSession = explorerAuthId();
        QVERIFY(!explorerSession.isEmpty());

        // The runner itself: Explorer's token.
        Spec plain;
        plain.mode = u"once"_s;
        plain.role = u"--probe"_s;
        plain.runtimeExtra = {{u"debounceMs"_s, 0}, {u"memoryMB"_s, 256}};
        QVERIFY(!f.install(plain).isEmpty());
        QVERIFY(f.host->query(u"fk x"_s, Scope::All, 1));
        QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
        QCOMPARE(f.host->tokenSource(), Runner::TokenSource::Explorer);
        const QJsonObject runner = f.host->runnerFacts();
        qInfo() << "runner:" << QJsonDocument(runner).toJson(QJsonDocument::Compact).constData();
        QCOMPARE(runner.value(u"integrity"_s).toInt(), 8192);
        QCOMPARE(runner.value(u"elevated"_s).toBool(true), false);
        QCOMPARE(runner.value(u"admins"_s).toString(), u"deny-only"_s);
        QCOMPARE(runner.value(u"authId"_s).toString(), explorerSession);
        QVERIFY(runner.value(u"extensionPointsDisabled"_s).toBool());
        QVERIFY(runner.value(u"noRemoteImages"_s).toBool());
        QVERIFY(runner.value(u"inJob"_s).toBool());
        QCOMPARE(runner.value(u"jobActiveProcessLimit"_s).toInt(), 0); // the runner's job limits nothing but ends it

        // A plugin with no permissions.
        QJsonObject facts = f.finalFor(1)->items[0].data.toObject();
        qInfo() << "plugin (normal, no permissions):" << QJsonDocument(facts).toJson(QJsonDocument::Compact).constData();
        QCOMPARE(facts.value(u"integrity"_s).toInt(), 8192);
        QCOMPARE(facts.value(u"elevated"_s).toBool(true), false);
        QCOMPARE(facts.value(u"admins"_s).toString(), u"deny-only"_s);
        QCOMPARE(facts.value(u"authId"_s).toString(), explorerSession);
        QVERIFY(facts.value(u"inJob"_s).toBool());
        QVERIFY(facts.value(u"jobKillOnClose"_s).toBool());
        QCOMPARE(facts.value(u"jobActiveProcessLimit"_s).toInt(), 1);
        QCOMPARE(facts.value(u"jobMemoryMB"_s).toInt(), 256);
        const int ui = facts.value(u"jobUiRestrictions"_s).toInt();
        QVERIFY((ui & JOB_OBJECT_UILIMIT_READCLIPBOARD) && (ui & JOB_OBJECT_UILIMIT_WRITECLIPBOARD) && (ui & JOB_OBJECT_UILIMIT_DESKTOP));
        QVERIFY(facts.value(u"aslrBottomUp"_s).toBool());
        QVERIFY(facts.value(u"aslrHighEntropy"_s).toBool());
        QVERIFY(facts.value(u"extensionPointsDisabled"_s).toBool());
        QVERIFY(facts.value(u"noRemoteImages"_s).toBool());
        QVERIFY(facts.value(u"noLowLabelImages"_s).toBool());
        QVERIFY(!facts.value(u"dynamicCodeProhibited"_s).toBool()); // interpreters need it
        QVERIFY(!facts.value(u"consoleWindow"_s).toBool());
        QVERIFY(!facts.value(u"parentEnvLeaked"_s).toBool());
        QCOMPARE(facts.value(u"spawnOk"_s).toBool(true), false);
        QCOMPARE(facts.value(u"spawnError"_s).toInt(), 1816); // ERROR_NOT_ENOUGH_QUOTA: ActiveProcessLimit
        QCOMPARE(facts.value(u"clipRead"_s).toBool(true), false);
        if (facts.value(u"clipFormats"_s).toInt() > 0)
            QCOMPARE(facts.value(u"clipError"_s).toInt(), 5); // ERROR_ACCESS_DENIED from GetClipboardData
        QJsonArray writes = facts.value(u"writes"_s).toArray();
        QCOMPARE(writes.size(), 2);
        QVERIFY(writes[0].toObject().value(u"ok"_s).toBool()); // its data folder
        QVERIFY(writes[1].toObject().value(u"ok"_s).toBool()); // the user's profile: medium integrity may

        // With processes and the clipboard allowed.
        Spec allowed = plain;
        allowed.id = u"test.allowed"_s;
        allowed.keywords = {u"al"_s};
        allowed.permissions = {u"processes"_s, u"clipboard:read"_s};
        QVERIFY(!f.install(allowed).isEmpty());
        QVERIFY(f.host->query(u"al x"_s, Scope::All, 2));
        QVERIFY2(f.waitFinal(2), qPrintable(f.logs.join(u'\n')));
        facts = f.finalFor(2)->items[0].data.toObject();
        qInfo() << "plugin (processes, clipboard:read):" << QJsonDocument(facts).toJson(QJsonDocument::Compact).constData();
        QCOMPARE(facts.value(u"jobActiveProcessLimit"_s).toInt(), 0);
        QVERIFY(facts.value(u"spawnOk"_s).toBool());
        QVERIFY(facts.value(u"spawnedInJob"_s).toBool()); // the child is in the plugin's job too
        if (facts.value(u"clipFormats"_s).toInt() > 0)
            QVERIFY(facts.value(u"clipRead"_s).toBool());
        QVERIFY(!(facts.value(u"jobUiRestrictions"_s).toInt() & JOB_OBJECT_UILIMIT_READCLIPBOARD));

        // Strict: low integrity, its data folder writable, the profile not.
        Spec strict = plain;
        strict.id = u"test.strict"_s;
        strict.keywords = {u"st"_s};
        strict.runtimeExtra.insert(u"sandbox"_s, u"strict"_s);
        QVERIFY(!f.install(strict).isEmpty());
        QVERIFY(f.host->query(u"st x"_s, Scope::All, 3));
        QVERIFY2(f.waitFinal(3), qPrintable(f.logs.join(u'\n')));
        facts = f.finalFor(3)->items[0].data.toObject();
        qInfo() << "plugin (strict):" << QJsonDocument(facts).toJson(QJsonDocument::Compact).constData();
        QCOMPARE(facts.value(u"integrity"_s).toInt(), 4096);
        QCOMPARE(facts.value(u"elevated"_s).toBool(true), false);
        QCOMPARE(facts.value(u"admins"_s).toString(), u"deny-only"_s);
        QCOMPARE(facts.value(u"authId"_s).toString(), explorerSession);
        QVERIFY(facts.value(u"extensionPointsDisabled"_s).toBool());
        writes = facts.value(u"writes"_s).toArray();
        QCOMPARE(writes.size(), 2);
        QVERIFY2(writes[0].toObject().value(u"ok"_s).toBool(), "the low-labelled data folder");
        QCOMPARE(writes[1].toObject().value(u"ok"_s).toBool(true), false);
        QCOMPARE(writes[1].toObject().value(u"error"_s).toInt(), 5);
        f.host->shutdown();
    }

    // The job's limits and the mitigations are the runner's doing and show
    // without elevation too (the token is then the test's own).
    void jobLimitsUnelevatedToo()
    {
        Fixture f;
        Spec plain;
        plain.mode = u"once"_s;
        plain.role = u"--probe"_s;
        plain.runtimeExtra = {{u"debounceMs"_s, 0}};
        QVERIFY(!f.install(plain).isEmpty());
        QVERIFY(f.host->query(u"fk x"_s, Scope::All, 1));
        QVERIFY2(f.waitFinal(1), qPrintable(f.logs.join(u'\n')));
        const QJsonObject facts = f.finalFor(1)->items[0].data.toObject();
        QVERIFY(facts.value(u"inJob"_s).toBool());
        QCOMPARE(facts.value(u"jobActiveProcessLimit"_s).toInt(), 1);
        QCOMPARE(facts.value(u"jobMemoryMB"_s).toInt(), 512);
        QVERIFY(facts.value(u"extensionPointsDisabled"_s).toBool());
        QVERIFY(facts.value(u"noRemoteImages"_s).toBool());
        QVERIFY(!facts.value(u"dynamicCodeProhibited"_s).toBool());
        QCOMPARE(facts.value(u"spawnError"_s).toInt(), 1816);
        QCOMPARE(facts.value(u"clipRead"_s).toBool(true), false);
        QVERIFY(!facts.value(u"consoleWindow"_s).toBool());
        QVERIFY(facts.value(u"wsEnv"_s).toArray().contains(u"WS_QUERY"_s));
        QCOMPARE(facts.value(u"elevated"_s).toBool(true), false); // never, whatever the test's own token
        const QJsonObject runner = f.host->runnerFacts();
        QVERIFY(runner.value(u"extensionPointsDisabled"_s).toBool());
        QVERIFY(runner.value(u"inJob"_s).toBool());
    }

    // A strict plugin's data folder on a second disk: Windows' default root
    // ACL there gives Authenticated Users Modify, which lacks WRITE_OWNER, so
    // an unelevated copy (a portable one, a --profile) could not label the
    // folder it made, and the plugin could not write in it ("数据文件夹不能用").
    void labelLowWithoutWriteOwner()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString folder = QDir::toNativeSeparators(temp.filePath(u"x.data"_s));
        QVERIFY(QDir().mkpath(folder));
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        QVERIFY(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;0x1301bf;;;AU)(A;OICI;0x1200a9;;;BU)", SDDL_REVISION_1,
            &descriptor, nullptr));
        PACL dacl = nullptr;
        BOOL present = FALSE, defaulted = FALSE;
        QVERIFY(::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present);
        std::wstring native = folder.toStdWString();
        const DWORD set = ::SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
        ::LocalFree(descriptor);
        QCOMPARE(set, DWORD(ERROR_SUCCESS));

        QVERIFY(!ws::plugin::isLabelledLow(folder));
        QVERIFY(ws::plugin::labelLow(folder));
        QVERIFY(ws::plugin::isLabelledLow(folder));
    }

    // ---- the open policy (OpenPolicy) ----------------------------------------
    // Whether Win顺 carries out a plugin's declarative `open` depends only on
    // the plugin's `processes` permission and the target as it resolves; a fake
    // probe stands in for the environment, the file system and the registry so
    // the whole table runs without touching them.
    void openPolicy_data()
    {
        QTest::addColumn<bool>("url");
        QTest::addColumn<QString>("target");
        QTest::addColumn<QString>("args");
        QTest::addColumn<QString>("cwd");
        QTest::addColumn<bool>("processes");
        QTest::addColumn<int>("gate"); // OpenGate

        const int none = int(OpenGate::None);
        const int args = int(OpenGate::Args);
        const int scheme = int(OpenGate::Scheme);
        const int program = int(OpenGate::Program);

        // URLs, without `processes`.
        QTest::newRow("http") << true << u"http://example.com/x"_s << QString() << QString() << false << none;
        QTest::newRow("https") << true << u"https://example.com/x"_s << QString() << QString() << false << none;
        QTest::newRow("HTTPS upper") << true << u"HTTPS://example.com"_s << QString() << QString() << false << none;
        QTest::newRow("mailto") << true << u"mailto:a@b.com"_s << QString() << QString() << false << none;
        QTest::newRow("ms-settings") << true << u"ms-settings:display"_s << QString() << QString() << false << scheme;
        QTest::newRow("file url exe") << true << u"file:///C:/Windows/System32/cmd.exe"_s << QString() << QString() << false << scheme;
        QTest::newRow("custom scheme") << true << u"myapp:do?x=1"_s << QString() << QString() << false << scheme;
        QTest::newRow("winshun scheme") << true << u"winshun:settings"_s << QString() << QString() << false << scheme;
        // URLs, with `processes`: anything.
        QTest::newRow("ms-settings proc") << true << u"ms-settings:display"_s << QString() << QString() << true << none;
        QTest::newRow("custom scheme proc") << true << u"myapp:do"_s << QString() << QString() << true << none;

        // Folders, without `processes`.
        QTest::newRow("folder") << false << u"C:\\Users\\me\\Downloads"_s << QString() << QString() << false << none;
        QTest::newRow("folder env") << false << u"%USERPROFILE%\\Downloads"_s << QString() << QString() << false << none;
        QTest::newRow("drive root") << false << u"D:\\"_s << QString() << QString() << false << none;
        // Ordinary documents.
        QTest::newRow("pdf") << false << u"C:\\docs\\report.pdf"_s << QString() << QString() << false << none;
        QTest::newRow("txt") << false << u"C:\\docs\\notes.txt"_s << QString() << QString() << false << none;
        QTest::newRow("docx forward slash") << false << u"C:/docs/a.docx"_s << QString() << QString() << false << none;
        QTest::newRow("unc pdf") << false << u"\\\\server\\share\\a.pdf"_s << QString() << QString() << false << none;

        // Programs and scripts, without `processes`.
        QTest::newRow("exe") << false << u"C:\\Windows\\System32\\notepad.exe"_s << QString() << QString() << false << program;
        QTest::newRow("exe env") << false << u"%SystemRoot%\\notepad.exe"_s << QString() << QString() << false << program;
        QTest::newRow("bat") << false << u"C:\\x\\run.bat"_s << QString() << QString() << false << program;
        QTest::newRow("ps1") << false << u"C:\\x\\go.ps1"_s << QString() << QString() << false << program;
        QTest::newRow("msc") << false << u"C:\\x\\services.msc"_s << QString() << QString() << false << program;
        QTest::newRow("lnk") << false << u"C:\\x\\a.lnk"_s << QString() << QString() << false << program;
        QTest::newRow("url file") << false << u"C:\\x\\a.url"_s << QString() << QString() << false << program;
        QTest::newRow("settingcontent") << false << u"C:\\x\\a.settingcontent-ms"_s << QString() << QString() << false << program;
        QTest::newRow("reg") << false << u"C:\\x\\a.reg"_s << QString() << QString() << false << program;
        QTest::newRow("py") << false << u"C:\\x\\a.py"_s << QString() << QString() << false << program;
        QTest::newRow("unc exe") << false << u"\\\\server\\share\\evil.exe"_s << QString() << QString() << false << program;
        // Resolution tricks.
        QTest::newRow("trailing dot") << false << u"C:\\x\\evil.exe."_s << QString() << QString() << false << program;
        QTest::newRow("trailing space") << false << u"C:\\x\\evil.exe "_s << QString() << QString() << false << program;
        QTest::newRow("data stream") << false << u"C:\\x\\notes.txt:payload.exe"_s << QString() << QString() << false << program;
        QTest::newRow("relative") << false << u"run.exe"_s << QString() << QString() << false << program;
        QTest::newRow("bare name") << false << u"notepad"_s << QString() << QString() << false << program;
        QTest::newRow("extensionless abs") << false << u"C:\\x\\hosts"_s << QString() << QString() << false << program;
        QTest::newRow("dangerous assoc") << false << u"C:\\x\\a.weird"_s << QString() << QString() << false << program;

        // Arguments or a working folder, without `processes`.
        QTest::newRow("pdf with args") << false << u"C:\\docs\\a.pdf"_s << u"/x"_s << QString() << false << args;
        QTest::newRow("folder with cwd") << false << u"C:\\Users\\me\\Downloads"_s << QString() << u"C:\\Users\\me"_s << false << args;

        // With `processes`: everything a path can be.
        QTest::newRow("exe proc") << false << u"C:\\Windows\\System32\\notepad.exe"_s << QString() << QString() << true << none;
        QTest::newRow("exe args proc") << false << u"C:\\app\\OUTLOOK.EXE"_s << u"/select x"_s << QString() << true << none;
    }

    void openPolicy()
    {
        QFETCH(bool, url);
        QFETCH(QString, target);
        QFETCH(QString, args);
        QFETCH(QString, cwd);
        QFETCH(bool, processes);
        QFETCH(int, gate);

        OpenProbe probe;
        probe.expandEnv = [](const QString& s) {
            QString out = s;
            out.replace(u"%USERPROFILE%"_s, u"C:\\Users\\me"_s, Qt::CaseInsensitive);
            out.replace(u"%SystemRoot%"_s, u"C:\\Windows"_s, Qt::CaseInsensitive);
            return out;
        };
        const QStringList dirs
            = {u"C:\\Users\\me\\Downloads"_s, u"C:\\Users\\me"_s, u"C:\\Windows"_s, u"D:\\"_s};
        probe.isDirectory = [dirs](const QString& s) { return dirs.contains(QDir::toNativeSeparators(s), Qt::CaseInsensitive); };
        probe.dangerousAssoc = [](QStringView ext) { return ext == u".weird"; };
        probe.pathExt = {u".com"_s, u".exe"_s, u".bat"_s, u".cmd"_s, u".vbs"_s, u".js"_s, u".ps1"_s, u".msc"_s};

        Open open;
        open.kind = url ? Open::Kind::Url : Open::Kind::Path;
        open.target = target;
        open.args = args;
        open.cwd = cwd;
        const OpenRuling ruling = ruleOpen(open, processes, probe);
        QCOMPARE(int(ruling.gate), gate);
    }

    // A None open is always allowed (nothing is carried out), whatever the probe.
    void openPolicyNone()
    {
        QVERIFY(ruleOpen(Open {}, false, OpenProbe {}).allowed());
    }

    // The real probe on this machine (environment, GetFileAttributes,
    // AssocIsDangerous, PATHEXT): the verdicts a no-processes plugin's open
    // actually gets. Robust facts only, so it holds on any Windows: a program
    // is refused, a non-web scheme is refused, a real folder and a document are
    // allowed, and `processes` lifts all of it.
    void openPolicySystemProbe()
    {
        const OpenProbe probe = systemProbe();
        const auto path = [](const QString& t, const QString& a = {}) {
            Open o;
            o.kind = Open::Kind::Path;
            o.target = t;
            o.args = a;
            return o;
        };
        const auto url = [](const QString& t) {
            Open o;
            o.kind = Open::Kind::Url;
            o.target = t;
            return o;
        };
        QCOMPARE(ruleOpen(path(u"%SystemRoot%\\notepad.exe"_s), false, probe).gate, OpenGate::Program);
        QCOMPARE(ruleOpen(path(u"%SystemRoot%\\notepad.exe"_s), true, probe).gate, OpenGate::None);
        QCOMPARE(ruleOpen(path(u"%SystemRoot%\\notepad.exe"_s, u"C:\\x\\a.txt"_s), true, probe).gate, OpenGate::None);
        QCOMPARE(ruleOpen(path(u"%SystemRoot%\\notepad.exe"_s, u"C:\\x\\a.txt"_s), false, probe).gate, OpenGate::Args);
        QCOMPARE(ruleOpen(url(u"ms-settings:display"_s), false, probe).gate, OpenGate::Scheme);
        QCOMPARE(ruleOpen(url(u"https://example.com/"_s), false, probe).gate, OpenGate::None);
        QCOMPARE(ruleOpen(path(u"C:\\Windows"_s), false, probe).gate, OpenGate::None);
        QCOMPARE(ruleOpen(path(u"C:\\Users\\Public\\Documents\\readme.txt"_s), false, probe).gate, OpenGate::None);
    }
};

QTEST_GUILESS_MAIN(TestPlugin)
#include "tst_plugin.moc"
