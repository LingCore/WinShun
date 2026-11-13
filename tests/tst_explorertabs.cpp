// The Explorer-tabs plugin (src/explorer-tabs-plugin) without any window:
// the rules (Rules) -- which new folder windows become tabs and where they
// go, with made-up windows: the same folder already open, Ctrl+Shift, a
// window asked for from File Explorer (Ctrl+N, "open in new window", a tab
// dragged out) or a listed program, Control Panel, several tabs, the
// integrity level, the dev-only test root, how places are compared, the
// guess made before the folder is known; the hidden windows and their
// journal (Hider: in the journal from before hiding until after showing,
// what a restart reads back); the closing of spare tabs (Closer: only the
// active one, one at a time, one window at a time); the tabs kept for the
// next logon (Session: what is written and read back, what gets opened).

#include "Closer.h"
#include "Hider.h"
#include "Rules.h"
#include "Session.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>
#include <set>

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

HWND h(std::uintptr_t n)
{
    return reinterpret_cast<HWND>(n);
}

std::string readFile(const std::wstring& path)
{
    std::string text;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return text;
    char buffer[256];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0)
        text.append(buffer, n);
    std::fclose(f);
    return text;
}

bool lists(const std::wstring& journal, HWND w)
{
    for (const JournalLine& l : parseJournal(readFile(journal))) {
        if (l.window == w)
            return true;
    }
    return false;
}

// Made-up Explorer windows and their tabs for the closer.
struct FakeTabs {
    std::map<HWND, std::vector<HWND>> windows; // top -> tabs, the active first
    std::set<HWND> gone;
    std::vector<HWND> sent; // closes sent, in order
    std::int64_t now = 0;
    bool closeOnSend = false; // Explorer closes the ACTIVE tab at once

