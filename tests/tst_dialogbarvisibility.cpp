#include "DialogBarVisibility.h"
#include "DialogBarPlacement.h"
#include "platform/DialogOwner.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QSignalBlocker>
#include <QTest>

#include <dwmapi.h>

#include <future>
#include <string>
#include <string_view>
#include <thread>

namespace {

HWND handle(QWindow& window)
{
    return reinterpret_cast<HWND>(window.winId());
}

constexpr UINT kZOrderOnly = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;

bool topmost(HWND hwnd)
{
    return ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST;
}

// While the user is working in another program (seen with a windowed game,
// File Explorer and the desktop in front), Windows can ignore this
// process's requests to raise a window over the one in front, to the top of
// its band or into the topmost band, though SetWindowPos succeeds. A window
// merely put in front by a script, given input or a foreground lock, did not
// cause it (2026-10-27). Then the cases below cannot set up the stacking they
// check: each asks for it first, before any of Win顺's code runs, and skips
// if Windows refuses.
constexpr char kNoRaise[] = "Windows ignores this process's requests to raise windows now "
                            "(seen while the user works in another program): run it again when no one is";

// Somewhere above, in Z order.
bool above(HWND top, HWND bottom)
{
    for (HWND next = ::GetWindow(top, GW_HWNDNEXT); next; next = ::GetWindow(next, GW_HWNDNEXT)) {
        if (next == bottom)
            return true;
    }
    return false;
}

// Shown as a foreign dialog is: natively, not activated, unknown to Qt.
void showPassive(QWindow& window)
{
    ::ShowWindow(handle(window), SW_SHOWNOACTIVATE);
}

DWORD cloaked(QQuickWindow& window)
{
    DWORD value = 0;
    ::DwmGetWindowAttribute(handle(window), DWMWA_CLOAKED, &value, sizeof value);
    return value;
}

void prepare(QQuickWindow& window)
{
    window.setFlags(Qt::Tool | Qt::FramelessWindowHint);
    window.setColor(Qt::darkGray);
    window.setGeometry(80, 80, 480, 44);
    window.create();
}

// Deliver a frame from a render thread while the GUI is busy placing the
// bar. Its queued reveal must not yet let place() show the separate logo.
void swapFrame(QQuickWindow& window)
{
    std::thread render([&window] {
        emit window.afterSynchronizing();
        emit window.frameSwapped();
    });
    render.join();
}

// An elevated dialog (high integrity) by Win顺's bar (medium): across UIPI.
// A copy of this test at low integrity beside this one at medium meets the
// same boundary, without elevation. Its windows act on the messages below,
// which this side may send down to it.
namespace low {

constexpr wchar_t kClass[] = L"tst_dialogbarvisibility.low";
constexpr UINT kFollow = WM_APP + 1; // lParam: the dialog the companion follows, 0 for none; wParam: elevated
constexpr UINT kFollowOwn = WM_APP + 2; // the companion follows the child's own dialog
constexpr UINT kAdoptMenu = WM_APP + 3; // its menu, as DialogBar::prepareMenuWindow does
constexpr UINT kCanPin = WM_APP + 4; // whether Windows puts the companion on top now (kNoRaise): 1 yes, 2 no

struct Windows {
    HWND companion = nullptr;
    HWND dialog = nullptr; // of the child
    HWND menu = nullptr;
};

Windows g_windows; // the child's

LRESULT CALLBACK childProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case kFollow:
        ws::win::followDialog(g_windows.companion, reinterpret_cast<HWND>(lParam), wParam != 0);
        return 1;
    case kFollowOwn:
        ws::win::followDialog(g_windows.companion, g_windows.dialog, false);
        return 1;
    case kAdoptMenu:
        ws::win::followOwner(g_windows.menu, nullptr);
        ws::win::followOwner(g_windows.menu, g_windows.companion);
        ::ShowWindow(g_windows.menu, SW_SHOWNOACTIVATE);
        return 1;
    case kCanPin: {
        // Asked as followDialog asks it, then let go again.
        ::SetWindowPos(g_windows.companion, HWND_TOPMOST, 0, 0, 0, 0, kZOrderOnly);
        const bool pinned = topmost(g_windows.companion);
        ::SetWindowPos(g_windows.companion, HWND_NOTOPMOST, 0, 0, 0, 0, kZOrderOnly);
        return pinned ? 1 : 2;
    }
    default:
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

int runChild()
{
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSW type {};
    type.lpfnWndProc = childProc;
    type.hInstance = instance;
    type.lpszClassName = kClass;
    ::RegisterClassW(&type);
    const auto make = [instance](const wchar_t* name, DWORD exStyle, int y) {
        const HWND hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | exStyle, kClass, name, WS_POPUP,
            80, y, 160, 40, nullptr, nullptr, instance, nullptr);
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        return hwnd;
    };
    // The menu starts on top, as Qt::WindowStaysOnTopHint creates it.
    g_windows = {make(L"companion", 0, 200), make(L"dialog", 0, 260), make(L"menu", WS_EX_TOPMOST, 320)};
    MSG message;
    while (::GetMessageW(&message, nullptr, 0, 0) > 0)
        ::DispatchMessageW(&message);
    return 0;
}

class Child {
public:
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child()
    {
        if (!m_process.hProcess)
            return;
        ::TerminateProcess(m_process.hProcess, 0);
        ::WaitForSingleObject(m_process.hProcess, 5000);
        ::CloseHandle(m_process.hThread);
        ::CloseHandle(m_process.hProcess);
    }

