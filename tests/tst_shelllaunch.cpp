#include "CommandLaunch.h"
#include "platform/Integrity.h"
#include "platform/FileManagers.h"
#include "platform/FolderViewHit.h"
#include "platform/Shell.h"
#include "platform/FileDialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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

using namespace Qt::StringLiterals;

namespace {
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
private slots:
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
        // Explorer uses its own PATH, not CTest's Qt PATH. Stage only this
        // helper and its Qt DLLs so the test also works on a clean build.
        const QString helper = temporary.filePath(u"launch-probe.exe"_s);
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), helper));
        if (console) {
            QFile exe(helper);
            QVERIFY(exe.open(QIODevice::ReadWrite) && exe.seek(0x3c));
            quint32 header = 0;
            QCOMPARE(exe.read(reinterpret_cast<char*>(&header), 4), 4);
            const quint16 subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI; // after "PE\0\0" and the file header, 68 bytes in
            QVERIFY(exe.seek(header + 4 + 20 + 68));
            QCOMPARE(exe.write(reinterpret_cast<const char*>(&subsystem), 2), 2);
        }
        QCOMPARE(ws::commandlaunch::kindOf(helper), console ? ws::commandlaunch::Kind::Console : ws::commandlaunch::Kind::Gui);
        const QDir qtBin(QLibraryInfo::path(QLibraryInfo::BinariesPath));
        for (const auto* module : {"Core", "Gui", "Sql", "Test"}) {
            const QString dll = u"Qt6"_s + QString::fromLatin1(module)
                + (QLibraryInfo::isDebugBuild() ? u"d.dll"_s : u".dll"_s);
            QVERIFY(QFile::copy(qtBin.filePath(dll), temporary.filePath(dll)));
        }
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
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.contains(u"--picker-probe"_s))
        return pickerProbe();
    if (args.size() >= 4 && args[1] == u"--probe") {
        const QJsonObject result {
            {u"parent"_s, parentProgram()}, {u"cwd"_s, QDir::currentPath()},
            {u"elevated"_s, ws::integrity::processElevated()},
            {u"arguments"_s, QJsonArray::fromStringList(args)},
        };
        QFile file(args[2]);
        return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(result).toJson()) > 0 ? 0 : 1;
    }
    TestShellLaunch test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_shelllaunch.moc"
