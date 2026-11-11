#include "CommandLaunch.h"
#include "platform/Integrity.h"
#include "platform/FileManagers.h"
#include "platform/OneCommanderTabs.h"
#include "platform/FolderViewHit.h"
#include "platform/Shell.h"
#include "platform/FileDialog.h"
#include "platform/TessoaQuery.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QProcess>
#include <QTimer>
#include <QUuid>

#include <objbase.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <future>

using namespace Qt::StringLiterals;

namespace {
int tessoaFixture(const QString& ready, const QString& folderFile)
{
    const HWND window = ::CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Tessoa fixture", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, nullptr, nullptr);
    if (!window) return 1;
    const auto destroy = qScopeGuard([&] { ::DestroyWindow(window); });
    const std::wstring name = L"\\\\.\\pipe\\tessoa.q." + std::to_wstring(::GetCurrentProcessId());
    ws::win32::UniqueHandle pipe(::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr));
    if (!pipe.valid()) return 2;
    QFile signal(ready);
    if (!signal.open(QIODevice::WriteOnly)) return 3;
    signal.write(QByteArray::number(reinterpret_cast<quintptr>(window)));
    signal.close();
    for (int i = 0; i < 2; ++i) {
        if (!::ConnectNamedPipe(pipe.get(), nullptr) && ::GetLastError() != ERROR_PIPE_CONNECTED) return 4;
        quint32 length = 0;
        DWORD done = 0;
        if (!::ReadFile(pipe.get(), &length, 4, &done, nullptr) || done != 4 || length > 256) return 5;
        QByteArray request(length, Qt::Uninitialized);
        if (!::ReadFile(pipe.get(), request.data(), length, &done, nullptr) || done != length) return 6;
        if (request != QByteArray("v 1\nop query-cwd\nhwnd ") + QByteArray::number(reinterpret_cast<quintptr>(window)) + '\n') return 7;
        QFile content(folderFile);
        if (!content.open(QIODevice::ReadOnly)) return 8;
        const QByteArray answer = content.readAll();
        length = static_cast<quint32>(answer.size());
        if (!::WriteFile(pipe.get(), &length, 4, &done, nullptr)) return 9;
        if (length && !::WriteFile(pipe.get(), answer.data(), length, &done, nullptr)) return 10;
        ::FlushFileBuffers(pipe.get());
        ::DisconnectNamedPipe(pipe.get());
    }
    return 0;
}

int pickerProbe()
{
    QTemporaryDir fixture;
    if (!fixture.isValid())
        return 1;
    const QString target = fixture.filePath(u"选择文件夹"_s);
    if (!QDir().mkpath(target))
        return 2;
    const QString native = QDir::toNativeSeparators(target);
    const QString title = u"WinShun picker probe"_s;
    const DWORD guiThread = ::GetCurrentThreadId();
    int ticks = 0;
    bool navigated = false, separate = false;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        ++ticks;
        struct Find { const QString& title; HWND window = nullptr; } found {title};
        ::EnumWindows([](HWND window, LPARAM data) -> BOOL {
            auto& found = *reinterpret_cast<Find*>(data);
            DWORD process = 0;
            ::GetWindowThreadProcessId(window, &process);
            wchar_t text[128] {};
            ::GetWindowTextW(window, text, 128);
            if (process == ::GetCurrentProcessId() && found.title == QString::fromWCharArray(text)
                && ::IsWindowVisible(window)) {
                found.window = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&found));
        if (!found.window || ws::filedialog::kind(found.window) == ws::filedialog::Kind::None)
            return;
        separate = ::GetWindowThreadProcessId(found.window, nullptr) != guiThread;
        if (!navigated) {
            navigated = ws::filedialog::goTo(found.window, native.toStdWString());
            return;
        }
        if (ticks >= 4 && ws::filedialog::currentFolder(found.window) == native.toStdWString())
            ::PostMessageW(found.window, WM_CLOSE, 0, 0);
    });
    timer.start(50);
    const QString selected = ws::shell::pickFolder(nullptr, title);
    return selected.isEmpty() && separate && navigated && ticks >= 4 ? 0 : 3;
}

QString parentProgram()
{
    const ws::win32::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry {sizeof entry};
    DWORD parent = 0;
    if (::Process32FirstW(snapshot.get(), &entry)) {
        do {
            if (entry.th32ProcessID == ::GetCurrentProcessId()) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (::Process32NextW(snapshot.get(), &entry));
    }
    const ws::win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent));
    wchar_t path[32768] {};
    DWORD size = static_cast<DWORD>(std::size(path));
    return process.valid() && ::QueryFullProcessImageNameW(process.get(), 0, path, &size)
        ? QString::fromWCharArray(path, size) : QString();
}
} // namespace

class TestShellLaunch : public QObject {
    Q_OBJECT

