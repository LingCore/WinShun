// The keys and mouse buttons shown in a recording (GitHub #98,
// src/capture/Keycaps): what is shown and what never is, names, rows,
// counts, timing and size.
#include "Keycaps.h"
#include "KeycapRender.h"
#include "KeyStream.h"

#include "OwnInput.h"

#include <QGuiApplication>
#include <QImage>
#include <QTest>

#include <bitset>

#include <windows.h>

using namespace Qt::StringLiterals;
using namespace ws::capture::keycaps;

namespace {

constexpr qint64 kMs = 10'000;

Config config(Mode mode, bool mouse = false)
{
    Config value;
    value.mode = mode;
    value.mouse = mouse;
    value.doubleTap = 500 * kMs;
    return value;
}

Key press(std::uint16_t vk, std::uint8_t mods, qint64 ms, char16_t text = 0)
{
    Key key;
    key.vk = vk;
    key.mods = mods;
    key.time = ms * kMs;
    key.text[0] = text;
    return key;
}

// The keys the system has down, as KeyBook asks (GetAsyncKeyState in the hook).
std::bitset<256> g_down;

bool fakeHeld(int vk)
{
    switch (vk) {
    case VK_CONTROL:
        return g_down.test(VK_LCONTROL) || g_down.test(VK_RCONTROL);
    case VK_SHIFT:
        return g_down.test(VK_LSHIFT) || g_down.test(VK_RSHIFT);
    case VK_MENU:
        return g_down.test(VK_LMENU) || g_down.test(VK_RMENU);
    default:
        return vk > 0 && vk < 256 && g_down.test(size_t(vk));
    }
}

// A key through the hook; the system takes it in after, its state with it.
ws::capture::recording::KeyBook::Seen hook(ws::capture::recording::KeyBook& book, std::uint32_t vk, bool down,
    std::uint32_t ms, std::uint32_t scanCode = 0)
{
    const auto seen = book.key(vk, scanCode, down, ms, &fakeHeld);
    g_down.set(vk, down);
    return seen;
}

Key release(std::uint16_t vk, std::uint8_t mods, qint64 ms)
{
    Key key = press(vk, mods, ms);
    key.down = false;
    return key;
}

// A modifier tapped alone: down (nothing held before it), up (held).
void tap(Model& model, std::uint16_t vk, qint64 ms, qint64 heldMs = 80)
{
    model.feed(press(vk, 0, ms));
    model.feed(release(vk, modifierOf(vk), ms + heldMs));
}

// A shortcut: the modifiers go down in turn, the key, all up.
void chord(Model& model, std::uint8_t mods, std::uint16_t vk, qint64 ms)
{
    std::uint8_t held = 0;
    const std::pair<std::uint8_t, std::uint16_t> order[] {{kCtrl, VK_LCONTROL}, {kAlt, VK_LMENU}, {kShift, VK_LSHIFT}, {kWin, VK_LWIN}};
    for (const auto& [bit, key] : order) {
        if (mods & bit) {
            model.feed(press(key, held, ms));
            held |= bit;
        }
    }
    model.feed(press(vk, held, ms + 10));
    model.feed(release(vk, held, ms + 60));
    for (const auto& [bit, key] : order) {
        if (mods & bit) {
            model.feed(release(key, held, ms + 80));
            held &= ~bit;
        }
    }
}

QStringList caps(const Model& model, int row = -1)
{
    const auto& rows = model.rows();
    if (rows.empty())
        return {};
    return rows[row < 0 ? rows.size() - 1 : size_t(row)].caps;
}

} // namespace

class TestKeycaps : public QObject {
    Q_OBJECT
private slots:
    void ownInputAndPacketsAreNeverShown()
    {
        // Win顺's own keys (its paste, its masking key) whatever the key.
        QVERIFY(hidden('V', ws::kOwnInput));
        QVERIFY(hidden(VK_LCONTROL, ws::kOwnInput));
        QVERIFY(hidden('A', ws::kOwnMask));
        // The masking key and VK_PACKET, marked or not.
        QVERIFY(hidden(ws::kMaskKey, 0));
        QVERIFY(hidden(ws::kMaskKey, ws::kOwnMask));
        QVERIFY(hidden(VK_PACKET, 0));
        QVERIFY(hidden(VK_PACKET, ws::kOwnInput));
        QVERIFY(hidden(0, 0));
        QVERIFY(hidden(0xFF, 0));
        QVERIFY(hidden(kAltGrKey, 0)); // ours, never one from Windows
        // Another program's keys are shown: injected (AutoHotkey's mark,
        // or none, as a test's SendInput) or typed.
        QVERIFY(!hidden('A', 0));
        QVERIFY(!hidden('A', 0, LLKHF_INJECTED));
        QVERIFY(!hidden('A', 0xFFC3D44F, LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED));
        QVERIFY(!hidden(VK_LCONTROL, 0, LLKHF_INJECTED));
        QVERIFY(!hidden(VK_F5, 0));
    }

