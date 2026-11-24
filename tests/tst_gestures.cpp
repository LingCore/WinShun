// The mouse-gestures plugin (src/gestures-plugin) without any window: the
// rules (Gesture) -- the directions a drag draws (a stroke long enough,
// clearly along one axis, the same direction once, a slant for nothing)
// and the circles, the actions as written (names, keys, things to open,
// nonsense), the rules kept as JSON and which of them holds where, 1.0's
// settings read into rules, the screen's corners pushed into and edges
// rubbed along, and the programs left alone.

#include "Gesture.h"

#include <QTest>

#include <cmath>
#include <cstring>

#include <windows.h>

using namespace gestures;

class GesturesTest : public QObject {
    Q_OBJECT

private slots:
    void directionsOfADrag()
    {
        Recognizer r;
        r.reset(100, 100, 20);
        QVERIFY(!r.add(110, 102)); // not far enough yet
        QVERIFY(r.add(100, 125)); // down
        QCOMPARE(r.directions(), std::string("D"));
        QVERIFY(!r.add(102, 160)); // more of the same
        QVERIFY(r.add(140, 162)); // then right
        QCOMPARE(r.directions(), std::string("DR"));
        QVERIFY(!r.add(141, 163));
        QCOMPARE(r.directions(), std::string("DR"));
    }

    void aSlantCountsForNothing()
    {
        Recognizer r;
        r.reset(0, 0, 20);
        QVERIFY(!r.add(22, 22)); // 45 degrees
        QVERIFY(!r.add(30, 28));
        QVERIFY(r.directions().empty());
        // Long enough, the point moves on: what follows counts from there.
        QVERIFY(!r.add(70, 70));
        QVERIFY(r.add(70, 40)); // up from (70, 70)
        QCOMPARE(r.directions(), std::string("U"));
    }

    void tooLong()
    {
        Recognizer r;
        r.reset(0, 0, 10);
        int x = 0;
        for (int i = 0; i < 7; ++i) {
            x += i % 2 == 0 ? 30 : -30;
            r.add(x, 0);
        }
        QCOMPARE(r.directions(), std::string("RLRLRLR"));
        QVERIFY(r.tooLong());
    }

    void arrowsAndBack()
    {
        QCOMPARE(arrows("DR"), std::string("\xE2\x86\x93\xE2\x86\x92"));
        QCOMPARE(parseDirections(L"↓→"), std::string("DR"));
        QCOMPARE(parseDirections(L"ul"), std::string("UL"));
        QCOMPARE(parseDirections(L"下 右"), std::string("DR"));
        QCOMPARE(parseDirections(L"U-D"), std::string("UD"));
        QCOMPARE(parseDirections(L"DD"), std::string()); // twice the same: a typo
        QCOMPARE(parseDirections(L"X"), std::string());
        QCOMPARE(parseDirections(L"LRLRLRL"), std::string()); // too long
    }

    void actionsAsWritten()
    {
        Action a = parseAction(L"  后退 ");
        QCOMPARE(a.kind, Action::Kind::Builtin);
        QCOMPARE(a.builtin, Builtin::Back);
        QCOMPARE(parseAction(L"Search").builtin, Builtin::Search);
        QCOMPARE(parseAction(L"Win顺").builtin, Builtin::Search);
        QCOMPARE(parseAction(L"关闭标签页").builtin, Builtin::CloseTab);
        QCOMPARE(parseAction(L"").kind, Action::Kind::Nothing);

        a = parseAction(L"ctrl + shift + t");
        QCOMPARE(a.kind, Action::Kind::Keys);
        QCOMPARE(a.keys, (std::vector<std::uint16_t> {VK_CONTROL, VK_SHIFT, 'T'}));
        QCOMPARE(parseAction(L"F5").keys, (std::vector<std::uint16_t> {VK_F5}));
        QCOMPARE(parseAction(L"Alt+Left").keys, (std::vector<std::uint16_t> {VK_MENU, VK_LEFT}));
        QCOMPARE(parseAction(L"Win+D").keys, (std::vector<std::uint16_t> {VK_LWIN, 'D'}));
        QCOMPARE(parseAction(L"Ctrl+/").keys, (std::vector<std::uint16_t> {VK_CONTROL, VK_OEM_2}));
        QCOMPARE(parseAction(L"Ctrl+Ctrl+W").kind, Action::Kind::Invalid);
        QCOMPARE(parseAction(L"W+Ctrl").kind, Action::Kind::Invalid);

        a = parseAction(L"notepad.exe");
        QCOMPARE(a.kind, Action::Kind::Open);
        QCOMPARE(a.target, std::wstring(L"notepad.exe"));
        a = parseAction(L"\"C:\\Program Files\\App\\app.exe\"");
        QCOMPARE(a.kind, Action::Kind::Open);
        QCOMPARE(a.target, std::wstring(L"C:\\Program Files\\App\\app.exe"));
        QCOMPARE(parseAction(L"https://lcstp.com").kind, Action::Kind::Open);
        QCOMPARE(parseAction(L"%USERPROFILE%\\Downloads").kind, Action::Kind::Open);

        QCOMPARE(parseAction(L"随便写的").kind, Action::Kind::Invalid);
    }

    void titles()
    {
        QCOMPARE(actionTitle(parseAction(L"close"), true), std::wstring(L"关闭窗口"));
        QCOMPARE(actionTitle(parseAction(L"关闭窗口"), false), std::wstring(L"Close the window"));
        QCOMPARE(actionTitle(parseAction(L"Ctrl+W"), true), std::wstring(L"Ctrl+W"));
        QCOMPARE(actionTitle(parseAction(L"C:\\Tools\\x.exe"), true), std::wstring(L"打开 x.exe"));
        QCOMPARE(actionTitle(parseAction(L"https://a.b/c"), false), std::wstring(L"Open https://a.b/c"));
        QCOMPARE(actionTitle(parseAction(L""), true), std::wstring());
    }

