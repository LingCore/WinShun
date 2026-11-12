// ws plugin's side in the app (src/app/PluginCommands, GitHub #251): a real
// plugin host over folders of the test's own. What is done at once (a folder
// installed, a switch that needs no answer), what is handed to the page
// because only the user may answer it, and what the answers say.

#include "Catalog.h"
#include "Host.h"
#include "PluginCommands.h"
#include "PluginUpdates.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <optional>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

// A zero-code plugin: nothing to run, so nothing of the runner's is needed.
QByteArray manifest(const QString& id, const QString& version, const QString& permissions = {})
{
    QJsonObject o {{u"manifest"_s, 1}, {u"id"_s, id}, {u"name"_s, QJsonObject {{u"zh"_s, u"你好"_s}, {u"en"_s, u"Hello"_s}}},
        {u"version"_s, version},
        {u"commands"_s, QJsonArray {QJsonObject {{u"id"_s, u"list"_s}, {u"keyword"_s, u"hl"_s}, {u"title"_s, u"List"_s},
                            {u"run"_s, QJsonObject {{u"items"_s, QJsonArray {QJsonObject {{u"id"_s, u"a"_s}, {u"title"_s, u"Alpha"_s}}}}}}}}}};
    if (!permissions.isEmpty())
        o.insert(u"permissions"_s, QJsonArray {permissions});
    return QJsonDocument(o).toJson();
}

bool write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

struct Fixture {
    QTemporaryDir dir;
    QString userDir;
    QString bundledDir;
    std::unique_ptr<plugin::Host> host;
    std::unique_ptr<PluginCommands> commands;
    QList<PluginConfig> configs; // the settings
    QStringList asked; // what was handed to the page: "enable <id>", "remove <id>", "library <id>"
    PluginInstalled libraryAnswer; // as the page's install would answer
    QByteArray libraryInstalls; // ... having put this manifest in place; empty: nothing
    bool page = true; // there is a settings window to ask in

    Fixture()
    {
        userDir = dir.path() + u"/plugins"_s;
        bundledDir = dir.path() + u"/bundled"_s;
        write(bundledDir + u"/winshun.own/plugin.json"_s, manifest(u"winshun.own"_s, u"1.0.0"_s));
        plugin::Host::Options options;
        options.userPluginDir = userDir;
        options.bundledPluginDir = bundledDir;
        options.dataRoot = dir.path() + u"/data"_s;
        options.version = u"0.25.0"_s;
        host = std::make_unique<plugin::Host>(options);
        PluginCommands::Environment environment;
        environment.host = host.get();
        environment.locale = [] { return u"zh"_s; };
        environment.configs = [this] { return configs; };
        environment.setSwitch = [this](const QString& id, bool on) {
            config(id).enabled = on;
            apply();
        };
        environment.askEnable = [this](const QString& id) {
            asked.push_back(u"enable "_s + id);
            return page;
        };
        environment.askRemove = [this](const QString& id) {
            asked.push_back(u"remove "_s + id);
            return page;
        };
        environment.installFromLibrary = [this](const QString& id, std::function<void(PluginInstalled)> done) {
            asked.push_back(u"library "_s + id);
            if (!libraryInstalls.isEmpty()) {
                write(userDir + u'/' + id + u"/plugin.json"_s, libraryInstalls);
                host->catalog().scan();
            }
            done(libraryAnswer);
        };
        environment.librarySource = dir.path() + u"/index.json"_s;
        environment.keptList = dir.path() + u"/kept.json"_s;
        commands = std::make_unique<PluginCommands>(std::move(environment));
    }

    PluginConfig& config(const QString& id)
    {
        for (PluginConfig& c : configs) {
            if (c.id == id)
                return c;
        }
        configs.push_back({id});
        return configs.back();
    }

    // The settings told to the host, as App::applyPlugins tells them.
    void apply()
    {
        QHash<QString, QStringList> confirmed;
        QSet<QString> enabled;
        for (const PluginConfig& c : configs) {
            confirmed.insert(c.id, c.confirmed);
            if (c.enabled)
                enabled.insert(c.id);
        }
        host->catalog().setConfirmed(std::move(confirmed));
        host->catalog().setEnabled(enabled);
    }