    bool start()
    {
        HANDLE own = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(),
                TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY, &own))
            return false;
        HANDLE token = nullptr;
        const bool duplicated = ::DuplicateTokenEx(own, 0, nullptr, SecurityImpersonation, TokenPrimary, &token);
        ::CloseHandle(own);
        if (!duplicated)
            return false;
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
        PSID sid = nullptr;
        bool started = false;
        if (::AllocateAndInitializeSid(&authority, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &sid)) {
            TOKEN_MANDATORY_LABEL label {{sid, SE_GROUP_INTEGRITY}};
            wchar_t exe[MAX_PATH] {};
            ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring line = L"\"" + std::wstring(exe) + L"\" --low-companion";
            STARTUPINFOW startup {sizeof startup};
            started = ::SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof label + ::GetLengthSid(sid))
                && ::CreateProcessAsUserW(token, nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                    nullptr, nullptr, &startup, &m_process);
            ::FreeSid(sid);
        }
        ::CloseHandle(token);
        for (int i = 0; started && i < 200 && !found(); ++i)
            ::Sleep(25);
        return started && found();
    }

    const Windows& windows() const { return m_windows; }

    LRESULT send(UINT message, HWND target = nullptr, bool elevated = false) const
    {
        DWORD_PTR result = 0;
        ::SendMessageTimeoutW(m_windows.companion, message, elevated, reinterpret_cast<LPARAM>(target),
            SMTO_ABORTIFHUNG, 5000, &result);
        return static_cast<LRESULT>(result);
    }

private:
    bool found()
    {
        ::EnumWindows(
            [](HWND hwnd, LPARAM self) -> BOOL {
                auto& child = *reinterpret_cast<Child*>(self);
                DWORD process = 0;
                ::GetWindowThreadProcessId(hwnd, &process);
                wchar_t name[32] {};
                if (process != child.m_process.dwProcessId || !::GetClassNameW(hwnd, name, 32)
                    || std::wstring_view(name) != kClass || !::GetWindowTextW(hwnd, name, 32))
                    return TRUE;
                const std::wstring_view title(name);
                HWND& slot = title == L"companion" ? child.m_windows.companion
                    : title == L"dialog"           ? child.m_windows.dialog
                                                   : child.m_windows.menu;
                slot = hwnd;
                return TRUE;
            },
            reinterpret_cast<LPARAM>(this));
        return m_windows.companion && m_windows.dialog && m_windows.menu;
    }

    PROCESS_INFORMATION m_process {};
    Windows m_windows;
};

} // namespace low

} // namespace

