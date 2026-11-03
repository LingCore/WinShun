// The rules of the Explorer-tabs plugin (src/explorer-tabs-plugin/Rules):
// which new folder windows become tabs and where they go, with made-up
// windows -- the same folder already open, Ctrl+Shift, a tab dragged out,
// Control Panel, several tabs, the integrity level, the dev-only test root,
// and how places are compared.

#include "Rules.h"

#include <QTest>

using namespace tabs;

namespace {

Window window(std::uintptr_t id, int z, int opened, std::vector<std::wstring> places)
{
    Window w;
    w.id = id;
    w.zOrder = z;
    w.openOrder = opened;
    w.tabs = std::move(places);
    return w;
}

Candidate folder(std::wstring place)
{
    Candidate b;
    b.place = std::move(place);
    return b;
}

constexpr std::int64_t kNow = 100'000;

} // namespace

class TestExplorerTabs : public QObject {
    Q_OBJECT

private slots:
    void places()
    {
        QVERIFY(samePlace(L"C:\\Users\\a", L"c:/users/A/"));
        QVERIFY(samePlace(L"\"D:\\x y\\\"", L"D:\\X Y"));
        QVERIFY(samePlace(L"file:///C:/a%20b/%E4%B8%AD", L"C:\\a b\\中"));
        QVERIFY(samePlace(L"file://server/share/x", L"\\\\server\\share\\x"));
        QVERIFY(samePlace(L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"::{20d04fe0-3aea-1069-a2d8-08002b30309d}"));
        QVERIFY(samePlace(L"C:", L"C:\\"));
        QCOMPARE(normalized(L"C:\\"), std::wstring(L"C:\\"));
        QVERIFY(samePlace(L"C:\\a#b", L"C:\\A#B")); // '#' is part of a name, not a URL fragment
        QVERIFY(!samePlace(L"C:\\a", L"C:\\ab"));
        QVERIFY(!samePlace(L"", L""));
        QVERIFY(isUnder(L"C:\\root\\x\\y", L"C:\\Root"));
        QVERIFY(isUnder(L"C:\\root", L"C:\\root\\"));
        QVERIFY(!isUnder(L"C:\\rootx", L"C:\\root"));
        QVERIFY(isUnder(L"D:\\x", L"D:\\"));
        QVERIFY(!isUnder(L"", L"C:\\root"));
    }