    // The user said yes to what `id` asks for now, and it is on.
    void agreeAndTurnOn(const QString& id)
    {
        const plugin::Entry* entry = host->catalog().find(id);
        config(id).confirmed = entry && entry->manifest && !entry->manifest->permissions.names().isEmpty()
            ? entry->manifest->permissions.names() : QStringList {u"none"_s};
        config(id).enabled = true;
        apply();
    }

    QString source(const QString& name, const QByteArray& manifestText)
    {
        const QString folder = dir.path() + u"/src/"_s + name;
        write(folder + u"/plugin.json"_s, manifestText);
        return folder;
    }

    QJsonObject run(const char* tool, const QJsonObject& arguments = {})
    {
        std::optional<mcp::ToolResult> got;
        commands->run(QString::fromLatin1(tool), arguments, [&](mcp::ToolResult r) { got = std::move(r); });
        if (!QTest::qWaitFor([&] { return got.has_value(); }, 10000))
            return {{u"state"_s, u"no answer"_s}};
        return got->isError ? QJsonObject {{u"state"_s, u"error"_s}, {u"message"_s, got->errorText}} : got->structured;
    }
};

QString state(const QJsonObject& answer)
{
    return answer.value(u"state"_s).toString();
}

QString message(const QJsonObject& answer)
{
    return answer.value(u"message"_s).toString();
}

QJsonObject pluginOf(const QJsonObject& answer)
{
    return answer.value(u"plugin"_s).toObject();
}

} // namespace

class TestPluginCommands : public QObject {
    Q_OBJECT

private slots:
    void lists()
    {
        Fixture f;
        write(f.userDir + u"/me.hello/plugin.json"_s, manifest(u"me.hello"_s, u"1.0.0"_s, u"network"_s));
        write(f.userDir + u"/me.broken/plugin.json"_s, "{");
        f.host->catalog().scan();
        const QJsonObject answer = f.run("plugins_list");
        QCOMPARE(state(answer), u"done"_s);
        QHash<QString, QJsonObject> byId;
        for (const QJsonValue& v : answer.value(u"plugins"_s).toArray())
            byId.insert(v.toObject().value(u"id"_s).toString(), v.toObject());
        QCOMPARE(byId.size(), 3);
        const QJsonObject hello = byId.value(u"me.hello"_s);
        QCOMPARE(hello.value(u"name"_s).toString(), u"你好"_s);
        QCOMPARE(hello.value(u"version"_s).toString(), u"1.0.0"_s);
        QCOMPARE(hello.value(u"state"_s).toString(), u"off"_s);
        QCOMPARE(hello.value(u"source"_s).toString(), u"user"_s);
        QCOMPARE(hello.value(u"permissions"_s).toArray(), QJsonArray {u"network"_s});
        QCOMPARE(byId.value(u"winshun.own"_s).value(u"source"_s).toString(), u"bundled"_s);
        QCOMPARE(byId.value(u"me.broken"_s).value(u"state"_s).toString(), u"unusable"_s);
        QVERIFY(!byId.value(u"me.broken"_s).value(u"error"_s).toString().isEmpty());
    }

    // A folder of one's own: in place at once and off; over a version that is
    // on it is on; a version asking for more waits for the user's yes.
    void installsFolders()
    {
        Fixture f;
        const QString first = f.source(u"v1"_s, manifest(u"me.hello"_s, u"1.0.0"_s));
        QJsonObject answer = f.run("plugins_install", {{u"path"_s, first}});
        QCOMPARE(state(answer), u"done"_s);
        QCOMPARE(pluginOf(answer).value(u"state"_s).toString(), u"off"_s);
        QCOMPARE(message(answer), u"已安装 你好 1.0.0，还关着；打开它：ws plugin enable me.hello"_s);
        QVERIFY(QFileInfo::exists(f.userDir + u"/me.hello/plugin.json"_s));
        QVERIFY2(QFileInfo::exists(first + u"/plugin.json"_s), "the folder it came from is left as it was");
        QVERIFY(f.asked.isEmpty());

        f.agreeAndTurnOn(u"me.hello"_s);
        answer = f.run("plugins_install", {{u"path"_s, f.source(u"v2"_s, manifest(u"me.hello"_s, u"1.1.0"_s))}});
        QCOMPARE(state(answer), u"done"_s);
        QCOMPARE(pluginOf(answer).value(u"version"_s).toString(), u"1.1.0"_s);
        QCOMPARE(pluginOf(answer).value(u"state"_s).toString(), u"on"_s);
        QCOMPARE(message(answer), u"已安装 你好 1.1.0（原来是 1.0.0），开着，已经换上"_s);

        answer = f.run("plugins_install", {{u"path"_s, f.source(u"v3"_s, manifest(u"me.hello"_s, u"1.2.0"_s, u"network"_s))}});
        QCOMPARE(state(answer), u"done"_s);
        QCOMPARE(pluginOf(answer).value(u"state"_s).toString(), u"waiting"_s);
        QCOMPARE(pluginOf(answer).value(u"waiting_for"_s).toArray(), QJsonArray {u"network"_s});
        QVERIFY2(message(answer).contains(u"新版本要更多权限"_s) && message(answer).contains(u"ws plugin enable me.hello"_s), qPrintable(message(answer)));
        // ... and turning it on is then the user's to answer.
        answer = f.run("plugins_switch", {{u"id"_s, u"me.hello"_s}, {u"on"_s, true}});
        QCOMPARE(state(answer), u"asking"_s);
        QCOMPARE(f.asked, QStringList {u"enable me.hello"_s});
    }