    // A copy of this test as `exe` in `folder`, with its Qt DLLs beside it:
    // Explorer and the desktop shell's token have their own PATH, not
    // CTest's Qt PATH. `console`: its PE subsystem word made the console's.
    static void stage(const QTemporaryDir& folder, const QString& exe, bool console)
    {
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), exe));
        if (console) {
            QFile file(exe);
            QVERIFY(file.open(QIODevice::ReadWrite) && file.seek(0x3c));
            quint32 header = 0;
            QCOMPARE(file.read(reinterpret_cast<char*>(&header), 4), 4);
            const quint16 subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI; // after "PE\0\0" and the file header, 68 bytes in
            QVERIFY(file.seek(header + 4 + 20 + 68));
            QCOMPARE(file.write(reinterpret_cast<const char*>(&subsystem), 2), 2);
        }
        QCOMPARE(ws::commandlaunch::kindOf(exe), console ? ws::commandlaunch::Kind::Console : ws::commandlaunch::Kind::Gui);
        const QDir qtBin(QLibraryInfo::path(QLibraryInfo::BinariesPath));
        for (const auto* module : {"Core", "Gui", "Sql", "Test"}) {
            const QString dll = u"Qt6"_s + QString::fromLatin1(module)
                + (QLibraryInfo::isDebugBuild() ? u"d.dll"_s : u".dll"_s);
            if (!QFile::exists(folder.filePath(dll)))
                QVERIFY(QFile::copy(qtBin.filePath(dll), folder.filePath(dll)));
        }
    }