    void shortcutsModeHandsNoTypingOver()
    {
        QVERIFY(!passes(Mode::Shortcuts, 'A', 0));
        QVERIFY(!passes(Mode::Shortcuts, 'A', kShift)); // a capital
        QVERIFY(!passes(Mode::Shortcuts, 'Q', kAltGr)); // @ on a German layout
        QVERIFY(!passes(Mode::Shortcuts, VK_SPACE, 0));
        QVERIFY(!passes(Mode::Shortcuts, '1', kShift));
        QVERIFY(!passes(Mode::Shortcuts, VK_OEM_2, 0));
        QVERIFY(!passes(Mode::Shortcuts, VK_NUMPAD5, 0));
        QVERIFY(passes(Mode::Shortcuts, 'A', kCtrl));
        QVERIFY(passes(Mode::Shortcuts, VK_F4, kAlt));
        QVERIFY(passes(Mode::Shortcuts, 'E', kWin));
        QVERIFY(passes(Mode::Shortcuts, VK_SPACE, kCtrl));
        QVERIFY(passes(Mode::Shortcuts, VK_ESCAPE, 0));
        QVERIFY(passes(Mode::Shortcuts, VK_RETURN, 0));
        QVERIFY(passes(Mode::Shortcuts, VK_F5, 0));
        QVERIFY(passes(Mode::Shortcuts, VK_LEFT, kShift));
        QVERIFY(passes(Mode::Shortcuts, VK_LSHIFT, 0)); // modifiers: for their taps
        QVERIFY(passes(Mode::Shortcuts, kAltGrKey, 0));
        QVERIFY(passes(Mode::Shortcuts, VK_MBUTTON, 0));
        QVERIFY(passes(Mode::All, 'A', 0));
        QVERIFY(!passes(Mode::Off, VK_ESCAPE, 0));
        QVERIFY(passes(Mode::Off, VK_LBUTTON, 0));
    }

    void modeNames()
    {
        for (const Mode mode : {Mode::Off, Mode::Shortcuts, Mode::All})
            QVERIFY(modeFromName(modeName(mode)) == mode);
        QCOMPARE(modeName(Mode::Shortcuts), u"shortcuts"_s);
        QVERIFY(modeFromName(u"ALL") == Mode::Off);
        QVERIFY(modeFromName(u"") == Mode::Off);
    }

    void namesAndOrder()
    {
        const Words words;
        QCOMPARE(modifierNames(kShift | kCtrl | kAlt | kWin), QStringList({u"Win"_s, u"Ctrl"_s, u"Alt"_s, u"Shift"_s}));
        QCOMPARE(modifierNames(kAltGr | kShift), QStringList({u"AltGr"_s, u"Shift"_s}));
        QCOMPARE(keyName('S', 0, words), u"S"_s);
        QCOMPARE(keyName('7', 0, words), u"7"_s);
        QCOMPARE(keyName(VK_F12, 0, words), u"F12"_s);
        QCOMPARE(keyName(VK_F24, 0, words), u"F24"_s);
        QCOMPARE(keyName(VK_LEFT, 0, words), u"←"_s);
        QCOMPARE(keyName(VK_DOWN, 0, words), u"↓"_s);
        QCOMPARE(keyName(VK_NUMPAD3, 0, words), u"Num 3"_s);
        QCOMPARE(keyName(VK_OEM_2, u'/', words), u"/"_s);
        QCOMPARE(keyName(VK_OEM_1, u'ü', words), u"Ü"_s); // a German layout's key
        QCOMPARE(keyName(VK_OEM_PLUS, 0, words), QString()); // no legend: not shown
        QCOMPARE(keyName(VK_BROWSER_BACK, 0, words), QString());
        QCOMPARE(keyName(VK_SPACE, 0, words), words.space);
        QCOMPARE(keyName(VK_XBUTTON1, 0, words), words.back);
        Words chinese;
        chinese.space = u"空格"_s;
        QCOMPARE(keyName(VK_SPACE, 0, chinese), u"空格"_s);
        QVERIFY(fromRegistered(MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'A') == (Hotkey {std::uint8_t(kCtrl | kShift), 'A'}));
        QVERIFY(fromRegistered(MOD_ALT | MOD_WIN, VK_F1) == (Hotkey {std::uint8_t(kAlt | kWin), VK_F1}));
    }