    void installProblems()
    {
        Fixture f;
        QJsonObject answer = f.run("plugins_install", {{u"path"_s, f.dir.path() + u"/nothing-here"_s}});
        QCOMPARE(state(answer), u"failed"_s);
        QCOMPARE(answer.value(u"code"_s).toString(), u"not_found"_s);

        const QString empty = f.dir.path() + u"/src/empty"_s;
        QVERIFY(QDir().mkpath(empty));
        answer = f.run("plugins_install", {{u"path"_s, empty}});
        QCOMPARE(state(answer), u"failed"_s);
        QVERIFY(!answer.contains(u"code"_s));
        QVERIFY2(message(answer).startsWith(u"不能安装 empty："_s), qPrintable(message(answer)));

        // A folder of the id's name that is no plugin (the user's own files): never put aside unasked.
        QVERIFY(write(f.userDir + u"/me.mine/notes.txt"_s, "mine"));
        answer = f.run("plugins_install", {{u"path"_s, f.source(u"mine"_s, manifest(u"me.mine"_s, u"1.0.0"_s))}});
        QCOMPARE(state(answer), u"failed"_s);
        QVERIFY2(message(answer).contains(u"不是装好的插件"_s), qPrintable(message(answer)));
        QVERIFY(QFileInfo::exists(f.userDir + u"/me.mine/notes.txt"_s));
        QVERIFY(!QFileInfo::exists(f.userDir + u"/me.mine/plugin.json"_s));

        QCOMPARE(state(f.run("plugins_install")), u"failed"_s);
    }

