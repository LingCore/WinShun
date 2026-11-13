#include "platform/FolderViewHit.h"

#include <QtTest>

class OpusFocusTest : public QObject {
    Q_OBJECT

private slots:
    void onlyTheFileDisplay()
    {
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        const wchar_t* classes[] {
            L"dopus.lister", L"dopus.filedisplaycontainer", L"dopus.filedisplay", L"WinShun.Other"};
        for (const wchar_t* name : classes) {
            WNDCLASSW windowClass {};
            windowClass.lpfnWndProc = ::DefWindowProcW;
            windowClass.hInstance = instance;
            windowClass.lpszClassName = name;
            QVERIFY(::RegisterClassW(&windowClass));
        }
        const auto create = [instance](const wchar_t* name, HWND parent = nullptr) {
            return ::CreateWindowExW(
                0, name, L"", parent ? WS_CHILD : WS_OVERLAPPED, 0, 0, 100, 100, parent, nullptr, instance, nullptr);
        };
        const HWND root = create(L"dopus.lister");
        const HWND other = create(L"WinShun.Other");
        const HWND container = create(L"dopus.filedisplaycontainer", root);
        const HWND view = create(L"dopus.filedisplay", container);
        const HWND rename = create(L"Edit", view);
        const HWND address = create(L"Edit", container);
        const HWND fakeView = create(L"dopus.filedisplay", root); // wrong parent
        QVERIFY(root && other && container && view && rename && address && fakeView);
        QVERIFY(ws::folderview::isOpusLister(root));
        QVERIFY(!ws::folderview::isOpusLister(other));
        QVERIFY(ws::folderview::isOpusFileDisplay(root, view));
        for (const HWND focus : {root, container, rename, address, fakeView, HWND(nullptr)})
            QVERIFY(!ws::folderview::isOpusFileDisplay(root, focus));
        QVERIFY(!ws::folderview::isOpusFileDisplay(other, view));
        ::DestroyWindow(root); // all its children, without ever showing or activating them
        ::DestroyWindow(other);
        for (const wchar_t* name : classes)
            QVERIFY(::UnregisterClassW(name, instance));
    }

    // Which file display a typed key belongs to (GitHub #311). Opus's own
    // menus leave the focus on the files and the thread out of menu mode;
    // they hold the mouse. Windows of this thread stand in for Opus's: never
    // shown, the focus and the capture are this thread's own state.
    void typingStopsWhileAMenuHoldsTheMouse()
    {
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        const wchar_t* classes[] {
            L"dopus.lister", L"dopus.filedisplaycontainer", L"dopus.filedisplay", L"dopus.button.display.popup"};
        for (const wchar_t* name : classes) {
            WNDCLASSW windowClass {};
            windowClass.lpfnWndProc = ::DefWindowProcW;
            windowClass.hInstance = instance;
            windowClass.lpszClassName = name;
            QVERIFY(::RegisterClassW(&windowClass));
        }
        const auto create = [instance](const wchar_t* name, HWND parent = nullptr) {
            return ::CreateWindowExW( // far off every screen: the menu is shown for a moment below
                parent ? 0 : WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, name, L"", parent ? WS_CHILD : WS_POPUP,
                parent ? 0 : -32000, parent ? 0 : -32000, 100, 100, parent, nullptr, instance, nullptr);
        };
        const HWND root = create(L"dopus.lister");
        const HWND container = create(L"dopus.filedisplaycontainer", root);
        const HWND view = create(L"dopus.filedisplay", container);
        const HWND rename = create(L"Edit", view);
        const HWND menu = create(L"dopus.button.display.popup");
        QVERIFY(root && container && view && rename && menu);
        QCOMPARE(ws::folderview::opusTypingView(nullptr), HWND(nullptr));
        QCOMPARE(ws::folderview::opusTypingView(root), HWND(nullptr)); // nothing focused yet

        ::SetFocus(view);
        if (::GetFocus() != view)
            QSKIP("This session gives a hidden window no keyboard focus");
        QCOMPARE(ws::folderview::opusTypingView(root), view);
        ::SetCapture(menu); // the context menu opens: 删除(D) is its key
        QCOMPARE(ws::folderview::opusTypingView(root), HWND(nullptr));
        ::ReleaseCapture(); // closed again
        QCOMPARE(ws::folderview::opusTypingView(root), view);
        ::ShowWindow(menu, SW_SHOWNOACTIVATE); // fading in: shown, the mouse not taken yet
        QCOMPARE(ws::folderview::opusTypingView(root), HWND(nullptr));
        ::ShowWindow(menu, SW_HIDE);
        QCOMPARE(ws::folderview::opusTypingView(root), view);
        ::SetCapture(view); // a drag in the files themselves
        QCOMPARE(ws::folderview::opusTypingView(root), HWND(nullptr));
        ::ReleaseCapture();
        ::SetFocus(rename); // a name being edited
        QCOMPARE(ws::folderview::opusTypingView(root), HWND(nullptr));

        ::DestroyWindow(root);
        ::DestroyWindow(menu);
        for (const wchar_t* name : classes)
            QVERIFY(::UnregisterClassW(name, instance));
    }

    // Opt-in, against the real Directory Opus in front (WINSHUN_OPUS_MENU_WATCH
    // = seconds to watch): the operator opens a file's context menu and closes
    // it meanwhile. Sends Opus nothing.
    void liveMenuWatch()
    {
        const int seconds = qEnvironmentVariableIntValue("WINSHUN_OPUS_MENU_WATCH");
        if (seconds <= 0)
            QSKIP("Set WINSHUN_OPUS_MENU_WATCH to watch the Opus lister in front while a menu is opened and closed");
        const HWND root = ::GetForegroundWindow();
        QVERIFY2(ws::folderview::isOpusLister(root), "no Opus lister in front");
        QList<bool> seen; // typing allowed or not, each time it changed
        QElapsedTimer watch;
        watch.start();
        while (watch.elapsed() < seconds * 1000 && ::GetForegroundWindow() == root) {
            const bool typing = ws::folderview::opusTypingView(root) != nullptr;
            if (seen.isEmpty() || seen.last() != typing) {
                seen.push_back(typing);
                qInfo("%lld ms: typing %s", watch.elapsed(), typing ? "searches" : "is left to Opus");
            }
            QTest::qSleep(20);
        }
        QCOMPARE(seen, (QList<bool> {true, false, true}));
    }
};

QTEST_GUILESS_MAIN(OpusFocusTest)
#include "tst_opusfocus.moc"
