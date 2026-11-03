#include "CaptureKeys.h"

#include <QTest>

using namespace ws::capture;
using namespace Qt::StringLiterals;

namespace {

QKeyCombination combo(Qt::KeyboardModifiers mods, Qt::Key key) { return QKeyCombination(mods, key); }

} // namespace

class TestCaptureKeys : public QObject {
    Q_OBJECT
private slots:
    void defaultsAreTodaysKeys()
    {
        const CaptureKeys k;
        QCOMPARE(k.key(KeyAction::Pin), combo({}, Qt::Key_F3));
        QCOMPARE(k.key(KeyAction::Save), combo(Qt::ControlModifier, Qt::Key_S));
        QCOMPARE(k.key(KeyAction::PinClose), combo({}, Qt::Key_Escape));
        QCOMPARE(k.key(KeyAction::PinCopy), combo(Qt::ControlModifier, Qt::Key_C));
        QCOMPARE(k.key(KeyAction::PinSave), combo(Qt::ControlModifier, Qt::Key_S));
        QCOMPARE(k.key(KeyAction::PinCollapse), combo({}, Qt::Key_Space));
        QCOMPARE(k.key(KeyAction::PinTextPick), combo({}, Qt::Key_T));
        QCOMPARE(k.key(KeyAction::PinRotateRight), combo({}, Qt::Key_R));
        QCOMPARE(k.key(KeyAction::PinRotateLeft), combo(Qt::ShiftModifier, Qt::Key_R));
        QCOMPARE(k.key(KeyAction::PinFlipHorizontal), combo({}, Qt::Key_H));
        QCOMPARE(k.key(KeyAction::PinFlipVertical), combo({}, Qt::Key_V));
        QCOMPARE(CaptureKeys::defaults(), k);
        QCOMPARE(QString::fromLatin1(keys::info(KeyAction::PinFlipVertical).id), u"PinFlipVertical"_s);
        for (std::size_t i = 0; i < keys::kCount; ++i)
            QCOMPARE(static_cast<std::size_t>(keys::kActions[i].action), i);
    }

    void portableRoundTrip()
    {
        for (const keys::ActionInfo& a : keys::kActions) {
            const CaptureKeys k;
            const QString text = k.portable(a.action);
            QVERIFY(!text.isEmpty());
            const auto parsed = CaptureKeys::parse(text);
            QVERIFY2(parsed.has_value(), qPrintable(text));
            QCOMPARE(*parsed, a.key);
        }
        QCOMPARE(CaptureKeys().portable(KeyAction::Pin), u"F3"_s);
        QCOMPARE(CaptureKeys().portable(KeyAction::PinRotateLeft), u"Shift+R"_s);
        // Cleared: a valid answer, and no label.
        const auto none = CaptureKeys::parse(QString());
        QVERIFY(none.has_value());
        QVERIFY(none->key() == Qt::Key_unknown);
        CaptureKeys k;
        k.set(KeyAction::Pin, *none);
        QVERIFY(!k.has(KeyAction::Pin));
        QVERIFY(k.portable(KeyAction::Pin).isEmpty());
        QVERIFY(k.text(KeyAction::Pin).isEmpty());
        QVERIFY(!k.matches(KeyAction::Pin, Qt::Key_F3, {}));
        // Plus and a chord keep their meaning through the text.
        const auto plus = CaptureKeys::parse(u"Ctrl+Shift+F5"_s);
        QVERIFY(plus.has_value());
        QCOMPARE(*plus, combo(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_F5));
        QCOMPARE(CaptureKeys().text(KeyAction::Save), u"Ctrl+S"_s);
    }

    void garbageIsRejected()
    {
        QVERIFY(!CaptureKeys::parse(u"nonsense"_s).has_value());
        QVERIFY(!CaptureKeys::parse(u"Ctrl+Banana"_s).has_value());
        QVERIFY(!CaptureKeys::parse(u"Ctrl+A, Ctrl+B"_s).has_value());
    }

