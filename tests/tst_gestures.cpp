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
};

QTEST_GUILESS_MAIN(GesturesTest)
#include "tst_gestures.moc"