private slots:
    void tessoaProductionDiscovery()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const QString executable = fixture.filePath(u"tessoa.exe"_s);
        stage(fixture, executable, false);
        const QString ready = fixture.filePath(u"ready"_s);
        const QString path = fixture.filePath(u"path"_s);
        const QString folder = QDir::toNativeSeparators(fixture.filePath(u"中文 空格"_s));
        QVERIFY(QDir().mkpath(folder));
        QFile state(path);
        QVERIFY(state.open(QIODevice::WriteOnly));
        state.write(folder.toUtf8());
        state.close();
        QProcess child;
        const auto stop = qScopeGuard([&] { if (child.state() != QProcess::NotRunning) { child.kill(); child.waitForFinished(); } });
        child.start(executable, {u"--tessoa-fixture"_s, ready, path});
        QVERIFY(child.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(ready).size() > 0, 5000);
        QFile handshake(ready);
        QVERIFY(handshake.open(QIODevice::ReadOnly));
        const HWND window = reinterpret_cast<HWND>(handshake.readAll().toULongLong());
        QVERIFY(ws::filemanager::kindOf(window) == ws::filemanager::Kind::Tessoa);
        QCOMPARE(QString::fromStdWString(ws::filemanager::frontFolder(window)), folder);
        QVERIFY(state.open(QIODevice::WriteOnly | QIODevice::Truncate));
        state.close(); // Tessoa's next active tab is a virtual page.
        QVERIFY(ws::filemanager::frontFolder(window).empty());
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitCode(), 0);
    }

    void tessoaProtocol_data()
    {
        QTest::addColumn<QByteArray>("answer");
        QTest::addColumn<int>("mode"); // 1: partial header, 2: stall, 3: oversized
        QTest::addColumn<QString>("expected");
        QTest::newRow("unicode-fragmented") << QStringLiteral("C:\\项目 资料\\2026").toUtf8() << 0 << QStringLiteral("C:\\项目 资料\\2026");
        QTest::newRow("unc") << QByteArray("\\\\nas\\team") << 0 << QStringLiteral("\\\\nas\\team");
        QTest::newRow("virtual-page") << QByteArray() << 0 << QString();
        QTest::newRow("relative") << QByteArray("previous-folder") << 0 << QString();
        QTest::newRow("invalid-utf8") << QByteArray("C:\\\xff", 4) << 0 << QString();
        QTest::newRow("incomplete-utf8") << QByteArray("C:\\\xc3", 4) << 0 << QString();
        QTest::newRow("embedded-nul") << QByteArray("C:\\good\0bad", 11) << 0 << QString();
        QTest::newRow("partial-header") << QByteArray() << 1 << QString();
        QTest::newRow("stalled-server") << QByteArray() << 2 << QString();
        QTest::newRow("oversized") << QByteArray() << 3 << QString();
    }

    void tessoaProtocol()
    {
        QFETCH(QByteArray, answer);
        QFETCH(int, mode);
        QFETCH(QString, expected);
        const HWND window = ::CreateWindowExW(0, L"STATIC", L"WinShun Tessoa protocol fixture", 0,
            0, 0, 10, 10, HWND_MESSAGE, nullptr, nullptr, nullptr);
        QVERIFY(window);
        const auto destroy = qScopeGuard([&] { ::DestroyWindow(window); });
        const std::wstring name = L"\\\\.\\pipe\\tessoa.q." + std::to_wstring(::GetCurrentProcessId());
        ws::win32::UniqueHandle server(::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr));
        QVERIFY(server.valid());
        auto worker = std::async(std::launch::async, [&] {
            QByteArray received;
            if (!::ConnectNamedPipe(server.get(), nullptr) && ::GetLastError() != ERROR_PIPE_CONNECTED) return received;
            DWORD got = 0;
            quint32 length = 0;
            if (!::ReadFile(server.get(), &length, 4, &got, nullptr) || got != 4 || length > 256) return received;
            received.resize(length);
            if (!::ReadFile(server.get(), received.data(), length, &got, nullptr) || got != length) return QByteArray();
            if (mode == 2) { ::Sleep(750); return received; }
            length = mode == 3 ? (1u << 20) + 1 : static_cast<quint32>(answer.size());
            QByteArray packet(reinterpret_cast<const char*>(&length), mode == 1 ? 2 : 4);
            if (mode == 0) packet += answer;
            for (const char byte : packet) {
                if (!::WriteFile(server.get(), &byte, 1, &got, nullptr)) break;
            }
            // Let the client consume the complete response before closing.
            if (mode == 1) ::DisconnectNamedPipe(server.get());
            return received;
        });
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(ws::tessoa::currentFolder(window), expected);
        QVERIFY2(clock.elapsed() < 1500, "Tessoa must not block the folder worker indefinitely");
        QCOMPARE(worker.get(), QByteArray("v 1\nop query-cwd\nhwnd ")
            + QByteArray::number(reinterpret_cast<quintptr>(window)) + '\n');
    }

    void tessoaAbsentAndLookalike()
    {
        const HWND window = ::CreateWindowExW(0, L"STATIC", L"Tessoa", 0,
            0, 0, 10, 10, nullptr, nullptr, nullptr, nullptr);
        QVERIFY(window);
        const auto destroy = qScopeGuard([&] { ::DestroyWindow(window); });
        QVERIFY(!ws::filemanager::kindOf(window)); // title alone never identifies the executable
        QVERIFY(ws::tessoa::currentFolder(window).isEmpty());
        QVERIFY(ws::tessoa::currentFolder(nullptr).isEmpty());
    }

    void nativePickerNavigation()
    {
        std::wstring name = (u"WinShun-picker-"_s + QUuid::createUuid().toString(QUuid::Id128)).toStdWString();
        const HDESK desktop = ::CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        QVERIFY(desktop);
        const auto close = qScopeGuard([&] { ::CloseDesktop(desktop); });
        QProcess child;
        child.setCreateProcessArgumentsModifier([&](QProcess::CreateProcessArguments* args) {
            args->startupInfo->lpDesktop = name.data();
            args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            args->startupInfo->wShowWindow = SW_HIDE;
        });
        child.start(QCoreApplication::applicationFilePath(), {u"--picker-probe"_s});
        QVERIFY(child.waitForStarted());
        if (!child.waitForFinished(20000)) {
            child.kill(); // only this test's child, on its isolated desktop
            child.waitForFinished();
            QFAIL("Native picker did not navigate and cancel within 20 seconds");
        }
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
    }

    void oneCommanderLookalike()
    {
        // WPF's window class is shared by unrelated apps. Even a matching
        // assembly name and title must not be enough to identify a manager.
        constexpr const wchar_t* name = L"HwndWrapper[OneCommander.exe;;WinShunTest]";
        WNDCLASSW wc {};
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = name;
        QVERIFY(::RegisterClassW(&wc));
        auto unregister = qScopeGuard([&] { ::UnregisterClassW(name, wc.hInstance); });
        const HWND window = ::CreateWindowExW(
            0, name, L"OneCommander", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        QVERIFY(window);
        auto destroy = qScopeGuard([&] { ::DestroyWindow(window); });
        QVERIFY(!ws::filemanager::kindOf(window));
        QVERIFY(!ws::filemanager::kindOf(nullptr));
    }

    void oneCommanderNavigation_data()
    {
        QTest::addColumn<int>("model");
        QTest::newRow("production-STA") << int(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        QTest::newRow("MTA") << int(COINIT_MULTITHREADED);
    }

    void oneCommanderSessionTabs()
    {
        using ws::onecommander::TabState;
        using ws::onecommander::sessionTabs;
        const auto json = [](const QStringList& paths) {
            QJsonArray tabs;
            for (const QString& path : paths)
                tabs.append(QJsonObject {{u"FullPath"_s, path}, {u"IsFolder"_s, true}});
            return QJsonDocument(tabs).toJson();
        };
        const QString a = u"C:\\工作 目录\\A"_s;
        const QString b = u"C:\\工作 目录\\John's B"_s;
        const QString c = u"\\\\server\\share\\资料"_s;
        const QString d = u"D:\\Other"_s;
        const std::array<QByteArray, 2> panes {json({a, b}), json({c, d})};
        std::array<TabState, 2> state {{{2, 1}, {2, 0}}};
        const auto tabs = sessionTabs(panes, state, b);
        QVERIFY(tabs);
        QCOMPARE(tabs->size(), 4);
        QCOMPARE(tabs->at(0).path, a);
        QCOMPARE(tabs->at(0).state, 0);
        QCOMPARE(tabs->at(1).path, b);
        QCOMPARE(tabs->at(1).state, 1);
        QCOMPARE(tabs->at(2).state, 2);
        QCOMPARE(tabs->at(3).state, 0);
        state[1].visible = false;
        QCOMPARE(sessionTabs(panes, state, b)->size(), 2);
        state[0].count = 1;
        QVERIFY(!sessionTabs(panes, state, b)); // saved data predates a tab closing
        state[0] = {2, 0};
        QVERIFY(!sessionTabs(panes, state, b)); // selection no longer agrees
        state[0] = {2, 1};
        QVERIFY(!sessionTabs({QByteArray("{"), panes[1]}, state, b));
        QVERIFY(!sessionTabs(panes, state, u"C:\\elsewhere"_s));
        QVERIFY(!sessionTabs(panes, std::span<const TabState>(), b));
        QCOMPARE(sessionTabs({panes[0], {}}, state, b)->size(), 2); // untouched second pane
        const QByteArray virtualTabs = R"([{"FullPath":"::{ThisPC}","IsFolder":true},{"FullPath":"C:\\Work","IsFolder":false}])";
        QVERIFY(!sessionTabs({virtualTabs, {}}, state, b));
    }

    void oneCommanderOpenTabs()
    {
        const QString root = qEnvironmentVariable("WINSHUN_ONECOMMANDER_TABS_ROOT");
        if (root.isEmpty())
            QSKIP("Set WINSHUN_ONECOMMANDER_TABS_ROOT to an isolated prepared tab fixture");
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        auto uninitialize = qScopeGuard([] { ::CoUninitialize(); });
        const DWORD clipboard = ::GetClipboardSequenceNumber();
        const HWND foreground = ::GetForegroundWindow();
        QStringList paths;
        for (const auto& folder : ws::filemanager::openFolders()) {
            if (folder.shownIn == ws::filemanager::Kind::OneCommander)
                paths.append(QDir::fromNativeSeparators(QString::fromStdWString(folder.path)));
        }
        const QDir fixtures(root);
        QVERIFY2(paths.contains(fixtures.filePath(u"Hidden-B"_s)), qPrintable(paths.join(u'\n')));
        QVERIFY2(paths.contains(fixtures.filePath(u"Current-A"_s)), qPrintable(paths.join(u'\n')));
        QVERIFY(!paths.contains(fixtures.filePath(u"Other-Pane-C"_s))); // closed, but still in RecentFolders
        QCOMPARE(paths.count(fixtures.filePath(u"Hidden-B"_s)), 1);
        QCOMPARE(::GetClipboardSequenceNumber(), clipboard);
        QCOMPARE(::GetForegroundWindow(), foreground);
    }

    // Opt-in: the HWND must belong to an isolated OneCommander copy. Uses
    // the same live active-pane query as Ctrl+G and the dialog suggestions.
    void oneCommanderNavigation()
    {
        bool valid = false;
        const qulonglong handle = qEnvironmentVariable("WINSHUN_ONECOMMANDER_TEST_WINDOW").toULongLong(&valid);
        if (!valid || !handle)
            QSKIP("Set WINSHUN_ONECOMMANDER_TEST_WINDOW to an isolated OneCommander main window");
        const HWND window = reinterpret_cast<HWND>(static_cast<quintptr>(handle));
        QVERIFY(ws::filemanager::kindOf(window) == ws::filemanager::Kind::OneCommander);
        QFETCH(int, model);
        const HRESULT com = ::CoInitializeEx(nullptr, static_cast<DWORD>(model));
        QVERIFY(SUCCEEDED(com));
        auto uninitialize = qScopeGuard([] { ::CoUninitialize(); });
        const std::wstring original = ws::filemanager::frontFolder(window);
        QVERIFY(!original.empty());
        QTemporaryDir temporary(QDir::tempPath() + u"/WinShun OneCommander-XXXXXX"_s);
        QVERIFY(temporary.isValid());
        const QString child = temporary.filePath(u"中文 空格 John's 文件夹"_s);
        QVERIFY(QDir().mkpath(child));
        const auto native = [](const QString& path) { return QDir::toNativeSeparators(path).toStdWString(); };
        const auto shows = [&](const std::wstring& path) {
            return QDir::fromNativeSeparators(QString::fromStdWString(ws::filemanager::frontFolder(window)))
                == QDir::fromNativeSeparators(QString::fromStdWString(path));
        };
        auto restore
            = qScopeGuard([&] { ws::filemanager::goToInFront(ws::filemanager::Kind::OneCommander, original); });
        QVERIFY(ws::filemanager::installed(ws::filemanager::Kind::OneCommander));
        QVERIFY(ws::filemanager::goToInFront(ws::filemanager::Kind::OneCommander, native(child)));
        QTRY_VERIFY_WITH_TIMEOUT(shows(native(child)), 5000);
        const DWORD clipboard = ::GetClipboardSequenceNumber();
        const auto folders = ws::filemanager::openFolders();
        QCOMPARE(::GetClipboardSequenceNumber(), clipboard);
        const auto found = std::ranges::find_if(folders, [&](const ws::filemanager::Folder& folder) {
            return folder.shownIn == ws::filemanager::Kind::OneCommander && folder.path == native(child);
        });
        QVERIFY(found != folders.end());
        QCOMPARE(folders.front().path, native(child));
        QCOMPARE(std::ranges::count_if(
                     folders, [&](const ws::filemanager::Folder& folder) { return folder.path == found->path; }),
            1);

        QFile file(temporary.filePath(u"选中 中文.txt"_s));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(ws::filemanager::reveal(ws::filemanager::Kind::OneCommander, {native(file.fileName())}));
        QTRY_VERIFY_WITH_TIMEOUT(shows(native(temporary.path())), 5000);
        QVERIFY(ws::filemanager::reveal(ws::filemanager::Kind::OneCommander, {native(child)}));
        QTRY_VERIFY_WITH_TIMEOUT(shows(native(temporary.path())), 5000);
        QFile nested(child + u"/另一个.txt"_s);
        QVERIFY(nested.open(QIODevice::WriteOnly));
        nested.close();
        QVERIFY(ws::filemanager::reveal(
            ws::filemanager::Kind::OneCommander, {native(file.fileName()), native(nested.fileName())}));
        QTRY_VERIFY_WITH_TIMEOUT(shows(native(child)), 5000);
        QVERIFY(ws::filemanager::openFolder(ws::filemanager::Kind::OneCommander, native(child)));
        QTRY_VERIFY_WITH_TIMEOUT(shows(native(child)), 5000);
        QVERIFY(ws::filemanager::goToInFront(ws::filemanager::Kind::OneCommander, L"C:\\"));
        QTRY_VERIFY_WITH_TIMEOUT(shows(L"C:\\"), 5000);
        QVERIFY(ws::filemanager::goToInFront(ws::filemanager::Kind::OneCommander, original));
        QTRY_VERIFY_WITH_TIMEOUT(shows(original), 5000);
        restore.dismiss();
    }

    // Opt-in live check only: the operator must focus an Opus file display
    // already showing this isolated fixture folder. Never moves a user folder
    // or changes Opus's preferences; restores the original location on exit.
    void opusTypingNavigation()
    {
        const QString source = qEnvironmentVariable("WINSHUN_OPUS_TEST_FOLDER");
        if (source.isEmpty())
            QSKIP("Set WINSHUN_OPUS_TEST_FOLDER to an isolated folder and focus its Opus file display");
        const HWND root = ::GetForegroundWindow();
        GUITHREADINFO focus {sizeof focus};
        QVERIFY(::GetGUIThreadInfo(::GetWindowThreadProcessId(root, nullptr), &focus));
        const HWND view = focus.hwndFocus;
        QVERIFY(ws::folderview::isOpusFileDisplay(root, view));
        const QString original = QString::fromStdWString(ws::filemanager::opusFolder(view));
        QCOMPARE(QDir::fromNativeSeparators(original), QDir::fromNativeSeparators(source));
        QTemporaryDir temporary(QDir(source).filePath(u"WinShun-opus-test-XXXXXX"_s));
        QVERIFY(temporary.isValid());
        const QString child = temporary.filePath(u"child"_s);
        QVERIFY(QDir().mkpath(child));
        QFile fixture(temporary.filePath(u"probe.txt"_s));
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("Opus navigation fixture");
        fixture.close();
        const auto go = [&](const QString& folder, const QString& select = {}) {
            return ws::filemanager::goToInOpusView(root, view, QDir::toNativeSeparators(folder).toStdWString(),
                QDir::toNativeSeparators(select).toStdWString());
        };
        auto restore = qScopeGuard([&] { go(original); });
        QVERIFY(go(child));
        QCOMPARE(QDir::fromNativeSeparators(QString::fromStdWString(ws::filemanager::opusFolder(view))), child);
        QVERIFY(go(temporary.path(), fixture.fileName()));
        QCOMPARE(
            QDir::fromNativeSeparators(QString::fromStdWString(ws::filemanager::opusFolder(view))), temporary.path());
        QVERIFY(go(original));
        restore.dismiss();
    }

    // A program starts through Explorer, a console program not: one whose
    // parent is explorer.exe takes it for a double-click in a folder window,
    // and Go tools built with cobra then quit (GitHub #139). The console one
    // is this probe with its PE subsystem word made the console's.
    void customCommand_data()
    {
        QTest::addColumn<bool>("console");
        QTest::newRow("program, via Explorer") << false;
        QTest::newRow("console program, not via Explorer") << true;
    }

    void customCommand()
    {
        QFETCH(bool, console);
        if (!::GetShellWindow())
            QSKIP("No desktop shell in this test session");
        QTemporaryDir temporary(QDir::tempPath() + u"/WinShun launch-XXXXXX"_s);
        QVERIFY(temporary.isValid());
        const QString helper = temporary.filePath(u"launch-probe.exe"_s);
        stage(temporary, helper, console);
        if (QTest::currentTestFailed())
            return;
        const QString working = temporary.filePath(u"工作 folder"_s);
        QVERIFY(QDir().mkpath(working));
        const QString output = temporary.filePath(u"result.json"_s);
        const QString query = u"中文 \"引号\" C:\\末尾\\ {query} %TEMP% & text"_s;
        const QString command = u'"' + helper
            + u"\" --probe \""_s + output + u"\" {query}"_s;
        const auto done = std::make_shared<std::atomic<int>>(-1);
        ws::shell::runCustom(command, working, query, [done](bool ok) { done->store(ok ? 1 : 0); });
        QTRY_VERIFY_WITH_TIMEOUT(done->load() >= 0, 10000);
        QCOMPARE(done->load(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output), 10000);
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject result = QJsonDocument::fromJson(file.readAll()).object();
        // Through rundll32, which has often quit already: no parent to name.
        QVERIFY2(result.value(u"parent"_s).toString().endsWith(u"\\explorer.exe", Qt::CaseInsensitive) == !console,
            qPrintable(result.value(u"parent"_s).toString()));
        QVERIFY(!result.value(u"elevated"_s).toBool());
        QCOMPARE(QDir::cleanPath(result.value(u"cwd"_s).toString()), QDir::cleanPath(working));
        QCOMPARE(result.value(u"arguments"_s).toArray().last().toString(), query);
        qInfo() << "Launched by" << result.value(u"parent"_s).toString() << "with the requested arguments and folder";
    }

    // Elevated, with Secondary Logon off (WINSHUN_TEST_NO_SECLOGON), a silent
    // custom command is created with the desktop shell as the parent: a
    // console program must still not see explorer.exe there (#139), and
    // gets no console. The staged runner (this test) asks for it, and is the
    // helper in between (hiddenHelper), as WinShun.exe is.
    void silentConsoleWithoutSecondaryLogon()
    {
        if (!ws::integrity::processElevated())
            QSKIP("Only an elevated Win顺 starts with the desktop shell's token: run this test elevated.");
        if (!::GetShellWindow())
            QSKIP("No desktop shell in this test session");
        QTemporaryDir temporary(QDir::tempPath() + u"/WinShun launch-XXXXXX"_s);
        QVERIFY(temporary.isValid());
        const QString runner = temporary.filePath(u"runner.exe"_s);
        const QString probe = temporary.filePath(u"launch-probe.exe"_s);
        stage(temporary, runner, false);
        stage(temporary, probe, true);
        if (QTest::currentTestFailed())
            return;
        const QString output = temporary.filePath(u"result.json"_s);
        QProcess child;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"WINSHUN_TEST_NO_SECLOGON"_s, u"1"_s);
        child.setProcessEnvironment(environment);
        const QString command = u'"' + probe + u"\" --probe \""_s + output + u"\" silent"_s;
        child.start(runner, {u"--run-silent"_s, command, temporary.path()});
        QVERIFY(child.waitForFinished(15000));
        QCOMPARE(child.exitCode(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output), 10000);
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject result = QJsonDocument::fromJson(file.readAll()).object();
        // The helper has often quit already: no parent to name, but never Explorer.
        const QString parent = result.value(u"parent"_s).toString();
        QVERIFY2(!parent.endsWith(u"\\explorer.exe"_s, Qt::CaseInsensitive), qPrintable(parent));
        QVERIFY(!result.value(u"elevated"_s).toBool());
        QVERIFY(!result.value(u"console"_s).toBool()); // CREATE_NO_WINDOW: none made
        QCOMPARE(QDir::cleanPath(result.value(u"cwd"_s).toString()), QDir::cleanPath(temporary.path()));
        qInfo() << "Launched by" << parent;
    }

    // The application catalog supplies an AppsFolder ID, not an .exe path.
    // Exercise the same executeUnelevated path with Windows' own console app,
    // passing only a command that writes to our temporary directory and exits.
    void appsFolderConsole_data()
    {
        QTest::addColumn<bool>("explicitDirectory");
        QTest::newRow("explicit working directory") << true;
        QTest::newRow("default target directory") << false;
    }

    void appsFolderConsole()
    {
        QFETCH(bool, explicitDirectory);
        if (!::GetShellWindow())
            QSKIP("No desktop shell in this test session");
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString output = temporary.filePath(u"app-console.json"_s);
        QString quotedOutput = QDir::toNativeSeparators(output);
        quotedOutput.replace(u'\'', u"''"_s);
        const QString script = u"$p=(Get-CimInstance Win32_Process -Filter ('ProcessId='+$PID)).ParentProcessId; "_s
            + u"$v=@{parent_pid=$p; parent=(Get-Process -Id $p -ErrorAction SilentlyContinue).ProcessName; cwd=(Get-Location).Path}; "_s
            + u"[IO.File]::WriteAllText('"_s + quotedOutput
            + u"',($v|ConvertTo-Json -Compress),[Text.UTF8Encoding]::new($false))"_s;
        const QString command = u"\"shell:AppsFolder\\{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe\" "_s
            + u"-NoProfile -NonInteractive -Command \""_s + script + u'"';
        const auto done = std::make_shared<std::atomic<int>>(-1);
        ws::shell::runCustom(command, explicitDirectory ? temporary.path() : QString(), {},
            [done](bool ok) { done->store(ok ? 1 : 0); });
        QTRY_COMPARE_WITH_TIMEOUT(done->load(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output), 15000);
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonParseError error;
        const QJsonObject result = QJsonDocument::fromJson(file.readAll(), &error).object();
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(result.value(u"parent_pid"_s).toInteger() > 0);
        QVERIFY(result.value(u"parent"_s).toString().compare(u"explorer"_s, Qt::CaseInsensitive) != 0);
        wchar_t system[MAX_PATH];
        const UINT length = ::GetSystemDirectoryW(system, MAX_PATH);
        QVERIFY(length > 0 && length < MAX_PATH);
        const QString expected = explicitDirectory ? temporary.path()
            : QString::fromWCharArray(system, static_cast<qsizetype>(length)) + u"/WindowsPowerShell/v1.0"_s;
        QCOMPARE(QDir::cleanPath(result.value(u"cwd"_s).toString()).toLower(), QDir::cleanPath(expected).toLower());
    }

    // A console program run for what it writes (the Skill is installed so): both
    // streams, the exit code, no window of its own.
    void capturesWhatAProgramWrites()
    {
        const std::wstring cmd = qEnvironmentVariable("ComSpec").toStdWString();
        QVERIFY(!cmd.empty());
        const auto run = ws::shell::runUnelevatedCaptured(cmd, L"/d /c \"echo out& echo err 1>&2& exit /b 3\"", 15000);
        QVERIFY(run.started);
        QVERIFY(!run.timedOut);
        QCOMPARE(run.exitCode, 3ul);
        const QString text = QString::fromLatin1(run.output.data(), static_cast<qsizetype>(run.output.size()));
        QVERIFY2(text.contains(u"out"_s) && text.contains(u"err"_s), qPrintable(text));
        // Nothing on standard input to wait for: it ends at once.
        const auto reads = ws::shell::runUnelevatedCaptured(cmd, L"/d /c \"set /p x=& echo done\"", 15000);
        QVERIFY(reads.started && !reads.timedOut);
        QVERIFY(QString::fromLatin1(reads.output.data(), static_cast<qsizetype>(reads.output.size())).contains(u"done"_s));
        // A program that does not exist does not start.
        QVERIFY(!ws::shell::runUnelevatedCaptured(L"C:\\wstest\\nothing\\not-there.exe", {}, 1000).started);
    }

    // What did not start says which step failed and Windows' error (GitHub
    // #172: the drop pie's helper failed with nothing in the log).
    void startUnelevatedSaysWhyNot()
    {
        const QTemporaryDir folder;
        const std::wstring missing = QDir::toNativeSeparators(folder.filePath(u"not-there.exe"_s)).toStdWString();
        ws::shell::StartFailure failure;
        QVERIFY(!ws::shell::startUnelevated(missing, {}, {}, &failure).valid());
        QVERIFY(failure.step);
        QCOMPARE(QByteArray(failure.step),
            ws::integrity::processElevated() ? QByteArray("CreateProcessWithTokenW") : QByteArray("CreateProcessW"));
        QCOMPARE(failure.error, static_cast<unsigned long>(ERROR_FILE_NOT_FOUND));
        failure = {};
        QVERIFY(!ws::shell::startUnelevated(L"C:\\wstest\\a\"quote.exe", {}, {}, &failure).valid());
        QCOMPARE(QByteArray(failure.step), QByteArray("quote in the path"));
        // One that starts: with the user's rights, never elevated.
        const std::wstring cmd = qEnvironmentVariable("ComSpec").toStdWString();
        failure = {};
        const ws::win32::UniqueHandle started = ws::shell::startUnelevated(cmd, L"/d /c exit 7", {}, &failure);
        QVERIFY2(started.valid(), failure.step);
        QCOMPARE(::WaitForSingleObject(started.get(), 15000), DWORD(WAIT_OBJECT_0));
        DWORD code = 0;
        QVERIFY(::GetExitCodeProcess(started.get(), &code));
        QCOMPARE(code, 7ul);
    }

    // A program of ours with "以管理员身份运行此程序" ticked in its properties
    // (error 740 for every start with normal rights) starts all the same, as
    // the invoker; someone else's program so set is left to say 740 (GitHub
    // #111: the drop pie's helper of such a copy never started).
    void startsOurOwnProgramSetToRunAsAdministrator()
    {
        const QString comSpec = qEnvironmentVariable("ComSpec");
        const QString ours = QDir(QCoreApplication::applicationDirPath())
            .filePath(u"ws-invoker-%1.exe"_s.arg(QUuid::createUuid().toString(QUuid::Id128).left(8)));
        const QTemporaryDir elsewhere;
        const QString theirs = elsewhere.filePath(u"theirs.exe"_s);
        QVERIFY(QFile::copy(comSpec, ours));
        QVERIFY(QFile::copy(comSpec, theirs));
        const wchar_t* const layers = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
        HKEY key = nullptr;
        QCOMPARE(::RegCreateKeyExW(HKEY_CURRENT_USER, layers, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr),
            LSTATUS(ERROR_SUCCESS));
        const std::wstring ourPath = QDir::toNativeSeparators(ours).toStdWString();
        const std::wstring theirPath = QDir::toNativeSeparators(theirs).toStdWString();
        const auto cleanup = qScopeGuard([&] {
            ::RegDeleteValueW(key, ourPath.c_str());
            ::RegDeleteValueW(key, theirPath.c_str());
            ::RegCloseKey(key);
            QFile::remove(ours);
        });
        const wchar_t flag[] = L"~ RUNASADMIN";
        for (const std::wstring& path : {ourPath, theirPath})
            QCOMPARE(::RegSetValueExW(key, path.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(flag), sizeof flag),
                LSTATUS(ERROR_SUCCESS));

        // 7: started with the marker dropInvokerLayer looks for (the layer itself Windows
        // hands on with the flag after it, "RunAsInvoker RunAsAdmin"), and not with an
        // administrator's rights (S-1-16-12288, the high integrity level), also when this test has them.
        const std::wstring says = L"/d /c if %WINSHUN_AS_INVOKER%==1 (whoami /groups | find \"S-1-16-12288\" >nul || exit 7)";
        ws::shell::StartFailure failure;
        const ws::win32::UniqueHandle started = ws::shell::startUnelevated(ourPath, says, {}, &failure);
        QVERIFY2(started.valid(), qPrintable(u"%1, error %2"_s.arg(QLatin1StringView(failure.step)).arg(failure.error)));
        QCOMPARE(::WaitForSingleObject(started.get(), 15000), DWORD(WAIT_OBJECT_0));
        DWORD code = 0;
        QVERIFY(::GetExitCodeProcess(started.get(), &code));
        QCOMPARE(code, 7ul);

        failure = {};
        QVERIFY(!ws::shell::startUnelevated(theirPath, says, {}, &failure).valid());
        QCOMPARE(failure.error, static_cast<unsigned long>(ERROR_ELEVATION_REQUIRED));

        // What the program then does first: both gone, so that what it starts can ask for rights.
        qputenv("WINSHUN_AS_INVOKER", "1");
        qputenv("__COMPAT_LAYER", "RunAsInvoker");
        ws::shell::dropInvokerLayer();
        QVERIFY(!qEnvironmentVariableIsSet("WINSHUN_AS_INVOKER"));
        QVERIFY(!qEnvironmentVariableIsSet("__COMPAT_LAYER"));
        // Not ours to take out: a layer the user set themselves.
        qputenv("__COMPAT_LAYER", "RunAsInvoker");
        ws::shell::dropInvokerLayer();
        QCOMPARE(qEnvironmentVariable("__COMPAT_LAYER"), u"RunAsInvoker"_s);
        qunsetenv("__COMPAT_LAYER");
    }

    // The program gets the user's environment as the registry has it now, not the one this
    // process started with: a variable set (setx) after Win顺 started is seen by ws.
    void getsTheUsersEnvironmentAsTheRegistryHasIt()
    {
        const std::wstring cmd = qEnvironmentVariable("ComSpec").toStdWString();
        const QString name = u"WS_TEST_ENV_"_s + QUuid::createUuid().toString(QUuid::Id128).left(12);
        HKEY key = nullptr;
        QCOMPARE(::RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr), LSTATUS(ERROR_SUCCESS));
        const std::wstring value = L"from the registry";
        QCOMPARE(::RegSetValueExW(key, name.toStdWString().c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                     static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))),
            LSTATUS(ERROR_SUCCESS));
        const auto remove = qScopeGuard([&] {
            ::RegDeleteValueW(key, name.toStdWString().c_str());
            ::RegCloseKey(key);
            _wputenv_s(L"WS_TEST_ONLY_HERE", L"");
        });
        _wputenv_s(L"WS_TEST_ONLY_HERE", L"set by a parent");
        const auto run = ws::shell::runUnelevatedCaptured(cmd, (L"/d /c \"echo [%" + name.toStdWString() + L"%] [%WS_TEST_ONLY_HERE%]\"").c_str(), 15000);
        QVERIFY(run.started && !run.timedOut);
        const QString text = QString::fromLatin1(run.output.data(), static_cast<qsizetype>(run.output.size()));
        QVERIFY2(text.contains(u"[from the registry]"_s), qPrintable(text));
        // Over this process's own variables (the user's account): what a parent set stays.
        if (!ws::integrity::processElevated())
            QVERIFY2(text.contains(u"[set by a parent]"_s), qPrintable(text));
        // The shell's account, which this one is (the tests run as the user).
        QVERIFY(ws::shell::runsAsShellUser());
    }

    void endsAProgramThatTakesTooLong()
    {
        const std::wstring cmd = qEnvironmentVariable("ComSpec").toStdWString();
        QElapsedTimer timer;
        timer.start();
        const auto run = ws::shell::runUnelevatedCaptured(cmd, L"/d /c \"ping -n 30 127.0.0.1 >nul\"", 400);
        QVERIFY(run.started);
        QVERIFY(run.timedOut);
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
    }
};