    void matchesIsExact()
    {
        const CaptureKeys k;
        QVERIFY(k.matches(KeyAction::Save, Qt::Key_S, Qt::ControlModifier));
        QVERIFY(k.matches(KeyAction::Save, Qt::Key_S, Qt::ControlModifier | Qt::KeypadModifier));
        QVERIFY(!k.matches(KeyAction::Save, Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier));
        QVERIFY(!k.matches(KeyAction::Save, Qt::Key_S, {}));
        QVERIFY(k.matches(KeyAction::Pin, Qt::Key_F3, {}));
        QVERIFY(!k.matches(KeyAction::Pin, Qt::Key_F3, Qt::ShiftModifier));
        CaptureKeys e;
        e.set(KeyAction::Pin, combo(Qt::ControlModifier, Qt::Key_Enter));
        QVERIFY(e.matches(KeyAction::Pin, Qt::Key_Return, Qt::ControlModifier));
        QVERIFY(e.matches(KeyAction::Pin, Qt::Key_Enter, Qt::ControlModifier | Qt::KeypadModifier));
    }

    void triggeredByContext()
    {
        const CaptureKeys k;
        // Ctrl+S is Save in the capture and PinSave in the pin window.
        QVERIFY(k.triggered(KeyContext::Capture, Qt::Key_S, Qt::ControlModifier) == KeyAction::Save);
        QVERIFY(k.triggered(KeyContext::PinWindow, Qt::Key_S, Qt::ControlModifier) == KeyAction::PinSave);
        QVERIFY(k.triggered(KeyContext::Capture, Qt::Key_F3, {}) == KeyAction::Pin);
        QVERIFY(!k.triggered(KeyContext::PinWindow, Qt::Key_F3, {}).has_value());
        QVERIFY(k.triggered(KeyContext::PinWindow, Qt::Key_R, Qt::ShiftModifier) == KeyAction::PinRotateLeft);
        QVERIFY(k.triggered(KeyContext::PinWindow, Qt::Key_R, {}) == KeyAction::PinRotateRight);
        QVERIFY(!k.triggered(KeyContext::PinWindow, Qt::Key_R, Qt::ControlModifier).has_value());
    }

    void duplicatesInOneContext()
    {
        const CaptureKeys k;
        const auto clash = k.problem(KeyAction::PinCopy, combo({}, Qt::Key_Escape));
        QVERIFY(clash.has_value());
        QVERIFY(clash->contains(u"关闭贴图"_s));
        // The same key as itself is no clash.
        QVERIFY(!k.problem(KeyAction::PinClose, combo({}, Qt::Key_Escape)).has_value());
        // Ctrl+S in the pin window is fine though the capture's Save has it.
        QVERIFY(!k.problem(KeyAction::PinSave, combo(Qt::ControlModifier, Qt::Key_S)).has_value());
        const auto capture = k.problem(KeyAction::Pin, combo(Qt::ControlModifier, Qt::Key_S));
        QVERIFY(capture.has_value());
        QVERIFY(capture->contains(u"保存"_s));
        // Cleared is always fine.
        QVERIFY(!k.problem(KeyAction::Pin, QKeyCombination()).has_value());
    }

