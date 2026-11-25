// The always-on-top plugin (src/topmost-plugin) without its shortcut and its
// note: which windows are the user's (Pins: listed), the switch itself,
// Windows' topmost bit, and the line round a window on top (Border), on
// windows this test makes (never shown, never the user's own).

#include "Border.h"
#include "Pins.h"

#include <QTest>

using namespace Qt::StringLiterals;

using namespace topmost;

namespace {

// A top-level window of the test's, hidden.
struct TestWindow {
    HWND hwnd = nullptr;
    explicit TestWindow(const wchar_t* title, DWORD exStyle = 0)
        : hwnd(::CreateWindowExW(exStyle, L"STATIC", title, WS_OVERLAPPEDWINDOW, 0, 0, 200, 120, nullptr, nullptr,
              ::GetModuleHandleW(nullptr), nullptr))
    {
    }
    ~TestWindow()
    {
        if (hwnd)
            ::DestroyWindow(hwnd);
    }
};

// The pieces of the lines there are in this process (shown or not).
int stripWindows()
{
    int count = 0;
    ::EnumWindows(
        [](HWND hwnd, LPARAM out) -> BOOL {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(hwnd, &pid);
            wchar_t name[64] {};
            ::GetClassNameW(hwnd, name, 64);
            if (pid == ::GetCurrentProcessId() && std::wstring_view(name) == L"WinShunTopmostBorder")
                ++*reinterpret_cast<int*>(out);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&count));
    return count;
}

// A window 800 x 600 at (100, 50) on a 1920 x 1080 monitor.
Shape plain()
{
    Shape s;
    s.frame = {100, 50, 900, 650};
    s.monitor = {0, 0, 1920, 1080};
    s.work = {0, 0, 1920, 1040};
    s.shown = true;
    s.framed = true;
    s.roundsCorners = true;
    return s;
}

bool same(const RECT& a, const RECT& b)
{
    return ::EqualRect(&a, &b) != FALSE;
}

Facts ordinary()
{
    Facts f;
    f.visible = true;
    f.titled = true;
    return f;
}

} // namespace

class TopmostTest : public QObject {
    Q_OBJECT

private slots:
    // As Alt+Tab lists them: seen, titled, no dialog or tool window of
    // another, unless it says it belongs on the taskbar.
    void whichWindowsCount()
    {
        QVERIFY(listed(ordinary()));
        Facts f = ordinary();
        f.visible = false;
        QVERIFY(!listed(f));
        f = ordinary();
        f.cloaked = true; // on another virtual desktop
        QVERIFY(!listed(f));
        f = ordinary();
        f.titled = false;
        QVERIFY(!listed(f));
        f = ordinary();
        f.shell = true; // the desktop, a taskbar
        QVERIFY(!listed(f));
        f = ordinary();
        f.ours = true; // the plugin's own note
        QVERIFY(!listed(f));
        f = ordinary();
        f.noActivate = true; // an overlay
        QVERIFY(!listed(f));
        f = ordinary();
        f.toolWindow = true;
        QVERIFY(!listed(f));
        f.appWindow = true;
        QVERIFY(listed(f));
        f = ordinary();
        f.owned = true; // a dialog
        QVERIFY(!listed(f));
        f.appWindow = true;
        QVERIFY(listed(f));
        f.noActivate = true;
        QVERIFY(!listed(f));
    }