    void shortcutRows()
    {
        Model model(config(Mode::Shortcuts));
        chord(model, kCtrl | kShift, 'S', 100);
        QCOMPARE(model.rows().size(), size_t(1));
        QCOMPARE(caps(model), QStringList({u"Ctrl"_s, u"Shift"_s, u"S"_s}));
        QCOMPARE(model.rows().back().count, 1);
        // The same again while it shows: counted, no second row.
        chord(model, kCtrl | kShift, 'S', 400);
        chord(model, kCtrl | kShift, 'S', 700);
        QCOMPARE(model.rows().size(), size_t(1));
        QCOMPARE(model.rows().back().count, 3);
        QCOMPARE(model.rows().back().last, 710 * kMs);
        // Something else: a row of its own, the newest last.
        model.feed(press(VK_F5, 0, 900));
        QCOMPARE(model.rows().size(), size_t(2));
        QCOMPARE(caps(model), QStringList({u"F5"_s}));
        // At most three; the oldest goes.
        model.feed(press(VK_ESCAPE, 0, 1000));
        chord(model, kAlt, VK_F4, 1100);
        QCOMPARE(model.rows().size(), size_t(3));
        QCOMPARE(caps(model, 0), QStringList({u"F5"_s}));
        QCOMPARE(caps(model), QStringList({u"Alt"_s, u"F4"_s}));
        // Ctrl+S once it has gone: a new row, counted from one.
        Model later(config(Mode::Shortcuts));
        chord(later, kCtrl, 'S', 0);
        later.advance(10 * kMs + kLinger + kFadeOut);
        QVERIFY(later.rows().empty());
        chord(later, kCtrl, 'S', 3000);
        QCOMPARE(later.rows().size(), size_t(1));
        QCOMPARE(later.rows().back().count, 1);
    }

    void typingIsHiddenInShortcutsMode()
    {
        Model model(config(Mode::Shortcuts));
        // What the hook hands over in this mode: presses without identity.
        for (int i = 0; i < 5; ++i)
            model.feed(press(kHiddenKey, 0, 100 + i * 50));
        model.feed(press(kHiddenKey, kShift, 400));
        QVERIFY(model.rows().empty());
        // Even when a key slips through, typing is not shown.
        model.feed(press('A', 0, 500, u'a'));
        model.feed(press('A', kShift, 550, u'A'));
        model.feed(press(VK_SPACE, 0, 600, u' '));
        QVERIFY(model.rows().empty());
        model.feed(press(VK_RETURN, 0, 700, u'\r'));
        QCOMPARE(caps(model), QStringList({u"Enter"_s}));
        model.feed(press(VK_LEFT, kShift, 800));
        QCOMPARE(caps(model), QStringList({u"Shift"_s, u"←"_s}));
    }

    void heldKeysCountOnce()
    {
        Model model(config(Mode::Shortcuts));
        model.feed(press(VK_RIGHT, 0, 100));
        for (int i = 1; i <= 60; ++i) {
            Key repeat = press(VK_RIGHT, 0, 100 + 500 + i * 33);
            repeat.repeat = true;
            model.feed(repeat);
        }
        QCOMPARE(model.rows().size(), size_t(1));
        QCOMPARE(model.rows().back().count, 1);
        QCOMPARE(model.rows().back().last, (600 + 60 * 33) * kMs); // still shown while held
        // A held modifier's repeats are no new presses.
        Model ctrl(config(Mode::All));
        ctrl.feed(press(VK_LCONTROL, 0, 0));
        for (int i = 1; i < 10; ++i) {
            Key repeat = press(VK_LCONTROL, 0, 500 + i * 33);
            repeat.repeat = true;
            ctrl.feed(repeat);
        }
        ctrl.feed(release(VK_LCONTROL, kCtrl, 900));
        QCOMPARE(ctrl.rows().size(), size_t(1));
        QCOMPARE(caps(ctrl), QStringList({u"Ctrl"_s}));
        QCOMPARE(ctrl.rows().back().count, 1);
    }