    void moreGestures()
    {
        const auto more = parseMore(L"↑← = Ctrl+Shift+T; RL=notepad.exe；bad = x; UD=; =F5\nDLR = 刷新");
        QCOMPARE(more.size(), std::size_t(3));
        QCOMPARE(more[0].first, std::string("UL"));
        QCOMPARE(more[0].second, std::wstring(L"Ctrl+Shift+T"));
        QCOMPARE(more[1].first, std::string("RL"));
        QCOMPARE(more[2].first, std::string("DLR"));
        QCOMPARE(more[2].second, std::wstring(L"刷新"));
    }

    void programsLeftAlone()
    {
        const auto list = programList(L"Photoshop.exe; mstsc , \"C:\\Games\\Game.EXE\"；");
        QCOMPARE(list, (std::vector<std::wstring> {L"photoshop.exe", L"mstsc.exe", L"game.exe"}));
        QVERIFY(listed(list, L"C:\\Program Files\\Adobe\\PHOTOSHOP.EXE"));
        QVERIFY(listed(list, L"mstsc.exe"));
        QVERIFY(!listed(list, L"notepad.exe"));
        QVERIFY(!listed(list, L""));
    }

    void circles()
    {
        // Round from the top, clockwise on the screen (y down), and the
        // other way: a circle, whatever directions it went through.
        const auto draw = [](bool clockwise, double turns, double flat = 1.0) {
            Recognizer r;
            const double pi = 3.14159265358979;
            const auto at = [&](double a) {
                return std::pair {static_cast<int>(std::lround(300 + 80 * std::sin(a))), static_cast<int>(std::lround(300 - 80 * flat * std::cos(a)))};
            };
            const auto [x0, y0] = at(0);
            r.reset(x0, y0, 24);
            const int steps = static_cast<int>(48 * turns);
            for (int i = 1; i <= steps; ++i) {
                const auto [x, y] = at((clockwise ? 1 : -1) * 2 * pi * i / 48);
                r.add(x, y);
            }
            return r;
        };
        QCOMPARE(draw(true, 1).gesture(), std::string(kClockwise));
        QCOMPARE(draw(false, 1).gesture(), std::string(kCounterClockwise));
        QCOMPARE(draw(true, 2).gesture(), std::string(kClockwise)); // twice round
        QVERIFY(!draw(true, 2).tooLong());
        QCOMPARE(draw(true, 0.5).gesture(), draw(true, 0.5).directions()); // half: just directions
        QVERIFY(draw(true, 1, 0.2).gesture() != std::string(kClockwise)); // too flat

        // A square, corner by corner: turns a quarter three times, no circle.
        Recognizer square;
        square.reset(0, 0, 24);
        for (int x = 10; x <= 200; x += 10)
            square.add(x, 0);
        for (int y = 10; y <= 200; y += 10)
            square.add(200, y);
        for (int x = 190; x >= 0; x -= 10)
            square.add(x, 200);
        for (int y = 190; y >= 0; y -= 10)
            square.add(0, y);
        QCOMPARE(square.gesture(), std::string("RDLU"));

        // A zigzag turns back sharply: no part of a circle.
        Recognizer zigzag;
        zigzag.reset(0, 0, 24);
        for (int i = 1; i <= 6; ++i)
            zigzag.add(i % 2 ? 100 : 0, i * 10);
        QVERIFY(zigzag.gesture() != std::string(kClockwise) && zigzag.gesture() != std::string(kCounterClockwise));

        QCOMPARE(arrows(kClockwise), std::string("\xE2\x86\xBB"));
        QCOMPARE(arrows(kCounterClockwise), std::string("\xE2\x86\xBA"));
        QCOMPARE(parseDirections(L"↻"), std::string(kClockwise));
        QCOMPARE(parseDirections(L" 逆时针 "), std::string(kCounterClockwise));
        QCOMPARE(parseDirections(L"CW"), std::string(kClockwise));
    }

