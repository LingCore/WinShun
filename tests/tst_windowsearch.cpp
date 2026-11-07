#include "platform/WindowSearch.h"
#include "Favorites.h"
#include "IndexService.h"
#include "SearchEngine.h"
#include "ResultColumns.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QtTest>
#include <QScopeGuard>
#include <atomic>
#include <algorithm>
#include <thread>

using namespace Qt::StringLiterals;
using namespace ws;

class WindowSearchTest : public QObject {
    Q_OBJECT
private slots:
    void snapshotCost()
    {
        QElapsedTimer timer;
        timer.start();
        qsizetype count = 0;
        for (int i = 0; i < 20; ++i) {
            const auto rows = windowsearch::snapshot();
            QVERIFY(rows.size() <= 256);
            count = rows.size();
        }
        qInfo("Window enumeration: %lld task windows, %.3f ms per scan (20 scans, no titles logged)",
            static_cast<long long>(count), timer.nsecsElapsed() / 20.0 / 1000000.0);
    }
    void matchingAndIdentity()
    {
        SearchResult first;
        first.app = AppKind::Window;
        first.name = u"年度报告 — 编辑器"_s;
        first.target = u"C:\\apps\\editor.exe"_s;
        first.path = u"winshun-window:1/2/3"_s;
        auto second = first;
        second.path = u"winshun-window:1/2/4"_s;
        second.name = u"客户合同 — 编辑器"_s;
        const SearchResults windows {first, second};
        QCOMPARE(windowsearch::matching(windows, u"baogao"_s).size(), 1);
        QCOMPARE(windowsearch::matching(windows, u"EDITOR"_s).size(), 2);
        QCOMPARE(windowsearch::matching(windows, u"editor 合同"_s).first().path, second.path);
        QCOMPARE(windowsearch::matching(windows, u"editor !合同"_s).first().path, first.path);
        for (const QString& query : {u""_s, u"   "_s, u"ext:txt"_s, u"size:>1mb"_s, u"C:\\apps\\editor"_s})
            QVERIFY(windowsearch::matching(windows, query).isEmpty());
        QVERIFY(!favoriteOf(first));
        QVERIFY(first.isApp());
        QVERIFY(!first.isPlace());
        QCOMPARE(resultSource(first), u"已打开"_s);
        QCOMPARE(resultFolder(first), QString());
    }

    void engineScopeAndOrder()
    {
        QTemporaryDir dir;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(dir.filePath(u"index"_s), options);
        {
            const auto index = service.index();
            const auto lock = index->writeLock();
            const auto root = index->addRoot("F:");
            index->add(root, "report.txt", 0);
        }
        SearchEngine engine(&service);
        QSignalSpy received(&engine, &SearchEngine::resultsReady);
        std::atomic<int> calls = 0;
        SearchResult window;
        window.app = AppKind::Window;
        window.path = u"winshun-window:1/2/3"_s;
        window.name = u"report"_s;
        for (int scenario = 0; scenario < 6; ++scenario) {
            received.clear();
            SearchEngine::Request request;
            request.text = u"report"_s;
            request.openWindows = [&] { ++calls; return SearchResults {window}; };
            if (scenario == 1) request.scope = Scope::Files;
            if (scenario == 2) request.text = u"report ext:txt"_s;
            if (scenario == 3) request.order.key = SortKey::Name;
            if (scenario == 4) request.currentFolder = u"F:\\"_s;
            if (scenario == 5) request.text.clear();
            engine.submit(std::move(request));
            QVERIFY(received.wait(5000));
            const auto rows = received.last().at(1).value<SearchResults>();
            QVERIFY2(calls.load() == 1, qPrintable(u"window provider unexpectedly called in scenario %1 (calls %2)"_s.arg(scenario).arg(calls.load())));
            if (scenario == 0) {
                QVERIFY(rows.size() >= 2);
                QVERIFY(rows.first().isWindow());
                QVERIFY(!rows.last().isWindow());
            } else {
                QVERIFY(std::ranges::none_of(rows, [](const auto& row) { return row.isWindow(); }));
            }
        }
    }