class DialogBarVisibilityTest : public QObject {
    Q_OBJECT

private slots:
    void placementUsesAvailableRowsAndHysteresis()
    {
        using namespace ws::dialogbar;
        QCOMPARE(chooseSide(220, 600, 600, 200, 60, 480, 24, Side::Auto, Side::Below), Side::Below);
        QCOMPARE(chooseSide(199, 600, 600, 200, 60, 480, 24, Side::Auto, Side::Below), Side::Right);
        for (int below = 200; below < 224; ++below)
            QCOMPARE(chooseSide(below, 600, 600, 200, 60, 480, 24, Side::Auto, Side::Right), Side::Right);
        QCOMPARE(chooseSide(224, 600, 600, 200, 60, 480, 24, Side::Auto, Side::Right), Side::Below);
        QCOMPARE(chooseSide(300, 600, 600, 200, 60, 480, 24, Side::Left, Side::Below), Side::Left);
        QCOMPARE(chooseSide(300, 100, 600, 200, 60, 480, 24, Side::Left, Side::Below), Side::Below);
    }

    // 固定在下方 (#95): below while the bar itself fits, its list opening
    // upward; beside only when not even the bar does. Auto and a side set stay
    // as they were. At 150%: the bar and two rows 242 px, the bar 72, a side 726.
    void fixedBelowKeepsTheBarWhileItFits()
    {
        using namespace ws::dialogbar;
        const auto side = [](int below, int left, int right, Side preferred, Side current) {
            return chooseSide(below, left, right, 242, 72, 726, 36, preferred, current);
        };
        for (int below : {260, 242, 200, 120, 72})
            QCOMPARE(side(below, 800, 800, Side::Below, Side::Below), Side::Below);
        QCOMPARE(side(72, 800, 800, Side::Below, Side::Right), Side::Below); // back as soon as the bar fits
        QCOMPARE(side(71, 800, 800, Side::Below, Side::Below), Side::Right); // not even the bar: beside
        QCOMPARE(side(71, 800, 300, Side::Below, Side::Below), Side::Left);
        QCOMPARE(side(71, 300, 300, Side::Below, Side::Below), Side::Below); // nowhere: place() puts it above or over
        QCOMPARE(side(260, 800, 800, Side::Auto, Side::Below), Side::Below);
        QCOMPARE(side(200, 800, 800, Side::Auto, Side::Below), Side::Right);
        QCOMPARE(side(120, 800, 800, Side::Auto, Side::Below), Side::Right);
        QCOMPARE(side(200, 300, 300, Side::Auto, Side::Below), Side::Below);
        QCOMPARE(side(200, 800, 800, Side::Right, Side::Below), Side::Right);
        QCOMPARE(side(260, 800, 300, Side::Right, Side::Below), Side::Below);
    }

    // Never owned by the dialog: an owner on another thread joins the input of
    // the two (#95). Topmost only while the dialog is; the dialog untouched.
    void companionTakesOnlyItsDialogsBand()
    {
        QQuickWindow dialog;
        QQuickWindow companion;
        prepare(dialog);
        prepare(companion);
        const HWND box = handle(dialog);
        const HWND bar = handle(companion);
        const HWND foreground = ::GetForegroundWindow();
        ws::win::followDialog(bar, box, false);
        QCOMPARE(::GetWindow(bar, GW_OWNER), HWND(nullptr));
        QVERIFY(!topmost(bar));
        ::SetWindowPos(box, HWND_TOPMOST, 0, 0, 0, 0, kZOrderOnly);
        ws::win::followDialog(bar, box, false);
        QCOMPARE(::GetWindow(bar, GW_OWNER), HWND(nullptr));
        QVERIFY(topmost(bar));
        QQuickWindow other;
        prepare(other);
        ws::win::followDialog(bar, handle(other), false);
        QCOMPARE(::GetWindow(bar, GW_OWNER), HWND(nullptr));
        QVERIFY(!topmost(bar));
        QVERIFY(topmost(box));
        // Had the bar an owner (as #95's first fix gave it), it lets go of it
        // first: leaving the top would take a pinned owner down with it.
        ws::win::followDialog(bar, box, false);
        ::SetWindowLongPtrW(bar, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(box));
        ws::win::followDialog(bar, handle(other), false);
        QCOMPARE(::GetWindow(bar, GW_OWNER), HWND(nullptr));
        QVERIFY(!topmost(bar));
        QVERIFY(topmost(box));
        QCOMPARE(::GetForegroundWindow(), foreground);
        ws::win::followDialog(bar, nullptr, false);
        QVERIFY(!topmost(bar));
    }