    // An id: the library's, installed on the page; its answer passed on.
    void installsFromTheLibraryOnThePage()
    {
        Fixture f;
        f.libraryAnswer = {PluginInstalled::Step::Asking, u"me.native"_s, u"2.0.0"_s, {}};
        QJsonObject answer = f.run("plugins_install", {{u"id"_s, u"me.native"_s}});
        QCOMPARE(state(answer), u"asking"_s);
        QVERIFY2(message(answer).contains(u"me.native 2.0.0"_s), qPrintable(message(answer)));
        QCOMPARE(f.asked, QStringList {u"library me.native"_s});

        f.libraryAnswer = {PluginInstalled::Step::Failed, u"me.native"_s, {}, u"下载的文件校验不对"_s};
        answer = f.run("plugins_install", {{u"id"_s, u"me.native"_s}});
        QCOMPARE(state(answer), u"failed"_s);
        QCOMPARE(message(answer), u"下载的文件校验不对"_s);

        // Installed by the page: said in ws's own words, with what to run next.
        f.libraryInstalls = manifest(u"me.hello"_s, u"1.0.0"_s);
        f.libraryAnswer = {PluginInstalled::Step::Done, u"me.hello"_s, u"1.0.0"_s, u"已安装 你好 1.0.0，打开它的开关就能用"_s};
        answer = f.run("plugins_install", {{u"id"_s, u"me.hello"_s}});
        QCOMPARE(state(answer), u"done"_s);
        QCOMPARE(message(answer), u"已安装 你好 1.0.0，还关着；打开它：ws plugin enable me.hello"_s);
        // Up to date already: the page's words for that.
        f.libraryInstalls.clear();
        f.libraryAnswer = {PluginInstalled::Step::Done, u"me.hello"_s, u"1.0.0"_s, u"你好 已经是最新版（1.0.0）"_s};
        answer = f.run("plugins_install", {{u"id"_s, u"me.hello"_s}});
        QCOMPARE(message(answer), u"你好 已经是最新版（1.0.0）。打开它：ws plugin enable me.hello"_s);
        // A newer version over one that is on.
        f.agreeAndTurnOn(u"me.hello"_s);
        f.libraryInstalls = manifest(u"me.hello"_s, u"1.1.0"_s);
        f.libraryAnswer = {PluginInstalled::Step::Done, u"me.hello"_s, u"1.1.0"_s, u"已安装 你好 1.1.0"_s};
        answer = f.run("plugins_install", {{u"id"_s, u"me.hello"_s}});
        QCOMPARE(message(answer), u"已安装 你好 1.1.0（原来是 1.0.0），开着，已经换上"_s);

        // What is neither there as a file nor an id: not handed on.
        f.asked.clear();
        answer = f.run("plugins_install", {{u"id"_s, u"My Plugin.zip"_s}});
        QCOMPARE(state(answer), u"failed"_s);
        QCOMPARE(answer.value(u"code"_s).toString(), u"not_found"_s);
        QVERIFY(f.asked.isEmpty());
    }

    void switches()
    {
        Fixture f;
        QVERIFY(write(f.userDir + u"/me.hello/plugin.json"_s, manifest(u"me.hello"_s, u"1.0.0"_s, u"network"_s)));
        QVERIFY(write(f.userDir + u"/me.broken/plugin.json"_s, "{"));
        f.host->catalog().scan();
        const auto turn = [&](const QString& id, bool on) { return f.run("plugins_switch", {{u"id"_s, id}, {u"on"_s, on}}); };

        // Never agreed to: the page asks, nothing is turned on here.
        QJsonObject answer = turn(u"me.hello"_s, true);
        QCOMPARE(state(answer), u"asking"_s);
        QCOMPARE(f.asked, QStringList {u"enable me.hello"_s});
        QVERIFY(!f.config(u"me.hello"_s).enabled);
        // No window to ask in: said, not left hanging.
        f.page = false;
        QCOMPARE(state(turn(u"me.hello"_s, true)), u"failed"_s);
        f.page = true;

        QCOMPARE(message(turn(u"me.hello"_s, false)), u"你好 本来就关着"_s);

        // Agreed to before: on at once, nobody asked.
        f.config(u"me.hello"_s).confirmed = {u"network"_s};
        f.apply();
        f.asked.clear();
        answer = turn(u"me.hello"_s, true);
        QCOMPARE(state(answer), u"done"_s);
        QCOMPARE(message(answer), u"已打开 你好"_s);
        QCOMPARE(pluginOf(answer).value(u"state"_s).toString(), u"on"_s);
        QVERIFY(f.asked.isEmpty());
        QVERIFY(f.config(u"me.hello"_s).enabled);
        QCOMPARE(message(turn(u"me.hello"_s, true)), u"你好 本来就开着"_s);

        answer = turn(u"me.hello"_s, false);
        QCOMPARE(state(answer), u"done"_s);
        QCOMPARE(message(answer), u"已关闭 你好"_s);
        QVERIFY(!f.config(u"me.hello"_s).enabled);

        answer = turn(u"me.absent"_s, true);
        QCOMPARE(state(answer), u"failed"_s);
        QCOMPARE(answer.value(u"code"_s).toString(), u"not_found"_s);
        answer = turn(u"me.broken"_s, true);
        QCOMPARE(state(answer), u"failed"_s);
        QVERIFY2(message(answer).contains(u"用不了"_s), qPrintable(message(answer)));
    }