    void fadesAndExpiry()
    {
        Model model(config(Mode::Shortcuts));
        chord(model, kCtrl, 'Z', 0);
        const Row row = model.rows().back();
        QCOMPARE(row.shown, 10 * kMs);
        QCOMPARE(Model::opacity(row, row.shown), 0.0f);
        QCOMPARE(Model::opacity(row, row.shown + kFadeIn / 2), 0.5f);
        QCOMPARE(Model::opacity(row, row.shown + kFadeIn), 1.0f);
        QCOMPARE(Model::opacity(row, row.last + kLinger), 1.0f);
        QCOMPARE(Model::opacity(row, row.last + kLinger + kFadeOut / 2), 0.5f);
        QCOMPARE(Model::opacity(row, row.last + kLinger + kFadeOut), 0.0f);
        const quint64 before = model.revision();
        model.advance(row.last + kLinger + kFadeOut - 1);
        QCOMPARE(model.rows().size(), size_t(1));
        QCOMPARE(model.revision(), before);
        model.advance(row.last + kLinger + kFadeOut);
        QVERIFY(model.rows().empty());
        QVERIFY(model.revision() != before);
        // A press keeps the row: its count goes up and it lingers on.
        Model again(config(Mode::Shortcuts));
        chord(again, kCtrl, 'Z', 0);
        chord(again, kCtrl, 'Z', 2000);
        again.advance(10 * kMs + kLinger + kFadeOut);
        QCOMPARE(again.rows().size(), size_t(1));
        QCOMPARE(again.rows().back().count, 2);
    }

    void modifierTaps()
    {
        // Shortcuts mode: a lone Ctrl, Shift or AltGr is nothing.
        Model model(config(Mode::Shortcuts));
        tap(model, VK_LCONTROL, 0);
        tap(model, VK_LSHIFT, 2000);
        tap(model, kAltGrKey, 4000);
        QVERIFY(model.rows().empty());
        // Its double tap is; a third counts on.
        tap(model, VK_LCONTROL, 6000);
        tap(model, VK_LCONTROL, 6300);
        QCOMPARE(caps(model), QStringList({u"Ctrl"_s}));
        QCOMPARE(model.rows().back().count, 2);
        tap(model, VK_RCONTROL, 6600);
        QCOMPARE(model.rows().back().count, 3);
        // Too far apart, or something in between: no double tap.
        Model apart(config(Mode::Shortcuts));
        tap(apart, VK_LSHIFT, 0);
        tap(apart, VK_LSHIFT, 700);
        QVERIFY(apart.rows().empty());
        tap(apart, VK_LCONTROL, 3000);
        apart.feed(press(kHiddenKey, 0, 3150));
        tap(apart, VK_LCONTROL, 3300);
        QVERIFY(apart.rows().empty());
        // Win and Alt alone are shortcuts (Start, the menu bar).
        tap(apart, VK_LWIN, 5000);
        QCOMPARE(caps(apart), QStringList({u"Win"_s}));
        tap(apart, VK_LMENU, 5300);
        QCOMPARE(caps(apart), QStringList({u"Alt"_s}));
        // Held long: no tap.
        Model held(config(Mode::All));
        tap(held, VK_LSHIFT, 0, 1200);
        QVERIFY(held.rows().empty());
        // All mode: every tap.
        tap(held, VK_LSHIFT, 3000);
        QCOMPARE(caps(held), QStringList({u"Shift"_s}));
    }

    void modifierChords()
    {
        // Alt+Shift, Ctrl+Shift (the input language): shown in Shortcuts mode.
        Model model(config(Mode::Shortcuts));
        model.feed(press(VK_LMENU, 0, 0));
        model.feed(press(VK_LSHIFT, kAlt, 40));
        model.feed(release(VK_LSHIFT, kAlt | kShift, 120));
        QVERIFY(model.rows().empty()); // Alt still held
        model.feed(release(VK_LMENU, kAlt, 160));
        QCOMPARE(caps(model), QStringList({u"Alt"_s, u"Shift"_s}));
        // A modifier held from before the hook saw it: no chord, no tap.
        Model before(config(Mode::Shortcuts));
        before.feed(press(VK_LSHIFT, kCtrl, 0));
        before.feed(release(VK_LSHIFT, kCtrl | kShift, 80));
        before.feed(release(VK_LCONTROL, kCtrl, 120));
        QVERIFY(before.rows().empty());
        // A release never seen (it went to an administrator's window): the
        // next tap starts afresh.
        Model lost(config(Mode::All));
        lost.feed(press(VK_LCONTROL, 0, 0));
        tap(lost, VK_LSHIFT, 2000);
        QCOMPARE(caps(lost), QStringList({u"Shift"_s}));
    }

