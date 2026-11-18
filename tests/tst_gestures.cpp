// The mouse-gestures plugin (src/gestures-plugin) without any window: the
// rules (Gesture) -- the directions a drag draws (a stroke long enough,
// clearly along one axis, the same direction once, a slant for nothing),
// the actions as written (names, keys, things to open, nonsense), the
// directions as written, the "more" setting and the programs left alone.

#include "Gesture.h"

#include <QTest>

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

    void defaultsRead()
    {
        for (const Slot& entry : defaultSlots()) {
            const Action a = parseAction(entry.fallback);
            QCOMPARE(a.kind, Action::Kind::Builtin);
            QCOMPARE(parseDirections(std::wstring(entry.directions, entry.directions + std::strlen(entry.directions))), std::string(entry.directions));
        }
    }
};

QTEST_GUILESS_MAIN(GesturesTest)
#include "tst_gestures.moc"