    TabOps ops()
    {
        return {[this](HWND top) { return windows[top]; },
            [this](HWND w) {
                if (gone.contains(w))
                    return false;
                if (windows.contains(w))
                    return true;
                for (const auto& [top, tabs] : windows) {
                    if (std::ranges::find(tabs, w) != tabs.end())
                        return true;
                }
                return false;
            },
            [this](HWND tab) {
                sent.push_back(tab);
                if (closeOnSend)
                    closeActiveOf(tab);
                return true;
            },
            [this] { return now; }};
    }
    void closeActiveOf(HWND tab)
    {
        for (auto& [top, tabs] : windows) {
            if (std::ranges::find(tabs, tab) != tabs.end() && !tabs.empty()) {
                gone.insert(tabs.front()); // the active one, whichever was asked
                tabs.erase(tabs.begin());
            }
        }
    }
};

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
        b.requested = true;
        QCOMPARE(decide({}, false, {}, b, others, {}, kNow).why, Why::Requested);
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
        Candidate asked = b;
        asked.requested = true; // Ctrl+N: not even hidden for a moment
        QVERIFY(!hideEarly({}, false, {}, asked, 1, false));
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

    void explicitWindowRequests()
    {
        const Settings s;
        const std::vector<Window> others {window(1, 0, 0, {L"C:\\a"})};
        // Ctrl+N in A (in front) opens A's own folder: a window, not "already open, switch to it".
        Opener explorer {L"CabinetWClass", L"explorer.exe"};
        QVERIFY(openerWantsWindow(explorer, s, {}));
        Candidate b = folder(L"C:\\a");
        b.requested = openerWantsWindow(explorer, s, {});
        Decision d = decide(s, false, {}, b, others, {}, kNow);
        QCOMPARE(d.verdict, Verdict::Keep);
        QCOMPARE(d.why, Why::Requested);
        // The desktop (double-click, the left button still held), Win顺, the Start menu: a tab.
        for (const wchar_t* cls : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Qt6110QWindowIcon"}) {
            Opener o {cls, L"explorer.exe"};
            QVERIFY(!openerWantsWindow(o, s, {}));
            b = folder(L"C:\\new");
            b.requested = openerWantsWindow(o, s, {});
            QCOMPARE(decide(s, false, {}, b, others, {}, kNow).verdict, Verdict::NewTab);
        }
        // An Explorer window the plugin itself put in front (the last merge),
        // no input since: not the user asking.
        Opener ours = explorer;
        ours.broughtByUs = true;
        QVERIFY(!openerWantsWindow(ours, s, {}));
        // Listed programs keep their windows (scripts, terminals: explorer /n).
        Settings listed;
        listed.windowsFor = programList(L"WindowsTerminal; cmd.exe");
        QVERIFY(openerWantsWindow({L"CASCADIA_HOSTING_WINDOW_CLASS", L"WindowsTerminal.exe"}, listed, {}));
        QVERIFY(openerWantsWindow({L"ConsoleWindowClass", L"C:\\Windows\\System32\\CMD.EXE"}, listed, {}));
        QVERIFY(!openerWantsWindow({L"ConsoleWindowClass", L"pwsh.exe"}, listed, {}));
        QVERIFY(!openerWantsWindow({L"CASCADIA_HOSTING_WINDOW_CLASS", L"WindowsTerminal.exe"}, s, {}));
        // Restricted: only a test window counts as the user's Explorer.
        Restriction r;
        r.root = L"C:\\scratch\\root";
        QVERIFY(!openerWantsWindow(explorer, s, r));
        Opener test = explorer;
        test.testWindow = true;
        QVERIFY(openerWantsWindow(test, s, r));
    }

    void programLists()
    {
        const std::vector<std::wstring> expected {L"a.exe", L"b.exe", L"c.exe", L"x.exe"};
        QVERIFY(programList(L" a.exe; B.EXE ,c\n\"D:\\tools\\x.exe\";;a") == expected);
        QVERIFY(programList(L"").empty());
        QVERIFY(programList(L" ; ,\r\n").empty());
    }

    void explorerTabsNeedWindows11_22H2()
    {
        QVERIFY(!explorerHasTabs(19045)); // Windows 10 22H2
        QVERIFY(!explorerHasTabs(22000)); // Windows 11 21H2
        QVERIFY(!explorerHasTabs(22620));
        QVERIFY(explorerHasTabs(22621)); // Windows 11 22H2
        QVERIFY(explorerHasTabs(26100));
        QVERIFY(explorerHasTabs(0)); // unknown: as before
    }

    void guessing()
    {
        std::vector<Window> candidates {window(1, 2, 0, {}), window(2, 0, 2, {}), window(3, 1, 1, {})};
        QCOMPARE(guessTarget({}, candidates), std::uintptr_t(2)); // in front
        Settings first;
        first.target = Target::First;
        QCOMPARE(guessTarget(first, candidates), std::uintptr_t(1));
        candidates[1].usable = false;
        candidates[0].aboveUs = true;
        QCOMPARE(guessTarget({}, candidates), std::uintptr_t(3));
        QCOMPARE(guessTarget(first, candidates), std::uintptr_t(3));
        QCOMPARE(guessTarget({}, {}), std::uintptr_t(0));
        QVERIFY(confirmsGuess({Verdict::NewTab, Why::Merged, 3, -1}, 3));
        QVERIFY(!confirmsGuess({Verdict::NewTab, Why::Merged, 2, -1}, 3)); // another window
        QVERIFY(!confirmsGuess({Verdict::SwitchTab, Why::Reused, 3, 0}, 3)); // a tab already there
        QVERIFY(!confirmsGuess({Verdict::Keep, Why::Requested, 0, -1}, 3));
        QVERIFY(!confirmsGuess({Verdict::NewTab, Why::Merged, 0, -1}, 0));
    }

    void journalLines()
    {
        const std::vector<JournalLine> lines {{h(0x1a2b), true}, {h(0x30), false}};
        const std::string text = journalText(lines);
        QCOMPARE(text, std::string("1a2b 1\n30 0\n"));
        const std::vector<JournalLine> back = parseJournal(text);
        QCOMPARE(back.size(), std::size_t(2));
        QCOMPARE(back[0].window, h(0x1a2b));
        QVERIFY(back[0].addedLayered);
        QCOMPARE(back[1].window, h(0x30));
        QVERIFY(!back[1].addedLayered);
        // What cannot be read is skipped (a line cut off, a stray byte); CRLF is fine.
        const std::vector<JournalLine> mixed = parseJournal("zz 1\n12 5\n\n0 1\n1a 1 extra\n44 0\r\n55");
        QCOMPARE(mixed.size(), std::size_t(1));
        QCOMPARE(mixed[0].window, h(0x44));
    }

    void hiderKeepsTheJournalAhead()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::wstring journal = QDir::toNativeSeparators(dir.filePath(QStringLiteral("hidden.txt"))).toStdWString();
        std::vector<std::string> events;
        std::set<HWND> layered {h(0x20)};
        WindowOps ops;
        ops.conceal = [&](HWND w, bool add) {
            events.push_back("conceal " + std::to_string(reinterpret_cast<std::uintptr_t>(w)) + (add ? " add" : "") + (lists(journal, w) ? " listed" : " NOT LISTED"));
        };
        ops.reveal = [&](HWND w, bool remove) {
            events.push_back("reveal " + std::to_string(reinterpret_cast<std::uintptr_t>(w)) + (remove ? " remove" : "") + (lists(journal, w) ? " listed" : " NOT LISTED"));
            return true;
        };
        ops.layered = [&](HWND w) { return layered.contains(w); };
        Hider hider(journal, ops);
        QVERIFY(hider.hide(h(0x10)));
        QVERIFY(!hider.hide(h(0x10))); // already
        QVERIFY(hider.hide(h(0x20)));
        QVERIFY(hider.hidden(h(0x10)));
        QCOMPARE(hider.count(), std::size_t(2));
        hider.show(h(0x10));
        QVERIFY(!hider.hidden(h(0x10)));
        QVERIFY(!lists(journal, h(0x10)));
        QVERIFY(lists(journal, h(0x20)));
        hider.forget(h(0x20)); // it became a tab: nothing to show
        QVERIFY(!lists(journal, h(0x20)));
        QVERIFY(::GetFileAttributesW(journal.c_str()) == INVALID_FILE_ATTRIBUTES); // nothing hidden: no file
        QVERIFY(hider.hide(h(0x30)));
        QVERIFY(hider.hide(h(0x40)));
        hider.showAll();
        QCOMPARE(hider.count(), std::size_t(0));
        QVERIFY(::GetFileAttributesW(journal.c_str()) == INVALID_FILE_ATTRIBUTES);
        const std::vector<std::string> expected {"conceal 16 add listed", "conceal 32 listed", "reveal 16 remove listed", "conceal 48 add listed",
            "conceal 64 add listed", "reveal 48 remove listed", "reveal 64 remove listed"};
        QCOMPARE(events, expected);
        // Stopping: nothing more is hidden; a snapshot is shown without striking it.
        QVERIFY(hider.hide(h(0x50)));
        hider.close();
        QVERIFY(!hider.hide(h(0x60)));
        hider.revealSnapshot();
        QCOMPARE(events.back(), std::string("reveal 80 remove listed"));
        QVERIFY(lists(journal, h(0x50)));
        hider.showAll();
        QVERIFY(!lists(journal, h(0x50)));
    }

    void restoreReadsTheJournalBack()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::wstring journal = QDir::toNativeSeparators(dir.filePath(QStringLiteral("hidden.txt"))).toStdWString();
        {
            FILE* f = nullptr;
            QCOMPARE(_wfopen_s(&f, journal.c_str(), L"wb"), 0);
            std::fputs("1 1\n2 0\n3 1\ngarbage\n", f);
            std::fclose(f);
        }
        std::set<HWND> hidden {h(1), h(2)}; // 3: that handle is something else now
        std::vector<HWND> revealed;
        bool twoFails = true;
        const auto stillHidden = [&](HWND w) { return hidden.contains(w); };
        const auto reveal = [&](HWND w, bool) {
            revealed.push_back(w);
            if (w == h(2) && twoFails)
                return false;
            hidden.erase(w);
            return true;
        };
        QCOMPARE(restoreJournal(journal, stillHidden, reveal), 1);
        QCOMPARE(revealed, (std::vector<HWND> {h(1), h(2)}));
        // The one that could not be shown stays for the next time, alone.
        QVERIFY(!lists(journal, h(1)));
        QVERIFY(lists(journal, h(2)));
        QVERIFY(!lists(journal, h(3)));
        twoFails = false;
        QCOMPARE(restoreJournal(journal, stillHidden, reveal), 1);
        QVERIFY(::GetFileAttributesW(journal.c_str()) == INVALID_FILE_ATTRIBUTES);
        QCOMPARE(restoreJournal(journal, stillHidden, reveal), 0); // no file: nothing
    }

    void spareTabsCloseOnlyWhileActive()
    {
        FakeTabs fake;
        const HWND a = h(100), user = h(101), spare = h(102);
        fake.windows[a] = {user, spare}; // the user picked their own tab meanwhile
        SpareCloser closer(fake.ops());
        closer.close(a, {user}, spare);
        QVERIFY(closer.pending(a));
        QVERIFY(closer.tick().empty());
        QVERIFY(fake.sent.empty()); // a close now would close the user's tab
        fake.windows[a] = {spare, user};
        QVERIFY(closer.tick().empty());
        QCOMPARE(fake.sent, std::vector<HWND> {spare});
        QVERIFY(closer.tick().empty());
        QCOMPARE(fake.sent.size(), std::size_t(1)); // one at a time: not answered yet
        closer.answered(spare);
        QVERIFY(closer.tick().empty());
        QCOMPARE(fake.sent.size(), std::size_t(2)); // ignored (a tab still being made): again
        closer.answered(spare);
        fake.closeActiveOf(spare);
        const std::vector<SpareCloser::Done> done = closer.tick();
        QCOMPARE(done.size(), std::size_t(1));
        QVERIFY(done[0].closed);
        QCOMPARE(fake.windows[a], std::vector<HWND> {user});
        QVERIFY(!closer.pending(a));
        QVERIFY(!closer.any());
    }

    void spareTabsNotKnownYet()
    {
        FakeTabs fake;
        fake.closeOnSend = true;
        const HWND a = h(200), user = h(201), spare = h(202), other = h(203);
        SpareCloser closer(fake.ops());
        // Not seen yet: the one tab that was not there before.
        fake.windows[a] = {spare, user};
        closer.close(a, {user}, nullptr);
        QVERIFY(closer.tick().empty());
        QCOMPARE(fake.sent, std::vector<HWND> {spare});
        closer.answered(spare);
        std::vector<SpareCloser::Done> done = closer.tick();
        QCOMPARE(done.size(), std::size_t(1));
        QVERIFY(done[0].closed);
        // Two new tabs (the user opened one too): no guessing; left after the limit.
        fake.sent.clear();
        fake.windows[a] = {other, spare, user};
        fake.gone.clear();
        closer.close(a, {user}, nullptr);
        QVERIFY(closer.tick().empty());
        QVERIFY(fake.sent.empty());
        QVERIFY(closer.isSpare(a, other) && closer.isSpare(a, spare)); // either may be it: neither counts as a tab of A
        QVERIFY(!closer.isSpare(a, user));
        QVERIFY(!closer.isSpare(h(299), other));
        fake.now += SpareCloser::kLimitMs;
        done = closer.tick();
        QCOMPARE(done.size(), std::size_t(1));
        QVERIFY(!done[0].closed);
        QCOMPARE(fake.windows[a].size(), std::size_t(3)); // nothing of the user's closed
    }

    void spareTabsPerWindow()
    {
        FakeTabs fake;
        fake.closeOnSend = true;
        const HWND a = h(300), aTab = h(301), aSpare = h(302), b = h(310), bTab = h(311), bSpare = h(312);
        fake.windows[a] = {aSpare, aTab};
        fake.windows[b] = {bTab, bSpare}; // not the active one there yet
        SpareCloser closer(fake.ops());
        closer.close(a, {aTab}, aSpare);
        closer.close(b, {bTab}, bSpare);
        QVERIFY(closer.pending(a) && closer.pending(b));
        // A switch to a tab in a window whose spare is still closing waits for it.
        QVERIFY(closer.deferSwitch(a, aTab));
        QVERIFY(!closer.deferSwitch(h(399), aTab));
        QVERIFY(closer.tick().empty()); // a's close sent (b's spare is not active: nothing)
        QCOMPARE(fake.sent, std::vector<HWND> {aSpare});
        closer.answered(aSpare);
        std::vector<SpareCloser::Done> done = closer.tick();
        QCOMPARE(done.size(), std::size_t(1));
        QCOMPARE(done[0].top, a);
        QCOMPARE(done[0].thenSelect, aTab);
        QVERIFY(done[0].front);
        QVERIFY(!closer.pending(a));
        QVERIFY(closer.pending(b)); // b's own close is untouched by a's
        QCOMPARE(fake.windows[b].size(), std::size_t(2));
        // The window itself went: over.
        fake.gone.insert(b);
        done = closer.tick();
        QCOMPARE(done.size(), std::size_t(1));
        QCOMPARE(done[0].top, b);
        QVERIFY(!closer.any());
    }

    // ---- the tabs kept for the next logon (Session, #273) ----

    static SavedWindow saved(std::vector<std::wstring> places, int active = 0)
    {
        SavedWindow w;
        w.left = 10;
        w.top = 20;
        w.right = 810;
        w.bottom = 620;
        w.active = active;
        w.tabs = std::move(places);
        return w;
    }

    void sessionReadsBackWhatItWrote()
    {
        Session session;
        session.open = true;
        session.windows = {saved({L"C:\\a b", L"D:\\文件夹\\子", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"}, 1), saved({L"\\\\server\\share"})};
        session.windows[1].maximized = true;
        session.windows[1].left = -1920; // a screen to the left
        session.previous = {saved({L"E:\\old"})};
        QVERIFY(parseSession(serialize(session)) == session);
        QVERIFY(parseSession(serialize(Session {})) == Session {});
    }

    void sessionLeavesOutWhatItCannotRead()
    {
        QVERIFY(parseSession("") == Session {});
        QVERIFY(parseSession("something else\nopen 1\nwindow 0 0 1 1 0 0\ntab C:\\a\n") == Session {});
        // A window line cut short takes its tabs with it; a window without
        // tabs is none; an index past the tabs becomes the first; CRLF reads too.
        const Session read = parseSession("WinShun.ExplorerTabs.Session 1\r\nopen 1\r\nwindow 1 2 3\r\ntab C:\\lost\r\n"
                                          "window 0 0 100 100 0 7\r\ntab C:\\kept\r\nfuture line\r\nwindow 0 0 5 5 0 0\r\n");
        QVERIFY(read.open);
        QCOMPARE(read.windows.size(), std::size_t(1));
        QCOMPARE(read.windows[0].tabs, std::vector<std::wstring> {L"C:\\kept"});
        QCOMPARE(read.windows[0].active, 0);
        // Nothing readable: nothing was open.
        QVERIFY(!parseSession("WinShun.ExplorerTabs.Session 1\nopen 1\n").open);
    }

    void sessionKeepsTheLastWindowsWhenAllClose()
    {
        Session stored = noted({}, {saved({L"C:\\a", L"C:\\b"})});
        QVERIFY(stored.open);
        QCOMPARE(tabCount(stored.windows), 2);
        // The user closed the last window: what it was stays, marked closed.
        stored = noted(stored, {});
        QVERIFY(!stored.open);
        QCOMPARE(tabCount(stored.windows), 2);
        // A new logon: that is what the logon before left, and it stays so
        // while this logon's windows are written.
        stored = newLogon(stored);
        stored = noted(stored, {saved({L"C:\\now"})});
        QCOMPARE(stored.previous.size(), std::size_t(1));
        QCOMPARE(stored.previous[0].tabs, (std::vector<std::wstring> {L"C:\\a", L"C:\\b"}));
        QCOMPARE(stored.windows[0].tabs, std::vector<std::wstring> {L"C:\\now"});
    }

    void restoreOpensWhatIsNotOpen()
    {
        const std::vector<SavedWindow> left {saved({L"C:\\a", L"C:\\b", L"C:\\c"}, 2), saved({L"D:\\x"})};
        // Nothing open: both windows, tabs and the one in front as they were.
        std::vector<Reopen> plan = planRestore(left, {});
        QCOMPARE(plan.size(), std::size_t(2));
        QCOMPARE(plan[0].into, std::uintptr_t(0));
        QVERIFY(plan[0].window == left[0]);
        QVERIFY(plan[1].window == left[1]);
        // All open already (written differently): nothing.
        QVERIFY(planRestore(left, {{7, {L"c:/A/", L"C:\\B", L"C:\\c"}}, {8, {L"D:\\x"}}}).empty());
        // Windows brought one tab of the first window back itself: the
        // others join it there, and what is in front there stays.
        plan = planRestore(left, {{7, {L"C:\\b"}}});
        QCOMPARE(plan.size(), std::size_t(2));
        QCOMPARE(plan[0].into, std::uintptr_t(7));
        QCOMPARE(plan[0].window.tabs, (std::vector<std::wstring> {L"C:\\a", L"C:\\c"}));
        QCOMPARE(plan[0].window.active, -1);
        QCOMPARE(plan[1].into, std::uintptr_t(0));
        // A tab open in a window whose place is not known does not count.
        QCOMPARE(planRestore({saved({L"D:\\x"})}, {{9, {L""}}}).size(), std::size_t(1));
    }

    void restoreSkipsWhatTabsCannotShow()
    {
        // Control Panel never comes back as a tab; the tab in front follows
        // the tabs that are left.
        const std::vector<Reopen> plan =
            planRestore({saved({L"::{26EE0668-A00A-44D7-9371-BEB064C98683}\\0", L"C:\\a", L"C:\\b"}, 2)}, {});
        QCOMPARE(plan.size(), std::size_t(1));
        QCOMPARE(plan[0].window.tabs, (std::vector<std::wstring> {L"C:\\a", L"C:\\b"}));
        QCOMPARE(plan[0].window.active, 1);
        QVERIFY(planRestore({saved({L"::{21EC2020-3AEA-1069-A2DD-08002B30309D}"})}, {}).empty());
    }
};

QTEST_GUILESS_MAIN(TestExplorerTabs)
#include "tst_explorertabs.moc"