    void clicksSpoilTaps()
    {
        // Ctrl+click twice: no "Ctrl ×2", with the buttons shown or not.
        for (const bool mouse : {false, true}) {
            Model model(config(Mode::Shortcuts, mouse));
            for (const qint64 ms : {0, 300}) {
                model.feed(press(VK_LCONTROL, 0, ms));
                model.feed(press(VK_LBUTTON, kCtrl, ms + 50));
                model.feed(release(VK_LCONTROL, kCtrl, ms + 100));
            }
            QCOMPARE(model.rows().size(), size_t(mouse ? 1 : 0));
            if (mouse) {
                QCOMPARE(caps(model), QStringList({u"Ctrl"_s, Words().left}));
                QCOMPARE(model.rows().back().count, 2);
            }
        }
    }

    void mouseButtons()
    {
        Model off(config(Mode::All, false));
        off.feed(press(VK_LBUTTON, 0, 0));
        QVERIFY(off.rows().empty());
        Model model(config(Mode::Off, true));
        model.feed(press(VK_LBUTTON, 0, 0));
        QCOMPARE(caps(model), QStringList({Words().left}));
        model.feed(press(VK_LBUTTON, 0, 200)); // a double click
        QCOMPARE(model.rows().back().count, 2);
        model.feed(press(VK_RBUTTON, 0, 400));
        model.feed(press(VK_MBUTTON, kShift, 600));
        QCOMPARE(caps(model), QStringList({u"Shift"_s, Words().middle}));
        Key outside = press(VK_XBUTTON2, 0, 800);
        outside.inside = false;
        model.feed(outside);
        QCOMPARE(caps(model), QStringList({u"Shift"_s, Words().middle}));
        model.feed(press(VK_XBUTTON1, 0, 900));
        QCOMPARE(caps(model), QStringList({Words().back}));
        QCOMPARE(model.rows().size(), size_t(3));
        // Keys are not shown with the keys off.
        model.feed(press('S', kCtrl, 1000));
        QCOMPARE(caps(model), QStringList({Words().back}));
    }

    void typedText()
    {
        Model model(config(Mode::All));
        const QString hello = u"Hello"_s;
        for (int i = 0; i < hello.size(); ++i)
            model.feed(press(std::uint16_t(hello[i].toUpper().unicode()), i == 0 ? kShift : 0, 100 + i * 80, hello[i].unicode()));
        QCOMPARE(model.rows().size(), size_t(1));
        QVERIFY(model.rows().back().text);
        QCOMPARE(caps(model), QStringList({u"Hello"_s}));
        model.feed(press(VK_SPACE, 0, 600, u' '));
        model.feed(press('W', 0, 700, u'w'));
        QCOMPARE(caps(model), QStringList({u"Hello w"_s}));
        // Shortcuts and command keys are keycaps; typing goes on in a new row.
        chord(model, kCtrl, 'S', 800);
        QCOMPARE(caps(model), QStringList({u"Ctrl"_s, u"S"_s}));
        model.feed(press(VK_RETURN, 0, 900, u'\r'));
        QCOMPARE(caps(model), QStringList({u"Enter"_s}));
        // A space alone is a key, not an empty row of text.
        model.feed(press(VK_SPACE, 0, 1000, u' '));
        QCOMPARE(caps(model), QStringList({Words().space}));
        model.feed(press('X', 0, 1100, u'x'));
        QCOMPARE(caps(model), QStringList({u"x"_s}));
        // Only the tail shown is kept.
        for (int i = 0; i < 30; ++i)
            model.feed(press('A' + i % 26, 0, 1200 + i * 30, char16_t(u'a' + i % 26)));
        const Row& row = model.rows().back();
        QVERIFY(row.cut);
        QCOMPARE(int(row.caps.front().size()), kTextTail);
        QCOMPARE(row.caps.front(), u"opqrstuvwxyzabcd"_s);
        // AltGr: a character, not a shortcut.
        Model german(config(Mode::All));
        german.feed(press(kAltGrKey, 0, 0));
        german.feed(press('Q', kAltGr, 50, u'@'));
        german.feed(release(kAltGrKey, kAltGr, 100));
        QCOMPARE(german.rows().size(), size_t(1));
        QCOMPARE(caps(german), QStringList({u"@"_s}));
    }