    void factsOfARealWindow()
    {
        const TestWindow window(L"置顶测试窗口");
        QVERIFY(window.hwnd);
        Facts f = factsOf(window.hwnd);
        QVERIFY(!f.visible && f.titled && !f.owned && !f.toolWindow && !f.shell && f.ours);
        const TestWindow tool(L"工具窗口", WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        f = factsOf(tool.hwnd);
        QVERIFY(f.toolWindow && f.noActivate);
        const TestWindow untitled(L"");
        QVERIFY(!factsOf(untitled.hwnd).titled);
        QVERIFY(factsOf(::GetShellWindow()).shell || !::GetShellWindow());
        QVERIFY(!listed(factsOf(nullptr)));
    }

    // The switch: on, off, asked twice, and a window that has gone.
    void onTopAndOff()
    {
        HWND gone = nullptr;
        {
            const TestWindow window(L"置顶测试窗口");
            QVERIFY(window.hwnd);
            gone = window.hwnd;
            QVERIFY(!pinned(window.hwnd));
            QCOMPARE(setPinned(window.hwnd, true), Result::Done);
            QVERIFY(pinned(window.hwnd));
            QVERIFY(::GetWindowLongPtrW(window.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST);
            QCOMPARE(setPinned(window.hwnd, true), Result::Done); // as it is already
            QVERIFY(pinned(window.hwnd));
            QVERIFY(!::IsWindowVisible(window.hwnd)); // changed, never shown or activated
            QCOMPARE(setPinned(window.hwnd, false), Result::Done);
            QVERIFY(!pinned(window.hwnd));
        }
        QCOMPARE(setPinned(gone, true), Result::Gone);
        QCOMPARE(setPinned(nullptr, true), Result::Gone);
    }

    void describedAndFound()
    {
        const TestWindow window(L"季度报告 Report.docx");
        const Window w = describe(window.hwnd);
        QCOMPARE(QString::fromStdWString(w.title), u"季度报告 Report.docx"_s);
        QCOMPARE(w.pid, ::GetCurrentProcessId());
        QCOMPARE(QString::fromStdWString(w.program).toLower(), u"tst_topmost.exe"_s);
        QVERIFY(QString::fromStdWString(w.path).endsWith(u"tst_topmost.exe"_s, Qt::CaseInsensitive));
        QVERIFY(!w.pinned);
        QVERIFY(matches(w, L""));
        QVERIFY(matches(w, lowered(L"REPORT")));
        QVERIFY(matches(w, L"季度"));
        QVERIFY(matches(w, L"tst_top"));
        QVERIFY(!matches(w, L"合同"));
    }

    // The user's windows as they are now: every one counts by the rule, none
    // is this test's (hidden).
    void theWindowsThereAre()
    {
        const TestWindow window(L"置顶测试窗口");
        for (const Window& w : windows()) {
            QVERIFY(w.hwnd != window.hwnd);
            QVERIFY(listed(factsOf(w.hwnd)) || !::IsWindow(w.hwnd)); // unless it closed meanwhile
            QVERIFY(w.pid != ::GetCurrentProcessId());
        }
    }

    // Which window gets a line, how thick and how round, by the scale and by
    // the corners Windows gives it.
    void theLineOfAWindow()
    {
        const auto at100 = outlineFor(plain());
        QVERIFY(at100);
        QVERIFY(same(at100->frame, plain().frame));
        QCOMPARE(at100->thickness, 2);
        QCOMPARE(at100->radius, 8);

        Shape s = plain();
        s.scale = 1.5f;
        QCOMPARE(outlineFor(s)->thickness, 3);
        QCOMPARE(outlineFor(s)->radius, 12);
        s.scale = 1.25f;
        QCOMPARE(outlineFor(s)->thickness, 3);
        QCOMPARE(outlineFor(s)->radius, 10);
        s.scale = 1.75f;
        QCOMPARE(outlineFor(s)->thickness, 4);
        QCOMPARE(outlineFor(s)->radius, 14);

        s = plain();
        s.corners = Corners::Small;
        QCOMPARE(outlineFor(s)->radius, 4);
        s.corners = Corners::Square;
        QCOMPARE(outlineFor(s)->radius, 0);
        s.corners = Corners::Round;
        s.framed = false;
        QCOMPARE(outlineFor(s)->radius, 8);
        s.corners = Corners::Default; // no frame: Windows 11 leaves it square
        QCOMPARE(outlineFor(s)->radius, 0);
        s = plain();
        s.roundsCorners = false; // Windows 10
        QCOMPARE(outlineFor(s)->radius, 0);

        s = plain();
        s.shown = false; // minimized, hidden, on another desktop
        QVERIFY(!outlineFor(s));
        s = plain();
        s.frame = {};
        QVERIFY(!outlineFor(s));
        s = plain();
        s.frame = s.monitor; // full screen
        QVERIFY(!outlineFor(s));
        s.frame = {-1, -1, 1921, 1081};
        QVERIFY(!outlineFor(s));

        // Maximized: square, cut to the work area (the frame reaches past
        // it, a pixel under the taskbar included).
        s = plain();
        s.maximized = true;
        s.frame = {-8, -8, 1928, 1041};
        const auto maximized = outlineFor(s);
        QVERIFY(maximized);
        QVERIFY(same(maximized->frame, RECT {0, 0, 1920, 1040}));
        QCOMPARE(maximized->radius, 0);
        s.frame = s.monitor; // maximized and over the taskbar: full screen
        QVERIFY(!outlineFor(s));
        s.work = s.monitor; // a taskbar that hides itself: maximized is this large
        QVERIFY(outlineFor(s));
        QVERIFY(same(outlineFor(s)->frame, s.monitor));
        s.maximized = false;
        QVERIFY(!outlineFor(s));
    }

    // Four pieces that make the whole edge and never overlap.
    void thePiecesOfTheLine()
    {
        Outline o;
        o.frame = {100, 50, 900, 650};
        o.thickness = 3;
        o.radius = 12;
        auto strips = stripsOf(o);
        QVERIFY(same(strips[Top], RECT {100, 50, 900, 62}));
        QVERIFY(same(strips[Bottom], RECT {100, 638, 900, 650}));
        QVERIFY(same(strips[Left], RECT {100, 62, 103, 638}));
        QVERIFY(same(strips[Right], RECT {897, 62, 900, 638}));

        o.radius = 0; // square: the top and bottom are as thin as the sides
        strips = stripsOf(o);
        QVERIFY(same(strips[Top], RECT {100, 50, 900, 53}));
        QVERIFY(same(strips[Left], RECT {100, 53, 103, 647}));

        o.radius = 12; // a window lower than two corners: no sides
        o.frame = {0, 0, 300, 20};
        strips = stripsOf(o);
        QVERIFY(same(strips[Top], RECT {0, 0, 300, 10}));
        QVERIFY(same(strips[Bottom], RECT {0, 10, 300, 20}));
        QVERIFY(::IsRectEmpty(&strips[Left]) && ::IsRectEmpty(&strips[Right]));
    }

    void shapeOfARealWindow()
    {
        const TestWindow window(L"置顶测试窗口");
        const Shape s = shapeOf(window.hwnd);
        QVERIFY(!s.shown && s.framed && !s.maximized);
        QVERIFY(s.scale >= 1 && !::IsRectEmpty(&s.monitor) && !::IsRectEmpty(&s.work) && !::IsRectEmpty(&s.frame));
        QVERIFY(!outlineFor(s)); // not shown: no line
        QVERIFY(!shapeOf(nullptr).shown);
    }

    // A line lives as long as its window is on top: it goes when asked, when
    // the window comes off the top by another hand, and when it closes.
    void linesComeAndGo()
    {
        QCOMPARE(stripWindows(), 0);
        {
            Borders borders;
            const TestWindow first(L"置顶测试窗口 一");
            const TestWindow second(L"置顶测试窗口 二");
            borders.add(first.hwnd); // not on top: none
            QTest::qWait(100);
            QCOMPARE(stripWindows(), 0);

            QCOMPARE(setPinned(first.hwnd, true), Result::Done);
            QCOMPARE(setPinned(second.hwnd, true), Result::Done);
            borders.add(first.hwnd);
            borders.add(second.hwnd);
            borders.add(first.hwnd); // twice: still one line
            QTRY_COMPARE(stripWindows(), 8);
            borders.remove(second.hwnd);
            QTRY_COMPARE(stripWindows(), 4);
            borders.add(second.hwnd);
            QTRY_COMPARE(stripWindows(), 8);

            QCOMPARE(setPinned(first.hwnd, false), Result::Done); // by anyone
            QTRY_COMPARE(stripWindows(), 4);
            {
                const TestWindow third(L"置顶测试窗口 三");
                QCOMPARE(setPinned(third.hwnd, true), Result::Done);
                borders.add(third.hwnd);
                QTRY_COMPARE(stripWindows(), 8);
            } // closed
            QTRY_COMPARE(stripWindows(), 4);
            borders.clear();
            QTRY_COMPARE(stripWindows(), 0);
            borders.add(second.hwnd);
            QTRY_COMPARE(stripWindows(), 4);
        } // the lines go with their owner
        QCOMPARE(stripWindows(), 0);
    }
};

QTEST_APPLESS_MAIN(TopmostTest)
#include "tst_topmost.moc"
