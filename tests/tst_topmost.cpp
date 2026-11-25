// The always-on-top plugin (src/topmost-plugin) without its shortcut and its
// note: which windows are the user's (Pins: listed), and the switch itself,
// Windows' topmost bit, on windows this test makes (never shown, never the
// user's own).

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
};

QTEST_APPLESS_MAIN(TopmostTest)
#include "tst_topmost.moc"