    void stopHotkeyIsNotShown()
    {
        Config value = config(Mode::Shortcuts);
        value.own = {fromRegistered(MOD_CONTROL | MOD_SHIFT, 'A'), fromRegistered(0, VK_SNAPSHOT)};
        Model model(value);
        chord(model, kCtrl | kShift, 'A', 0);
        model.feed(press(VK_SNAPSHOT, 0, 300));
        QVERIFY(model.rows().empty()); // and no chord of its modifiers on release
        chord(model, kCtrl, 'A', 500); // other modifiers: another shortcut
        QCOMPARE(caps(model), QStringList({u"Ctrl"_s, u"A"_s}));
        model.feed(press(VK_SNAPSHOT, kAlt, 700));
        QCOMPARE(caps(model), QStringList({u"Alt"_s, u"PrtSc"_s}));
    }

    void clearForgetsEverything()
    {
        Model model(config(Mode::All));
        model.feed(press('A', 0, 0, u'a'));
        model.feed(press(VK_LCONTROL, 0, 100)); // held across the pause
        const quint64 before = model.revision();
        model.clear();
        QVERIFY(model.rows().empty());
        QVERIFY(model.revision() != before);
        model.feed(release(VK_LCONTROL, kCtrl, 200));
        QVERIFY(model.rows().empty());
        model.feed(press('B', 0, 300, u'b'));
        QCOMPARE(caps(model), QStringList({u"b"_s})); // a new row: nothing typed before is kept
        // A first tap before it: no double tap after.
        Model shortcuts(config(Mode::Shortcuts));
        tap(shortcuts, VK_LCONTROL, 0);
        shortcuts.clear();
        tap(shortcuts, VK_LCONTROL, 200);
        QVERIFY(shortcuts.rows().empty());
    }