    // Each real dialog is on another thread: never the bar's owner, so its
    // closing cannot take the bar along either.
    void foreignThreadDialogNeverOwnsTheBar()
    {
        std::promise<std::pair<HWND, DWORD>> ready;
        auto handleReady = ready.get_future();
        std::jthread foreign([&] {
            const HWND dialog = ::CreateWindowExW(0, L"STATIC", L"isolated dialog", WS_POPUP,
                0, 0, 200, 200, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
            MSG message;
            ::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
            ready.set_value({dialog, ::GetCurrentThreadId()});
            while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            ::DestroyWindow(dialog);
        });
        const auto [box, thread] = handleReady.get();
        QQuickWindow companion;
        prepare(companion);
        const HWND bar = handle(companion);
        ws::win::followDialog(bar, box, false);
        const HWND owner = ::GetWindow(bar, GW_OWNER);
        const bool rightAbove = ::GetWindow(box, GW_HWNDPREV) == bar;
        ::PostThreadMessageW(thread, WM_QUIT, 0, 0);
        foreign.join();
        QCOMPARE(owner, HWND(nullptr));
        QVERIFY(rightAbove);
        QVERIFY(!::IsWindow(box));
        QVERIFY(::IsWindow(bar));
    }

    void shownCompanionStaysAboveDialogAndPinnedOwner()
    {
        QQuickWindow pin;
        QQuickWindow dialog;
        QQuickWindow companion;
        prepare(pin);
        prepare(dialog);
        prepare(companion);
        const HWND pinned = handle(pin);
        const HWND owner = handle(dialog);
        const HWND bar = handle(companion);
        // A passive pin -> save dialog -> search bar, without taking focus
        // from the user's actual foreground window.
        ::SetWindowLongPtrW(owner, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(pinned));
        ::SetWindowPos(pinned, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!topmost(pinned)) {
            ::SetWindowLongPtrW(owner, GWLP_HWNDPARENT, 0);
            QSKIP(kNoRaise);
        }
        ::ShowWindow(pinned, SW_SHOWNOACTIVATE);
        ::ShowWindow(owner, SW_SHOWNOACTIVATE);
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&companion, [](bool) {});
        const HWND foreground = ::GetForegroundWindow();
        visibility.show(owner);
        QTRY_VERIFY(visibility.isRevealed());
        QCOMPARE(::GetWindow(bar, GW_OWNER), HWND(nullptr));
        QVERIFY(topmost(bar));
        QVERIFY(topmost(pinned));
        QVERIFY(topmost(owner));
        QCOMPARE(::GetWindow(owner, GW_HWNDPREV), bar);
        QVERIFY(above(owner, pinned));
        visibility.hide();
        QQuickWindow ordinary;
        prepare(ordinary);
        visibility.show(handle(ordinary));
        QTRY_VERIFY(visibility.isRevealed());
        QCOMPARE(::GetWindow(bar, GW_OWNER), HWND(nullptr));
        QVERIFY(!topmost(bar));
        QVERIFY(topmost(pinned));
        QVERIFY(topmost(owner));
        QCOMPARE(::GetForegroundWindow(), foreground);
        visibility.hide();
        // The dialog's own owner: let go of before Qt destroys these windows in reverse lifetime order.
        ::SetWindowLongPtrW(owner, GWLP_HWNDPARENT, 0);
    }