    // Removing is the user's to answer, always; what came with Win顺 cannot be.
    void removes()
    {
        Fixture f;
        QVERIFY(write(f.userDir + u"/me.hello/plugin.json"_s, manifest(u"me.hello"_s, u"1.0.0"_s)));
        f.host->catalog().scan();
        QJsonObject answer = f.run("plugins_remove", {{u"id"_s, u"me.hello"_s}});
        QCOMPARE(state(answer), u"asking"_s);
        QCOMPARE(f.asked, QStringList {u"remove me.hello"_s});
        QVERIFY(QFileInfo::exists(f.userDir + u"/me.hello/plugin.json"_s));

        f.asked.clear();
        answer = f.run("plugins_remove", {{u"id"_s, u"winshun.own"_s}});
        QCOMPARE(state(answer), u"failed"_s);
        QVERIFY2(message(answer).contains(u"ws plugin disable winshun.own"_s), qPrintable(message(answer)));
        QVERIFY(f.asked.isEmpty());
        QCOMPARE(f.run("plugins_remove", {{u"id"_s, u"me.absent"_s}}).value(u"code"_s).toString(), u"not_found"_s);
    }

    // The library's list for a search: fetched, filtered, each entry with what
    // is here of it; the copy kept when it cannot be fetched.
    void searches()
    {
        Fixture f;
        const QString sha(64, u'a');
        const auto entry = [&](const QString& id, const QString& name, const QString& version, const QString& review) {
            return QJsonObject {{u"id"_s, id}, {u"name"_s, QJsonObject {{u"zh"_s, name}}}, {u"version"_s, version},
                {u"description"_s, QJsonObject {{u"zh"_s, name + u"的说明"_s}}}, {u"url"_s, id + u".wsplugin"_s}, {u"sha256"_s, sha},
                {u"size"_s, 100}, {u"review"_s, review}, {u"permissions"_s, QJsonArray {u"network"_s}}};
        };
        const QJsonObject index {{u"schema"_s, 1},
            {u"plugins"_s, QJsonArray {entry(u"me.hello"_s, u"你好"_s, u"1.1.0"_s, u"reviewed"_s),
                               entry(u"other.translate"_s, u"翻译"_s, u"2.0.0"_s, u"unreviewed-native"_s)}}};
        QVERIFY(write(f.dir.path() + u"/index.json"_s, QJsonDocument(index).toJson()));
        QVERIFY(write(f.userDir + u"/me.hello/plugin.json"_s, manifest(u"me.hello"_s, u"1.0.0"_s)));
        f.host->catalog().scan();

        QJsonObject answer = f.run("plugins_search");
        QCOMPARE(state(answer), u"done"_s);
        QVERIFY(!answer.contains(u"message"_s));
        QJsonArray plugins = answer.value(u"plugins"_s).toArray();
        QCOMPARE(plugins.size(), 2);
        QCOMPARE(plugins[0].toObject().value(u"id"_s).toString(), u"me.hello"_s);
        QCOMPARE(plugins[0].toObject().value(u"state"_s).toString(), u"update"_s);
        QCOMPARE(plugins[0].toObject().value(u"installed"_s).toString(), u"1.0.0"_s);
        QCOMPARE(plugins[1].toObject().value(u"state"_s).toString(), u"install"_s);
        QCOMPARE(plugins[1].toObject().value(u"review"_s).toString(), u"unreviewed-native"_s);

        plugins = f.run("plugins_search", {{u"query"_s, u"翻译 说明"_s}}).value(u"plugins"_s).toArray();
        QCOMPARE(plugins.size(), 1);
        QCOMPARE(plugins[0].toObject().value(u"id"_s).toString(), u"other.translate"_s);
        QCOMPARE(f.run("plugins_search", {{u"query"_s, u"nothing-like-it"_s}}).value(u"plugins"_s).toArray().size(), 0);

        // The library cannot be reached: the copy kept, said to be one.
        QVERIFY(QFile::rename(f.dir.path() + u"/index.json"_s, f.dir.path() + u"/kept.json"_s));
        answer = f.run("plugins_search");
        QCOMPARE(state(answer), u"done"_s);
        QCOMPARE(answer.value(u"plugins"_s).toArray().size(), 2);
        QVERIFY2(message(answer).startsWith(u"连不上插件库"_s), qPrintable(message(answer)));
        QVERIFY(QFile::remove(f.dir.path() + u"/kept.json"_s));
        QCOMPARE(state(f.run("plugins_search")), u"failed"_s);
    }
};

QTEST_GUILESS_MAIN(TestPluginCommands)
#include "tst_plugincommands.moc"
