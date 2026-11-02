#include "platform/PluginHotkeys.h"
#include "platform/HotkeyText.h"

#include <QCoreApplication>
#include <QScopeGuard>
#include <QTest>

using namespace Qt::StringLiterals;
using ws::PluginHotkeys;

namespace {
bool available(const QString& key)
{
    UINT modifiers = 0, vk = 0;
    ws::hotkeys::parse(key, &modifiers, &vk);
    if (!::RegisterHotKey(nullptr, 0x7101, modifiers, vk))
        return false;
    ::UnregisterHotKey(nullptr, 0x7101);
    return true;
}
QString freeKey()
{
    for (int n = 24; n >= 13; --n) {
        const QString key = u"Ctrl+Alt+Shift+F%1"_s.arg(n);
        if (available(key))
            return key;
    }
    return {};
}
}

class TestPluginHotkeys : public QObject {
    Q_OBJECT
private slots:
    void validate()
    {
        QVERIFY(PluginHotkeys::valid({u"F1"_s, u"Ctrl+Alt+Z"_s, u"PrintScreen"_s, u"A"_s}));
        QVERIFY(!PluginHotkeys::valid(QStringList(33, u"F1"_s)));
        QVERIFY(!PluginHotkeys::valid({QString(65, u'F')}));
        QVERIFY(PluginHotkeys::allowed(u"F1"_s));
        QVERIFY(PluginHotkeys::allowed(u"Ctrl+Alt+Z"_s));
        QVERIFY(PluginHotkeys::allowed(u"PrintScreen"_s));
        QVERIFY(!PluginHotkeys::allowed(u"A"_s));
        QVERIFY(!PluginHotkeys::allowed(u"Ctrl+A+B"_s));
        QVERIFY(!PluginHotkeys::allowed(u"Shift+Space"_s));
        QVERIFY(!PluginHotkeys::allowed(u"Shift+A"_s));
    }

    // One key edited by hand into something not allowed costs that key only.
    void disallowedKeyLeavesTheOthers()
    {
        const QString key = freeKey();
        QVERIFY(!key.isEmpty());
        PluginHotkeys registry({});
        const QJsonObject errors = registry.replace(u"capture"_s, {u"Shift+A"_s, key});
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.value(u"Shift+A"_s).toInt(), ERROR_INVALID_PARAMETER);
        QVERIFY(!available(key));
        registry.remove(u"capture"_s);
        QVERIFY(available(key));
    }

    // The same key twice: no error under the spelling of the one holding it.
    void repeatedKey()
    {
        const QString key = freeKey();
        QVERIFY(!key.isEmpty());
        PluginHotkeys registry({});
        QVERIFY(registry.replace(u"capture"_s, {key, key}).isEmpty());
        QVERIFY(!available(key));
        const QJsonObject errors = registry.replace(u"capture"_s, {key, key.toLower()});
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.value(key.toLower()).toInt(), ERROR_HOTKEY_ALREADY_REGISTERED);
        QVERIFY(!available(key));
    }

    void ownershipAndConflict()
    {
        const QString key = freeKey();
        QVERIFY(!key.isEmpty());
        PluginHotkeys registry({});
        QVERIFY(registry.replace(u"first"_s, {key}).isEmpty());
        QVERIFY(!available(key));
        QVERIFY(registry.replace(u"first"_s, {key.toLower()}).isEmpty());
        QCOMPARE(registry.replace(u"second"_s, {key}).value(key).toInt(), ERROR_HOTKEY_ALREADY_REGISTERED);
        registry.remove(u"second"_s);
        QVERIFY(!available(key));
        QVERIFY(registry.replace(u"first"_s, QStringList(33, u"A"_s)).contains(u"invalid"_s));
        QVERIFY(!available(key)); // malformed requests preserve the last working shortcut
        registry.remove(u"first"_s);
        QVERIFY(available(key));
        QVERIFY(registry.replace(u"second"_s, {key}).isEmpty());
    }

    void replacementAndQueuedPress()
    {
        const QString first = freeKey();
        QVERIFY(!first.isEmpty());
        int calls = 0;
        QString lastOwner, lastKey;
        PluginHotkeys registry([&](const QString& owner, const QString& key) {
            ++calls; lastOwner = owner; lastKey = key;
        });
        QVERIFY(registry.replace(u"capture"_s, {first}).isEmpty());
        const HWND window = ::FindWindowExW(HWND_MESSAGE, nullptr, L"WinShun.PluginHotkeys", nullptr);
        QVERIFY(window);
        // IDs are monotonic. A queued press of the old shortcut must not fire
        // after recording, rebinding, a crash or switching plugin ownership.
        QVERIFY(::PostMessageW(window, WM_HOTKEY, 1, 0));
        registry.remove(u"capture"_s);
        QVERIFY(registry.replace(u"other"_s, {first}).isEmpty());
        QCoreApplication::processEvents();
        QCOMPARE(calls, 0);
        QVERIFY(::PostMessageW(window, WM_HOTKEY, 2, 0));
        QTRY_COMPARE(calls, 1);
        QCOMPARE(lastOwner, u"other"_s);
        QCOMPARE(lastKey, first);
    }

    void destructorReleasesKeys()
    {
        const QString key = freeKey();
        QVERIFY(!key.isEmpty());
        {
            PluginHotkeys registry({});
            QVERIFY(registry.replace(u"capture"_s, {key}).isEmpty());
            QVERIFY(!available(key));
        }
        QVERIFY(available(key));
    }
};

QTEST_GUILESS_MAIN(TestPluginHotkeys)
#include "tst_pluginhotkeys.moc"