int main(int argc, char** argv)
{
    // "--run-hidden <command line>": the helper in between, as in WinShun.exe.
    if (const int hidden = ws::shell::hiddenHelper(); hidden >= 0)
        return hidden;
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 4 && args[1] == u"--tessoa-fixture")
        return tessoaFixture(args[2], args[3]);
    if (args.contains(u"--picker-probe"_s))
        return pickerProbe();
    if (args.size() >= 4 && args[1] == u"--run-silent") { // a silent custom command, then its answer
        std::atomic<int> done {-1};
        ws::shell::runCustom(args[2], args[3], {}, [&done](bool ok) { done = ok ? 1 : 0; }, {.silent = true});
        for (int waited = 0; done < 0 && waited < 10000; waited += 20)
            ::Sleep(20);
        return done == 1 ? 0 : 1;
    }
    if (args.size() >= 4 && args[1] == u"--probe") {
        const QJsonObject result {
            {u"parent"_s, parentProgram()}, {u"cwd"_s, QDir::currentPath()},
            {u"elevated"_s, ws::integrity::processElevated()}, {u"console"_s, ::GetConsoleWindow() != nullptr},
            {u"arguments"_s, QJsonArray::fromStringList(args)},
        };
        QFile file(args[2]);
        return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(result).toJson()) > 0 ? 0 : 1;
    }
    TestShellLaunch test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_shelllaunch.moc"