    // The bar comes up right above its dialog from under the windows behind
    // (the #104 live test), and no other window moves, an unrelated topmost
    // one (a pinned screenshot) least of all.
    void companionStacksDirectlyAboveItsDialog()
    {
        QQuickWindow pin;
        QQuickWindow behind;
        QQuickWindow dialog;
        QQuickWindow cover;
        QQuickWindow companion;
        for (QQuickWindow* window : {&pin, &behind, &dialog, &cover, &companion})
            prepare(*window);
        const HWND pinned = handle(pin);
        const HWND owner = handle(dialog);
        const HWND bar = handle(companion);
        ::SetWindowPos(pinned, HWND_TOPMOST, 0, 0, 0, 0, kZOrderOnly);
        for (QQuickWindow* window : {&pin, &behind, &dialog, &cover, &companion})
            showPassive(*window);
        const HWND foreground = ::GetForegroundWindow();
        // The bar under them all, then another window raised over both.
        ::SetWindowPos(bar, HWND_BOTTOM, 0, 0, 0, 0, kZOrderOnly);
        ::SetWindowPos(handle(behind), HWND_TOP, 0, 0, 0, 0, kZOrderOnly);
        ::SetWindowPos(owner, HWND_TOP, 0, 0, 0, 0, kZOrderOnly);
        ::SetWindowPos(handle(cover), HWND_TOP, 0, 0, 0, 0, kZOrderOnly);
        const HWND overCover = ::GetWindow(handle(cover), GW_HWNDPREV);
        if (overCover && !topmost(overCover))
            QSKIP(kNoRaise);
        QVERIFY(above(handle(cover), owner) && above(owner, handle(behind)) && above(handle(behind), bar));
        ws::win::followDialog(bar, owner, false);
        QCOMPARE(::GetWindow(bar, GW_OWNER), HWND(nullptr));
        QCOMPARE(::GetWindow(owner, GW_HWNDPREV), bar);
        QVERIFY(above(handle(cover), bar));
        QVERIFY(above(owner, handle(behind)));
        QVERIFY(above(pinned, bar));
        QVERIFY(!topmost(bar));
        QVERIFY(!topmost(owner));
        QVERIFY(topmost(pinned));
        ws::win::followDialog(bar, owner, false); // already there
        QCOMPARE(::GetWindow(owner, GW_HWNDPREV), bar);
        // The dialog in front, first of its band; the bar shown again from below.
        ::SetWindowPos(bar, HWND_BOTTOM, 0, 0, 0, 0, kZOrderOnly);
        ::SetWindowPos(owner, HWND_TOP, 0, 0, 0, 0, kZOrderOnly);
        const HWND overDialog = ::GetWindow(owner, GW_HWNDPREV);
        QVERIFY(!overDialog || topmost(overDialog));
        ws::win::followDialog(bar, owner, false);
        QCOMPARE(::GetWindow(owner, GW_HWNDPREV), bar);
        QVERIFY(!topmost(bar));
        QVERIFY(above(pinned, bar));
        QCOMPARE(::GetForegroundWindow(), foreground);
    }