    void captureRules_data()
    {
        QTest::addColumn<int>("mods");
        QTest::addColumn<int>("key");
        QTest::addColumn<bool>("ok");
        const auto row = [](const char* name, Qt::KeyboardModifiers m, Qt::Key k, bool ok) {
            QTest::newRow(name) << int(m) << int(k) << ok;
        };
        row("bare letter", {}, Qt::Key_P, false);
        row("shift letter", Qt::ShiftModifier, Qt::Key_P, false);
        row("digit", {}, Qt::Key_5, false);
        row("space", {}, Qt::Key_Space, false);
        row("period", {}, Qt::Key_Period, false);
        row("esc", {}, Qt::Key_Escape, false);
        row("enter", {}, Qt::Key_Return, false);
        row("keypad enter", Qt::KeypadModifier, Qt::Key_Enter, false);
        row("tab", {}, Qt::Key_Tab, false);
        row("backspace", {}, Qt::Key_Backspace, false);
        row("delete", {}, Qt::Key_Delete, false);
        row("arrow", {}, Qt::Key_Left, false);
        row("ctrl arrow", Qt::ControlModifier, Qt::Key_Left, false);
        row("f1", {}, Qt::Key_F1, false);
        row("ctrl c", Qt::ControlModifier, Qt::Key_C, false);
        row("ctrl v", Qt::ControlModifier, Qt::Key_V, false);
        row("ctrl x", Qt::ControlModifier, Qt::Key_X, false);
        row("ctrl z", Qt::ControlModifier, Qt::Key_Z, false);
        row("ctrl y", Qt::ControlModifier, Qt::Key_Y, false);
        row("ctrl shift z", Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Z, false);
        row("ctrl l", Qt::ControlModifier, Qt::Key_L, false);
        row("ctrl r", Qt::ControlModifier, Qt::Key_R, false);
        row("ctrl a", Qt::ControlModifier, Qt::Key_A, false);
        row("ctrl d", Qt::ControlModifier, Qt::Key_D, false);
        row("ctrl plus", Qt::ControlModifier, Qt::Key_Plus, false);
        row("ctrl minus", Qt::ControlModifier, Qt::Key_Minus, false);
        row("ctrl zero", Qt::ControlModifier, Qt::Key_0, false);
        row("ctrl equal", Qt::ControlModifier, Qt::Key_Equal, false);
        row("ctrl bracket", Qt::ControlModifier, Qt::Key_BracketLeft, false);
        row("alt f4", Qt::AltModifier, Qt::Key_F4, false);
        row("shift only", Qt::ShiftModifier, Qt::Key_Shift, false);
        row("f2", {}, Qt::Key_F2, true);
        row("f3", {}, Qt::Key_F3, true);
        row("shift f1", Qt::ShiftModifier, Qt::Key_F1, true);
        row("ctrl f2", Qt::ControlModifier, Qt::Key_F2, true);
        row("ctrl t", Qt::ControlModifier, Qt::Key_T, true);
        row("alt p", Qt::AltModifier, Qt::Key_P, true);
        row("ctrl shift s", Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_S, true);
        row("ctrl q", Qt::ControlModifier, Qt::Key_Q, true);
        row("ctrl alt p", Qt::ControlModifier | Qt::AltModifier, Qt::Key_P, true);
    }
    void captureRules()
    {
        QFETCH(int, mods);
        QFETCH(int, key);
        QFETCH(bool, ok);
        const CaptureKeys k;
        const auto combination = QKeyCombination(Qt::KeyboardModifiers(mods), Qt::Key(key));
        // Pin is not taken by anything in the table for these keys, so the verdict is the rules'.
        const auto why = k.problem(KeyAction::Pin, combination);
        QCOMPARE(!why.has_value(), ok);
    }

    void capturePrintableHasAHint()
    {
        const auto why = CaptureKeys().problem(KeyAction::Pin, combo({}, Qt::Key_P));
        QVERIFY(why && why->contains(u"Ctrl"_s));
        const auto fixed = CaptureKeys().problem(KeyAction::Pin, combo(Qt::ControlModifier, Qt::Key_Z));
        QVERIFY(fixed && fixed->contains(u"已经有用处"_s));
    }