    void special()
    {
        QVERIFY(tabsCannotHost(L"::{26EE0668-A00A-44D7-9371-BEB064C98683}"));
        QVERIFY(tabsCannotHost(L"::{26EE0668-A00A-44D7-9371-BEB064C98683}\\0\\::{BB06C0E4-D293-4F75-8A90-CB05B6477EEE}"));
        QVERIFY(tabsCannotHost(L"::{21ec2020-3aea-1069-a2dd-08002b30309d}"));
        QVERIFY(tabsCannotHost(L"shell:::{ED7BA470-8E54-465E-825C-99712043E01C}"));
        QVERIFY(!tabsCannotHost(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}")); // This PC: a tab shows it
        QVERIFY(!tabsCannotHost(L"C:\\Control Panel"));
        const Decision d = decide({}, false, {}, folder(L"::{26EE0668-A00A-44D7-9371-BEB064C98683}"), {window(1, 0, 0, {L"C:\\a"})}, {}, kNow);
        QCOMPARE(d.verdict, Verdict::Keep);
        QCOMPARE(d.why, Why::Special);
    }

    void mergesIntoTheRecentWindow()
    {
        const std::vector<Window> others {window(1, 2, 0, {L"C:\\a"}), window(2, 0, 1, {L"C:\\b"}), window(3, 1, 2, {L"C:\\c"})};
        Decision d = decide({}, false, {}, folder(L"C:\\new"), others, {}, kNow);
        QCOMPARE(d.verdict, Verdict::NewTab);
        QCOMPARE(d.window, std::uintptr_t(2)); // in front
        Settings first;
        first.target = Target::First;
        d = decide(first, false, {}, folder(L"C:\\new"), others, {}, kNow);
        QCOMPARE(d.window, std::uintptr_t(1)); // opened first
    }

    void onlyUsableWindows()
    {
        std::vector<Window> others {window(1, 0, 0, {L"C:\\a"}), window(2, 1, 1, {L"C:\\b"})};
        others[0].usable = false; // hidden, cloaked on another desktop, or one we hide
        Decision d = decide({}, false, {}, folder(L"C:\\new"), others, {}, kNow);
        QCOMPARE(d.window, std::uintptr_t(2));
        others[1].aboveUs = true; // an elevated Explorer: not ours to drive
        d = decide({}, false, {}, folder(L"C:\\new"), others, {}, kNow);
        QCOMPARE(d.verdict, Verdict::Keep);
        QCOMPARE(d.why, Why::NoWindow);
        d = decide({}, false, {}, folder(L"C:\\new"), {}, {}, kNow);
        QCOMPARE(d.why, Why::NoWindow);
    }

    void reusesATabOnTheSameFolder()
    {
        const std::vector<Window> others {window(1, 0, 0, {L"C:\\a", L""}), window(2, 1, 1, {L"C:\\x", L"D:\\Same\\"})};
        Decision d = decide({}, false, {}, folder(L"d:\\same"), others, {}, kNow);
        QCOMPARE(d.verdict, Verdict::SwitchTab);
        QCOMPARE(d.why, Why::Reused);
        QCOMPARE(d.window, std::uintptr_t(2));
        QCOMPARE(d.tab, 1);
        Settings noReuse;
        noReuse.reuse = false;
        d = decide(noReuse, false, {}, folder(L"d:\\same"), others, {}, kNow);
        QCOMPARE(d.verdict, Verdict::NewTab);
        QCOMPARE(d.window, std::uintptr_t(1));
        // The same folder in the preferred window and in another: the preferred one.
        const std::vector<Window> both {window(1, 1, 0, {L"C:\\s"}), window(2, 0, 1, {L"C:\\other", L"C:\\s"})};
        d = decide({}, false, {}, folder(L"C:\\s"), both, {}, kNow);
        QCOMPARE(d.window, std::uintptr_t(2));
        QCOMPARE(d.tab, 1);
    }

    void userWantsAWindow()
    {
        const std::vector<Window> others {window(1, 0, 0, {L"C:\\a"})};
        Candidate b = folder(L"C:\\new");
        b.bypass = true;
        QCOMPARE(decide({}, false, {}, b, others, {}, kNow).why, Why::Bypass);
        b = folder(L"C:\\new");
        b.dragging = true;
        QCOMPARE(decide({}, false, {}, b, others, {}, kNow).why, Why::Dragging);
        b = folder(L"C:\\new");
        b.movedTab = true;
        QCOMPARE(decide({}, false, {}, b, others, {}, kNow).why, Why::MovedTab);
        // A tab on this folder closed a moment ago: dragged out into its own window.
        const std::vector<Closed> closed {{L"C:\\new", kNow - 1500}};
        QCOMPARE(decide({}, false, {}, folder(L"C:\\new"), others, closed, kNow).why, Why::DraggedOut);
        const std::vector<Closed> old {{L"C:\\new", kNow - kDraggedOutMs - 1}};
        QCOMPARE(decide({}, false, {}, folder(L"C:\\new"), others, old, kNow).verdict, Verdict::NewTab);
        const std::vector<Closed> elsewhere {{L"C:\\other", kNow - 100}};
        QCOMPARE(decide({}, false, {}, folder(L"C:\\new"), others, elsewhere, kNow).verdict, Verdict::NewTab);
    }

    void leftAlone()
    {
        const std::vector<Window> others {window(1, 0, 0, {L"C:\\a"})};
        Settings off;
        off.merge = false;
        QCOMPARE(decide(off, false, {}, folder(L"C:\\new"), others, {}, kNow).why, Why::Off);
        QCOMPARE(decide({}, true, {}, folder(L"C:\\new"), others, {}, kNow).why, Why::Paused);
        Candidate b = folder(L"C:\\new");
        b.tabs = 2;
        QCOMPARE(decide({}, false, {}, b, others, {}, kNow).why, Why::MultiTab);
        b = folder(L"C:\\new");
        b.aboveUs = true;
        QCOMPARE(decide({}, false, {}, b, others, {}, kNow).why, Why::AboveUs);
        QCOMPARE(decide({}, false, {}, folder(L""), others, {}, kNow).why, Why::Unknown);
    }

    void testRoot()
    {
        Restriction r;
        r.root = L"C:\\scratch\\root";
        // A folder outside the root is not touched.
        const std::vector<Window> mine {window(1, 1, 0, {L"C:\\scratch\\root\\A"})};
        QCOMPARE(decide({}, false, r, folder(L"C:\\Users\\me\\Documents"), mine, {}, kNow).why, Why::OutsideTestRoot);
        // The user's window in front is never the target, nor one with a user's tab in it.
        const std::vector<Window> others {window(7, 0, 0, {L"C:\\Users\\me"}), window(8, 1, 1, {L"C:\\scratch\\root\\A", L"D:\\work"}),
            window(9, 2, 2, {L"C:\\scratch\\root\\A", L"C:\\scratch\\root\\B"})};
        Decision d = decide({}, false, r, folder(L"C:\\scratch\\root\\Y"), others, {}, kNow);
        QCOMPARE(d.verdict, Verdict::NewTab);
        QCOMPARE(d.window, std::uintptr_t(9));
        // A tab not looked at ("") makes a window unusable while restricted.
        const std::vector<Window> unknown {window(9, 0, 0, {L"C:\\scratch\\root\\A", L""})};
        QCOMPARE(decide({}, false, r, folder(L"C:\\scratch\\root\\Y"), unknown, {}, kNow).why, Why::NoWindow);
        // Same-folder reuse only among test windows.
        const std::vector<Window> same {window(7, 0, 0, {L"C:\\scratch\\root\\Y"}), window(9, 1, 1, {L"C:\\scratch\\root\\A"})};
        d = decide({}, false, r, folder(L"C:\\scratch\\root\\Y"), same, {}, kNow);
        QCOMPARE(d.verdict, Verdict::SwitchTab);
        QCOMPARE(d.window, std::uintptr_t(7));
        std::vector<Window> sameMixed = same; // that window also shows a user's folder
        sameMixed[0].tabs.push_back(L"C:\\Users\\me");
        d = decide({}, false, r, folder(L"C:\\scratch\\root\\Y"), sameMixed, {}, kNow);
        QCOMPARE(d.verdict, Verdict::NewTab);
        QCOMPARE(d.window, std::uintptr_t(9));
    }

    void hidingBeforeTheFolderIsKnown()
    {
        Candidate b;
        QVERIFY(hideEarly({}, false, {}, b, 1, false));
        QVERIFY(!hideEarly({}, false, {}, b, 0, false)); // no other window: it stays a window anyway
        QVERIFY(!hideEarly({}, true, {}, b, 1, false));
        Settings off;
        off.merge = false;
        QVERIFY(!hideEarly(off, false, {}, b, 1, false));
        Candidate held = b;
        held.bypass = true;
        QVERIFY(!hideEarly({}, false, {}, held, 1, false));
        Candidate drag = b;
        drag.dragging = true;
        QVERIFY(!hideEarly({}, false, {}, drag, 1, false));
        Candidate moved = b;
        moved.movedTab = true;
        QVERIFY(!hideEarly({}, false, {}, moved, 1, false));
        Candidate above = b;
        above.aboveUs = true;
        QVERIFY(!hideEarly({}, false, {}, above, 1, false));
        Candidate two = b;
        two.tabs = 2;
        QVERIFY(!hideEarly({}, false, {}, two, 1, false));
        // Restricted: only a window whose title names the test folder.
        Restriction r;
        r.root = L"C:\\scratch\\root";
        QVERIFY(!hideEarly({}, false, r, b, 1, false));
        QVERIFY(hideEarly({}, false, r, b, 1, true));
    }
};

QTEST_GUILESS_MAIN(TestExplorerTabs)
#include "tst_explorertabs.moc"
