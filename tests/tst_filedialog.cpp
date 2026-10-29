#include "FakeFileDialog.h"
#include "platform/FileDialog.h"
#include "Win32Util.h"

#include <QTest>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>

#include <windows.h>

#include <chrono>
#include <future>

using ws::filedialog::Kind;
namespace fd = ws::filedialog;
using namespace std::chrono_literals;

namespace {
void registerClass(const wchar_t* name, const wchar_t* base = nullptr)
{
    WNDCLASSW wc {};
    if (base)
        ::GetClassInfoW(nullptr, base, &wc);
    else
        wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = name;
    ::RegisterClassW(&wc);
}

struct Window {
    HWND handle = nullptr;
    Window(const wchar_t* type, HWND parent = nullptr, DWORD extra = 0)
        : handle(::CreateWindowExW(0, type, L"", (parent ? WS_CHILD : WS_POPUP) | WS_VISIBLE | extra,
            -20000, -20000, 200, 100, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr))
    {
    }
    ~Window() { if (handle) ::DestroyWindow(handle); }
};

struct FixtureWindow {
    HANDLE job;
    HWND window = nullptr;
    bool owns(HWND candidate) const
    {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(candidate, &pid);
        const ws::win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        BOOL belongs = FALSE;
        return process.valid() && ::IsProcessInJob(process.get(), job, &belongs) && belongs;
    }
};
} // namespace

