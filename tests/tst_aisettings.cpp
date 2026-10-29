// The settings AI may read and change (src/app/AiSettings): the table is
// default-deny, what must never be written is not, every writable entry
// survives a save and a load, and a value Settings::load() would reject or
// clamp is a problem naming the range, not a quiet change.

#include "AiSettings.h"
#include "Settings.h"

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

using namespace ws;
using namespace ws::aisettings;
using namespace Qt::StringLiterals;

namespace {

QJsonArray one(const QString& key, const QJsonValue& value)
{
    return {QJsonObject {{u"key"_s, key}, {u"value"_s, value}}};
}

QJsonArray adding(const QString& key, const QJsonValue& items)
{
    return {QJsonObject {{u"key"_s, key}, {u"add"_s, items}}};
}

QJsonArray removing(const QString& key, const QJsonValue& items)
{
    return {QJsonObject {{u"key"_s, key}, {u"remove"_s, items}}};
}

State defaultState()
{
    return {Settings::defaults(), false};
}

// What the app tells the catalog, as far as tests can: every shortcut that
// parses is one Windows takes, nothing is elevated, one favorite has a keyword.
Env appEnv()
{
    Env env;
    env.hotkeyValid = [](const QString& h) { return !Settings::normalizedHotkey(h).isEmpty(); };
    env.isAdministrator = [] { return true; };
    env.isElevated = [] { return false; };
    env.fileManagerAvailable = [](const QString&) { return true; };
    env.keywordOwner = [](const QString& k) { return k == u"zzfav" ? u"My favorite"_s : QString(); };
    return env;
}

// A value `entry` accepts and does not have yet, for the round trip.
QJsonValue sampleFor(const aisettings::Entry& e, State& s, int variant)
{
    switch (e.type) {
    case Type::Bool:
        return !e.flag(s);
    case Type::Choice: {
        std::vector<QString> usable;
        for (const Choice& c : e.choices) {
            if (c.value != u"custom" && c.value != u"direct" && c.value != e.text(s))
                usable.push_back(c.value);
        }
        return usable.empty() ? QJsonValue(e.text(s)) : QJsonValue(usable[static_cast<std::size_t>(variant) % usable.size()]);
    }
    case Type::Int:
        return variant == 0 ? e.max : e.min + 1;
    case Type::Number:
        return variant == 0 ? 2.5 : 0.5; // 512 KB
    case Type::Text:
        return u"zzkw"_s;
    case Type::Hotkey:
        return u"Ctrl+Alt+F9"_s;
    case Type::List: {
        switch (e.list) {
        case ListKind::Programs:
            return QJsonArray {u"Sample.exe"_s, u"other"_s}; // "other" becomes other.exe
        case ListKind::Extensions:
        case ListKind::ContentExtensions:
            return QJsonArray {u"zzz"_s, u"yyy"_s};
        case ListKind::Names:
            return QJsonArray {u"sample_folder"_s};
        case ListKind::FileNames:
            return QJsonArray {u"~$*"_s, u"Sample*.tmp"_s};
        case ListKind::Folders:
            return QJsonArray {u"D:\\Sample\\Folder"_s};
        case ListKind::LocalFolders: // on a disk of this PC's whatever the test machine is
            return QJsonArray {u"C:\\Sample\\Folder"_s};
        }
        break;
    }
    case Type::WebShortcuts:
        break;
    }
    return {};
}

QString problemText(const Plan& plan)
{
    return plan.problems.join(u'\n');
}

} // namespace

class AiSettingsTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_profile; // before anything saves: never the user's own file

