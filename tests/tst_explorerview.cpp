#include "platform/FolderViewHit.h"

#include <QRect>
#include <QtTest>

#include <string_view>
#include <vector>

using namespace Qt::StringLiterals;
using namespace ws;

// File Explorer's windows as far as folderview:: looks at them (classes,
// parents, visibility, bounds): Windows 10 22H2's, the chain GitHub #203's
// diagnostics have (folder-menu-double-click), and the tabs of Windows 11.
// Classes of this module named as the real ones (its own come before another
// of the same name); far off the desktop, never activated.
class ExplorerViewTest : public QObject {
    Q_OBJECT

    static constexpr int kFar = -20000;
    std::vector<HWND> m_roots;

    static HWND make(const wchar_t* type, HWND parent, const QRect& at, bool shown = true)
    {
        return ::CreateWindowExW(parent ? 0 : WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, type, L"",
            (parent ? WS_CHILD : WS_POPUP) | (shown ? WS_VISIBLE : 0), parent ? at.x() : kFar + at.x(),
            parent ? at.y() : kFar + at.y(), at.width(), at.height(), parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    }
    HWND window(const QRect& at)
    {
        const HWND root = make(L"CabinetWClass", nullptr, at);
        if (root)
            m_roots.push_back(root);
        return root;
    }
    // A tab at `at` (in the window), its file list at `list` (in the tab):
    // its items view, and the folder view around it in `folderView`.
    static HWND tab(HWND root, const QRect& at, const QRect& list, bool shown, HWND* folderView)
    {
        const QRect all(QPoint(0, 0), at.size());
        const HWND host = make(L"DirectUIHWND", make(L"DUIViewWndClassName", make(L"ShellTabWindowClass", root, at, shown), all), all);
        *folderView = make(L"SHELLDLL_DefView", make(L"CtrlNotifySink", host, list), QRect(QPoint(0, 0), list.size()));
        return make(L"DirectUIHWND", *folderView, QRect(QPoint(0, 0), list.size()));
    }
    static QRect bounds(const RECT& r) { return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top); }
    static QRect bounds(HWND window)
    {
        RECT r {};
        ::GetWindowRect(window, &r);
        return bounds(r);
    }
    static HWND tabOf(HWND items)
    {
        for (HWND up = items; up; up = ::GetParent(up)) {
            wchar_t name[64] {};
            ::GetClassNameW(up, name, 64);
            if (std::wstring_view(name) == L"ShellTabWindowClass")
                return up;
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        for (const wchar_t* name : {L"CabinetWClass", L"ShellTabWindowClass", L"DUIViewWndClassName", L"DirectUIHWND",
                 L"CtrlNotifySink", L"SHELLDLL_DefView"}) {
            WNDCLASSW windowClass {};
            windowClass.lpfnWndProc = ::DefWindowProcW;
            windowClass.hInstance = ::GetModuleHandleW(nullptr);
            windowClass.lpszClassName = name;
            ::RegisterClassW(&windowClass); // fails harmlessly once registered
        }
    }
    void cleanup()
    {
        for (const HWND root : m_roots)
            ::DestroyWindow(root); // and all in it
        m_roots.clear();
    }

    // One tab, under the ribbon and the address bar; the list right of the
    // navigation pane, above the status bar.
    void windows10()
    {
        const HWND root = window({0, 0, 1200, 800});
        HWND folderView = nullptr;
        const HWND items = tab(root, {0, 120, 1200, 680}, {260, 0, 940, 650}, true, &folderView);
        QVERIFY(root && items && folderView);
        QCOMPARE(QString::fromStdWString(folderview::classChain(items)),
            u"DirectUIHWND<SHELLDLL_DefView<CtrlNotifySink<DirectUIHWND<DUIViewWndClassName<ShellTabWindowClass<CabinetWClass"_s);
        const folderview::Hit hit = folderview::classify(items, {});
        QVERIFY(hit.surface == folderview::Surface::Explorer);
        QVERIFY(hit.view == items);
        QVERIFY(hit.root == root);
        QVERIFY(hit.tab == tabOf(items));
        QVERIFY(folderview::classifyWindow(root).tab == hit.tab); // the hotkey's, by the window alone
        QCOMPARE(bounds(folderview::explorerFileList(root, hit.tab)), QRect(kFar + 260, kFar + 120, 940, 650));
        QCOMPARE(bounds(folderview::explorerFileList(root, hit.tab)), bounds(folderView));
        // A name being edited there is not the items view.
        QVERIFY(folderview::classify(make(L"Edit", items, {10, 10, 200, 20}), {}).surface == folderview::Surface::None);
    }

    // A ShellTabWindowClass per tab, the ones not on top hidden: the list of
    // the tab typed in, never a hidden one's, even asked for the window.
    void windows11Tabs()
    {
        const HWND root = window({0, 0, 1400, 900});
        HWND hiddenView = nullptr, shownView = nullptr;
        const HWND hiddenItems = tab(root, {0, 100, 1400, 800}, {260, 0, 1140, 770}, false, &hiddenView);
        const HWND items = tab(root, {0, 100, 1400, 800}, {300, 0, 1100, 740}, true, &shownView);
        QVERIFY(root && hiddenItems && items);
        const folderview::Hit hit = folderview::classify(items, {});
        QVERIFY(hit.surface == folderview::Surface::Explorer);
        QVERIFY(hit.tab == tabOf(items) && hit.tab != tabOf(hiddenItems));
        QCOMPARE(bounds(folderview::explorerFileList(root, hit.tab)), bounds(shownView));
        QCOMPARE(bounds(folderview::explorerFileList(root, root)), bounds(shownView));
        QCOMPARE(bounds(folderview::explorerFileList(root, nullptr)), bounds(shownView));
    }

    // A list with more than fits: its scroll bars (each a ScrollBar window in
    // a CtrlNotifySink of the items view) are left out, so that the box put
    // in the list's corner keeps clear of them (GitHub #234); ones hidden
    // while all fits are not.
    void scrollBarsLeftOut()
    {
        const HWND root = window({0, 0, 1200, 800});
        HWND folderView = nullptr;
        const HWND items = tab(root, {0, 120, 1200, 680}, {260, 0, 940, 650}, true, &folderView);
        QVERIFY(root && items);
        const HWND vertical = make(L"ScrollBar", make(L"CtrlNotifySink", items, {923, 0, 17, 633}), {0, 0, 17, 633});
        const HWND horizontal = make(L"ScrollBar", make(L"CtrlNotifySink", items, {0, 633, 923, 17}), {0, 0, 923, 17});
        QVERIFY(vertical && horizontal);
        const HWND hit = folderview::classify(items, {}).tab;
        QCOMPARE(bounds(folderview::explorerFileList(root, hit)), QRect(kFar + 260, kFar + 120, 923, 633));
        ::ShowWindow(::GetParent(horizontal), SW_HIDE); // the columns fit again
        QCOMPARE(bounds(folderview::explorerFileList(root, hit)), QRect(kFar + 260, kFar + 120, 923, 650));
        ::ShowWindow(::GetParent(vertical), SW_HIDE);
        QCOMPARE(bounds(folderview::explorerFileList(root, hit)), bounds(folderView));
    }
    // No folder view (none yet, or a page of another kind): the tab; no tab
    // either, the window's client area; no window, nothing.
    void noFileList()
    {
        const HWND root = window({0, 0, 1000, 700});
        const HWND only = make(L"ShellTabWindowClass", root, {0, 100, 1000, 600});
        QVERIFY(root && only);
        QCOMPARE(bounds(folderview::explorerFileList(root, only)), bounds(only));
        const HWND bare = window({0, 0, 900, 600});
        QVERIFY(bare);
        QCOMPARE(bounds(folderview::explorerFileList(bare, nullptr)), QRect(kFar, kFar, 900, 600));
        QVERIFY(bounds(folderview::explorerFileList(nullptr, nullptr)).isEmpty());
    }
};

QTEST_GUILESS_MAIN(ExplorerViewTest)
#include "tst_explorerview.moc"