    void pinRules_data()
    {
        QTest::addColumn<int>("mods");
        QTest::addColumn<int>("key");
        QTest::addColumn<bool>("ok");
        const auto row = [](const char* name, Qt::KeyboardModifiers m, Qt::Key k, bool ok) {
            QTest::newRow(name) << int(m) << int(k) << ok;
        };
        row("left", {}, Qt::Key_Left, false);
        row("shift right", Qt::ShiftModifier, Qt::Key_Right, false);
        row("plus", {}, Qt::Key_Plus, false);
        row("minus", {}, Qt::Key_Minus, false);
        row("alt f4", Qt::AltModifier, Qt::Key_F4, false);
        row("f10", {}, Qt::Key_F10, false);
        row("alt space", Qt::AltModifier, Qt::Key_Space, false);
        row("shift key", {}, Qt::Key_Shift, false);
        row("meta p", Qt::MetaModifier, Qt::Key_P, false);
        row("not hearable", {}, Qt::Key_Semicolon, false);
        row("letter", {}, Qt::Key_Q, true);
        row("ctrl shift c", Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_C, true);
        row("f5", {}, Qt::Key_F5, true);
        row("alt q", Qt::AltModifier, Qt::Key_Q, true);
        row("digit", {}, Qt::Key_7, true);
        row("home", {}, Qt::Key_Home, true);
        row("esc", {}, Qt::Key_Escape, true);
    }
    void pinRules()
    {
        QFETCH(int, mods);
        QFETCH(int, key);
        QFETCH(bool, ok);
        CaptureKeys k;
        k.set(KeyAction::PinTextPick, QKeyCombination()); // nothing else in the table has these
        k.set(KeyAction::PinClose, QKeyCombination());
        const auto combination = QKeyCombination(Qt::KeyboardModifiers(mods), Qt::Key(key));
        const auto why = k.problem(KeyAction::PinTextPick, combination);
        QCOMPARE(!why.has_value(), ok);
    }

    void virtualKeys()
    {
        using K = CaptureKeys;
        QCOMPARE(K::fromVirtualKey('A', false, false, false), combo({}, Qt::Key_A));
        QCOMPARE(K::fromVirtualKey('R', false, true, false), combo(Qt::ShiftModifier, Qt::Key_R));
        QCOMPARE(K::fromVirtualKey('S', true, false, true), combo(Qt::ControlModifier | Qt::AltModifier, Qt::Key_S));
        QCOMPARE(K::fromVirtualKey('5', false, false, false), combo({}, Qt::Key_5));
        QCOMPARE(K::fromVirtualKey(0x63, false, false, false), combo({}, Qt::Key_3)); // VK_NUMPAD3
        QCOMPARE(K::fromVirtualKey(0x70, false, false, false), combo({}, Qt::Key_F1));
        QCOMPARE(K::fromVirtualKey(0x87, false, false, false), combo({}, Qt::Key_F24));
        QCOMPARE(K::fromVirtualKey(0x20, false, false, false), combo({}, Qt::Key_Space));
        QCOMPARE(K::fromVirtualKey(0x1B, false, false, false), combo({}, Qt::Key_Escape));
        QCOMPARE(K::fromVirtualKey(0x0D, false, false, false), combo({}, Qt::Key_Return));
        QCOMPARE(K::fromVirtualKey(0x21, false, false, false), combo({}, Qt::Key_PageUp));
        QCOMPARE(K::fromVirtualKey(0x25, false, false, false), combo({}, Qt::Key_Left));
        QCOMPARE(K::fromVirtualKey(0xBB, false, false, false), combo({}, Qt::Key_Plus));
        QCOMPARE(K::fromVirtualKey(0x6B, false, false, false), combo({}, Qt::Key_Plus));
        QCOMPARE(K::fromVirtualKey(0xBD, false, false, false), combo({}, Qt::Key_Minus));
        QCOMPARE(K::fromVirtualKey(0x6D, false, false, false), combo({}, Qt::Key_Minus));
        // Not known: none, whatever the modifiers.
        QCOMPARE(K::fromVirtualKey(0x10, false, true, false).key(), Qt::Key_unknown); // VK_SHIFT
        QCOMPARE(K::fromVirtualKey(0xBA, true, false, false).key(), Qt::Key_unknown); // VK_OEM_1
        // Every default pin key is one the window can hear and the validator lets through.
        for (const keys::ActionInfo& a : keys::kActions) {
            if (a.context == KeyContext::PinWindow)
                QVERIFY(keys::pinCanHear(a.key.key()));
        }
    }
};

QTEST_APPLESS_MAIN(TestCaptureKeys)
#include "tst_capturekeys.moc"