    void nativeWindowsOnPrivateDesktop()
    {
        bool finished = false;
        std::thread native([&] {
        // A separate, never displayed desktop: no foreground, clipboard or
        // live user windows are changed by the activation checks.
        HDESK previous = ::GetThreadDesktop(::GetCurrentThreadId());
        const std::wstring name = (u"WinShun-window-test-%1"_s.arg(::GetCurrentProcessId())).toStdWString();
        HDESK desktop = ::CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        QVERIFY(desktop);
        const auto restore = qScopeGuard([&] { ::SetThreadDesktop(previous); ::CloseDesktop(desktop); });
        QVERIFY(::SetThreadDesktop(desktop));
        const HWND window = ::CreateWindowExW(0, L"STATIC", L"window-search-owned", WS_OVERLAPPEDWINDOW,
            10, 10, 320, 200, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(window);
        const auto destroy = qScopeGuard([&] { ::DestroyWindow(window); });
        ::ShowWindow(window, SW_SHOWNOACTIVATE);
        auto rows = windowsearch::snapshot(0);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().windowHandle, reinterpret_cast<quintptr>(window));
        QVERIFY(windowsearch::snapshot().isEmpty()); // self excluded in production
        QVERIFY(windowsearch::current(rows.first()));
        // A desktop which is not the input desktop cannot take foreground.
        // The production result must report that refusal, not claim success.
        QCOMPARE(windowsearch::activate(rows.first()), windowsearch::Activation::Refused);
        ::ShowWindow(window, SW_MINIMIZE);
        QCOMPARE(windowsearch::snapshot(0).size(), 1); // minimized remains searchable
        QCOMPARE(windowsearch::activate(rows.first()), windowsearch::Activation::Refused);
        // ShowWindowAsync posts to the owning thread. A real application has
        // a message loop; the private native fixture pumps that queue here.
        for (int i = 0; i < 100 && ::IsIconic(window); ++i) {
            MSG message;
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            ::Sleep(1);
        }
        QVERIFY(!::IsIconic(window));
        auto stale = rows.first();
        ++stale.windowProcess;
        QCOMPARE(windowsearch::activate(stale), windowsearch::Activation::Gone);
        ::SetWindowTextW(window, L"another document");
        QCOMPARE(windowsearch::activate(rows.first()), windowsearch::Activation::Gone);
        ::ShowWindow(window, SW_HIDE);
        QVERIFY(windowsearch::snapshot(0).isEmpty());
        ::ShowWindow(window, SW_SHOWNOACTIVATE);
        ::SetWindowLongPtrW(window, GWL_EXSTYLE, WS_EX_TOOLWINDOW);
        QVERIFY(windowsearch::snapshot(0).isEmpty());
        finished = true;
        });
        native.join();
        QVERIFY(finished);
    }

    void switchesBetweenOwnedWindows()
    {
        const HWND previous = ::GetForegroundWindow();
        const HWND first = ::CreateWindowExW(0, L"STATIC", L"WinShun switch source", WS_OVERLAPPEDWINDOW,
            24000, 24000, 320, 200, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        const HWND second = ::CreateWindowExW(0, L"STATIC", L"WinShun switch target", WS_OVERLAPPEDWINDOW,
            24400, 24000, 320, 200, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        const auto cleanup = qScopeGuard([&] {
            const bool ownsForeground = ::GetForegroundWindow() == first || ::GetForegroundWindow() == second;
            ::DestroyWindow(second);
            ::DestroyWindow(first);
            if (ownsForeground && ::IsWindow(previous)) ::SetForegroundWindow(previous);
        });
        QVERIFY(first && second);
        // CTest starts the process hidden. ShowWindow's first call can use
        // that inherited startup state instead of SW_SHOWNOACTIVATE.
        const UINT showFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW;
        QVERIFY(::SetWindowPos(first, nullptr, 0, 0, 0, 0, showFlags));
        QVERIFY(::SetWindowPos(second, nullptr, 0, 0, 0, 0, showFlags));
        QVERIFY(::IsWindowVisible(first) && ::IsWindowVisible(second));
        {
            // A background test runner has no initial foreground grant.
            // Share the current input queue only while preparing the fixture,
            // then detach before exercising the production activation path.
            const DWORD foregroundThread = ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
            const DWORD testThread = ::GetCurrentThreadId();
            const bool attach = foregroundThread && foregroundThread != testThread;
            if (attach) QVERIFY(::AttachThreadInput(testThread, foregroundThread, TRUE));
            const auto detach = qScopeGuard([&] {
                if (attach) ::AttachThreadInput(testThread, foregroundThread, FALSE);
            });
            QVERIFY(::SetForegroundWindow(first));
        }
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), first, 1000);
        const auto windows = windowsearch::snapshot(0);
        const auto target = std::ranges::find_if(windows, [&](const auto& row) {
            return row.windowHandle == reinterpret_cast<quintptr>(second);
        });
        QVERIFY(target != windows.end());
        QCOMPARE(windowsearch::activate(*target), windowsearch::Activation::Switched);
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), second, 1000);
        QVERIFY(::SetForegroundWindow(first));
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), first, 1000);
        ::ShowWindow(second, SW_MINIMIZE);
        QCOMPARE(windowsearch::activate(*target), windowsearch::Activation::Switched);
        QTRY_VERIFY_WITH_TIMEOUT(!::IsIconic(second), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), second, 1000);
        if (::IsWindow(previous)) {
            QVERIFY(::SetForegroundWindow(previous));
            QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), previous, 1000);
        }
    }
};

QTEST_GUILESS_MAIN(WindowSearchTest)
#include "tst_windowsearch.moc"