    void picture()
    {
        std::vector<Row> rows(2);
        rows[0].caps = QStringList({u"Ctrl"_s, u"Shift"_s, u"S"_s});
        rows[0].count = 3;
        rows[1].caps = QStringList({u"hello world"_s});
        rows[1].text = true;
        rows[1].cut = true;
        const int unit = 54;
        const Picture picture = render(rows, QGuiApplication::font(), unit);
        QCOMPARE(picture.image.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(picture.rows.size(), size_t(2));
        for (const QRect& row : picture.rows) {
            QCOMPARE(row.height(), unit);
            QVERIFY(picture.image.rect().contains(row));
            QVERIFY(row.width() > unit);
        }
        QVERIFY(!picture.rows[0].intersects(picture.rows[1]));
        // A dark, nearly opaque row; transparent outside its rounded corners.
        const QRect& first = picture.rows[0];
        const QRgb inside = picture.image.pixel(first.left() + 2, first.center().y());
        QVERIFY(qAlpha(inside) > 200);
        QVERIFY(qRed(inside) < 60);
        QCOMPARE(qAlpha(picture.image.pixel(first.topLeft())), 0);
        QCOMPARE(qAlpha(picture.image.pixel(first.bottomRight())), 0);
        // White letters on the keycaps.
        int white = 0;
        for (int y = first.top(); y <= first.bottom(); ++y)
            for (int x = first.left(); x <= first.right(); ++x) {
                const QRgb pixel = picture.image.pixel(x, y);
                if (qAlpha(pixel) > 200 && qRed(pixel) > 220 && qGreen(pixel) > 220 && qBlue(pixel) > 220)
                    ++white;
            }
        QVERIFY2(white > 40, qPrintable(QString::number(white)));
        // The newest (the last) at the bottom right, the one before above it.
        const QSize region(1920, 1080);
        const auto right = place(picture, region, unit, false);
        QCOMPARE(right[1], QPoint(1920 - marginFor(unit) - picture.rows[1].width(), 1080 - marginFor(unit) - unit));
        QCOMPARE(right[0], QPoint(1920 - marginFor(unit) - picture.rows[0].width(), right[1].y() - rowGapFor(unit) - unit));
        const auto left = place(picture, region, unit, true);
        QCOMPARE(left[0].x(), marginFor(unit));
        QCOMPARE(left[1].x(), marginFor(unit));
        QCOMPARE(left[1].y(), right[1].y());
        QVERIFY(render({}, QGuiApplication::font(), unit).image.isNull());
        // A count adds to the row; a GIF's larger unit makes all of it larger.
        rows[0].count = 1;
        QVERIFY(render(rows, QGuiApplication::font(), unit).rows[0].width() < first.width());
        QVERIFY(render(rows, QGuiApplication::font(), 91).rows[0].width() > first.width());
    }

    void keyQueue()
    {
        using ws::capture::recording::KeyStream;
        KeyStream stream;
        Key key;
        key.vk = 'A';
        key.time = 100;
        stream.push(key);
        key.time = 110;
        stream.push(key);
        auto batch = stream.take();
        QCOMPARE(batch.count, size_t(2));
        QCOMPARE(batch.keys[1].time, qint64(110));
        QCOMPARE(stream.take().count, size_t(0));
        // A pause: what is queued goes, and what comes late from before it.
        stream.push(key);
        stream.discardBefore(200);
        key.time = 199;
        stream.push(key);
        QCOMPARE(stream.take().count, size_t(0));
        key.time = 200;
        stream.push(key);
        QCOMPARE(stream.take().count, size_t(1));
        // Bounded: the newest kept.
        for (int i = 0; i < 100; ++i) {
            key.time = 300 + i;
            stream.push(key);
        }
        batch = stream.take();
        QCOMPARE(batch.count, KeyStream::capacity);
        QCOMPARE(batch.keys[0].time, qint64(300 + 100 - KeyStream::capacity));
        QCOMPARE(batch.keys[batch.count - 1].time, qint64(399));
    }

    // KeyHook's record of the keys held (the #98 review): AltGr and the
    // modifiers as the system has them, whatever the hook did not see.
    void keysHeldAsTheSystemHasThem()
    {
        using ws::capture::recording::KeyBook;
        g_down.reset();
        KeyBook book;
        // AltGr: the left Ctrl Windows sends first (scan code with 0x200), then the right Alt.
        QVERIFY(hook(book, VK_LCONTROL, true, 0, 0x21D).skip);
        auto seen = hook(book, VK_RMENU, true, 1);
        QCOMPARE(seen.vk, kAltGrKey);
        QVERIFY(!seen.repeat);
        QCOMPARE(hook(book, 'Q', true, 50).mods, std::uint8_t(kAltGr)); // "@" on a German keyboard: typing
        hook(book, 'Q', false, 80);
        // Let go on the secure desktop (AltGr+Del, a UAC prompt) or in a
        // program run as administrator: the hook never saw it.
        g_down.reset(VK_RMENU);
        g_down.reset(VK_LCONTROL);
        QVERIFY(!hook(book, VK_LCONTROL, true, 2000).skip); // the real left Ctrl
        QCOMPARE(hook(book, 'S', true, 2050).mods, std::uint8_t(kCtrl)); // Ctrl + S, not AltGr
        hook(book, 'S', false, 2080);
        hook(book, VK_LCONTROL, false, 2100);
        QCOMPARE(book.modifiers(&fakeHeld), std::uint8_t(0));

        // In again while AltGr is held (pressed while it was out): AltGr, not Ctrl + Alt.
        g_down.set(VK_LCONTROL);
        g_down.set(VK_RMENU);
        book.resume(&fakeHeld);
        QCOMPARE(hook(book, 'Q', true, 3000).mods, std::uint8_t(kAltGr));
        hook(book, 'Q', false, 3030);
        QCOMPARE(hook(book, VK_RMENU, false, 3100).vk, kAltGrKey);
        QVERIFY(hook(book, VK_LCONTROL, false, 3101, 0x21D).skip);
        // A left Ctrl seen down for itself, the right Alt pressed while it was out: Ctrl + Alt.
        hook(book, VK_LCONTROL, true, 4000);
        g_down.set(VK_RMENU);
        book.resume(&fakeHeld);
        QCOMPARE(hook(book, 'X', true, 4100).mods, std::uint8_t(kCtrl | kAlt));
        hook(book, 'X', false, 4130);
        hook(book, VK_RMENU, false, 4200);
        hook(book, VK_LCONTROL, false, 4210);

        // A modifier held from before the hook went in: its auto-repeat is
        // no press, so letting it go is no tap of it.
        g_down.set(VK_LMENU);
        book.resume(&fakeHeld);
        Model model(config(Mode::Shortcuts));
        seen = hook(book, VK_LMENU, true, 5000);
        QVERIFY(seen.repeat);
        Key key = press(seen.vk, seen.mods, 5000);
        key.repeat = true;
        model.feed(key);
        seen = hook(book, VK_LMENU, false, 5300);
        key = press(seen.vk, seen.mods, 5300);
        key.down = false;
        model.feed(key);
        QVERIFY(model.rows().empty());
        // Pressed afresh: a press, its auto-repeat a repeat, and alone a tap of Alt.
        seen = hook(book, VK_LMENU, true, 6000);
        QVERIFY(!seen.repeat);
        model.feed(press(seen.vk, seen.mods, 6000));
        QVERIFY(hook(book, VK_LMENU, true, 6400).repeat);
        seen = hook(book, VK_LMENU, false, 6500);
        key = press(seen.vk, seen.mods, 6500);
        key.down = false;
        model.feed(key);
        QCOMPARE(model.rows().size(), size_t(1));
        QCOMPARE(model.rows().back().caps, QStringList {u"Alt"_s});

        // Other keys: seen down and not let go since, lately.
        QVERIFY(!hook(book, 'A', true, 7000).repeat);
        QVERIFY(hook(book, 'A', true, 7030).repeat);
        g_down.reset('A'); // let go while the hook was out
        book.resume(&fakeHeld);
        QVERIFY(!hook(book, 'A', true, 7100).repeat);
        g_down.reset('A'); // let go unseen, down again long after
        QVERIFY(!hook(book, 'A', true, 9000).repeat);
        hook(book, 'A', false, 9050);
    }

    // Windows took the hooks out: input well past their last call.
    void droppedHooks()
    {
        using ws::capture::recording::inputWentPast;
        QVERIFY(!inputWentPast(1000, 1000));
        QVERIFY(!inputWentPast(1500, 1000)); // 500 ms: not yet
        QVERIFY(inputWentPast(1501, 1000));
        QVERIFY(!inputWentPast(900, 1000)); // before the last call
        QVERIFY(inputWentPast(0x00000200u, 0xFFFFFF00u)); // across the clock's wrap: 768 ms
        QVERIFY(!inputWentPast(0xFFFFFF00u, 0x00000200u));
    }

    void sizes()
    {
        QCOMPARE(unitFor(QSize(1920, 1080), 1, 1), 54); // 5% of 1080
        QCOMPARE(unitFor(QSize(1280, 720), 1, 1), 36); // 36 logical pixels
        QCOMPARE(unitFor(QSize(1280, 720), 1.5, 1), 54);
        QCOMPARE(unitFor(QSize(2560, 1440), 1.5, 1), 72);
        // GIF: 34 pixels of its own after scaling down.
        QCOMPARE(unitFor(QSize(2560, 1440), 1.5, 960.0 / 2560), 91);
        QCOMPARE(unitFor(QSize(1920, 1080), 1, 1280.0 / 1920), 54);
        QCOMPARE(unitFor(QSize(1920, 1080), 1, 960.0 / 1920), 68);
        // Small regions: at most 16% of the shorter side, 20 at least.
        QCOMPARE(unitFor(QSize(300, 150), 1, 1), 24);
        QCOMPARE(unitFor(QSize(100, 60), 2, 1), 20);
        QCOMPARE(marginFor(54), 27);
        QCOMPARE(marginFor(20), 10);
        QCOMPARE(rowsThatFit(QSize(1920, 1080), 54), kRows);
        QCOMPARE(rowsThatFit(QSize(300, 100), 24), 2);
        QCOMPARE(rowsThatFit(QSize(100, 30), 20), 1);
        // Out of the pointer's way.
        const QSize region(1920, 1080);
        QVERIFY(preferLeft(QPoint(1800, 1000), region, 54));
        QVERIFY(preferLeft(QPoint(1920 - 9 * 54, 1080 - 243), region, 54));
        QVERIFY(!preferLeft(QPoint(1919 - 9 * 54, 1000), region, 54));
        QVERIFY(!preferLeft(QPoint(1800, 1080 - 244), region, 54));
        QVERIFY(!preferLeft(QPoint(-5, -5), region, 54));
        QVERIFY(preferLeft(QPoint(399, 299), QSize(400, 300), 54)); // half the region at most
        QVERIFY(!preferLeft(QPoint(199, 299), QSize(400, 300), 54));
    }
};

QTEST_MAIN(TestKeycaps)
#include "tst_keycaps.moc"
