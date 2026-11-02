#include "Settings.h"
#include "platform/NumberKeys.h"

#include <QGuiApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <tuple>

using namespace Qt::StringLiterals;

class NumberHintsTest : public QObject {
    Q_OBJECT

private slots:
    void fileSizeSettings()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        QCOMPARE(ws::Settings::defaults().showFileSize, false);
        settings.load();
        QCOMPARE(settings.showFileSize, false);
        for (const bool enabled : {true, false}) {
            settings.showFileSize = enabled;
            settings.showModified = !enabled; // independent preferences survive a reload
            settings.save();
            ws::Settings read;
            read.load();
            QCOMPARE(read.showFileSize, enabled);
            QCOMPARE(read.showModified, !enabled);
        }
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.remove(u"Results/ShowFileSize"_s);
        ini.sync();
        settings.load();
        QCOMPARE(settings.showFileSize, false);
    }

    void favoriteFolderTabSettings()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        QCOMPARE(ws::Settings::defaults().tabEntersFavoriteFolder, false);
        settings.load();
        QCOMPARE(settings.tabEntersFavoriteFolder, false);
        for (const bool enabled : {true, false}) {
            settings.tabEntersFavoriteFolder = enabled;
            settings.save();
            ws::Settings read;
            read.load();
            QCOMPARE(read.tabEntersFavoriteFolder, enabled);
        }
        // An upgraded profile without this preference gets the consistent default.
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.remove(u"Results/TabEntersFavoriteFolder"_s);
        ini.sync();
        settings.load();
        QCOMPARE(settings.tabEntersFavoriteFolder, false);
    }

    void globalQuickPasteSettings()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.clipboardQuickPaste, u"off"_s);
        for (const QString mode : {u"ctrl"_s, u"alt"_s, u"off"_s}) {
            settings.clipboardQuickPaste = mode;
            settings.save();
            ws::Settings read;
            read.load();
            QCOMPARE(read.clipboardQuickPaste, mode);
            QCOMPARE(read.clipboardNumberKeys, u"alt"_s); // panel shortcut stays independent
        }
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.setValue(u"Clipboard/QuickPaste"_s, u" CTRL "_s);
        ini.sync();
        settings.load();
        QCOMPARE(settings.clipboardQuickPaste, u"ctrl"_s);
        ini.setValue(u"Clipboard/QuickPaste"_s, u"invalid"_s);
        ini.sync();
        settings.load();
        QCOMPARE(settings.clipboardQuickPaste, u"off"_s);
    }

    void settings()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.numberHints, u"always"_s);
        for (const QString mode : {u"held"_s, u"always"_s, u"off"_s}) {
            settings.numberHints = mode;
            settings.save();
            ws::Settings read;
            read.load();
            QCOMPARE(read.numberHints, mode);
            QCOMPARE(read.numberKeys, u"ctrl"_s);
            QCOMPARE(read.clipboardNumberKeys, u"alt"_s);
        }
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.setValue(u"Results/NumberHints"_s, u"invalid"_s);
        ini.sync();
        settings.load();
        QCOMPARE(settings.numberHints, u"always"_s);
        ini.setValue(u"Results/NumberHints"_s, u" ALWAYS "_s);
        ini.sync();
        settings.load();
        QCOMPARE(settings.numberHints, u"always"_s);
    }

    // Off for a new install; an older ini without the key keeps it on (off
    // would clear its index). Written either way, so it holds next time.
    void contentIndexDefault()
    {
        const auto loaded = [](const std::function<void(QSettings&)>& write) {
            QTemporaryDir profile;
            ws::Settings::useProfile(profile.path());
            {
                QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
                write(ini);
            }
            ws::Settings settings;
            settings.load();
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            const QString stored
                = ini.contains(u"Content/Index"_s) ? ini.value(u"Content/Index"_s).toString() : u"missing"_s;
            ws::Settings again;
            again.load();
            return std::tuple {settings.contentIndex, stored, again.contentIndex};
        };
        QCOMPARE(ws::Settings::defaults().contentIndex, false);
        const auto fresh = loaded([](QSettings&) {});
        QCOMPARE(fresh, std::tuple(false, u"false"_s, false));
        const auto older = loaded([](QSettings& ini) { ini.setValue(u"Content/DefaultsVersion"_s, 3); });
        QCOMPARE(older, std::tuple(true, u"true"_s, true));
        const auto off = loaded([](QSettings& ini) { ini.setValue(u"Content/Index"_s, false); });
        QCOMPARE(off, std::tuple(false, u"false"_s, false));
        const auto on = loaded([](QSettings& ini) { ini.setValue(u"Content/Index"_s, true); });
        QCOMPARE(on, std::tuple(true, u"true"_s, true));
    }

    // The clipboard window's layout by the caret (GitHub #38): at first
    // turned over above it, the search box by it; "far" read however it was
    // written, anything else back to "near".
    void clipboardLayout()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.clipboardFlipAbove, true);
        QCOMPARE(settings.clipboardSearchBoxSide, u"near"_s);
        QCOMPARE(settings.clipboardHoldToStep, false); // GitHub #107: off at first
        settings.clipboardHoldToStep = true;
        settings.clipboardFlipAbove = false;
        settings.clipboardSearchBoxSide = u"far"_s;
        settings.save();
        ws::Settings read;
        read.load();
        QCOMPARE(read.clipboardFlipAbove, false);
        QCOMPARE(read.clipboardSearchBoxSide, u"far"_s);
        QCOMPARE(read.clipboardHoldToStep, true);
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        QCOMPARE(ini.value(u"Clipboard/HoldToStep"_s).toBool(), true);
        QCOMPARE(ini.value(u"Clipboard/FlipAbove"_s).toBool(), false);
        ini.setValue(u"Clipboard/SearchBoxSide"_s, u" FAR "_s);
        ini.sync();
        read.load();
        QCOMPARE(read.clipboardSearchBoxSide, u"far"_s);
        ini.setValue(u"Clipboard/SearchBoxSide"_s, u"left"_s);
        ini.sync();
        read.load();
        QCOMPARE(read.clipboardSearchBoxSide, u"near"_s);
        QCOMPARE(read.clipboardFlipAbove, false);
    }

    void keys_data()
    {
        QTest::addColumn<int>("key");
        QTest::newRow("Ctrl in launcher input") << int(Qt::Key_Control);
        QTest::newRow("Alt in clipboard input") << int(Qt::Key_Alt);
    }

    void keys()
    {
        QFETCH(int, key);
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import "../src/app/qml"
            Item {
                width: 200; height: 60
                property alias hints: hints
                property alias field: field
                NumberHints { id: hints; listening: true }
                TextInput {
                    id: field
                    anchors.fill: parent
                    focus: true
                    Keys.onPressed: event => hints.press(event)
                    Keys.onReleased: event => hints.release(event)
                }
            }
        )", QUrl::fromLocalFile(QStringLiteral(WINSHUN_TEST_SOURCE) + u"/hints.qml"_s));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        QQuickWindow window;
        auto* item = qobject_cast<QQuickItem*>(root.get());
        item->setParentItem(window.contentItem());
        auto* hints = root->property("hints").value<QObject*>();
        auto* field = root->property("field").value<QObject*>();
        QVERIFY(hints);
        hints->setProperty("key", key);
        const auto send = [&](QEvent::Type type, int code, bool repeat = false) {
            const auto modifier = key == Qt::Key_Control ? Qt::ControlModifier : Qt::AltModifier;
            QKeyEvent event(type, code, modifier, {}, repeat);
            // Qt events only: never send a system chord to another app's hotkey.
            QCoreApplication::sendEvent(field, &event);
        };
        const auto shown = [&] { return hints->property("shown").toBool(); };
        QVERIFY(!shown());
        QElapsedTimer elapsed;
        elapsed.start();
        send(QEvent::KeyPress, key);
        QTest::qWait(120);
        QVERIFY(!shown());
        while (!shown() && elapsed.elapsed() < 300)
            QTest::qWait(5);
        QVERIFY(shown());
        qInfo("Hint became ready at %lld ms", elapsed.elapsed());
        QVERIFY(elapsed.elapsed() >= 150);
        QTRY_COMPARE_WITH_TIMEOUT(hints->property("hintOpacity").toReal(), 1.0, 200);
        send(QEvent::KeyRelease, key);
        QVERIFY(!shown());
        QTRY_COMPARE_WITH_TIMEOUT(hints->property("hintOpacity").toReal(), 0.0, 100);

        // Two quick taps, and a quick chord followed by a long modifier hold.
        for (int i = 0; i < 2; ++i) {
            send(QEvent::KeyPress, key);
            QTest::qWait(40);
            send(QEvent::KeyRelease, key);
            QTest::qWait(40);
            QVERIFY(!shown());
        }
        send(QEvent::KeyPress, key);
        send(QEvent::KeyPress, Qt::Key_C);
        QTest::qWait(220);
        QVERIFY(!shown());
        send(QEvent::KeyRelease, key);

        send(QEvent::KeyPress, key);
        QTest::qWait(100);
        send(QEvent::KeyPress, key, true);
        QTest::qWait(110);
        QVERIFY(shown()); // auto-repeat must not restart the delay
        hints->setProperty("listening", false);
        QVERIFY(!shown());
        hints->setProperty("listening", true);
        QTest::qWait(220);
        QVERIFY(!shown()); // no stuck hold after focus loss or hiding

        hints->setProperty("mode", u"always"_s);
        QVERIFY(shown());
        hints->setProperty("key", -1);
        QVERIFY(!shown());
        hints->setProperty("key", key);
        QVERIFY(shown());
        hints->setProperty("mode", u"off"_s);
        send(QEvent::KeyPress, key);
        QTest::qWait(220);
        QVERIFY(!shown());

        hints->setProperty("mode", u"held"_s);
        send(QEvent::KeyPress, key);
        QTest::qWait(80);
        hints->setProperty("listening", false);
        hints->setProperty("listening", true);
        QTest::qWait(220);
        QVERIFY(!shown()); // losing focus during the delay also cancels it
        send(QEvent::KeyPress, key);
        hints->setProperty("key", key == Qt::Key_Control ? Qt::Key_Alt : Qt::Key_Control);
        QTest::qWait(220);
        QVERIFY(!shown());
    }

    // The digits the hook takes before other programs' global hotkeys
    // (GitHub #175): its modifier alone, Shift or not; never AltGr, Win or
    // a bare digit (an input method's candidates).
    void takenDigit()
    {
        using ws::numberkeys::Modifier;
        using ws::numberkeys::takenDigit;
        constexpr unsigned digits = 1u << 3 | 1u << 5;
        static_assert(takenDigit('5', Qt::ControlModifier, Modifier::Ctrl, digits) == 5);
        QCOMPARE(takenDigit('3', Qt::ControlModifier, Modifier::Ctrl, digits), 3);
        QCOMPARE(takenDigit('3', Qt::ControlModifier | Qt::ShiftModifier, Modifier::Ctrl, digits), 0); // not probed
        QCOMPARE(takenDigit('5', Qt::AltModifier, Modifier::Alt, digits), 5);
        QCOMPARE(takenDigit('4', Qt::ControlModifier, Modifier::Ctrl, digits), 0); // nobody else holds it
        QCOMPARE(takenDigit('5', Qt::AltModifier, Modifier::Ctrl, digits), 0); // the other modifier
        QCOMPARE(takenDigit('5', Qt::ControlModifier | Qt::AltModifier, Modifier::Ctrl, digits), 0); // AltGr
        QCOMPARE(takenDigit('5', Qt::ControlModifier | Qt::MetaModifier, Modifier::Ctrl, digits), 0); // Win
        QCOMPARE(takenDigit('5', Qt::NoModifier, Modifier::Ctrl, digits), 0); // typed
        QCOMPARE(takenDigit('5', Qt::ShiftModifier, Modifier::Ctrl, digits), 0);
        QCOMPARE(takenDigit('0', Qt::ControlModifier, Modifier::Ctrl, ~0u), 0);
        QCOMPARE(takenDigit('A', Qt::ControlModifier, Modifier::Ctrl, ~0u), 0);
        QCOMPARE(takenDigit(VK_NUMPAD5, Qt::ControlModifier, Modifier::Ctrl, ~0u), 0);
    }

    // The clipboard's number keys follow the rows in view (GitHub #175), as
    // the search list's do: mixed row heights, scrolled, upside down.
    void shownRows()
    {
        QQmlEngine engine;
        const auto make = [&](const char* direction) {
            QQmlComponent component(&engine);
            component.setData(QByteArray(R"(
                import QtQuick
                import "../src/app/qml"
                ListView {
                    id: list
                    width: 200; height: 200
                    topMargin: 4; bottomMargin: 4
                    verticalLayoutDirection: ListView.)") + direction + R"(
                    model: 30
                    // Every fourth row is a picture row, three times as tall.
                    delegate: Item { required property int index; width: 200; height: index % 4 === 3 ? 60 : 20 }
                    property alias rows: rows
                    ShownRows { id: rows; view: list }
                }
            )", QUrl::fromLocalFile(QStringLiteral(WINSHUN_TEST_SOURCE) + u"/rows.qml"_s));
            std::unique_ptr<QObject> made(component.create());
            if (!made)
                qWarning().noquote() << component.errorString();
            return made;
        };
        std::unique_ptr<QObject> root = make("TopToBottom");
        QVERIFY(root);
        QObject* rows = root->property("rows").value<QObject*>();
        QVERIFY(rows);
        const auto first = [&] { return rows->property("first").toInt(); };
        const auto last = [&] { return rows->property("last").toInt(); };
        const auto rowOf = [&](int number) {
            int row = -2;
            QMetaObject::invokeMethod(rows, "rowOf", Q_RETURN_ARG(int, row), Q_ARG(int, number));
            return row;
        };
        const auto numberOf = [&](int index) {
            int number = -2;
            QMetaObject::invokeMethod(rows, "numberOf", Q_RETURN_ARG(int, number), Q_ARG(int, index));
            return number;
        };
        // Rows 0-2 at 0-60, 3 at 60-120, 4-6 at 120-180, 7 at 180-240: at
        // rest (contentY -4) 0…6 are in view, 7 is not half in view.
        QTRY_COMPARE(first(), 0);
        QCOMPARE(last(), 6);
        QCOMPARE(rowOf(1), 0);
        QCOMPARE(rowOf(7), 6);
        QCOMPARE(rowOf(8), -1);
        QCOMPARE(numberOf(3), 4);

        // Scrolled by half a short row and a bit: row 0 is out, 1 is the first.
        root->setProperty("contentY", 11);
        QCOMPARE(first(), 1);
        QCOMPARE(rowOf(1), 1);
        QCOMPARE(numberOf(1), 1);
        QCOMPARE(numberOf(0), 0);
        QCOMPARE(last(), 7); // 180-240 against 11-211: 31 of 60 in view

        // Into the tall row: less than half of it in view, the next one counts.
        root->setProperty("contentY", 95);
        QCOMPARE(first(), 4);
        QCOMPARE(rowOf(1), 4);
        // At most nine.
        root->setProperty("contentY", 240);
        QCOMPARE(first(), 8);
        QVERIFY(last() - first() <= 8);
        QCOMPARE(rowOf(last() - first() + 1), last());
        QCOMPARE(rowOf(last() - first() + 2), -1);

        // Upside down: the first row at the bottom, the numbers going up.
        root = make("BottomToTop");
        QVERIFY(root);
        rows = root->property("rows").value<QObject*>();
        QTRY_COMPARE(first(), 0);
        QCOMPARE(last(), 6);
        QVERIFY(root->property("contentY").toReal() < 0); // the rows go up from 0
        root->setProperty("contentY", root->property("contentY").toReal() - 15); // with or without its margin, under half of row 0 left
        QCOMPARE(first(), 1);
        QCOMPARE(rowOf(1), 1);

        // Nothing to number.
        root->setProperty("model", 0);
        QCOMPARE(first(), -1);
        QCOMPARE(rowOf(1), -1);
        QCOMPARE(numberOf(0), 0);
    }
};

QTEST_MAIN(NumberHintsTest)
#include "tst_numberhints.moc"
