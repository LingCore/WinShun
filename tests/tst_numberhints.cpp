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

    // 字母直达 (GitHub #175): the key tapped by itself turns the letters on and
    // off; a hold, a chord, a tap while it is off or while nobody listens do not.
    void letterTap()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import "../src/app/qml"
            Item {
                width: 200; height: 60
                property alias hints: hints
                property alias field: field
                NumberHints { id: hints; listening: true; key: Qt.Key_Control; mode: "always" }
                TextInput {
                    id: field
                    anchors.fill: parent
                    focus: true
                    readOnly: hints.jumping
                    Keys.onPressed: event => hints.press(event)
                    Keys.onReleased: event => hints.release(event)
                }
            }
        )", QUrl::fromLocalFile(QStringLiteral(WINSHUN_TEST_SOURCE) + u"/hints.qml"_s));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        QQuickWindow window;
        qobject_cast<QQuickItem*>(root.get())->setParentItem(window.contentItem());
        auto* hints = root->property("hints").value<QObject*>();
        auto* field = root->property("field").value<QObject*>();
        const auto send = [&](QEvent::Type type, int code, Qt::KeyboardModifiers modifiers = Qt::ControlModifier, bool repeat = false) {
            QKeyEvent event(type, code, modifiers, {}, repeat);
            QCoreApplication::sendEvent(field, &event); // Qt events only
        };
        const auto tap = [&](int waitMs = 30) {
            send(QEvent::KeyPress, Qt::Key_Control);
            QTest::qWait(waitMs);
            send(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        };
        const auto jumping = [&] { return hints->property("jumping").toBool(); };
        const auto letter = [&](int offset) {
            QString out;
            QMetaObject::invokeMethod(hints, "letterOf", Q_RETURN_ARG(QString, out), Q_ARG(int, offset));
            return out;
        };
        // As a key event comes to QML: Qt's key and the virtual key (0: none, a key made up in Qt).
        const auto offset = [&](int key, int vk = 0) {
            int out = -2;
            const QVariantMap event {{u"key"_s, key}, {u"nativeVirtualKey"_s, vk}};
            QMetaObject::invokeMethod(hints, "offsetOf", Q_RETURN_ARG(int, out), Q_ARG(QVariant, event));
            return out;
        };
        // Off (the default): a tap is nothing.
        tap();
        QVERIFY(!jumping());
        QVERIFY(hints->property("shown").toBool());
        hints->setProperty("jumpEnabled", true);
        QCOMPARE(letter(0), QString()); // no letters until the tap
        tap();
        QVERIFY(jumping());
        QVERIFY(!hints->property("shown").toBool()); // the letters are in the numbers' place
        QVERIFY(field->property("readOnly").toBool()); // no input method takes the letters
        // The home row first; 26 rows, each key its own row.
        QCOMPARE(letter(0), u"A"_s);
        QCOMPARE(letter(1), u"S"_s);
        QCOMPARE(letter(9), u"Q"_s);
        QCOMPARE(letter(25), u"M"_s);
        QCOMPARE(letter(26), QString());
        QCOMPARE(letter(-1), QString());
        QCOMPARE(offset(Qt::Key_A), 0);
        QCOMPARE(offset(Qt::Key_F), 3);
        QCOMPARE(offset(Qt::Key_M), 25);
        QCOMPARE(offset(Qt::Key_1), -1);
        QCOMPARE(offset(Qt::Key_Escape), -1);
        QSet<int> rows;
        for (int key = Qt::Key_A; key <= Qt::Key_Z; ++key)
            rows.insert(offset(key));
        QCOMPARE(rows.size(), 26);
        // The tap again: off.
        tap();
        QVERIFY(!jumping());
        QVERIFY(hints->property("shown").toBool());
        // Held longer than a tap: not one.
        tap(450);
        QVERIFY(!jumping());
        // A chord (Ctrl+C), or another modifier with it: not a tap.
        send(QEvent::KeyPress, Qt::Key_Control);
        send(QEvent::KeyPress, Qt::Key_C);
        send(QEvent::KeyRelease, Qt::Key_C);
        send(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        QVERIFY(!jumping());
        send(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier | Qt::ShiftModifier);
        send(QEvent::KeyRelease, Qt::Key_Control, Qt::ShiftModifier);
        QVERIFY(!jumping());
        // A release whose press was not seen here (the second of a double tap that opened the window): not one.
        send(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        QVERIFY(!jumping());
        // Auto-repeat of a held key does not arm it again.
        send(QEvent::KeyPress, Qt::Key_Control);
        QTest::qWait(400);
        send(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier, true);
        send(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        QVERIFY(!jumping());
        // On, then nobody listens (the window hidden, a menu open): off, and a tap then is nothing.
        tap();
        QVERIFY(jumping());
        hints->setProperty("listening", false);
        QVERIFY(!jumping());
        tap();
        QVERIFY(!jumping());
        hints->setProperty("listening", true);
        // The setting turned off while they show: gone.
        tap();
        QVERIFY(jumping());
        hints->setProperty("jumpEnabled", false);
        QVERIFY(!jumping());
        // No number keys at all: Ctrl is the key.
        hints->setProperty("key", -1);
        hints->setProperty("jumpEnabled", true);
        tap();
        QVERIFY(jumping());
        // With Alt as the number key, Alt is.
        hints->setProperty("key", int(Qt::Key_Alt));
        QVERIFY(!jumping());
        tap();
        QVERIFY(!jumping());
        send(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
        send(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
        QVERIFY(jumping());

        // GitHub #357: the key chosen, whatever the number keys use.
        hints->setProperty("trigger", u"shift"_s);
        QVERIFY(!jumping());
        send(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
        send(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
        QVERIFY(!jumping()); // Alt is no longer it
        send(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        send(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
        QVERIFY(jumping());
        send(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        send(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
        QVERIFY(!jumping());
        hints->setProperty("trigger", u"ctrl"_s);
        tap();
        QVERIFY(jumping());
        // The letters chosen, digits among them, in their order; a digit by its
        // place on the keyboard: Shift+1 ("!") and the number pad's 1 are 1.
        hints->setProperty("letters", u"QWERT12"_s);
        QVERIFY(!jumping()); // other letters: those shown are gone
        tap();
        QCOMPARE(letter(0), u"Q"_s);
        QCOMPARE(letter(5), u"1"_s);
        QCOMPARE(letter(7), QString());
        QCOMPARE(offset(Qt::Key_Q, 0x51), 0);
        QCOMPARE(offset(Qt::Key_1, 0x31), 5);
        QCOMPARE(offset(Qt::Key_Exclam, 0x31), 5);
        QCOMPARE(offset(Qt::Key_1, 0x61), 5);
        QCOMPARE(offset(Qt::Key_2), 6); // no virtual key: Qt's
        QCOMPARE(offset(Qt::Key_A, 0x41), -1); // not one of them
        // Shift+letter picks a row: the letters go to its buttons; the tap leaves.
        QMetaObject::invokeMethod(hints, "pick");
        QVERIFY(!jumping());
        QVERIFY(hints->property("picking").toBool());
        QVERIFY(!hints->property("shown").toBool());
        QCOMPARE(letter(0), QString()); // none on the rows
        tap();
        QVERIFY(!hints->property("picking").toBool());
        QVERIFY(!jumping()); // out, not back to the rows' letters
        QMetaObject::invokeMethod(hints, "pick");
        hints->setProperty("listening", false);
        QVERIFY(!hints->property("picking").toBool());
    }

    // When number keys are off and 字母直达 is on, `mode` ("always" | "held" | "off")
    // controls passive letter preview while typing; tapping the jump key still arms
    // jumping (high-contrast keycaps, search input read-only) so typing vs jump mode
    // stays unmistakable.
    void letterPreviewWhenNumberKeysOff()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import "../src/app/qml"
            Item {
                width: 200; height: 60
                property alias hints: hints
                property alias field: field
                NumberHints {
                    id: hints
                    listening: true
                    key: -1
                    mode: "always"
                    jumpEnabled: true
                }
                TextInput {
                    id: field
                    anchors.fill: parent
                    focus: true
                    readOnly: hints.jumping
                    Keys.onPressed: event => hints.press(event)
                    Keys.onReleased: event => hints.release(event)
                }
            }
        )", QUrl::fromLocalFile(QStringLiteral(WINSHUN_TEST_SOURCE) + u"/hints-preview.qml"_s));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        QQuickWindow window;
        qobject_cast<QQuickItem*>(root.get())->setParentItem(window.contentItem());
        auto* hints = root->property("hints").value<QObject*>();
        auto* field = root->property("field").value<QObject*>();
        const auto send = [&](QEvent::Type type, int code, Qt::KeyboardModifiers modifiers) {
            QKeyEvent event(type, code, modifiers);
            QCoreApplication::sendEvent(field, &event);
        };
        const auto tap = [&](int code, Qt::KeyboardModifiers mod, int waitMs = 30) {
            send(QEvent::KeyPress, code, mod);
            QTest::qWait(waitMs);
            send(QEvent::KeyRelease, code, Qt::NoModifier);
        };
        const auto letter = [&](int offset) {
            QString out;
            QMetaObject::invokeMethod(hints, "letterOf", Q_RETURN_ARG(QString, out), Q_ARG(int, offset));
            return out;
        };

        // 1. "always" mode with number keys off: passive preview active, input still editable.
        QVERIFY(hints->property("shown").toBool());
        QVERIFY(hints->property("letterPreview").toBool());
        QVERIFY(hints->property("lettersShown").toBool());
        QVERIFY(!hints->property("jumping").toBool());
        QVERIFY(!field->property("readOnly").toBool());
        QCOMPARE(hints->property("jumpKeyName").toString(), u"Ctrl"_s);
        QCOMPARE(letter(0), u"A"_s);
        QCOMPARE(letter(1), u"S"_s);
        QCOMPARE(letter(9), u"Q"_s);

        // Tap Ctrl: arms jumping (read-only) while keeping hintOpacity warm (no blink on cancel).
        tap(Qt::Key_Control, Qt::ControlModifier);
        QVERIFY(hints->property("jumping").toBool());
        QVERIFY(!hints->property("shown").toBool());
        QVERIFY(!hints->property("letterPreview").toBool());
        QVERIFY(hints->property("lettersShown").toBool());
        QVERIFY(field->property("readOnly").toBool());
        QCOMPARE(hints->property("hintOpacity").toReal(), 1.0);
        QCOMPARE(letter(0), u"A"_s);

        // Tap Ctrl again: cancels back to passive preview with zero blink.
        tap(Qt::Key_Control, Qt::ControlModifier);
        QVERIFY(!hints->property("jumping").toBool());
        QVERIFY(hints->property("shown").toBool());
        QVERIFY(hints->property("letterPreview").toBool());
        QVERIFY(!field->property("readOnly").toBool());
        QCOMPARE(hints->property("hintOpacity").toReal(), 1.0);
        QCOMPARE(letter(0), u"A"_s);

        // Picking action buttons clears row letters immediately; leaving restores passive preview.
        QMetaObject::invokeMethod(hints, "pick");
        QVERIFY(hints->property("picking").toBool());
        QVERIFY(!hints->property("lettersShown").toBool());
        QCOMPARE(letter(0), QString());
        QMetaObject::invokeMethod(hints, "leave");
        QVERIFY(hints->property("letterPreview").toBool());
        QCOMPARE(letter(0), u"A"_s);

        // 2. "held" mode with number keys off (including Shift trigger): silent at rest,
        // holding >=180 ms previews letters, holding >350 ms releases cleanly without arming,
        // typing a Shift+A chord cancels the hold without flashing, quick tap arms.
        hints->setProperty("mode", u"held"_s);
        QTRY_COMPARE_WITH_TIMEOUT(hints->property("hintOpacity").toReal(), 0.0, 200);
        QVERIFY(!hints->property("letterPreview").toBool());
        QCOMPARE(letter(0), QString());

        hints->setProperty("trigger", u"shift"_s);
        QCOMPARE(hints->property("jumpKeyName").toString(), u"Shift"_s);
        send(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        send(QEvent::KeyPress, Qt::Key_A, Qt::ShiftModifier);
        QTest::qWait(220);
        QVERIFY(!hints->property("letterPreview").toBool()); // typing Shift+A never flashes preview
        send(QEvent::KeyRelease, Qt::Key_A, Qt::ShiftModifier);
        send(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
        QVERIFY(!hints->property("jumping").toBool());

        send(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        QTest::qWait(220);
        QVERIFY(hints->property("shown").toBool());
        QVERIFY(hints->property("letterPreview").toBool());
        QCOMPARE(letter(0), u"A"_s);
        QTest::qWait(200); // total hold > 350 ms: not a tap
        send(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
        QVERIFY(!hints->property("jumping").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(hints->property("hintOpacity").toReal(), 0.0, 200);
        QVERIFY(!hints->property("letterPreview").toBool());
        QCOMPARE(letter(0), QString());

        tap(Qt::Key_Shift, Qt::ShiftModifier);
        QVERIFY(hints->property("jumping").toBool());
        QCOMPARE(letter(0), u"A"_s);
        QMetaObject::invokeMethod(hints, "leave");
        QVERIFY(!hints->property("lettersShown").toBool());
        QCOMPARE(letter(0), QString());

        // 3. "off" mode with number keys off: no passive preview even when held, tap still arms.
        hints->setProperty("trigger", u"auto"_s);
        hints->setProperty("mode", u"off"_s);
        send(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
        QTest::qWait(220);
        QVERIFY(!hints->property("letterPreview").toBool());
        QCOMPARE(letter(0), QString());
        QTest::qWait(200);
        send(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        QVERIFY(!hints->property("jumping").toBool());
        tap(Qt::Key_Control, Qt::ControlModifier);
        QVERIFY(hints->property("jumping").toBool());
        QCOMPARE(letter(0), u"A"_s);
        QMetaObject::invokeMethod(hints, "leave");
        QVERIFY(!hints->property("lettersShown").toBool());
        QCOMPARE(letter(0), QString());

        // 4. Number keys enabled again: numbers keep priority at rest (no passive letters).
        hints->setProperty("mode", u"always"_s);
        hints->setProperty("key", int(Qt::Key_Control));
        QVERIFY(hints->property("shown").toBool());
        QVERIFY(!hints->property("letterPreview").toBool());
        QCOMPARE(letter(0), QString());
        tap(Qt::Key_Control, Qt::ControlModifier);
        QVERIFY(hints->property("jumping").toBool());
        QCOMPARE(letter(0), u"A"_s);
    }

    void letterJumpSetting()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QVERIFY(!settings.letterJump); // a tap of Ctrl means nothing until asked for
        settings.letterJump = true;
        settings.save();
        ws::Settings read;
        read.load();
        QVERIFY(read.letterJump);
        QCOMPARE(read.numberHints, u"always"_s);
        QCOMPARE(read.letterJumpKey, u"auto"_s);
        QCOMPARE(read.letterJumpKeys, ws::Settings::defaultJumpKeys());

        // GitHub #357: the key and the letters kept; letters cleaned up.
        QCOMPARE(ws::Settings::normalizedJumpKeys(u"qwe rt!1q2"_s), u"QWERT12"_s);
        QCOMPARE(ws::Settings::normalizedJumpKeys(u"  "_s), ws::Settings::defaultJumpKeys());
        QCOMPARE(ws::Settings::normalizedJumpKeys(u"字母"_s), ws::Settings::defaultJumpKeys());
        read.letterJumpKey = u"shift"_s;
        read.letterJumpKeys = u"QWERTASDFGZXCVB12345"_s;
        read.save();
        ws::Settings again;
        again.load();
        QCOMPARE(again.letterJumpKey, u"shift"_s);
        QCOMPARE(again.letterJumpKeys, u"QWERTASDFGZXCVB12345"_s);
        {
            QSettings raw(ws::Settings::filePath(), QSettings::IniFormat);
            raw.setValue(u"Results/LetterJumpKey"_s, u"win"_s);
            raw.setValue(u"Results/LetterJumpKeys"_s, u"aab3"_s);
        }
        ws::Settings odd;
        odd.load();
        QCOMPARE(odd.letterJumpKey, u"auto"_s);
        QCOMPARE(odd.letterJumpKeys, u"AB3"_s);
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
        QCOMPARE(settings.clipboardPasteElevated, true); // GitHub #277: on at first
        settings.clipboardHoldToStep = true;
        settings.clipboardPasteElevated = false;
        settings.clipboardFlipAbove = false;
        settings.clipboardSearchBoxSide = u"far"_s;
        settings.save();
        ws::Settings read;
        read.load();
        QCOMPARE(read.clipboardFlipAbove, false);
        QCOMPARE(read.clipboardSearchBoxSide, u"far"_s);
        QCOMPARE(read.clipboardHoldToStep, true);
        QCOMPARE(read.clipboardPasteElevated, false);
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

        // Not followed while no number shows (ShownRows.tracking): nothing is
        // worked out as the list scrolls, and a key still picks the row that
        // number would show.
        QList<int> tracked;
        for (int number = 1; number <= 10; ++number)
            tracked.append(rowOf(number));
        rows->setProperty("tracking", false);
        QCOMPARE(first(), -1);
        QCOMPARE(numberOf(1), 0);
        for (int number = 1; number <= 10; ++number)
            QCOMPARE(rowOf(number), tracked[number - 1]);
        root->setProperty("contentY", root->property("contentY").toReal() - 20); // scrolled meanwhile
        QCOMPARE(first(), -1);
        const int firstNow = rowOf(1);
        rows->setProperty("tracking", true);
        QCOMPARE(first(), firstNow);

        // Nothing to number.
        root->setProperty("model", 0);
        QCOMPARE(first(), -1);
        QCOMPARE(rowOf(1), -1);
        QCOMPARE(numberOf(0), 0);
    }
};

QTEST_MAIN(NumberHintsTest)
#include "tst_numberhints.moc"