class TestFileDialog : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        for (const auto* name : {L"TSelectFolderForm", L"TSelectDirDlg", L"TFolderTreeView", L"TStartMenuFolderTreeView"})
            registerClass(name);
        for (const auto* name : {L"TNewPathEdit", L"TNewEdit", L"TEdit"})
            registerClass(name, L"Edit");
    }

    void innoFolder_data()
    {
        QTest::addColumn<QString>("form");
        QTest::addColumn<QString>("edit");
        QTest::newRow("current") << QStringLiteral("TSelectFolderForm") << QStringLiteral("TNewPathEdit");
        QTest::newRow("inno6") << QStringLiteral("TSelectDirDlg") << QStringLiteral("TNewEdit");
        QTest::newRow("old-edit") << QStringLiteral("TSelectDirDlg") << QStringLiteral("TEdit");
    }

    void innoFolder()
    {
        QFETCH(QString, form);
        QFETCH(QString, edit);
        Window dialog(form.toStdWString().c_str());
        Window tree(L"TFolderTreeView", dialog.handle);
        Window box(edit.toStdWString().c_str(), dialog.handle);
        QVERIFY(dialog.handle && tree.handle && box.handle);
        QCOMPARE(fd::kind(dialog.handle), Kind::Folder);
        for (const auto* path : {L"D:\\", L"D:\\新 项目\\软件", L"\\\\server\\share\\工具"}) {
            QVERIFY(fd::goTo(dialog.handle, path));
            QCOMPARE(fd::currentFolder(dialog.handle), std::wstring(path));
            QVERIFY(::IsWindow(dialog.handle));
        }
        QVERIFY(!fd::setFileName(dialog.handle, L"unwanted.exe"));
        QVERIFY(!fd::accept(dialog.handle));
        ::EnableWindow(box.handle, FALSE);
        QCOMPARE(fd::kind(dialog.handle), Kind::None);
        ::EnableWindow(box.handle, TRUE);
        ::ShowWindow(box.handle, SW_HIDE);
        QCOMPARE(fd::kind(dialog.handle), Kind::None);
    }

    void rejectLookalikes()
    {
        Window dialog(L"TSelectFolderForm");
        Window tree(L"TStartMenuFolderTreeView", dialog.handle);
        Window box(L"TNewPathEdit", dialog.handle);
        QCOMPARE(fd::kind(dialog.handle), Kind::None);
        Window other(L"#32770");
        Window otherTree(L"TFolderTreeView", other.handle);
        Window otherEdit(L"TNewPathEdit", other.handle);
        QCOMPARE(fd::kind(other.handle), Kind::None);
        Window readonlyDialog(L"TSelectFolderForm");
        Window readonlyTree(L"TFolderTreeView", readonlyDialog.handle);
        Window readonlyEdit(L"TNewPathEdit", readonlyDialog.handle, ES_READONLY);
        QCOMPARE(fd::kind(readonlyDialog.handle), Kind::None);
    }

    // The address bar gets the folder with a trailing backslash, so it can
    // only resolve a folder ("a.txt" too), and the file name box nothing.
    void addressBar_data()
    {
        QTest::addColumn<QString>("leaf"); // under a temporary folder; empty: that drive's root
        QTest::addColumn<bool>("slash"); // asked for with the backslash already
        QTest::newRow("folder") << QStringLiteral("target") << false;
        QTest::newRow("with backslash") << QStringLiteral("target") << true;
        QTest::newRow("named like a file") << QStringLiteral("a.txt") << false;
        QTest::newRow("non-ascii, space") << QStringLiteral("目标 文件夹") << false;
        QTest::newRow("drive root") << QString() << false;
    }

    void addressBar()
    {
        QFETCH(QString, leaf);
        QFETCH(bool, slash);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString base = QDir::toNativeSeparators(root.path());
        if (!leaf.isEmpty())
            QVERIFY(QDir(root.path()).mkdir(leaf));
        QString folder = leaf.isEmpty() ? base.left(3) : base + QLatin1Char('\\') + leaf; // "C:\"
        if (slash)
            folder += QLatin1Char('\\');
        fakedialog::OpenDialog dialog((base + QStringLiteral("\\start")).toStdWString());
        QVERIFY(dialog.valid());
        QCOMPARE(fd::kind(dialog.handle()), Kind::Open);
        QVERIFY(fd::goTo(dialog.handle(), folder.toStdWString()));
        QCOMPARE(dialog.entered().size(), std::size_t(1));
        const QString typed = folder.endsWith(QLatin1Char('\\')) ? folder : folder + QLatin1Char('\\');
        QCOMPARE(QString::fromStdWString(dialog.entered().front()), typed);
        QCOMPARE(QString::fromStdWString(fd::currentFolder(dialog.handle())),
            QDir::toNativeSeparators(QDir::cleanPath(folder)));
        QCOMPARE(QString::fromStdWString(dialog.fileName()), QString());
    }

    // No text box yet: one click on the crumbs makes it. A dialog that is
    // not DPI aware never shows the one it makes, which is used all the same
    // (not the file name box and OK).
    void addressBarByClick_data()
    {
        QTest::addColumn<bool>("shown");
        QTest::newRow("shown") << true;
        QTest::newRow("left hidden (not DPI aware)") << false;
    }

    void addressBarByClick()
    {
        QFETCH(bool, shown);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkdir(QStringLiteral("target")));
        const QString base = QDir::toNativeSeparators(root.path());
        const QString folder = base + QStringLiteral("\\target");
        fakedialog::OpenDialog dialog((base + QStringLiteral("\\start")).toStdWString(),
            shown ? fakedialog::AddressBox::ShownByClick : fakedialog::AddressBox::HiddenByClick);
        QVERIFY(dialog.valid());
        // From another thread, as by the app's worker: this one takes the
        // posted click (and the messages sent) meanwhile, and must till it is
        // done: GetWindowText among them waits on these windows with no timeout.
        auto going = std::async(std::launch::async, [&] { return fd::goTo(dialog.handle(), folder.toStdWString()); });
        while (going.wait_for(0ms) != std::future_status::ready)
            QTest::qWait(5);
        QVERIFY(going.get());
        QVERIFY(dialog.clicks() >= 1); // one, but another if this thread was held up past 300 ms
        QCOMPARE(dialog.entered().size(), std::size_t(1));
        QCOMPARE(QString::fromStdWString(dialog.entered().front()), folder + QLatin1Char('\\'));
        QCOMPARE(QString::fromStdWString(fd::currentFolder(dialog.handle())), folder);
        QCOMPARE(QString::fromStdWString(dialog.fileName()), QString());
    }

    // A folder gone by the time the dialog would go there, or a file: nothing
    // is typed anywhere (the file name box and OK would get an error box).
    void addressBarGone()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString base = QDir::toNativeSeparators(root.path());
        QFile file(root.filePath(QStringLiteral("note.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        fakedialog::OpenDialog dialog((base + QStringLiteral("\\start")).toStdWString());
        QVERIFY(dialog.valid());
        const std::wstring before = dialog.address();
        for (const QString& gone : {base + QStringLiteral("\\gone"), base + QStringLiteral("\\note.txt")}) {
            QVERIFY(!fd::goTo(dialog.handle(), gone.toStdWString()));
            QVERIFY(dialog.entered().empty());
            QCOMPARE(QString::fromStdWString(dialog.fileName()), QString());
            QCOMPARE(QString::fromStdWString(dialog.address()), QString::fromStdWString(before));
        }
    }

    void realInnoPicker()
    {
        const QString setup = qEnvironmentVariable("WINSHUN_TEST_INNO_SETUP");
        if (setup.isEmpty())
            QSKIP("Set WINSHUN_TEST_INNO_SETUP to the compiled tests/fixtures/inno-folder.iss");
        // Separate desktop: no focus, clicks or windows on the user's desktop.
        std::wstring desktopName = (QStringLiteral("WinShunDialogTest-")
            + QUuid::createUuid().toString(QUuid::Id128)).toStdWString();
        HDESK desktop = ::CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        QVERIFY(desktop);
        const auto closeDesktop = qScopeGuard([&] { ::CloseDesktop(desktop); });
        const ws::win32::UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        QVERIFY(::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits));
        STARTUPINFOW startup {sizeof startup};
        startup.lpDesktop = desktopName.data();
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_SHOWNOACTIVATE;
        PROCESS_INFORMATION info {};
        std::wstring command = L"\"" + setup.toStdWString() + L"\" /SP- /LOG=\"" + setup.toStdWString() + L".log\"";
        QVERIFY(::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
            nullptr, nullptr, &startup, &info));
        const ws::win32::UniqueHandle process(info.hProcess), thread(info.hThread);
        QVERIFY(::AssignProcessToJobObject(job.get(), process.get()));
        QVERIFY(::ResumeThread(thread.get()) != static_cast<DWORD>(-1));
        FixtureWindow browse {job.get()};
        const auto findBrowse = [&] {
            ::EnumDesktopWindows(desktop, [](HWND window, LPARAM context) -> BOOL {
                auto& found = *reinterpret_cast<FixtureWindow*>(context);
                if (!found.owns(window))
                    return TRUE;
                wchar_t name[64] {};
                ::GetClassNameW(window, name, 64);
                if (std::wcscmp(name, L"TWizardForm") != 0)
                    return TRUE;
                ::EnumChildWindows(window, [](HWND child, LPARAM out) -> BOOL {
                    wchar_t title[128] {}, name[64] {};
                    ::GetWindowTextW(child, title, 128);
                    ::GetClassNameW(child, name, 64);
                    const QString caption = QString::fromWCharArray(title).remove(u'&');
                    if (::IsWindowVisible(child) && caption.startsWith(QStringLiteral("Browse"))
                        && caption.size() < 20) {
                        qInfo() << "Browse button:" << QString::fromWCharArray(name) << caption;
                        reinterpret_cast<FixtureWindow*>(out)->window = child;
                    }
                    return TRUE;
                }, context);
                return TRUE;
            }, reinterpret_cast<LPARAM>(&browse));
            return browse.window != nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(findBrowse(), 10000);
        QVERIFY(::PostMessageW(::GetParent(browse.window), WM_COMMAND,
            MAKEWPARAM(::GetDlgCtrlID(browse.window), BN_CLICKED), reinterpret_cast<LPARAM>(browse.window)));
        FixtureWindow picker {job.get()};
        const auto findPicker = [&] {
            ::EnumDesktopWindows(desktop, [](HWND window, LPARAM context) -> BOOL {
                auto& found = *reinterpret_cast<FixtureWindow*>(context);
                if (!found.owns(window))
                    return TRUE;
                wchar_t name[64] {};
                ::GetClassNameW(window, name, 64);
                if (std::wcscmp(name, L"TSelectFolderForm") == 0 || std::wcscmp(name, L"TSelectDirDlg") == 0)
                    found.window = window;
                return TRUE;
            }, reinterpret_cast<LPARAM>(&picker));
            return picker.window != nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(findPicker(), 10000);
        const HWND dialog = picker.window;
        wchar_t actualClass[64] {};
        ::GetClassNameW(dialog, actualClass, 64);
        qInfo() << "Real Inno picker:" << QString::fromWCharArray(actualClass);
        QTRY_COMPARE_WITH_TIMEOUT(fd::kind(dialog), Kind::Folder, 5000);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const std::wstring target = QDir::toNativeSeparators(folder.path()).toStdWString();
        QVERIFY(fd::goTo(dialog, target));
        QCOMPARE(fd::currentFolder(dialog), target);
        QTest::qWait(200);
        QVERIFY(::IsWindowVisible(dialog)); // navigation never accepts the dialog
        QVERIFY(!fd::accept(dialog));
        QVERIFY(!fd::setFileName(dialog, L"unused.exe"));
    }
};

QTEST_GUILESS_MAIN(TestFileDialog)
#include "tst_filedialog.moc"