    // Through a real Qt show.
    void shownCompanionComesUpOverWindowsBehind()
    {
        QQuickWindow dialog;
        QQuickWindow behind;
        QQuickWindow companion;
        for (QQuickWindow* window : {&dialog, &behind, &companion})
            prepare(*window);
        const HWND owner = handle(dialog);
        const HWND bar = handle(companion);
        showPassive(behind);
        showPassive(dialog);
        ::SetWindowPos(handle(behind), HWND_TOP, 0, 0, 0, 0, kZOrderOnly);
        ::SetWindowPos(owner, HWND_TOP, 0, 0, 0, 0, kZOrderOnly);
        ::SetWindowPos(bar, HWND_BOTTOM, 0, 0, 0, 0, kZOrderOnly);
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&companion, [](bool) {});
        const HWND foreground = ::GetForegroundWindow();
        visibility.show(owner);
        QTRY_VERIFY(visibility.isRevealed());
        QCOMPARE(::GetWindow(bar, GW_OWNER), HWND(nullptr));
        QCOMPARE(::GetWindow(owner, GW_HWNDPREV), bar);
        QVERIFY(above(bar, handle(behind)));
        QCOMPARE(::GetForegroundWindow(), foreground);
        visibility.hide();
    }

    void menuStaysAboveTheBar_data()
    {
        QTest::addColumn<bool>("pinnedDialog");
        QTest::newRow("bar by a normal dialog") << false;
        QTest::newRow("bar by a pinned dialog") << true;
    }

    // A menu or tip, as DialogBar::prepareMenuWindow (without its styling) and
    // ContextMenu.popup() treat it: owned by the bar, in its band, above it.
    // Dropping its own on-top flag demotes neither the bar nor a pinned dialog.
    // The dialog clicked into comes up over both: the bar, following it again,
    // takes the menu along.
    void menuStaysAboveTheBar()
    {
        QFETCH(bool, pinnedDialog);
        QQuickWindow dialog;
        QQuickWindow companion;
        QQuickWindow menu;
        prepare(dialog);
        prepare(companion);
        menu.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
        menu.setColor(Qt::gray);
        menu.setGeometry(100, 140, 240, 120);
        menu.create();
        const HWND owner = handle(dialog);
        const HWND bar = handle(companion);
        const HWND popup = handle(menu);
        if (pinnedDialog)
            ::SetWindowPos(owner, HWND_TOPMOST, 0, 0, 0, 0, kZOrderOnly);
        showPassive(dialog);
        ws::win::followDialog(bar, owner, false);
        showPassive(companion);
        QCOMPARE(topmost(bar), pinnedDialog);
        const HWND foreground = ::GetForegroundWindow();
        menu.setProperty("_q_showWithoutActivating", true);
        menu.setTransientParent(nullptr);
        ws::win::followOwner(popup, nullptr);
        menu.setFlag(Qt::WindowStaysOnTopHint, false);
        menu.setTransientParent(&companion);
        ws::win::followOwner(popup, bar);
        menu.show();
        menu.raise();
        QCOMPARE(::GetWindow(popup, GW_OWNER), bar);
        QVERIFY(above(popup, bar));
        QVERIFY(above(bar, owner));
        QCOMPARE(topmost(popup), pinnedDialog);
        QCOMPARE(topmost(bar), pinnedDialog);
        QCOMPARE(topmost(owner), pinnedDialog);
        ::SetWindowPos(owner, HWND_TOP, 0, 0, 0, 0, kZOrderOnly);
        QVERIFY(above(owner, bar));
        ws::win::followDialog(bar, owner, false);
        QCOMPARE(::GetWindow(owner, GW_HWNDPREV), bar);
        QVERIFY(above(popup, bar));
        QCOMPARE(::GetWindow(popup, GW_OWNER), bar);
        QCOMPARE(topmost(popup), pinnedDialog);
        QCOMPARE(topmost(bar), pinnedDialog);
        QCOMPARE(topmost(owner), pinnedDialog);
        QCOMPARE(::GetForegroundWindow(), foreground);
        menu.hide();
        menu.setTransientParent(nullptr);
        ws::win::followOwner(popup, nullptr);
    }

    // An elevated dialog: the bar is topmost and unowned, its menu above it,
    // as before #95 (it shows only while that dialog is in front). By a dialog
    // it can follow, it leaves the top for right above that one; by none, it
    // is neither owned nor on top. Across a real integrity boundary: the low
    // copy's bar by this side's window.
    void elevatedDialogKeepsCompanionOnTop()
    {
        QQuickWindow elevated; // of higher integrity than the child
        prepare(elevated);
        showPassive(elevated);
        const HWND higher = handle(elevated);
        const HWND foreground = ::GetForegroundWindow();
        low::Child child;
        QVERIFY2(child.start(), "the low-integrity copy did not start");
        const LRESULT canPin = child.send(low::kCanPin);
        QVERIFY(canPin == 1 || canPin == 2);
        if (canPin == 2)
            QSKIP(kNoRaise);
        const low::Windows& its = child.windows();
        QCOMPARE(child.send(low::kFollow, higher, true), LRESULT(1));
        QCOMPARE(::GetWindow(its.companion, GW_OWNER), HWND(nullptr));
        QVERIFY(topmost(its.companion));
        QVERIFY(!topmost(higher));
        QCOMPARE(child.send(low::kFollow, higher, true), LRESULT(1)); // again: as it is
        QCOMPARE(::GetWindow(its.companion, GW_OWNER), HWND(nullptr));
        QVERIFY(topmost(its.companion));
        QCOMPARE(child.send(low::kAdoptMenu), LRESULT(1));
        QCOMPARE(::GetWindow(its.menu, GW_OWNER), its.companion);
        QVERIFY(topmost(its.menu));
        QVERIFY(above(its.menu, its.companion));
        QCOMPARE(child.send(low::kFollowOwn), LRESULT(1));
        QCOMPARE(::GetWindow(its.companion, GW_OWNER), HWND(nullptr));
        QVERIFY(!topmost(its.companion));
        QVERIFY(!topmost(its.dialog));
        QCOMPARE(::GetWindow(its.dialog, GW_HWNDPREV), its.companion);
        QVERIFY(above(its.menu, its.companion)); // came along
        QCOMPARE(child.send(low::kAdoptMenu), LRESULT(1));
        QVERIFY(!topmost(its.menu));
        QVERIFY(above(its.menu, its.companion));
        QCOMPARE(child.send(low::kFollow, higher, true), LRESULT(1));
        QCOMPARE(::GetWindow(its.companion, GW_OWNER), HWND(nullptr));
        QVERIFY(topmost(its.companion));
        QVERIFY(!topmost(its.dialog));
        QVERIFY(!topmost(higher));
        QCOMPARE(child.send(low::kFollow), LRESULT(1));
        QCOMPARE(::GetWindow(its.companion, GW_OWNER), HWND(nullptr));
        QVERIFY(!topmost(its.companion));
        QCOMPARE(::GetForegroundWindow(), foreground);
    }

    void frameWaitsForGuiReveal()
    {
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        visibility.show();
        QVERIFY(window.isVisible());
        QVERIFY(cloaked(window) != 0);
        swapFrame(window);
        QVERIFY(!visibility.isRevealed());
        QVERIFY(!logo);
        QTRY_VERIFY(visibility.isRevealed());
        QCOMPARE(cloaked(window), DWORD(0));
        QVERIFY(logo);
    }

    void previousFrameCannotRevealNewOpening()
    {
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        visibility.show();
        swapFrame(window);
        visibility.hide();
        visibility.show();
        // Deliver only the old posted frame, without drawing a new one.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(!visibility.isRevealed());
        QVERIFY(!logo);
        QVERIFY(cloaked(window) != 0);
        QTRY_VERIFY(visibility.isRevealed());
        QVERIFY(logo);
        QCOMPARE(cloaked(window), DWORD(0));
    }

    void frameInFlightCannotRevealNewOpening()
    {
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        visibility.show();
        std::thread synchronize([&window] { emit window.afterSynchronizing(); });
        synchronize.join();
        visibility.hide();
        visibility.show();
        std::thread present([&window] { emit window.frameSwapped(); });
        present.join();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(!visibility.isRevealed());
        QVERIFY(!logo);
        QVERIFY(cloaked(window) != 0);
        QTRY_VERIFY(visibility.isRevealed());
        QVERIFY(logo);
    }

    void externalHideHidesLogo_data()
    {
        QTest::addColumn<bool>("close");
        QTest::newRow("Qt hide") << false;
        QTest::newRow("Qt close") << true;
    }

    void externalHideHidesLogo()
    {
        QFETCH(bool, close);
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        visibility.show();
        QTRY_VERIFY(logo);
        const HWND beforeClose = handle(window);
        if (close)
            window.close(); // close/Alt+F4, outside DialogBar::hideWindow
        else
            window.hide();
        QVERIFY(!window.isVisible());
        QVERIFY(!logo);
        visibility.hide(); // cleanup must also work after Qt already hid it
        QVERIFY(!logo);
        QVERIFY(::IsWindow(beforeClose));
        visibility.show();
        QTRY_VERIFY(logo);
        QCOMPARE(handle(window), beforeClose); // DialogJump's companion HWND stays valid
    }

    void nativeHiddenWindowRecoversWithoutActivation()
    {
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        visibility.show();
        QTRY_VERIFY(logo);
        ::ShowWindow(handle(window), SW_HIDE);
        QVERIFY(window.isVisible());
        QVERIFY(!::IsWindowVisible(handle(window)));
        visibility.show();
        QTRY_VERIFY_WITH_TIMEOUT(::IsWindowVisible(handle(window)), 1500);
        QTRY_VERIFY(visibility.isRevealed());
        QVERIFY(::GetForegroundWindow() != handle(window));
        QVERIFY(logo);
    }

    void timeoutShowsWindowWithoutFrameSignal()
    {
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        const QSignalBlocker noFrameSignal(&window);
        visibility.show();
        QTRY_VERIFY_WITH_TIMEOUT(logo, 1500);
        QVERIFY(visibility.isRevealed());
        QVERIFY(::IsWindowVisible(handle(window)));
        QCOMPARE(cloaked(window), DWORD(0));
        visibility.hide();
        QVERIFY(!logo);
        QVERIFY(!window.isVisible());
    }
};

int main(int argc, char** argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "--low-companion")
        return low::runChild();
    QGuiApplication app(argc, argv);
    DialogBarVisibilityTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_dialogbarvisibility.moc"