    void rulesKeptAsJson()
    {
        const std::vector<Rule> defaults = defaultRules();
        const auto again = parseRules(rulesJson(defaults));
        QVERIFY(again.has_value());
        QVERIFY(*again == defaults);

        std::vector<Rule> rules;
        Rule r;
        r.on = "cw";
        r.where = Zone::BottomRight;
        r.action = builtinAction(Builtin::Shutdown);
        rules.push_back(r);
        r = {};
        r.on = "L";
        r.apps = {L"photoshop.exe"};
        r.action = keysAction(L"Ctrl+Z");
        rules.push_back(r);
        r = {};
        r.on = "corner:tl";
        r.action = openAction(L"\"C:\\Tools\\x.exe\"");
        rules.push_back(r);
        r = {};
        r.on = "edge:right";
        r.action = typeAction(L"你好\nworld");
        rules.push_back(r);
        r = {};
        r.on = "DU";
        r.selection = true; // does nothing: keeps the plain one off while words are selected
        rules.push_back(r);
        r = {};
        r.on = "wheel:up";
        r.apps = {L"code.exe"};
        r.action = keysAction(L"Ctrl+PageUp");
        rules.push_back(r);
        const auto read = parseRules(rulesJson(rules));
        QVERIFY(read.has_value());
        QVERIFY(*read == rules);
        QCOMPARE((*read)[2].action.target, std::wstring(L"C:\\Tools\\x.exe"));

        // What does not read is left out; the same case twice: the later
        // one; a name not known is kept, as not understood; conditions only
        // where they mean something.
        const auto messy = parseRules(R"({"version":2,"rules":[{"on":"X","do":"copy"},{"on":"DD","do":"copy"},
            {"on":"L","do":"back"},{"on":"L","do":"forward"},{"on":"R","do":"fly"},
            {"on":"corner:tl","where":"br","selection":true,"apps":["a"],"do":"lock"},
            {"on":"U","apps":["A","a.exe","C:/x/B.EXE"],"do":"top"},7]})");
        QVERIFY(messy.has_value());
        QCOMPARE(messy->size(), std::size_t(4));
        QCOMPARE((*messy)[0].action.builtin, Builtin::Forward);
        QCOMPARE((*messy)[1].action.kind, Action::Kind::Invalid);
        QCOMPARE((*messy)[1].action.text, std::wstring(L"fly"));
        QCOMPARE((*messy)[2].where, Zone::Anywhere);
        QVERIFY(!(*messy)[2].selection && (*messy)[2].apps.empty());
        QCOMPARE((*messy)[3].apps, (std::vector<std::wstring> {L"a.exe", L"b.exe"}));
        QVERIFY(parseRules(rulesJson(*messy)).value() == *messy);

        QVERIFY(!parseRules("").has_value());
        QVERIFY(!parseRules("[1,2]").has_value());
        QVERIFY(!parseRules(R"({"rules":3})").has_value());
        QVERIFY(parseRules(R"({"rules":[]})").value().empty());
    }

    void whichRuleHolds()
    {
        std::vector<Rule> rules = defaultRules();
        Rule corner;
        corner.on = "cw";
        corner.where = Zone::BottomRight;
        corner.action = builtinAction(Builtin::Shutdown);
        rules.push_back(corner);
        Rule anywhere;
        anywhere.on = "cw";
        anywhere.action = builtinAction(Builtin::Refresh);
        rules.push_back(anywhere);
        Rule photoshop;
        photoshop.on = "D";
        photoshop.apps = {L"photoshop.exe"};
        photoshop.action = keysAction(L"Ctrl+Z");
        rules.push_back(photoshop);
        Rule off;
        off.on = "L";
        off.apps = {L"game.exe"};
        rules.push_back(off);

        const Context plain;
        QCOMPARE(match(rules, "D", plain)->action.builtin, Builtin::Bottom);
        Context selected;
        selected.selection = true;
        QCOMPARE(match(rules, "D", selected)->action.builtin, Builtin::Copy);
        QCOMPARE(match(rules, "U", selected)->action.builtin, Builtin::WebSearch);
        QCOMPARE(match(rules, "L", selected)->action.builtin, Builtin::Back); // none of its own: as ever
        selected.program = L"photoshop.exe";
        QCOMPARE(match(rules, "D", selected)->action.kind, Action::Kind::Keys); // the program's over the selection's

        Context inCorner;
        inCorner.zone = Zone::BottomRight;
        QCOMPARE(match(rules, "cw", inCorner)->action.builtin, Builtin::Shutdown);
        inCorner.zone = Zone::TopRight;
        QCOMPARE(match(rules, "cw", inCorner)->action.builtin, Builtin::Refresh);
        QCOMPARE(match(rules, "cw", plain)->action.builtin, Builtin::Refresh);

        Context game;
        game.program = L"game.exe";
        QCOMPARE(match(rules, "L", game)->action.kind, Action::Kind::Nothing); // kept off there
        QVERIFY(match(rules, "LR", plain) == nullptr);
        QCOMPARE(match(rules, "wheel:down", plain)->action.builtin, Builtin::NextTab);

        QVERIFY(selectionMatters(rules, "", L"notepad.exe"));
        QVERIFY(selectionMatters(rules, "D", L"notepad.exe"));
        QVERIFY(!selectionMatters(rules, "L", L"notepad.exe"));
        QVERIFY(!selectionMatters(rules, "DR", L"notepad.exe"));
        QVERIFY(anyFor(rules, TriggerKind::Wheel));
        QVERIFY(!anyFor(rules, TriggerKind::Corner));
        QVERIFY(!anyFor(rules, TriggerKind::Edge));

        QCOMPARE(triggerKind("corner:br"), TriggerKind::Corner);
        QCOMPARE(triggerKind("edge:left"), TriggerKind::Edge);
        QCOMPARE(triggerKind("wheel:up"), TriggerKind::Wheel);
        QCOMPARE(triggerKind("ccw"), TriggerKind::Gesture);
        QCOMPARE(triggerKind("LRLRLRL"), TriggerKind::Unknown);
        QCOMPARE(triggerTitle("corner:br", true), std::wstring(L"右下角"));
        QCOMPARE(triggerTitle("edge:top", false), std::wstring(L"Top edge"));
        QCOMPARE(triggerTitle("DR", true), std::wstring(L"↓→"));
    }

    void shortcuts()
    {
        // As the recorder writes them, as written by hand, and those that would get in the way of typing.
        QCOMPARE(keyTrigger(L"Ctrl+Alt+T"), std::string("key:Ctrl+Alt+T"));
        QCOMPARE(keyTrigger(L" alt + ctrl+t"), std::string("key:Ctrl+Alt+T"));
        QCOMPARE(keyTrigger(L"Shift+Win+space"), std::string("key:Win+Shift+Space"));
        QCOMPARE(keyTrigger(L"F8"), std::string("key:F8"));
        QCOMPARE(keyTrigger(L"Ctrl+/"), std::string("key:Ctrl+/"));
        QVERIFY(keyTrigger(L"T").empty());
        QVERIFY(keyTrigger(L"Shift+T").empty());
        QVERIFY(keyTrigger(L"Ctrl+Alt").empty());
        QVERIFY(keyTrigger(L"Ctrl+Nope").empty());
        QVERIFY(keyTrigger(L"").empty());

        QCOMPARE(triggerKind("key:Ctrl+Alt+T"), TriggerKind::Key);
        QCOMPARE(triggerKind("key:ctrl+alt+t"), TriggerKind::Unknown); // only as kept
        QCOMPARE(triggerKind("key:T"), TriggerKind::Unknown);
        QCOMPARE(triggerTitle("key:Ctrl+Alt+T", true), std::wstring(L"Ctrl+Alt+T"));
        unsigned modifiers = 0;
        unsigned vk = 0;
        QVERIFY(hotkeyOf("key:Win+Shift+Space", &modifiers, &vk));
        QCOMPARE(modifiers, unsigned(MOD_WIN | MOD_SHIFT));
        QCOMPARE(vk, unsigned(VK_SPACE));
        QVERIFY(hotkeyOf("key:F8", &modifiers, &vk));
        QCOMPARE(modifiers, 0u);
        QCOMPARE(vk, unsigned(VK_F8));
        QVERIFY(!hotkeyOf("DR", &modifiers, &vk));

        // Kept as JSON, the hand-written spelling read as kept; for words selected or not, per program.
        const auto read = parseRules(R"({"version":2,"rules":[{"on":"key:alt+ctrl+t","selection":true,"where":"br","do":"translate"},
            {"on":"key:Ctrl+Alt+T","do":"websearch"},{"on":"key:F9","apps":["code"],"keys":"Ctrl+S"},{"on":"key:Q","do":"copy"}]})");
        QVERIFY(read.has_value());
        QCOMPARE(read->size(), std::size_t(3));
        QCOMPARE((*read)[0].on, std::string("key:Ctrl+Alt+T"));
        QVERIFY((*read)[0].selection);
        QCOMPARE((*read)[0].where, Zone::Anywhere);
        QVERIFY(!(*read)[1].selection);
        QCOMPARE((*read)[2].apps, std::vector<std::wstring> {L"code.exe"});
        QVERIFY(parseRules(rulesJson(*read)).value() == *read);

        // Words selected: the rule for them; none: the plain one; a gesture is never asked about words for a shortcut's rule.
        std::vector<Rule> rules = *read;
        Context selected;
        selected.selection = true;
        QCOMPARE(match(rules, "key:Ctrl+Alt+T", selected)->action.builtin, Builtin::Translate);
        QCOMPARE(match(rules, "key:Ctrl+Alt+T", Context {})->action.builtin, Builtin::WebSearch);
        Context code;
        code.program = L"code.exe";
        QCOMPARE(match(rules, "key:F9", code)->action.kind, Action::Kind::Keys);
        QVERIFY(match(rules, "key:F9", Context {}) == nullptr);
        QVERIFY(!selectionMatters({rules[0]}, "", L"notepad.exe"));
        QVERIFY(anyFor(rules, TriggerKind::Key));
        QVERIFY(!anyFor(defaultRules(), TriggerKind::Key));
    }

    void oldSettingsRead()
    {
        std::vector<std::pair<std::string, std::wstring>> written;
        for (const Slot& s : legacySlots())
            written.emplace_back(s.key, s.fallback);
        QVERIFY(!migrate(written, L"").has_value()); // as they came: nothing to keep
        QVERIFY(!migrate({}, L"").has_value());

        written[0].second = L"Ctrl+Z"; // left
        written[5].second = L""; // downLeft: off
        const auto rules = migrate(written, L"↑← = Ctrl+Shift+T; DR = 刷新");
        QVERIFY(rules.has_value());
        QCOMPARE(match(*rules, "L", {})->action.keys, (std::vector<std::uint16_t> {VK_CONTROL, 'Z'}));
        QVERIFY(match(*rules, "DL", {}) == nullptr);
        QCOMPARE(match(*rules, "UL", {})->action.kind, Action::Kind::Keys);
        QCOMPARE(match(*rules, "DR", {})->action.builtin, Builtin::Refresh);
        Context selected;
        selected.selection = true;
        QCOMPARE(match(*rules, "D", selected)->action.builtin, Builtin::Copy); // the new ones come too
    }

    void cornersPushedInto()
    {
        Screen screen;
        screen.bounds = {0, 0, 1920, 1080};
        screen.hardLeft = screen.hardTop = screen.hardRight = screen.hardBottom = true;
        CornerWatch w;
        QVERIFY(w.move(0, 0, screen, 0));
        QVERIFY(w.due(100).empty());
        QCOMPARE(w.due(160), std::string_view("corner:tl"));
        QVERIFY(w.due(400).empty()); // once
        QVERIFY(!w.move(1, 1, screen, 500)); // still there: done already
        QVERIFY(!w.move(30, 30, screen, 600)); // not far enough away yet
        QVERIFY(w.due(800).empty());
        QVERIFY(!w.move(0, 0, screen, 900));
        QVERIFY(!w.move(100, 100, screen, 1000)); // away: again next time
        QVERIFY(w.move(0, 1, screen, 1100));
        QCOMPARE(w.due(1300), std::string_view("corner:tl"));

        // Gone before its time: nothing.
        QVERIFY(w.move(1919, 1079, screen, 2000));
        QVERIFY(!w.move(1800, 1000, screen, 2050));
        QVERIFY(w.due(2300).empty());
        QVERIFY(w.move(1918, 1078, screen, 2400));
        QCOMPARE(w.due(2600), std::string_view("corner:br"));

        // A side that goes on to another screen has no corner.
        CornerWatch other;
        screen.hardRight = false;
        QVERIFY(!other.move(1919, 0, screen, 0));
        QVERIFY(other.due(500).empty());
        QVERIFY(other.move(0, 1079, screen, 600));
        QCOMPARE(other.due(800), std::string_view("corner:bl"));

        QCOMPARE(zoneAt(10, 10, {0, 0, 1920, 1080}), Zone::TopLeft);
        QCOMPARE(zoneAt(1900, 1070, {0, 0, 1920, 1080}), Zone::BottomRight);
        QCOMPARE(zoneAt(1900, 500, {0, 0, 1920, 1080}), Zone::Anywhere);
        QCOMPARE(zoneAt(3500, 100, {1920, 0, 3840, 1080}), Zone::TopRight);
    }

    void edgesRubbed()
    {
        Screen screen;
        screen.bounds = {0, 0, 1920, 1080};
        screen.hardLeft = screen.hardTop = screen.hardRight = screen.hardBottom = true;
        const auto rub = [&](RubWatch& w, bool alongTop, int at, std::initializer_list<int> path, std::int64_t start, int stepMs) {
            std::string_view got;
            std::int64_t t = start;
            for (const int p : path) {
                const std::string_view e = alongTop ? w.move(p, at, screen, t) : w.move(at, p, screen, t);
                if (!e.empty())
                    got = e;
                t += stepMs;
            }
            return got;
        };
        const std::initializer_list<int> upAndDown {400, 440, 480, 440, 400, 440, 480, 440, 400};
        RubWatch w;
        QCOMPARE(rub(w, false, 0, upAndDown, 0, 60), std::string_view("edge:left")); // three turns
        QVERIFY(rub(w, false, 0, upAndDown, 600, 60).empty()); // right after: quiet
        QVERIFY(rub(w, false, 1, upAndDown, 5000, 400).empty()); // slowly: the turns too far apart

        RubWatch shortLegs;
        QVERIFY(rub(shortLegs, false, 0, {400, 410, 420, 410, 400, 410, 420, 410, 400}, 0, 50).empty());

        RubWatch off; // off the edge between: starts over
        QVERIFY(rub(off, false, 0, {400, 440, 480, 440, 400}, 0, 60).empty());
        QVERIFY(off.move(40, 400, screen, 400).empty());
        QVERIFY(rub(off, false, 0, {440, 480, 440}, 460, 60).empty());

        RubWatch top;
        QCOMPARE(rub(top, true, 1, {900, 940, 980, 940, 900, 940, 980, 940, 900}, 0, 50), std::string_view("edge:top"));

        screen.hardLeft = false; // goes on to another screen: no edge
        RubWatch soft;
        QVERIFY(rub(soft, false, 0, upAndDown, 0, 60).empty());
    }

    void defaultsRead()
    {
        for (const Slot& entry : legacySlots()) {
            const Action a = parseAction(entry.fallback);
            QCOMPARE(a.kind, Action::Kind::Builtin);
            QCOMPARE(parseDirections(std::wstring(entry.directions, entry.directions + std::strlen(entry.directions))), std::string(entry.directions));
        }
    }

    void shortcutMaskPolicy()
    {
        const auto check = [](std::string_view trigger, bool expected) {
            unsigned mods = 0;
            unsigned vk = 0;
            QVERIFY2(hotkeyOf(trigger, &mods, &vk), trigger.data());
            QCOMPARE(shortcutNeedsMask(mods), expected);
        };
        // Alt, Win and Shift chords need a mask key so releasing the modifier does not
        // focus Chrome/Edge's menu button, activate a menu bar, open Start, or toggle IME.
        check("key:Alt+D", true);
        check("key:Ctrl+Alt+T", true);
        check("key:Win+T", true);
        check("key:Ctrl+Shift+T", true);
        // Plain Ctrl chords and standalone function keys do not trigger menu/Start/IME on release.
        check("key:Ctrl+T", false);
        check("key:F8", false);
    }

    void triggerButtonsParsedAndFormatted()
    {
        QCOMPARE(parseTriggerButtons(""), (std::vector<MouseButton> {MouseButton::Right}));
        QCOMPARE(parseTriggerButtons("left, bogus"), (std::vector<MouseButton> {MouseButton::Right}));
        QCOMPARE(parseTriggerButtons("x2, right; middle x1, right"),
            (std::vector<MouseButton> {MouseButton::Right, MouseButton::Middle, MouseButton::X1, MouseButton::X2}));
        QCOMPARE(parseTriggerButtons("X1"), (std::vector<MouseButton> {MouseButton::X1}));
        QCOMPARE(formatTriggerButtons({MouseButton::X2, MouseButton::Middle}), std::string("middle,x2"));
        QCOMPARE(formatTriggerButtons({}), std::string("right"));
        QVERIFY(hasTriggerButton({MouseButton::Right, MouseButton::X1}, MouseButton::X1));
        QVERIFY(!hasTriggerButton({MouseButton::Right, MouseButton::X1}, MouseButton::Middle));
        QVERIFY(!hasTriggerButton({MouseButton::Right, MouseButton::X1}, MouseButton::Left));
    }

    void multiButtonGrammarAndBadges()
    {
        auto p = parseGestureTrigger("DR");
        QVERIFY(p.has_value());
        QCOMPARE(*p, (ParsedGesture {MouseButton::Any, "DR", MouseButton::Any}));

        p = parseGestureTrigger("m:DR");
        QVERIFY(p.has_value());
        QCOMPARE(*p, (ParsedGesture {MouseButton::Middle, "DR", MouseButton::Any}));

        p = parseGestureTrigger("x1:cw");
        QVERIFY(p.has_value());
        QCOMPARE(*p, (ParsedGesture {MouseButton::X1, "cw", MouseButton::Any}));

        p = parseGestureTrigger("D+l");
        QVERIFY(p.has_value());
        QCOMPARE(*p, (ParsedGesture {MouseButton::Any, "D", MouseButton::Left}));

        p = parseGestureTrigger("r:D+l");
        QVERIFY(p.has_value());
        QCOMPARE(*p, (ParsedGesture {MouseButton::Right, "D", MouseButton::Left}));

        p = parseGestureTrigger("+l");
        QVERIFY(p.has_value());
        QCOMPARE(*p, (ParsedGesture {MouseButton::Any, "", MouseButton::Left}));

        p = parseGestureTrigger("r:+l");
        QVERIFY(p.has_value());
        QCOMPARE(*p, (ParsedGesture {MouseButton::Right, "", MouseButton::Left}));

        p = parseGestureTrigger("x1:+r");
        QVERIFY(p.has_value());
        QCOMPARE(*p, (ParsedGesture {MouseButton::X1, "", MouseButton::Right}));

        p = parseGestureTrigger("x2:UDLRLR+m");
        QVERIFY(p.has_value());
        QCOMPARE(*p, (ParsedGesture {MouseButton::X2, "UDLRLR", MouseButton::Middle}));

        // Invalid triggers rejected cleanly.
        QVERIFY(!parseGestureTrigger("").has_value());
        QVERIFY(!parseGestureTrigger("r:").has_value());
        QVERIFY(!parseGestureTrigger("l:D").has_value()); // left is never a hold-back trigger
        QVERIFY(!parseGestureTrigger("r:+r").has_value()); // self-chord
        QVERIFY(!parseGestureTrigger("m:D+m").has_value()); // self-chord
        QVERIFY(!parseGestureTrigger("r:D+l+m").has_value());
        QVERIFY(!parseGestureTrigger("r:m:D").has_value());
        QVERIFY(!parseGestureTrigger("r:UDLRLRL+l").has_value()); // > 6 directions
        QVERIFY(!parseGestureTrigger("corner:tl").has_value());
        QVERIFY(!parseGestureTrigger("wheel:up").has_value());
        QVERIFY(!parseGestureTrigger("r:wheel:up").has_value());

        QCOMPARE(formatGestureTrigger(MouseButton::Right, "D", MouseButton::Left), std::string("r:D+l"));
        QCOMPARE(formatGestureTrigger(MouseButton::Any, "", MouseButton::Left), std::string("+l"));
        QCOMPARE(formatGestureTrigger(MouseButton::Right, "", MouseButton::Right), std::string());
        QCOMPARE(formatGestureTrigger(MouseButton::Left, "D", MouseButton::Any), std::string());

        // Wheel triggers (universal + button-specific).
        QCOMPARE(parseWheelTrigger("wheel:up"), std::optional<ParsedWheel>(ParsedWheel {MouseButton::Any, true}));
        QCOMPARE(parseWheelTrigger("m:wheel:down"), std::optional<ParsedWheel>(ParsedWheel {MouseButton::Middle, false}));
        QCOMPARE(parseWheelTrigger("x1:wheel:up"), std::optional<ParsedWheel>(ParsedWheel {MouseButton::X1, true}));
        QVERIFY(!parseWheelTrigger("l:wheel:up").has_value());
        QCOMPARE(triggerKind("m:wheel:down"), TriggerKind::Wheel);
        QCOMPARE(triggerKind("r:D+l"), TriggerKind::Gesture);
        QCOMPARE(triggerKind("r:+l"), TriggerKind::Gesture);
        QCOMPARE(triggerKind("r:+r"), TriggerKind::Unknown);

        // Badges & titles.
        QCOMPARE(gestureBadge("DR", true), std::wstring(L"↓→"));
        QCOMPARE(gestureBadge("D+l", true), std::wstring(L"↓+左"));
        QCOMPARE(gestureBadge("D+l", false), std::wstring(L"↓+L"));
        QCOMPARE(gestureBadge("r:+l", true), std::wstring(L"右+左"));
        QCOMPARE(gestureBadge("r:+l", false), std::wstring(L"R+L"));
        QCOMPARE(gestureBadge("x1:U", true), std::wstring(L"侧1 ↑"));
        QCOMPARE(gestureBadge("x1:U", false), std::wstring(L"X1 ↑"));
        QCOMPARE(gestureBadge("r:D+l", true), std::wstring(L"右 ↓+左"));
        QCOMPARE(triggerTitle("m:wheel:down", true), std::wstring(L"按住中键向下滚"));
        QCOMPARE(triggerTitle("m:wheel:down", false), std::wstring(L"Middle button + wheel down"));

        // JSON persistence round-trip & invalid filtering.
        const auto parsed = parseRules(R"({"version":2,"rules":[
            {"on":"D+l","do":"reopentab"},
            {"on":"r:+l","do":"closetab"},
            {"on":"x1:U","do":"newtab"},
            {"on":"x1:+r","do":"back"},
            {"on":"m:wheel:up","do":"volumeup"},
            {"on":"r:+r","do":"copy"},
            {"on":"l:D","do":"copy"}]})");
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->size(), std::size_t(5));
        QVERIFY(parseRules(rulesJson(*parsed)).value() == *parsed);
    }

    void multiButtonSpecificityAndChordMatches()
    {
        std::vector<Rule> rules = defaultRules();
        Rule x1Down;
        x1Down.on = "x1:D";
        x1Down.action = builtinAction(Builtin::NewTab);
        rules.push_back(x1Down);

        Rule psDown;
        psDown.on = "D";
        psDown.apps = {L"photoshop.exe"};
        psDown.action = keysAction(L"Ctrl+Z");
        rules.push_back(psDown);

        Rule psX1Down;
        psX1Down.on = "x1:D";
        psX1Down.apps = {L"photoshop.exe"};
        psX1Down.action = keysAction(L"Ctrl+Shift+Z");
        rules.push_back(psX1Down);

        Rule rightRocker;
        rightRocker.on = "r:+l";
        rightRocker.action = builtinAction(Builtin::CloseTab);
        rules.push_back(rightRocker);

        Rule gameNoRocker;
        gameNoRocker.on = "r:+l";
        gameNoRocker.apps = {L"game.exe"};
        rules.push_back(gameNoRocker); // Nothing override

        Rule selChord;
        selChord.on = "r:+m";
        selChord.selection = true;
        selChord.action = builtinAction(Builtin::Translate);
        rules.push_back(selChord);

        Rule anyRightChord;
        anyRightChord.on = "+r";
        anyRightChord.action = builtinAction(Builtin::Forward);
        rules.push_back(anyRightChord);

        Rule brRocker;
        brRocker.on = "x1:+l";
        brRocker.where = Zone::BottomRight;
        brRocker.action = builtinAction(Builtin::Shutdown);
        rules.push_back(brRocker);

        // Specificity: apps+button (12) > apps (8) > button (4) > selection (2) > zone (1) > plain (0).
        Context ctx;
        ctx.button = MouseButton::Right;
        QCOMPARE(match(rules, "D", ctx)->action.builtin, Builtin::Bottom);
        ctx.button = MouseButton::X1;
        QCOMPARE(match(rules, "D", ctx)->action.builtin, Builtin::NewTab);
        ctx.selection = true;
        QCOMPARE(match(rules, "D", ctx)->action.builtin, Builtin::NewTab); // x1:D (4) beats global selection D (2)
        ctx.program = L"photoshop.exe";
        QCOMPARE(match(rules, "D", ctx)->action.text, std::wstring(L"Ctrl+Shift+Z")); // apps+button (12)
        ctx.button = MouseButton::Middle;
        QCOMPARE(match(rules, "D", ctx)->action.text, std::wstring(L"Ctrl+Z")); // apps (8)

        // Self-chord safety: "+r" must NOT match when active trigger is Right, MUST match for Middle/X1.
        Context rightCtx;
        rightCtx.button = MouseButton::Right;
        QVERIFY(match(rules, "+r", rightCtx) == nullptr);
        Context midCtx;
        midCtx.button = MouseButton::Middle;
        QCOMPARE(match(rules, "+r", midCtx)->action.builtin, Builtin::Forward);

        // chordMatches checks plain + selection candidates, respects Nothing app overrides and zones.
        QVERIFY(chordMatches(rules, "", MouseButton::Left, rightCtx));
        Context x1Ctx;
        x1Ctx.button = MouseButton::X1;
        QVERIFY(!chordMatches(rules, "", MouseButton::Left, x1Ctx)); // brRocker requires BottomRight
        x1Ctx.zone = Zone::BottomRight;
        QVERIFY(chordMatches(rules, "", MouseButton::Left, x1Ctx));

        // Selection-only chord still matches on hook thread (before selection is fetched).
        QVERIFY(chordMatches(rules, "", MouseButton::Middle, rightCtx));

        // App-level Nothing override disables chord matching in that app.
        Context gameRight = rightCtx;
        gameRight.program = L"game.exe";
        QVERIFY(!chordMatches(rules, "", MouseButton::Left, gameRight));

        // selectionMatters honors button scoping.
        QVERIFY(selectionMatters({selChord}, "", L"notepad.exe", MouseButton::Right));
        QVERIFY(!selectionMatters({selChord}, "", L"notepad.exe", MouseButton::X1));
    }

    void sessionStateMachineIssue436()
    {
        Settings settings;
        settings.buttons = {MouseButton::Right, MouseButton::Middle, MouseButton::X1};
        settings.rules = defaultRules();

        Rule strokeChord; // Example 1: Right + Down + Left
        strokeChord.on = "D+l";
        strokeChord.action = builtinAction(Builtin::ReopenTab);
        settings.rules.push_back(strokeChord);

        Rule pureRocker; // Example 2: Right + Left
        pureRocker.on = "r:+l";
        pureRocker.action = builtinAction(Builtin::CloseTab);
        settings.rules.push_back(pureRocker);

        Rule x1RightChord; // Cross-trigger chord: X1 + Right
        x1RightChord.on = "x1:+r";
        x1RightChord.action = builtinAction(Builtin::Back);
        settings.rules.push_back(x1RightChord);

        Rule midWheel;
        midWheel.on = "m:wheel:up";
        midWheel.action = builtinAction(Builtin::VolumeUp);
        settings.rules.push_back(midWheel);

        const Context ctx;

        // 1. Plain click replay for Right, Middle, X1; X2 (disabled) passes straight through.
        {
            GestureSession s;
            for (const MouseButton btn : {MouseButton::Right, MouseButton::Middle, MouseButton::X1}) {
                auto d = s.onButtonDown(btn, 100, 100, 6, 24, true, settings, ctx);
                QVERIFY(d.swallow && d.startHoldTimer);
                auto u = s.onButtonUp(btn, 100, 100);
                QVERIFY(u.swallow && u.stopHoldTimer);
                QCOMPARE(u.event, SessionResult::Event::Click);
                QCOMPARE(u.button, btn);
                QCOMPARE(s.state(), GestureSession::State::Idle);
            }
            auto x2 = s.onButtonDown(MouseButton::X2, 100, 100, 6, 24, true, settings, ctx);
            QVERIFY(!x2.swallow);
            QCOMPARE(s.state(), GestureSession::State::Idle);
        }

        // 2. Hold-still pass-through (350ms) for Middle & X1 (preserves autoscroll & push-to-talk).
        {
            GestureSession s;
            QVERIFY(s.onButtonDown(MouseButton::X1, 100, 100, 6, 24, true, settings, ctx).swallow);
            auto hold = s.onHoldTimer();
            QCOMPARE(hold.event, SessionResult::Event::PressThrough);
            QCOMPARE(hold.button, MouseButton::X1);
            QCOMPARE(s.state(), GestureSession::State::Through);
            auto up = s.onButtonUp(MouseButton::X1, 100, 100);
            QVERIFY(!up.swallow); // let the app receive the matching UP
            QCOMPARE(s.state(), GestureSession::State::Idle);
        }

        // 3. Example 1 (Hold Right -> drag Down -> press Left -> release Left then Right),
        //    verifying stroke freezes on chord press despite hand jitter.
        {
            GestureSession s;
            QVERIFY(s.onButtonDown(MouseButton::Right, 100, 100, 6, 24, true, settings, ctx).swallow);
            auto m1 = s.onMove(100, 140);
            QVERIFY(m1.startBeforeMove);
            QCOMPARE(m1.event, SessionResult::Event::Move);
            QCOMPARE(m1.gesture, std::string("D"));

            auto lDown = s.onButtonDown(MouseButton::Left, 100, 140, 6, 24, false, settings, ctx);
            QVERIFY(lDown.swallow);
            QVERIFY(!lDown.startBeforeMove); // already in Gesturing
            QCOMPARE(lDown.event, SessionResult::Event::Move);
            QCOMPARE(lDown.gesture, std::string("D+l"));

            // Jitter right by 50px while holding chord: stroke MUST stay frozen at "D"!
            auto jitter = s.onMove(150, 140);
            QCOMPARE(jitter.event, SessionResult::Event::None);

            // Release Left first -> fires D+l immediately, enters Rocked.
            auto lUp = s.onButtonUp(MouseButton::Left, 150, 140);
            QVERIFY(lUp.swallow);
            QCOMPARE(lUp.event, SessionResult::Event::End);
            QCOMPARE(lUp.button, MouseButton::Right);
            QCOMPARE(lUp.gesture, std::string("D+l"));
            QCOMPARE(s.state(), GestureSession::State::Rocked);

            // Release Right -> swallowed cleanly, no double fire, no leaked click!
            auto rUp = s.onButtonUp(MouseButton::Right, 150, 140);
            QVERIFY(rUp.swallow);
            QCOMPARE(rUp.event, SessionResult::Event::None);
            QCOMPARE(s.state(), GestureSession::State::Idle);
        }

        // 4. Example 1 reverse simultaneous release (release Right FIRST, then Left 5ms later).
        {
            GestureSession s;
            QVERIFY(s.onButtonDown(MouseButton::Right, 100, 100, 6, 24, true, settings, ctx).swallow);
            s.onMove(100, 140);
            QVERIFY(s.onButtonDown(MouseButton::Left, 100, 140, 6, 24, false, settings, ctx).swallow);

            auto rUp = s.onButtonUp(MouseButton::Right, 100, 140);
            QVERIFY(rUp.swallow);
            QCOMPARE(rUp.event, SessionResult::Event::End);
            QCOMPARE(rUp.gesture, std::string("D+l"));
            QCOMPARE(s.state(), GestureSession::State::Idle);

            // Trailing Left UP MUST still be swallowed without firing again!
            auto lUp = s.onButtonUp(MouseButton::Left, 100, 140);
            QVERIFY(lUp.swallow);
            QCOMPARE(lUp.event, SessionResult::Event::None);
        }

        // 5. Example 2 (Pure rocker: Hold Right -> press Left -> release in either order, plus repeat tap).
        {
            GestureSession s;
            QVERIFY(s.onButtonDown(MouseButton::Right, 200, 200, 6, 24, true, settings, ctx).swallow);
            auto lDown1 = s.onButtonDown(MouseButton::Left, 200, 200, 6, 24, false, settings, ctx);
            QVERIFY(lDown1.swallow && lDown1.startBeforeMove);
            QCOMPARE(lDown1.event, SessionResult::Event::Move);
            QCOMPARE(lDown1.gesture, std::string("+l"));

            auto lUp1 = s.onButtonUp(MouseButton::Left, 200, 200);
            QVERIFY(lUp1.swallow);
            QCOMPARE(lUp1.event, SessionResult::Event::End);
            QCOMPARE(lUp1.gesture, std::string("+l"));
            QCOMPARE(s.state(), GestureSession::State::Rocked);

            // Second Left tap while still holding Right -> repeat rocker!
            auto lDown2 = s.onButtonDown(MouseButton::Left, 200, 200, 6, 24, false, settings, ctx);
            QVERIFY(lDown2.swallow && lDown2.startBeforeMove);
            QCOMPARE(lDown2.gesture, std::string("+l"));
            // Release Right first this time -> fires second time, trailing Left UP swallowed!
            auto rUp = s.onButtonUp(MouseButton::Right, 200, 200);
            QVERIFY(rUp.swallow);
            QCOMPARE(rUp.event, SessionResult::Event::End);
            QCOMPARE(rUp.gesture, std::string("+l"));
            auto lUp2 = s.onButtonUp(MouseButton::Left, 200, 200);
            QVERIFY(lUp2.swallow);
            QCOMPARE(lUp2.event, SessionResult::Event::None);
        }

        // 6. Pure rocker tolerance when hand twitches 8px before pressing Left.
        {
            GestureSession s;
            QVERIFY(s.onButtonDown(MouseButton::Right, 200, 200, 6, 24, true, settings, ctx).swallow);
            auto twitch = s.onMove(208, 200); // > 6px startDist, < 24px stroke -> Gesturing with ""
            QVERIFY(twitch.startBeforeMove);
            QCOMPARE(twitch.gesture, std::string());
            auto lDown = s.onButtonDown(MouseButton::Left, 208, 200, 6, 24, false, settings, ctx);
            QVERIFY(lDown.swallow);
            QCOMPARE(lDown.gesture, std::string("+l"));
        }

        // 7. Unmatched Left click preserves cancel-on-left-click behavior.
        {
            GestureSession s;
            QVERIFY(s.onButtonDown(MouseButton::Right, 200, 200, 6, 24, true, settings, ctx).swallow);
            s.onMove(200, 150); // "U" — no U+l rule
            auto lDown = s.onButtonDown(MouseButton::Left, 200, 150, 6, 24, false, settings, ctx);
            QVERIFY(!lDown.swallow);
            QCOMPARE(lDown.event, SessionResult::Event::Cancel);
            QCOMPARE(s.state(), GestureSession::State::Canceled);
            auto rUp = s.onButtonUp(MouseButton::Right, 200, 150);
            QVERIFY(rUp.swallow);
            QCOMPARE(rUp.event, SessionResult::Event::None);
        }

        // 8. Esc abort while chord button is held: neither fires, swallowed chord UP stays swallowed.
        {
            GestureSession s;
            QVERIFY(s.onButtonDown(MouseButton::Right, 200, 200, 6, 24, true, settings, ctx).swallow);
            QVERIFY(s.onButtonDown(MouseButton::Left, 200, 200, 6, 24, false, settings, ctx).swallow);
            auto aborted = s.onAbort();
            QCOMPARE(aborted.event, SessionResult::Event::Cancel);
            QCOMPARE(s.state(), GestureSession::State::Canceled);
            auto lUp = s.onButtonUp(MouseButton::Left, 200, 200);
            QVERIFY(lUp.swallow);
            QCOMPARE(lUp.event, SessionResult::Event::None);
            auto rUp = s.onButtonUp(MouseButton::Right, 200, 200);
            QVERIFY(rUp.swallow);
            QCOMPARE(rUp.event, SessionResult::Event::None);
        }

        // 9. Cross-trigger chord (X1 held -> Right clicked) does NOT hijack as a new Right trigger.
        {
            GestureSession s;
            QVERIFY(s.onButtonDown(MouseButton::X1, 100, 100, 6, 24, true, settings, ctx).swallow);
            QVERIFY(!s.wantsTriggerCheck(MouseButton::Right, settings.buttons, true));
            auto rDown = s.onButtonDown(MouseButton::Right, 100, 100, 6, 24, false, settings, ctx);
            QVERIFY(rDown.swallow && rDown.startBeforeMove);
            QCOMPARE(rDown.button, MouseButton::X1);
            QCOMPARE(rDown.gesture, std::string("+r"));
            auto rUp = s.onButtonUp(MouseButton::Right, 100, 100);
            QVERIFY(rUp.swallow);
            QCOMPARE(rUp.event, SessionResult::Event::End);
            QCOMPARE(rUp.button, MouseButton::X1);
            QCOMPARE(rUp.gesture, std::string("+r"));
            QVERIFY(s.onButtonUp(MouseButton::X1, 100, 100).swallow);
        }

        // 10. Middle button + wheel triggers button-specific wheel rule cleanly.
        {
            GestureSession s;
            QVERIFY(s.onButtonDown(MouseButton::Middle, 100, 100, 6, 24, true, settings, ctx).swallow);
            auto w = s.onWheel(WHEEL_DELTA, true, settings, ctx);
            QVERIFY(w.swallow);
            QCOMPARE(w.event, SessionResult::Event::Wheel);
            QCOMPARE(w.button, MouseButton::Middle);
            QVERIFY(w.wheelUp);
            QCOMPARE(w.wheelNotches, 1);
            auto mUp = s.onButtonUp(MouseButton::Middle, 100, 100);
            QVERIFY(mUp.swallow);
            QCOMPARE(mUp.event, SessionResult::Event::None);
        }
    }
};

QTEST_GUILESS_MAIN(GesturesTest)
#include "tst_gestures.moc"