private slots:
    void initTestCase()
    {
        QVERIFY(m_profile.isValid());
        Settings::useProfile(m_profile.path());
    }

    void tableIsSound()
    {
        QStringList keys;
        for (const aisettings::Entry& e : catalog()) {
            QVERIFY2(!keys.contains(e.key, Qt::CaseInsensitive), qPrintable(u"twice: "_s + e.key));
            keys.push_back(e.key);
            if (e.access == Access::Hidden)
                continue;
            QVERIFY2(!e.about.isEmpty(), qPrintable(e.key));
            QVERIFY2(e.title, qPrintable(e.key));
            QVERIFY2(sectionOf(e.section), qPrintable(e.key));
            if (e.type == Type::Choice)
                QVERIFY2(!e.choices.empty(), qPrintable(e.key));
            if (e.writable())
                QVERIFY2(e.bound(), qPrintable(e.key));
        }
        // The settings window's pages are the sections'.
        for (const Section& s : sections())
            QVERIFY2(s.page >= 0 && s.page <= 10, qPrintable(s.id));
    }

    // What AI must never write, whatever else changes: each is not writable
    // (hidden or read-only), and a change to it is refused as for a key
    // that does not exist (hidden) or one that can only be read.
    void neverWritable_data()
    {
        QTest::addColumn<QString>("key");
        for (const char* key : {"Mcp/Enabled", "Mcp/AllowOpen", "Mcp/Clipboard", "Mcp/Screenshot", "Mcp/ExcludedPaths",
                 "Mcp/Plugins", "Mcp/PluginActions", "Mcp/Settings", "Plugin.winshun.tools/Enabled", "Plugin.x/Setting.token",
                 "Plugins/DeveloperMode", "Launcher/FileManagerOpenCommand", "Launcher/FileManagerRevealCommand", "Backup/Folder",
                 "Index/Folder", "Shares/size", "Shares/1\\Path", "Capture/PrintScreenBefore", "Launcher/FolderMenuOpusBefore",
                 "Launcher/DialogAutoJumpAsked", "Network/Threads", "Proxy/Mode", "Proxy/Address"})
            QTest::newRow(key) << QString::fromLatin1(key);
    }
    void neverWritable()
    {
        QFETCH(QString, key);
        const aisettings::Entry* e = find(key);
        QVERIFY2(!e || !e->writable(), qPrintable(key));
        const State state = defaultState();
        const Plan plan = makePlan(state, one(key, true), {});
        QVERIFY2(!plan.problems.isEmpty(), qPrintable(key));
        QVERIFY(plan.items.empty());
        QCOMPARE(plan.result, state);
        // A hidden key is told exactly as one that is none.
        if (!e || !visible(*e)) {
            const Plan unknown = makePlan(state, one(u"No/SuchKey"_s, true), {});
            QCOMPARE(QString(plan.problems.first()).replace(key, u"X"_s), QString(unknown.problems.first()).replace(u"No/SuchKey"_s, u"X"_s));
        }
        // And is not listed (as a whole key: Index/Folders is not Index/Folder).
        const QByteArray listed = QJsonDocument(describe(state, {})).toJson();
        QVERIFY2(!listed.contains('"' + key.toUtf8() + '"') || (e && visible(*e)), qPrintable(key));
    }

    void hiddenAndSecretsAreNotListed()
    {
        State state = defaultState();
        state.settings.mcpExcludedPaths = {u"C:\\TopSecretFolder"_s};
        state.settings.backupFolder = u"E:\\MyBackups"_s;
        state.settings.indexFolder = u"F:\\MyIndex"_s;
        state.settings.shares = {{u"\\\\nas\\private"_s, u"Private"_s}};
        state.settings.fileManagerOpenCommand = u"\"C:\\Tools\\fm.exe\" {path}"_s;
        const QByteArray all = QJsonDocument(describe(state, {})).toJson();
        for (const char* secret : {"TopSecretFolder", "MyBackups", "MyIndex", "\"Private\"", "Mcp/"})
            QVERIFY2(!all.contains(secret), secret);
        // The commands are listed, as read-only.
        Query q;
        q.keys = {u"Launcher/FileManagerOpenCommand"_s};
        const QJsonArray rows = describe(state, q).value(u"settings"_s).toArray();
        QCOMPARE(rows.size(), 1);
        QVERIFY(!rows.first().toObject().value(u"writable"_s).toBool());
        for (const aisettings::Entry& e : catalog()) {
            if (e.access == Access::Hidden) {
                q.keys = {e.key};
                QVERIFY2(describe(state, q).value(u"settings"_s).toArray().isEmpty(), qPrintable(e.key));
            }
        }
    }

    // Every writable entry: a valid value, through the catalog, saved and
    // loaded, reads back the same.
    void writableRoundTrip()
    {
        int checked = 0;
        for (const aisettings::Entry& e : catalog()) {
            if (!e.writable() || e.type == Type::WebShortcuts)
                continue;
            for (int variant = 0; variant < 2; ++variant) {
                State state = defaultState();
                const QJsonValue value = sampleFor(e, state, variant);
                QVERIFY2(!value.isNull() && !value.isUndefined(), qPrintable(e.key));
                const Plan plan = makePlan(state, one(e.key, value), {});
                QVERIFY2(plan.problems.isEmpty(), qPrintable(e.key + u": "_s + problemText(plan)));
                QCOMPARE(plan.items.size(), std::size_t {1});
                State result = plan.result;
                const QJsonValue stored = valueOf(e, result);
                QCOMPARE(plan.items.front().newValue, stored);
                if (e.key == u"Launcher/Autostart") {
                    QVERIFY(result.autostart != state.autostart);
                    ++checked;
                    continue; // not in the file
                }
                result.settings.save();
                State loaded;
                loaded.settings.load();
                QCOMPARE(valueOf(e, loaded), stored); // the qPrintable below names it on failure
                if (!QTest::currentTestFailed())
                    ++checked;
                else
                    QFAIL(qPrintable(e.key));
            }
        }
        QVERIFY(checked > 60);
    }

    void webShortcutsRoundTrip()
    {
        const aisettings::Entry* e = find(u"WebSearch/Shortcuts"_s);
        QVERIFY(e && e->access == Access::AlwaysAsk);
        State state = defaultState();
        const qsizetype before = state.settings.webShortcuts.size();
        const Plan plan = makePlan(state,
            adding(e->key, QJsonObject {{u"keyword"_s, u"zzgh"_s}, {u"name"_s, u"GitHub"_s}, {u"url"_s, u"github.com/search?q=%s"_s}}), {});
        QVERIFY2(plan.problems.isEmpty(), qPrintable(problemText(plan)));
        QVERIFY(plan.askAlways());
        QVERIFY2(plan.items.front().addedText.contains(u"zzgh (GitHub) → https://github.com/search?q=%s"_s), qPrintable(plan.items.front().addedText));
        QCOMPARE(plan.result.settings.webShortcuts.size(), before + 1);
        plan.result.settings.save();
        State loaded;
        loaded.settings.load();
        QCOMPARE(loaded.settings.webShortcuts, plan.result.settings.webShortcuts);
        // Taken out again; one that is not there is no change.
        const Plan gone = makePlan(plan.result, removing(e->key, QJsonArray {u"ZZGH"_s}), {});
        QVERIFY(gone.problems.isEmpty());
        QCOMPARE(gone.result.settings.webShortcuts.size(), before);
        QVERIFY(gone.items.front().removedText.startsWith(u"− zzgh"_s));
        const Plan nothing = makePlan(plan.result, removing(e->key, QJsonArray {u"nope"_s}), {});
        QVERIFY(!nothing.changed());
    }

    void webShortcutsAreChecked()
    {
        const QString key = u"WebSearch/Shortcuts"_s;
        const State state = defaultState();
        const auto add = [&](const QJsonObject& o, const Env& env = {}) { return makePlan(state, adding(key, o), env); };
        const QString existing = state.settings.webShortcuts.first().keyword;
        QVERIFY(problemText(add({{u"keyword"_s, existing}, {u"url"_s, u"https://x.org/%s"_s}})).contains(u"already"));
        QVERIFY(!add({{u"keyword"_s, u"a b"_s}, {u"url"_s, u"https://x.org"_s}}).problems.isEmpty());
        QVERIFY(!add({{u"keyword"_s, u"zz"_s}}).problems.isEmpty()); // no url
        QVERIFY(!add({{u"keyword"_s, u"zz"_s}, {u"url"_s, u"file:///C:/Windows/notepad.exe"_s}}).problems.isEmpty());
        QVERIFY(!add({{u"keyword"_s, u"zz"_s}, {u"url"_s, u"ms-msdt:/id x"_s}}).problems.isEmpty()); // only pages
        QVERIFY(!add({{u"keyword"_s, u"zz"_s}, {u"url"_s, u"https://x.org"_s}, {u"typo"_s, 1}}).problems.isEmpty());
        Env env;
        env.keywordOwner = [](const QString& k) { return k == u"zzfav" ? u"My favorite"_s : QString(); };
        QVERIFY(problemText(add({{u"keyword"_s, u"zzfav"_s}, {u"url"_s, u"https://x.org"_s}}, env)).contains(u"My favorite"));
        State capture = state;
        capture.settings.captureEnabled = true;
        QVERIFY(!makePlan(capture, adding(key, QJsonObject {{u"keyword"_s, capture.settings.captureKeyword}, {u"url"_s, u"https://x.org"_s}}), {})
                     .problems.isEmpty());
        QVERIFY(!makePlan(state, one(key, QJsonArray()), {}).problems.isEmpty()); // value: not for this one
    }

    // A value Settings::load() would reject or clamp is a problem, not a change.
    void invalidValuesAreRejected_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QJsonValue>("value");
        QTest::addColumn<QString>("mention"); // what the message names
        QTest::newRow("bool as string") << u"Launcher/DoubleCtrl"_s << QJsonValue(u"yes"_s) << u"true or false"_s;
        QTest::newRow("bool as number") << u"Launcher/DoubleCtrl"_s << QJsonValue(1) << u"true or false"_s;
        QTest::newRow("unknown choice") << u"Results/Sort"_s << QJsonValue(u"alphabetical"_s) << u"match, newest, oldest"_s;
        QTest::newRow("choice as number") << u"Results/Sort"_s << QJsonValue(3) << u"match, newest, oldest"_s;
        QTest::newRow("theme") << u"Appearance/Theme"_s << QJsonValue(u"blue"_s) << u"system, light, dark"_s;
        QTest::newRow("items too few") << u"Clipboard/MaxItems"_s << QJsonValue(5) << u"10 to 100000"_s;
        QTest::newRow("items too many") << u"Clipboard/MaxItems"_s << QJsonValue(100001) << u"10 to 100000"_s;
        QTest::newRow("items fraction") << u"Clipboard/MaxItems"_s << QJsonValue(10.5) << u"whole number"_s;
        QTest::newRow("items text") << u"Clipboard/MaxItems"_s << QJsonValue(u"many"_s) << u"whole number"_s;
        QTest::newRow("days negative") << u"Clipboard/MaxDays"_s << QJsonValue(-1) << u"1 to 3650"_s;
        QTest::newRow("days too many") << u"Clipboard/MaxDays"_s << QJsonValue(4000) << u"1 to 3650"_s;
        QTest::newRow("size too big") << u"Content/MaxFileSizeMB"_s << QJsonValue(5000) << u"4096"_s;
        QTest::newRow("size zero") << u"Content/MaxFileSizeMB"_s << QJsonValue(0) << u"4096"_s;
        QTest::newRow("size text") << u"Content/MaxFileSizeMB"_s << QJsonValue(u"big"_s) << u"4096"_s;
        QTest::newRow("hotkey number") << u"Launcher/Hotkey"_s << QJsonValue(5) << u"string"_s;
        QTest::newRow("list as number") << u"Clipboard/ExcludedApps"_s << QJsonValue(5) << u"strings"_s;
        QTest::newRow("list item number") << u"Clipboard/ExcludedApps"_s << QJsonValue(QJsonArray {1}) << u"strings"_s;
        QTest::newRow("folder relative") << u"Index/ExcludedPaths"_s << QJsonValue(QJsonArray {u"Games"_s}) << u"full folder path"_s;
        QTest::newRow("folder dotdot") << u"Index/ExcludedPaths"_s << QJsonValue(QJsonArray {u"D:\\a\\..\\b"_s}) << u"full folder path"_s;
        QTest::newRow("extension empty") << u"Content/Extensions"_s << QJsonValue(QJsonArray {u"."_s}) << u"extension"_s;
        QTest::newRow("direct access") << u"Index/Access"_s << QJsonValue(u"direct"_s) << u"restarts"_s;
        QTest::newRow("capture keyword blank") << u"Capture/Keyword"_s << QJsonValue(u" "_s) << u"empty"_s;
        QTest::newRow("capture keyword space") << u"Capture/Keyword"_s << QJsonValue(u"a b"_s) << u"spaces"_s;
        QTest::newRow("read only") << u"Launcher/FileManagerOpenCommand"_s << QJsonValue(u"cmd.exe /c x"_s) << u"only be read"_s;
    }
    void invalidValuesAreRejected()
    {
        QFETCH(QString, key);
        QFETCH(QJsonValue, value);
        QFETCH(QString, mention);
        const Plan plan = makePlan(defaultState(), one(key, value), {});
        QVERIFY2(!plan.problems.isEmpty(), "accepted");
        QVERIFY2(problemText(plan).contains(mention), qPrintable(problemText(plan)));
        QVERIFY(plan.items.empty());
    }

    void boundariesAreAccepted()
    {
        const auto ok = [](const QString& key, const QJsonValue& v) {
            const Plan plan = makePlan(defaultState(), one(key, v), {});
            return plan.problems.isEmpty();
        };
        QVERIFY(ok(u"Clipboard/MaxItems"_s, 0)); // no limit
        QVERIFY(ok(u"Clipboard/MaxItems"_s, 10));
        QVERIFY(ok(u"Clipboard/MaxItems"_s, 100000));
        QVERIFY(!ok(u"Clipboard/MaxItems"_s, 9));
        QVERIFY(ok(u"Clipboard/MaxDays"_s, 0));
        QVERIFY(ok(u"Clipboard/MaxDays"_s, 3650));
        QVERIFY(ok(u"Content/MaxFileSizeMB"_s, 4096));
        QVERIFY(ok(u"Content/MaxFileSizeMB"_s, 1.0 / 1024));
        QVERIFY(!ok(u"Content/MaxFileSizeMB"_s, 1.0 / 2048));
        // Choices as load() reads them: case and spaces do not matter.
        QVERIFY(ok(u"Appearance/Theme"_s, u" Dark "_s));
        // Sizes are kept as the settings window keeps them.
        const Plan plan = makePlan(defaultState(), one(u"Content/MaxFileSizeMB"_s, 1.234), {});
        QCOMPARE(plan.items.front().newValue.toDouble(), 1.23);
    }

    void hotkeysAreCheckedByTheApp()
    {
        Env env;
        env.hotkeyValid = [](const QString& h) { return h.contains(u'+'); };
        QVERIFY(makePlan(defaultState(), one(u"Launcher/Hotkey"_s, u"Alt+Space"_s), env).problems.isEmpty());
        QVERIFY(makePlan(defaultState(), one(u"Launcher/Hotkey"_s, u""_s), env).problems.isEmpty()); // none
        const Plan bad = makePlan(defaultState(), one(u"Launcher/Hotkey"_s, u"Banana"_s), env);
        QVERIFY(problemText(bad).contains(u"Banana"));
    }

    // The settings window's recorder takes Ctrl, Alt or Win with a key, or an F key;
    // AI no more: no bare key, no Shift and a letter.
    void hotkeysNeedAModifier_data()
    {
        QTest::addColumn<QString>("hotkey");
        QTest::addColumn<bool>("accepted");
        for (const char* ok : {"Alt+Space", "Ctrl+Alt+D", "Win+V", "ctrl+shift+a", "Alt+F4", "F5", "Shift+F5", "F24", "Control+Alt+`", "Win+Shift+S"})
            QTest::newRow(ok) << QString::fromLatin1(ok) << true;
        for (const char* bad : {"Enter", "Space", "A", "a", "7", "PrintScreen", "Shift+A", "Shift+Enter", "Shift+Space", "Tab", "F25", "F0",
                 "Ctrl", "Ctrl+Alt", "Ctrl+A+B", "Banana", "Ctrl+Banana"})
            QTest::newRow(bad) << QString::fromLatin1(bad) << false;
    }
    void hotkeysNeedAModifier()
    {
        QFETCH(QString, hotkey);
        QFETCH(bool, accepted);
        QCOMPARE(Settings::hotkeyAllowed(hotkey), accepted);
        // Whatever the app's own check says, and with none.
        for (const Env& env : {appEnv(), Env {}}) {
            for (const char* key : {"Launcher/Hotkey", "Launcher/FolderMenuHotkey", "Clipboard/Hotkey", "Capture/Hotkey"}) {
                const Plan plan = makePlan(defaultState(), one(QString::fromLatin1(key), hotkey), env);
                QVERIFY2(plan.problems.isEmpty() == accepted, qPrintable(hotkey + u" for "_s + QString::fromLatin1(key) + u": "_s + problemText(plan)));
            }
        }
        // Taking a shortcut away is always fine.
        QVERIFY(makePlan(defaultState(), one(u"Launcher/Hotkey"_s, u""_s), appEnv()).problems.isEmpty());
    }

    void normalizedHotkeysCompare()
    {
        QCOMPARE(Settings::normalizedHotkey(u"Alt+Ctrl+V"_s), Settings::normalizedHotkey(u"control + ALT + v"_s));
        QCOMPARE(Settings::normalizedHotkey(u"Meta+Return"_s), u"win+enter"_s);
        QVERIFY(Settings::normalizedHotkey(u"Ctrl+A+B"_s).isEmpty());
        QVERIFY(Settings::normalizedHotkey(u"Ctrl+"_s).isEmpty());
    }

    // One shortcut has one use among Win顺's own settings, as they are after the batch.
    void hotkeysDiffer()
    {
        State state = defaultState();
        state.settings.hotkey = u"Alt+Space"_s;
        state.settings.clipboardHotkey = u"Ctrl+Alt+V"_s;
        const Env env = appEnv();
        Plan plan = makePlan(state, one(u"Launcher/FolderMenuHotkey"_s, u"alt+ctrl+v"_s), env);
        QVERIFY(problemText(plan).contains(u"already the shortcut of"));
        QVERIFY(problemText(plan).contains(u"Clipboard/Hotkey"));
        // One taken off in the same batch is free for the other.
        plan = makePlan(state, QJsonArray {QJsonObject {{u"key"_s, u"Clipboard/Hotkey"_s}, {u"value"_s, u""_s}},
                                 QJsonObject {{u"key"_s, u"Launcher/FolderMenuHotkey"_s}, {u"value"_s, u"Ctrl+Alt+V"_s}}}, env);
        QVERIFY2(plan.problems.isEmpty(), qPrintable(problemText(plan)));
        // Both to the same one at once.
        plan = makePlan(state, QJsonArray {QJsonObject {{u"key"_s, u"Clipboard/Hotkey"_s}, {u"value"_s, u"Ctrl+Alt+Z"_s}},
                                 QJsonObject {{u"key"_s, u"Launcher/FolderMenuHotkey"_s}, {u"value"_s, u"Alt+Ctrl+Z"_s}}}, env);
        QVERIFY(!plan.problems.isEmpty());
        // The screenshot shortcut counts while capture is on.
        state.settings.captureHotkey = u"Ctrl+Alt+A"_s;
        QVERIFY(makePlan(state, one(u"Clipboard/Hotkey"_s, u"Ctrl+Alt+A"_s), env).problems.isEmpty());
        state.settings.captureEnabled = true;
        QVERIFY(!makePlan(state, one(u"Clipboard/Hotkey"_s, u"Ctrl+Alt+A"_s), env).problems.isEmpty());
    }

    // The card shows all that is applied: name, address and home of a web shortcut.
    void webShortcutShownInFull()
    {
        const Plan plan = makePlan(defaultState(), adding(u"WebSearch/Shortcuts"_s,
            QJsonObject {{u"keyword"_s, u"zzgh"_s}, {u"name"_s, u"My Hub"_s}, {u"url"_s, u"https://hub.example/q=%s"_s},
                {u"home"_s, u"https://home.example/"_s}}), appEnv());
        QVERIFY2(plan.problems.isEmpty(), qPrintable(problemText(plan)));
        const QString shown = plan.items.front().addedText;
        for (const char* part : {"zzgh", "My Hub", "https://hub.example/q=%s", "https://home.example/"})
            QVERIFY2(shown.contains(QLatin1StringView(part)), part);
        QCOMPARE(plan.result.settings.webShortcuts.last().name, u"My Hub"_s);
        QCOMPARE(plan.result.settings.webShortcuts.last().home, u"https://home.example/"_s);
        // No name given: the site's, as the settings window derives it (one function for both).
        const Plan unnamed = makePlan(defaultState(), adding(u"WebSearch/Shortcuts"_s,
            QJsonObject {{u"keyword"_s, u"zzgh2"_s}, {u"url"_s, u"https://www.hub.example/q=%s"_s}}), appEnv());
        QCOMPARE(unnamed.result.settings.webShortcuts.last().name, u"hub.example"_s);
    }

    // What goes out is shown first and never cut away: a long change is summed up.
    void removalsComeFirstAndOverflowIsCounted()
    {
        State state = defaultState();
        QStringList old;
        for (int i = 0; i < 30; ++i)
            old.push_back(u"old_folder_%1"_s.arg(i));
        state.settings.excludedNames = old;
        QJsonArray fresh;
        for (int i = 0; i < 30; ++i)
            fresh.push_back(u"new_folder_%1"_s.arg(i));
        const Plan plan = makePlan(state, one(u"Index/ExcludedNames"_s, fresh), appEnv());
        QVERIFY2(plan.problems.isEmpty(), qPrintable(problemText(plan)));
        const Item& item = plan.items.front();
        QVERIFY(item.removedText.startsWith(u"− old_folder_0"_s));
        QVERIFY(item.addedText.startsWith(u"+ new_folder_0"_s));
        QVERIFY2(item.removedText.contains(u"还有 "), qPrintable(item.removedText));
        QVERIFY(item.removedText.size() < 260 && item.addedText.size() < 260);
        QVERIFY(item.diffText.startsWith(item.removedText)); // what goes comes first
        QVERIFY(item.askAlways); // names taken out
        const QJsonObject json = itemJson(item);
        QCOMPARE(json.value(u"removed_text"_s).toString(), item.removedText);
        // One very long item is cut.
        State s2 = defaultState();
        s2.settings.excludedNames = {QString(250, u'x')};
        QVERIFY(makePlan(s2, one(u"Index/ExcludedNames"_s, QJsonArray()), appEnv()).items.front().removedText.size() < 200);
    }

    // Text AI writes is plain and short: no line breaks, tabs, direction marks, separators.
    void hiddenCharactersAndLengthsAreRefused_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("kind"); // add | value
        QTest::addColumn<QJsonValue>("items");
        const QString names = u"Index/ExcludedNames"_s;
        for (const auto& [label, text] : {std::pair {"newline", u"a\nb"_s}, {"tab", u"a\tb"_s}, {"bidi override", u"gam\u202Ees"_s},
                 {"bidi isolate", u"a\u2066b"_s}, {"zero width", u"a\u200Bb"_s}, {"line separator", u"a\u2028b"_s},
                 {"paragraph separator", u"a\u2029b"_s}, {"nul", QString(u"a\0b"_s)}, {"bell", u"a\u0007b"_s},
                 {"tag character", u"a\U000E0041b"_s}, {"lone surrogate", QString(QChar(0xD800))}, {"too long", QString(261, u'x')}}) {
            QTest::addRow("names add %s", label) << names << u"add"_s << QJsonValue(QJsonArray {text});
            QTest::addRow("names value %s", label) << names << u"value"_s << QJsonValue(QJsonArray {text});
            QTest::addRow("programs %s", label) << u"Clipboard/ExcludedApps"_s << u"add"_s << QJsonValue(QJsonArray {text});
            QTest::addRow("extensions %s", label) << u"Content/Extensions"_s << u"add"_s << QJsonValue(QJsonArray {text});
        }
        QTest::newRow("folder newline") << u"Index/ExcludedPaths"_s << u"add"_s << QJsonValue(QJsonArray {u"D:\\a\nb"_s});
        QTest::newRow("folder too long") << u"Index/ExcludedPaths"_s << u"add"_s << QJsonValue(QJsonArray {u"D:\\"_s + QString(1030, u'x')});
        QJsonArray many;
        for (int i = 0; i < 51; ++i)
            many.push_back(u"name%1"_s.arg(i));
        QTest::newRow("too many items") << names << u"add"_s << QJsonValue(many);
        QTest::newRow("too many items in value") << names << u"value"_s << QJsonValue(many);
    }
    void hiddenCharactersAndLengthsAreRefused()
    {
        QFETCH(QString, key);
        QFETCH(QString, kind);
        QFETCH(QJsonValue, items);
        const QJsonArray changes {QJsonObject {{u"key"_s, key}, {kind, items}}};
        const Plan plan = makePlan(defaultState(), changes, appEnv());
        QVERIFY2(!plan.problems.isEmpty(), "accepted");
        QVERIFY2(problemText(plan).contains(key), qPrintable(problemText(plan))); // the offending key is named
        QVERIFY(plan.items.empty());
        // 50 plain ones are fine.
        QJsonArray fifty;
        for (int i = 0; i < 50; ++i)
            fifty.push_back(u"name%1"_s.arg(i));
        QVERIFY(makePlan(defaultState(), one(u"Index/ExcludedNames"_s, fifty), appEnv()).problems.isEmpty());
    }

    void webShortcutTextIsChecked()
    {
        const QString key = u"WebSearch/Shortcuts"_s;
        const auto add = [&](const QJsonObject& o) { return makePlan(defaultState(), adding(key, o), appEnv()); };
        const QJsonObject good {{u"keyword"_s, u"zzh"_s}, {u"url"_s, u"https://x.org/%s"_s}};
        QVERIFY(add(good).problems.isEmpty());
        for (const QString& field : {u"name"_s, u"keyword"_s, u"url"_s, u"home"_s}) {
            for (const QString& bad : {u"a\nb"_s, u"a\u202Eb"_s, u"a\u2028b"_s}) {
                QJsonObject o = good;
                o.insert(field, field == u"url" || field == u"home" ? u"https://x.org/"_s + bad : bad);
                QVERIFY2(!add(o).problems.isEmpty(), qPrintable(field + u" "_s + bad));
            }
        }
        QJsonObject o = good;
        o.insert(u"name"_s, QString(261, u'n'));
        QVERIFY(!add(o).problems.isEmpty());
        o = good;
        o.insert(u"url"_s, u"https://x.org/"_s + QString(1030, u'p'));
        QVERIFY(!add(o).problems.isEmpty());
        QVERIFY(!makePlan(defaultState(), removing(key, QJsonArray {u"a\nb"_s}), appEnv()).problems.isEmpty());
        QJsonArray many;
        for (int i = 0; i < 51; ++i)
            many.push_back(QJsonObject {{u"keyword"_s, u"zzk%1"_s.arg(i)}, {u"url"_s, u"https://x.org/%s"_s}});
        QVERIFY(!makePlan(defaultState(), adding(key, many), appEnv()).problems.isEmpty());
    }

    // What lets AI see more than before asks, in either mode; what shows less does not.
    void wideningAsks()
    {
        const Env env = appEnv();
        State state = defaultState();
        state.settings.excludedNames = {u"node_modules"_s};
        state.settings.clipboardExcludedApps = {u"KeePass.exe"_s};
        state.settings.clipboard = false;
        state.settings.clipboardImages = false;
        state.settings.captureEnabled = false;
        state.settings.includeRemovableDrives = false;
        state.settings.contentInLowPriority = false;
        state.settings.contentSearch = false;
        state.settings.contentDocuments = false;
        for (const char* key : {"Clipboard/Enabled", "Clipboard/Images", "Capture/Enabled", "Index/IncludeRemovableDrives",
                 "Content/IncludeSystemFolders", "Content/Enabled", "Content/Documents"}) {
            QVERIFY2(makePlan(state, one(QString::fromLatin1(key), true), env).askAlways(), key); // turning on
            State on = state;
            on.settings = state.settings;
            const aisettings::Entry* entry = find(QString::fromLatin1(key));
            entry->setFlag(on, true);
            QVERIFY2(!makePlan(on, one(QString::fromLatin1(key), false), env).askAlways(), key); // turning off
            QVERIFY2(!makePlan(on, one(QString::fromLatin1(key), true), env).askAlways(), key); // so already
        }
        QVERIFY(makePlan(state, removing(u"Index/ExcludedNames"_s, u"node_modules"_s), env).askAlways());
        QVERIFY(makePlan(state, one(u"Index/ExcludedNames"_s, QJsonArray {u"other"_s}), env).askAlways()); // replaced: one goes
        QVERIFY(!makePlan(state, adding(u"Index/ExcludedNames"_s, u"zzz"_s), env).askAlways());
        State typed = state; // skipped file types: taking one out shows its files again
        typed.settings.excludedTypes = {u"bak"_s};
        QVERIFY(makePlan(typed, removing(u"Index/ExcludedTypes"_s, u"bak"_s), env).askAlways());
        QVERIFY(!makePlan(typed, adding(u"Index/ExcludedTypes"_s, u"wbk"_s), env).askAlways());
        QVERIFY(makePlan(state, removing(u"Clipboard/ExcludedApps"_s, u"keepass"_s), env).askAlways());
        QVERIFY(!makePlan(state, adding(u"Clipboard/ExcludedApps"_s, u"Other.exe"_s), env).askAlways());
        // The folders indexed (GitHub #52): putting one in shows its files,
        // taking one out does not; a network place is refused (网络共享's).
        State scoped = state;
        scoped.settings.indexFolders = {u"C:\\Work"_s, u"E:\\"_s};
        QVERIFY(makePlan(scoped, adding(u"Index/Folders"_s, u"C:\\Other"_s), env).askAlways());
        QVERIFY(makePlan(scoped, one(u"Index/Folders"_s, QJsonArray {u"C:\\Work"_s, u"C:\\Other"_s}), env).askAlways());
        QVERIFY(!makePlan(scoped, removing(u"Index/Folders"_s, u"C:\\Work"_s), env).askAlways());
        QVERIFY(!makePlan(scoped, adding(u"Index/Folders"_s, u"c:\\work\\"_s), env).changed()); // so already
        QVERIFY(!makePlan(scoped, adding(u"Index/Folders"_s, u"E:\\"_s), env).changed());
        QVERIFY(problemText(makePlan(scoped, adding(u"Index/Folders"_s, u"\\\\nas\\docs"_s), env)).contains(u"网络共享"));
        // Changed, the list is the user's from then on, as the file will have it (#52 review).
        QVERIFY(!scoped.settings.indexFoldersChosen);
        QVERIFY(makePlan(scoped, adding(u"Index/Folders"_s, u"C:\\Other"_s), env).result.settings.indexFoldersChosen);
        QVERIFY(!makePlan(scoped, adding(u"Index/Folders"_s, u"E:\\"_s), env).result.settings.indexFoldersChosen);
        QVERIFY(makePlan(scoped, one(u"Index/Scope"_s, u"folders"_s), env).askAlways()); // every change
        QVERIFY(!makePlan(scoped, one(u"Index/FolderVerifyHours"_s, 4), env).askAlways());
        // A switch that shows nothing more is not asked.
        QVERIFY(!makePlan(state, one(u"Clipboard/ClickToPaste"_s, !state.settings.clipboardClickToPaste), env).askAlways());
        // The model is told which ones ask.
        Query q;
        q.keys = {u"Clipboard/Enabled"_s};
        QVERIFY(describe(state, q).value(u"settings"_s).toArray().first().toObject().contains(u"ask_always"_s));
    }

    // A list's value is an array: a bare string is no way to say "just this one".
    void listValueNeedsAnArray()
    {
        for (const char* key : {"Index/ExcludedNames", "Clipboard/ExcludedApps", "Content/Extensions", "Index/ExcludedPaths", "Index/ExcludedTypes"}) {
            const Plan plan = makePlan(defaultState(), one(QString::fromLatin1(key), u"x"_s), appEnv());
            QVERIFY2(problemText(plan).contains(u"array"), key);
            QVERIFY2(problemText(plan).contains(u"add"), key); // the hint
        }
        QVERIFY(makePlan(defaultState(), adding(u"Index/ExcludedNames"_s, u"x"_s), appEnv()).problems.isEmpty()); // add takes one
    }

    // Index/Access restarts Win顺 when it runs elevated: no change then.
    void indexAccessNotWhileElevated()
    {
        Env env = appEnv();
        State state = defaultState();
        QVERIFY(makePlan(state, one(u"Index/Access"_s, u"walk"_s), env).problems.isEmpty());
        env.isElevated = [] { return true; };
        const Plan plan = makePlan(state, one(u"Index/Access"_s, u"walk"_s), env);
        QVERIFY2(problemText(plan).contains(u"administrator rights"), qPrintable(problemText(plan)));
        QVERIFY(plan.items.empty());
        QVERIFY(makePlan(state, one(u"Index/Access"_s, state.settings.indexAccess), env).problems.isEmpty()); // so already
        QVERIFY(makePlan(state, one(u"Results/Sort"_s, u"newest"_s), env).problems.isEmpty()); // others are not affected
    }

    // The settings window's own setter runs its side effect on a real change only.
    void afterHookRunsOnRealChangeOnly()
    {
        State state = defaultState();
        state.settings.dialogAutoJump = false;
        state.settings.dialogAutoJumpAsked = false;
        Plan plan = makePlan(state, one(u"Launcher/DialogAutoJump"_s, false), appEnv()); // so already
        QVERIFY(!plan.changed());
        QVERIFY(!plan.result.settings.dialogAutoJumpAsked);
        QCOMPARE(plan.result, state);
        plan = makePlan(state, one(u"Launcher/DialogAutoJump"_s, true), appEnv());
        QVERIFY(plan.result.settings.dialogAutoJumpAsked);
        // Not in what the model reads: the hidden key stays hidden.
        QVERIFY(!QJsonDocument(describe(plan.result, {})).toJson().contains("DialogAutoJumpAsked"));
    }

    // The folder menu takes what Settings::load() takes: "on" too (not while-Listary-aware).
    void folderMenuTakesOn()
    {
        const Plan plan = makePlan(defaultState(), one(u"Launcher/FolderMenu"_s, u"on"_s), appEnv());
        QVERIFY2(plan.problems.isEmpty(), qPrintable(problemText(plan)));
        plan.result.settings.save();
        State loaded;
        loaded.settings.load();
        QCOMPARE(loaded.settings.folderMenu, u"on"_s);
        const aisettings::Entry* entry = find(u"Launcher/FolderMenu"_s);
        const auto it = std::ranges::find_if(entry->choices, [](const Choice& c) { return c.value == u"on"; });
        QVERIFY(it != entry->choices.end() && !it->meaning.isEmpty());
        QVERIFY(makePlan(defaultState(), one(u"Launcher/FolderMenu"_s, u"auto"_s), appEnv()).problems.isEmpty());
        QVERIFY(!makePlan(defaultState(), one(u"Launcher/FolderMenu"_s, u"sometimes"_s), appEnv()).problems.isEmpty());
    }

    // The same helpers as the settings window (Settings' statics) give the same items.
    void listItemsAreTheSettingsWindowsOwn()
    {
        QCOMPARE(Settings::programFileName(u"C:\\x\\KeePass"_s), u"KeePass.exe"_s);
        QCOMPARE(Settings::excludedFolderName(u" a\\b\\ "_s), u"a/b"_s);
        QCOMPARE(Settings::contentExtensionList(u"*.md, .LOG tar.gz md"_s), (QStringList {u"md"_s, u"log"_s, u"tar.gz"_s}));
        const Plan plan = makePlan(defaultState(), adding(u"Index/ExcludedNames"_s, u"a\\b\\"_s), appEnv());
        QCOMPARE(plan.result.settings.excludedNames.last(), Settings::excludedFolderName(u"a\\b\\"_s));
    }

    // All or nothing, every problem named, the same key not twice.
    void batchesAreChecked()
    {
        const State state = defaultState();
        QJsonArray changes {QJsonObject {{u"key"_s, u"Launcher/DoubleCtrl"_s}, {u"value"_s, false}},
            QJsonObject {{u"key"_s, u"Results/Sort"_s}, {u"value"_s, u"nope"_s}},
            QJsonObject {{u"key"_s, u"Mcp/Enabled"_s}, {u"value"_s, true}},
            QJsonObject {{u"key"_s, u"Launcher/DoubleCtrl"_s}, {u"value"_s, true}},
            QJsonObject {{u"key"_s, u"Results/FoldersFirst"_s}},
            QJsonObject {{u"key"_s, u"Results/FoldersFirst"_s}, {u"value"_s, true}, {u"add"_s, 1}},
            QJsonObject {{u"value"_s, true}}, QJsonValue(5)};
        const Plan plan = makePlan(state, changes, {});
        QCOMPARE(plan.problems.size(), 7); // all but the first
        QVERIFY(problemText(plan).contains(u"twice"));
        QVERIFY(!makePlan(state, {}, {}).problems.isEmpty()); // none given
        QVERIFY(!makePlan(state, one(u"Results/Sort"_s, u"newest"_s), {}).askAlways());
    }

    void unchangedAndAskAlways()
    {
        State state = defaultState();
        // So already: reported, not a change.
        Plan plan = makePlan(state, one(u"Results/Sort"_s, u"match"_s), {});
        QVERIFY(plan.problems.isEmpty());
        QVERIFY(!plan.changed());
        QVERIFY(!plan.items.front().changed);
        // Always asked, even when nothing else is.
        QVERIFY(makePlan(state, one(u"Launcher/Autostart"_s, !state.autostart), {}).askAlways());
        QVERIFY(makePlan(state, one(u"Index/Access"_s, u"walk"_s), {}).askAlways());
        QVERIFY(!makePlan(state, one(u"Index/Access"_s, u"auto"_s), {}).askAlways()); // so already
        // Taking an item out of what keeps things away from AI asks; putting one in does not.
        state.settings.clipboardExcludedApps = {u"KeePass.exe"_s};
        QVERIFY(makePlan(state, removing(u"Clipboard/ExcludedApps"_s, u"keepass"_s), {}).askAlways());
        QVERIFY(makePlan(state, one(u"Clipboard/ExcludedApps"_s, QJsonArray()), {}).askAlways());
        QVERIFY(!makePlan(state, adding(u"Clipboard/ExcludedApps"_s, u"Other.exe"_s), {}).askAlways());
        QVERIFY(makePlan(state, removing(u"Index/ExcludedPaths"_s, state.settings.excludedPaths.first()), {}).askAlways());
        QVERIFY(!makePlan(state, adding(u"Index/ExcludedPaths"_s, u"D:\\Games"_s), {}).askAlways());
        QVERIFY(!makePlan(state, adding(u"Index/ExcludedNames"_s, u"zzz"_s), {}).askAlways());
        // Leaving 'direct' restarts Win顺 too.
        state.settings.indexAccess = u"direct"_s;
        QVERIFY(!makePlan(state, one(u"Index/Access"_s, u"walk"_s), {}).problems.isEmpty());
    }

    void fileManagerNeedsTheUsersCommand()
    {
        Env env;
        env.fileManagerAvailable = [](const QString& m) { return m != u"xyplorer"; };
        State state = defaultState();
        QVERIFY(problemText(makePlan(state, one(u"Launcher/FileManager"_s, u"custom"_s), env)).contains(u"only the user can set"));
        QVERIFY(problemText(makePlan(state, one(u"Launcher/FileManager"_s, u"xyplorer"_s), env)).contains(u"not installed"));
        QVERIFY(makePlan(state, one(u"Launcher/FileManager"_s, u"totalcmd"_s), env).problems.isEmpty());
        QVERIFY(makePlan(state, one(u"Launcher/FileManager"_s, u"onecommander"_s), env).problems.isEmpty());
        env.fileManagerAvailable = [](const QString&) { return false; };
        QVERIFY(problemText(makePlan(state, one(u"Launcher/FileManager"_s, u"onecommander"_s), env)).contains(u"not installed"));
        env.fileManagerAvailable = [](const QString&) { return true; };
        state.settings.fileManagerOpenCommand = u"fm.exe {path}"_s;
        QVERIFY(makePlan(state, one(u"Launcher/FileManager"_s, u"custom"_s), env).problems.isEmpty());
    }

    void listsAreDeduplicatedAndNormalised()
    {
        State state = defaultState();
        state.settings.doubleCtrlExcludedApps = {u"Game.exe"_s};
        Plan plan = makePlan(state, adding(u"Launcher/DoubleCtrlExcludedApps"_s, QJsonArray {u"GAME"_s, u"C:\\x\\Other.EXE"_s, u"other"_s}), {});
        QVERIFY(plan.problems.isEmpty());
        QCOMPARE(plan.result.settings.doubleCtrlExcludedApps, (QStringList {u"Game.exe"_s, u"Other.EXE"_s}));
        QCOMPARE(plan.items.front().diffText, u"+ Other.EXE"_s);
        plan = makePlan(state, one(u"Content/Extensions"_s, QJsonArray {u".PDF"_s, u"*.docx"_s, u"pdf"_s, u".tar.gz"_s}), {});
        QCOMPARE(plan.result.settings.contentExtensions, (QStringList {u"pdf"_s, u"docx"_s, u"tar.gz"_s})); // as the settings window keeps it
        plan = makePlan(state, one(u"Results/PreferredTypes"_s, QJsonArray {u"a.tar.gz"_s}), {});
        QCOMPARE(plan.result.settings.preferredTypes, (QStringList {u"gz"_s})); // this list's own rule
        plan = makePlan(state, adding(u"Index/ExcludedPaths"_s, QJsonArray {u"d:/Games/"_s}), {});
        QVERIFY(plan.result.settings.excludedPaths.contains(u"d:\\Games"_s));
        plan = makePlan(plan.result, adding(u"Index/ExcludedPaths"_s, QJsonArray {u"D:\\games"_s}), {});
        QVERIFY(!plan.changed()); // the same folder
        plan = makePlan(state, adding(u"Launcher/DoubleCtrl"_s, u"x"_s), {});
        QVERIFY(problemText(plan).contains(u"takes a value"));
        plan = makePlan(state, one(u"Launcher/DialogAutoJump"_s, true), {});
        QVERIFY(plan.result.settings.dialogAutoJumpAsked); // as the settings window does
    }

    void readingFilters()
    {
        State state = defaultState();
        const auto keys = [&](const Query& q) {
            QStringList out;
            for (const QJsonValue& v : describe(state, q).value(u"settings"_s).toArray())
                out.push_back(v.toObject().value(u"key"_s).toString());
            return out;
        };
        const QStringList all = keys({});
        QVERIFY(all.size() > 60);
        Query q;
        q.sections = {u"clipboard"_s};
        const QStringList clip = keys(q);
        QVERIFY(clip.contains(u"Clipboard/WinV"_s) && !clip.contains(u"Results/Sort"_s));
        q = {};
        q.words = u"剪贴板"_s; // Chinese, from the title
        QVERIFY(keys(q).contains(u"Clipboard/Enabled"_s));
        q.words = u"best match"_s; // from a choice's meaning
        QVERIFY(keys(q).contains(u"Results/Sort"_s));
        q = {};
        q.keys = {u"launcher/doublectrl"_s, u"Mcp/Enabled"_s};
        QCOMPARE(keys(q), (QStringList {u"Launcher/DoubleCtrl"_s}));
        q = {};
        q.changedOnly = true;
        QVERIFY(keys(q).isEmpty());
        state.settings.sort = u"newest"_s;
        QCOMPARE(keys(q), (QStringList {u"Results/Sort"_s}));

        // What a row says.
        q = {};
        q.keys = {u"Clipboard/MaxItems"_s};
        const QJsonObject row = describe(state, q).value(u"settings"_s).toArray().first().toObject();
        QCOMPARE(row.value(u"value"_s).toInt(), 300);
        QCOMPARE(row.value(u"default"_s).toInt(), 300);
        QCOMPARE(row.value(u"min"_s).toInt(), 10);
        QCOMPARE(row.value(u"max"_s).toInt(), 100000);
        QCOMPARE(row.value(u"zero_means"_s).toString(), u"no limit"_s);
        QCOMPARE(row.value(u"writable"_s).toBool(), true);
        q.keys = {u"Index/FolderVerifyHours"_s}; // not only ASCII (GitHub #52)
        const QJsonObject verify = describe(state, q).value(u"settings"_s).toArray().first().toObject();
        QCOMPARE(verify.value(u"zero_means"_s).toString(), u"only when Win顺 starts"_s);
        QCOMPARE(verify.value(u"max"_s).toInt(), 168);
        q.keys = {u"Launcher/Autostart"_s};
        const QJsonObject autostart = describe(state, q).value(u"settings"_s).toArray().first().toObject();
        QVERIFY(autostart.contains(u"ask_always"_s) && !autostart.contains(u"default"_s));
    }

    void userTexts()
    {
        State state = defaultState();
        Plan plan = makePlan(state, one(u"Launcher/DoubleCtrl"_s, false), {});
        QCOMPARE(plan.items.front().oldText, u"开"_s);
        QCOMPARE(plan.items.front().newText, u"关"_s);
        plan = makePlan(state, one(u"Appearance/Theme"_s, u"dark"_s), {});
        QCOMPARE(plan.items.front().newText, u"深色"_s); // the label, never the id
        plan = makePlan(state, one(u"Clipboard/MaxItems"_s, 0), {});
        QCOMPARE(plan.items.front().newText, u"不限"_s);
        plan = makePlan(state, one(u"Content/MaxFileSizeMB"_s, 0.5), {});
        QCOMPARE(plan.items.front().newText, u"512 KB"_s);
        state.settings.clipboardExcludedApps = {u"A.exe"_s, u"B.exe"_s};
        plan = makePlan(state, one(u"Clipboard/ExcludedApps"_s, QJsonArray {u"B.exe"_s, u"C.exe"_s}), {});
        QCOMPARE(plan.items.front().diffText, u"− A.exe\n+ C.exe"_s); // what goes first
        const QJsonObject json = itemJson(plan.items.front());
        QCOMPARE(json.value(u"is_list"_s).toBool(), true);
        QCOMPARE(json.value(u"ask_always"_s).toBool(), true);
        QCOMPARE(json.value(u"effect"_s).toString(), u"now"_s);
        QCOMPARE(itemJson(makePlan(state, one(u"Clipboard/WinV"_s, true), {}).items.front()).value(u"effect"_s).toString(),
            u"explorer_restart"_s);
        QCOMPARE(itemJson(makePlan(state, one(u"Launcher/Renderer"_s, u"software"_s), {}).items.front()).value(u"effect"_s).toString(),
            u"app_restart"_s);
    }

    // An undo puts the old values back as they were.
    void restoreIsExact()
    {
        State state = defaultState();
        state.settings.clipboardExcludedApps = {u"A.exe"_s};
        const QJsonArray changes {QJsonObject {{u"key"_s, u"Clipboard/ExcludedApps"_s}, {u"remove"_s, u"A.exe"_s}},
            QJsonObject {{u"key"_s, u"WebSearch/Shortcuts"_s},
                {u"add"_s, QJsonObject {{u"keyword"_s, u"zzk"_s}, {u"url"_s, u"https://x.org/?q=%s"_s}}}},
            QJsonObject {{u"key"_s, u"Content/MaxFileSizeMB"_s}, {u"value"_s, 3}},
            QJsonObject {{u"key"_s, u"Launcher/Hotkey"_s}, {u"value"_s, u"Ctrl+Alt+K"_s}}};
        const Plan plan = makePlan(state, changes, {});
        QVERIFY2(plan.problems.isEmpty(), qPrintable(problemText(plan)));
        State back = plan.result;
        for (const Item& item : plan.items)
            restore(*item.entry, back, item.oldValue);
        QCOMPARE(back, state);
    }
};

QTEST_GUILESS_MAIN(AiSettingsTest)
#include "tst_aisettings.moc"
