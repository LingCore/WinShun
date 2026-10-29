// Unit tests for the Qt-free classifier the #104 service helper relies on
// (ws::dialognav), and for the service's folder-request check and the
// quoting of the helper's command line. The UI-Automation tree walk needs real
// elevated dialogs and is covered by the live test. Here: Inno Setup,
// SHBrowseForFolder, the directory check, and the address-bar route against a
// synthetic Open dialog (FakeFileDialog.h, shared with tst_filedialog).
#include "DialogNav.h"
#include "FakeFileDialog.h"
#include "ServiceLocation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW
#include <shlobj.h> // SHGetLocalizedName, IShellItem
#include <wrl/client.h>

#include <chrono>
#include <future>
#include <vector>

using ws::dialognav::Kind;
namespace dn = ws::dialognav;
using namespace std::chrono_literals;

namespace {
// The arguments CommandLineToArgvW makes of `command` (empty if it fails).
QStringList argvOf(const std::wstring& command)
{
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(command.c_str(), &argc);
    if (!argv)
        return {};
    const auto freeArgv = qScopeGuard([argv] { ::LocalFree(argv); });
    QStringList result;
    for (int i = 0; i < argc; ++i)
        result << QString::fromWCharArray(argv[i]);
    return result;
}

// A folder path of about `length` characters: "C:\" then 10-character
// segments, with `trailingSlash` as asked.
QString longFolder(int length, bool trailingSlash)
{
    QString path = QStringLiteral("C:\\");
    for (int i = 0; path.size() < length; ++i)
        path += QStringLiteral("seg %1\\").arg(i, 5, 10, QLatin1Char('0'));
    if (!trailingSlash)
        path.chop(1);
    return path;
}

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
    Window(const wchar_t* type, HWND parent = nullptr, DWORD extra = 0, int id = 0)
        : handle(::CreateWindowExW(0, type, L"", (parent ? WS_CHILD : WS_POPUP) | WS_VISIBLE | extra, -20000, -20000,
            200, 100, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), nullptr))
    {
    }
    ~Window()
    {
        if (handle)
            ::DestroyWindow(handle);
    }
};
} // namespace

class TestDialogNav : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        for (const auto* name : {L"TSelectFolderForm", L"TSelectDirDlg", L"TFolderTreeView", L"TStartMenuFolderTreeView",
                 L"SysTreeView32", L"SHBrowseForFolder ShellNameSpace Control"})
            registerClass(name);
        for (const auto* name : {L"TNewPathEdit", L"TNewEdit", L"TEdit"})
            registerClass(name, L"Edit");
        registerClass(L"WsNavButton", L"Button");
    }

    void nullAndPlain()
    {
        QCOMPARE(dn::classify(nullptr), Kind::None);
        Window plain(L"#32770");
        QCOMPARE(dn::classify(plain.handle), Kind::None); // no controls yet
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
        QCOMPARE(dn::classify(dialog.handle), Kind::Folder);
        ::ShowWindow(box.handle, SW_HIDE); // a hidden box is not the destination edit
        QCOMPARE(dn::classify(dialog.handle), Kind::None);
    }

    void rejectInnoLookalikes()
    {
        Window startMenu(L"TSelectFolderForm");
        Window smTree(L"TStartMenuFolderTreeView", startMenu.handle);
        Window smBox(L"TNewPathEdit", startMenu.handle);
        QCOMPARE(dn::classify(startMenu.handle), Kind::None);
        Window readOnly(L"TSelectFolderForm");
        Window roTree(L"TFolderTreeView", readOnly.handle);
        Window roBox(L"TNewPathEdit", readOnly.handle, ES_READONLY);
        QCOMPARE(dn::classify(readOnly.handle), Kind::None);
    }

    void browseOldStyle()
    {
        Window dialog(L"#32770");
        Window tree(L"SysTreeView32", dialog.handle, 0, 0x3741);
        Window ok(L"WsNavButton", dialog.handle, 0, IDOK);
        QVERIFY(dialog.handle && tree.handle && ok.handle);
        QCOMPARE(dn::classify(dialog.handle), Kind::Browse);
        ::DestroyWindow(ok.handle);
        ok.handle = nullptr;
        QCOMPARE(dn::classify(dialog.handle), Kind::None); // no OK button: not a browse dialog we drive
    }

    void browseNewStyle()
    {
        Window dialog(L"#32770");
        Window control(L"SHBrowseForFolder ShellNameSpace Control", dialog.handle);
        Window tree(L"SysTreeView32", control.handle);
        Window ok(L"WsNavButton", dialog.handle, 0, IDOK);
        QVERIFY(dialog.handle && control.handle && tree.handle && ok.handle);
        QCOMPARE(dn::classify(dialog.handle), Kind::Browse);
    }

    void existingDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::wstring path = QDir::toNativeSeparators(dir.path()).toStdWString();
        QVERIFY(dn::isExistingDirectory(path.c_str()));
        QVERIFY(!dn::isExistingDirectory((path + L"\\does-not-exist").c_str()));
        QVERIFY(!dn::isExistingDirectory(L""));
        const QString file = dir.path() + QStringLiteral("/a.txt");
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QVERIFY(!dn::isExistingDirectory(QDir::toNativeSeparators(file).toStdWString().c_str())); // a file, not a folder
    }

    // As tst_filedialog's: the address bar gets the folder with a trailing
    // backslash, a folder that is gone (or a file) nothing at all.
    void addressBar_data()
    {
        QTest::addColumn<QString>("leaf"); // under a temporary folder; empty: that drive's root
        QTest::addColumn<bool>("slash");
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
        QString folder = leaf.isEmpty() ? base.left(3) : base + QLatin1Char('\\') + leaf;
        if (slash)
            folder += QLatin1Char('\\');
        fakedialog::OpenDialog dialog((base + QStringLiteral("\\start")).toStdWString());
        QVERIFY(dialog.valid());
        QCOMPARE(dn::classify(dialog.handle()), Kind::Open);
        QVERIFY(dn::navigateToFolder(dialog.handle(), folder.toStdWString()));
        QCOMPARE(dialog.entered().size(), std::size_t(1));
        const QString typed = folder.endsWith(QLatin1Char('\\')) ? folder : folder + QLatin1Char('\\');
        QCOMPARE(QString::fromStdWString(dialog.entered().front()), typed);
        QCOMPARE(QString::fromStdWString(dialog.fileName()), QString());
    }

    // No text box yet: one click on the crumbs makes it. A dialog that is
    // not DPI aware never shows the one it makes, which is used all the same.
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
        // From another thread, as by the helper: this one takes the posted
        // click (and the messages sent) meanwhile, and must till it is done:
        // GetWindowText among them waits on these windows with no timeout.
        auto going = std::async(std::launch::async,
            [&] { return dn::navigateToFolder(dialog.handle(), folder.toStdWString()); });
        while (going.wait_for(0ms) != std::future_status::ready)
            QTest::qWait(5);
        QVERIFY(going.get());
        QVERIFY(dialog.clicks() >= 1); // one, but another if this thread was held up past 300 ms
        QCOMPARE(dialog.entered().size(), std::size_t(1));
        QCOMPARE(QString::fromStdWString(dialog.entered().front()), folder + QLatin1Char('\\'));
        QCOMPARE(QString::fromStdWString(dialog.address()), QStringLiteral("Address: ") + folder);
        QCOMPARE(QString::fromStdWString(dialog.fileName()), QString());
    }

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
            QVERIFY(!dn::navigateToFolder(dialog.handle(), gone.toStdWString()));
            QVERIFY(dialog.entered().empty());
            QCOMPARE(QString::fromStdWString(dialog.fileName()), QString());
            QCOMPARE(QString::fromStdWString(dialog.address()), QString::fromStdWString(before));
        }
    }

    // The service hands the folder to the helper on its command line, which
    // the helper's argv (the CommandLineToArgvW rules) must give back intact.
    void quoteRoundTrip_data()
    {
        QTest::addColumn<QString>("folder");
        QTest::newRow("drive root") << QStringLiteral("C:\\");
        QTest::newRow("trailing slash, space") << QStringLiteral("D:\\a b\\");
        QTest::newRow("spaces") << QStringLiteral("C:\\Program Files (x86)\\Common Files");
        QTest::newRow("unc") << QStringLiteral("\\\\server\\share\\x");
        QTest::newRow("unc trailing slash") << QStringLiteral("\\\\server\\share x\\y\\");
        QTest::newRow("non-ascii") << QStringLiteral("F:\\wsnav104\\nav7731目标");
        QTest::newRow("no space") << QStringLiteral("C:\\Windows");
        QTest::newRow("long 259") << longFolder(259, false).left(259);
        QTest::newRow("long 300 slash") << longFolder(300, true);
        QTest::newRow("long 1000") << longFolder(1000, false);
        QTest::newRow("long 32000 slash") << longFolder(32000, true);
        // Beyond what a folder request may hold, for the quoting itself.
        QTest::newRow("three trailing slashes") << QStringLiteral("C:\\a\\\\\\");
        QTest::newRow("embedded quote") << QStringLiteral("a\"b");
        QTest::newRow("slash quote") << QStringLiteral("a\\\"b");
        QTest::newRow("slashes quote slash") << QStringLiteral("a\\\\\"b\\");
        QTest::newRow("empty") << QString();
    }

    void quoteRoundTrip()
    {
        QFETCH(QString, folder);
        const std::wstring command = L"\"C:\\Program Files\\WinShun\\WinShunService.exe\" --navigate-dialog 1234 "
            + dn::quoteArgument(folder.toStdWString());
        const QStringList argv = argvOf(command);
        QCOMPARE(argv.size(), 4);
        QCOMPARE(argv.at(1), QStringLiteral("--navigate-dialog"));
        QCOMPARE(argv.at(3), folder);
    }

    void naiveQuotingLosesDriveRoot()
    {
        // The quoting the service used before: "C:\" escapes its own closing
        // quote, so the helper got `C:"` and refused it.
        const QStringList argv = argvOf(L"x.exe --navigate-dialog 1 \"C:\\\"");
        QCOMPARE(argv.size(), 4);
        QCOMPARE(argv.at(3), QStringLiteral("C:\""));
        QCOMPARE(QString::fromStdWString(dn::quoteArgument(L"C:\\")), QStringLiteral("\"C:\\\\\""));
    }

    // What the service checks before it builds that command line (string
    // rules only: it never looks the path up as LocalSystem).
    void folderRequest_data()
    {
        QTest::addColumn<QString>("folder");
        QTest::addColumn<bool>("valid");
        QTest::newRow("drive root") << QStringLiteral("C:\\") << true;
        QTest::newRow("lower-case drive") << QStringLiteral("d:\\x") << true;
        QTest::newRow("spaces") << QStringLiteral("C:\\Program Files\\Oray") << true;
        QTest::newRow("trailing slash") << QStringLiteral("D:\\a b\\") << true;
        QTest::newRow("non-ascii") << QStringLiteral("F:\\wsnav104\\nav7731目标") << true;
        QTest::newRow("unc share") << QStringLiteral("\\\\server\\share") << true;
        QTest::newRow("unc folder") << QStringLiteral("\\\\server\\share\\x\\") << true;
        QTest::newRow("at the limit") << longFolder(400, false).left(int(dn::kMaxFolderChars)) << true;

        QTest::newRow("empty") << QString() << false;
        QTest::newRow("too long") << longFolder(400, false).left(int(dn::kMaxFolderChars) + 1) << false;
        QTest::newRow("relative") << QStringLiteral("relative\\x") << false;
        QTest::newRow("drive only") << QStringLiteral("C:") << false;
        QTest::newRow("drive-relative") << QStringLiteral("C:x") << false;
        QTest::newRow("rooted") << QStringLiteral("\\x") << false;
        QTest::newRow("forward slashes") << QStringLiteral("C:/x") << false;
        QTest::newRow("quote") << QStringLiteral("C:\\a\"b") << false;
        QTest::newRow("tab") << QStringLiteral("C:\\a\tb") << false;
        QTest::newRow("newline") << QStringLiteral("C:\\a\nb") << false;
        QTest::newRow("del") << (QStringLiteral("C:\\a") + QChar(0x7F)) << false;
        QTest::newRow("c1 control") << (QStringLiteral("C:\\a") + QChar(0x85)) << false;
        QTest::newRow("nul") << (QStringLiteral("C:\\a") + QChar(0) + QStringLiteral("b")) << false;
        QTest::newRow("stream colon") << QStringLiteral("C:\\a:b") << false;
        QTest::newRow("wildcard") << QStringLiteral("C:\\a*") << false;
        QTest::newRow("question mark") << QStringLiteral("C:\\a?") << false;
        QTest::newRow("angle") << QStringLiteral("C:\\a<b>") << false;
        QTest::newRow("pipe") << QStringLiteral("C:\\a|b") << false;
        QTest::newRow("dot segment") << QStringLiteral("C:\\.\\x") << false;
        QTest::newRow("dot-dot segment") << QStringLiteral("C:\\a\\..\\b") << false;
        QTest::newRow("empty segment") << QStringLiteral("C:\\a\\\\b") << false;
        QTest::newRow("doubled trailing slash") << QStringLiteral("C:\\a\\\\") << false;
        QTest::newRow("unc server only") << QStringLiteral("\\\\server") << false;
        QTest::newRow("unc no share") << QStringLiteral("\\\\server\\") << false;
        QTest::newRow("unc empty server") << QStringLiteral("\\\\\\x") << false;
        QTest::newRow("unc empty share") << QStringLiteral("\\\\server\\\\x") << false;
        QTest::newRow("unc dot-dot share") << QStringLiteral("\\\\server\\..\\x") << false;
        QTest::newRow("device path") << QStringLiteral("\\\\.\\PhysicalDrive0") << false;
        QTest::newRow("dot-dot server") << QStringLiteral("\\\\..\\x") << false;
        QTest::newRow("long-path prefix") << QStringLiteral("\\\\?\\C:\\x") << false;
    }

    void folderRequest()
    {
        QFETCH(QString, folder);
        QFETCH(bool, valid);
        QCOMPARE(dn::isValidFolderRequest(folder.toStdWString()), valid);
        // What the service accepts, the helper's argv gives back intact.
        if (valid) {
            const QStringList argv = argvOf(L"x.exe --navigate-dialog 1 " + dn::quoteArgument(folder.toStdWString()));
            QCOMPARE(argv.size(), 4);
            QCOMPARE(argv.at(3), folder);
        }
    }

    void navigateChecksFolder()
    {
        // The helper's entry applies the same rules, then wants a directory,
        // before it acts: an Inno-style picker takes a good folder into its
        // path box and is left alone otherwise.
        Window dialog(L"TSelectFolderForm");
        Window tree(L"TFolderTreeView", dialog.handle);
        Window box(L"TNewPathEdit", dialog.handle);
        QVERIFY(dialog.handle && tree.handle && box.handle);
        QCOMPARE(dn::classify(dialog.handle), Kind::Folder);
        const auto boxText = [&box] {
            wchar_t text[512] {};
            ::GetWindowTextW(box.handle, text, 512);
            return QString::fromWCharArray(text);
        };
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::wstring path = QDir::toNativeSeparators(dir.path()).toStdWString();
        QVERIFY(dn::isExistingDirectory((path + L"\\..").c_str()));
        QVERIFY(!dn::navigateToFolder(dialog.handle, path + L"\\..")); // a real folder, not a plain request
        QVERIFY(!dn::navigateToFolder(dialog.handle, path + L"\\gone")); // not a directory
        QVERIFY(!dn::navigateToFolder(dialog.handle, L"relative"));
        QCOMPARE(boxText(), QString());
        QVERIFY(dn::navigateToFolder(dialog.handle, path));
        QCOMPARE(boxText(), QString::fromStdWString(path));
    }

    void requesterBesideService_data()
    {
        QTest::addColumn<QString>("client");
        QTest::addColumn<QString>("service");
        QTest::addColumn<bool>("accepted");
        const QString service = QStringLiteral("C:\\Program Files\\WinShun\\WinShunService.exe");
        QTest::newRow("beside") << QStringLiteral("C:\\Program Files\\WinShun\\WinShun.exe") << service << true;
        QTest::newRow("case") << QStringLiteral("c:\\PROGRAM FILES\\winshun\\WINSHUN.EXE") << service << true;
        QTest::newRow("other program") << QStringLiteral("C:\\Program Files\\WinShun\\WinShunMcp.exe") << service << false;
        QTest::newRow("portable") << QStringLiteral("D:\\tools\\WinShun\\WinShun.exe") << service << false;
        QTest::newRow("folder prefix") << QStringLiteral("C:\\Program Files\\WinShun2\\WinShun.exe") << service << false;
        QTest::newRow("subfolder") << QStringLiteral("C:\\Program Files\\WinShun\\bin\\WinShun.exe") << service << false;
        QTest::newRow("parent") << QStringLiteral("C:\\Program Files\\WinShun.exe") << service << false;
        QTest::newRow("name suffix") << QStringLiteral("C:\\Program Files\\WinShun\\WinShun.exe.bak") << service << false;
        QTest::newRow("bare name") << QStringLiteral("WinShun.exe") << service << false;
        QTest::newRow("empty") << QString() << service << false;
        QTest::newRow("service bare") << QStringLiteral("C:\\Program Files\\WinShun\\WinShun.exe")
                                      << QStringLiteral("WinShunService.exe") << false;
    }

    void requesterBesideService()
    {
        // Who the service offers its session helpers to (GitHub #104): only
        // WinShun.exe in its own folder.
        QFETCH(QString, client);
        QFETCH(QString, service);
        QFETCH(bool, accepted);
        QCOMPARE(ws::svcloc::isWinShunBeside(client.toStdWString(), service.toStdWString()), accepted);
    }

    void localizedNamesAsTheShell()
    {
        // The helper's folder-tree names, read without the shell namespace,
        // are the shell's own: "用户" for C:\Users on a Chinese Windows,
        // "Users" (no localized name needed) on an English one.
        QVERIFY(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
        const auto uninitialize = qScopeGuard([] { ::CoUninitialize(); });
        const auto shellName = [](const std::wstring& path) {
            Microsoft::WRL::ComPtr<IShellItem> item;
            PWSTR name = nullptr;
            if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))
                || FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)))
                return QString();
            const QString result = QString::fromWCharArray(name);
            ::CoTaskMemFree(name);
            return result;
        };
        const QString profile = QDir::toNativeSeparators(QDir::homePath());
        const QStringList folders {QStringLiteral("C:\\Users"), QStringLiteral("C:\\Users\\Public"),
            QStringLiteral("C:\\Program Files"), profile + QStringLiteral("\\Desktop"),
            profile + QStringLiteral("\\Documents"), profile + QStringLiteral("\\Downloads"),
            profile + QStringLiteral("\\Contacts")};
        int localized = 0;
        for (const QString& folder : folders) {
            if (!QFileInfo(folder).isDir())
                continue;
            const std::wstring path = folder.toStdWString();
            const QString shown = shellName(path);
            const QString leaf = folder.section(QLatin1Char('\\'), -1);
            const QString ours = QString::fromStdWString(dn::localizedName(path));
            if (!shown.isEmpty() && shown.compare(leaf, Qt::CaseInsensitive) != 0) {
                QCOMPARE(ours, shown);
                ++localized;
            } else {
                QVERIFY2(ours.isEmpty() || ours.compare(leaf, Qt::CaseInsensitive) == 0, qPrintable(folder + u' ' + ours));
            }
        }
        qInfo("%d folders with a localized name", localized);
        QCOMPARE(dn::localizedName(L"C:\\wsdn-no-such-folder"), std::wstring());
    }

    void localizedNameOnlyFromProtectedModules()
    {
        // desktop.ini is the user's to write: a name it points at in a module
        // a non-administrator can change is not read, though a copy of
        // Windows' own module has the very string.
        const QString common = qEnvironmentVariable("CommonProgramFiles");
        const QString source = common + QStringLiteral("\\system\\wab32res.dll"); // 联系人 / Contacts: 10100
        if (common.isEmpty() || !QFileInfo::exists(source))
            QSKIP("no wab32res.dll here");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString base = QDir::toNativeSeparators(dir.path());
        const QString copy = base + QStringLiteral("\\res\\wab32res.dll");
        QVERIFY(QDir().mkpath(base + QStringLiteral("\\res")));
        QVERIFY(QFile::copy(source, copy));
        const QDir sourceDir = QFileInfo(source).dir();
        for (const QString& language : sourceDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString mui = sourceDir.filePath(language + QStringLiteral("/wab32res.dll.mui"));
            if (QFileInfo::exists(mui) && QDir().mkpath(base + QStringLiteral("\\res\\") + language))
                QFile::copy(mui, base + QStringLiteral("\\res\\") + language + QStringLiteral("\\wab32res.dll.mui"));
        }
        const auto stringOf = [](const QString& module) {
            const HMODULE strings = ::LoadLibraryExW(reinterpret_cast<const wchar_t*>(module.utf16()), nullptr,
                LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            wchar_t text[256] {};
            const int n = strings ? ::LoadStringW(strings, 10100, text, 256) : 0;
            if (strings)
                ::FreeLibrary(strings);
            return n > 0 ? std::wstring(text, static_cast<std::size_t>(n)) : std::wstring();
        };
        const std::wstring name = stringOf(source);
        if (name.empty() || stringOf(copy) != name)
            QSKIP("the copy does not give the string by itself (no satellite for this language)");
        // Two folders (desktop.ini is cached per folder): one names the copy,
        // one Windows' own module.
        std::vector<std::wstring> made;
        const auto cleanup = qScopeGuard([&made] {
            for (const std::wstring& folder : made) {
                ::SetFileAttributesW((folder + L"\\desktop.ini").c_str(), FILE_ATTRIBUTE_NORMAL);
                ::SetFileAttributesW(folder.c_str(), FILE_ATTRIBUTE_NORMAL);
            }
        });
        const auto folderNaming = [&](const QString& folderName, const QString& module) {
            const std::wstring folder = (base + u'\\' + folderName).toStdWString();
            if (!::CreateDirectoryW(folder.c_str(), nullptr))
                return std::wstring();
            made.push_back(folder);
            const QString ini = QStringLiteral("[.ShellClassInfo]\r\nLocalizedResourceName=@%1,-10100\r\n").arg(module);
            QFile file(QString::fromStdWString(folder + L"\\desktop.ini"));
            if (!file.open(QIODevice::WriteOnly))
                return std::wstring();
            file.write("\xFF\xFE", 2); // UTF-16LE
            file.write(reinterpret_cast<const char*>(ini.utf16()), ini.size() * 2);
            file.close();
            ::SetFileAttributesW((folder + L"\\desktop.ini").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
            ::SetFileAttributesW(folder.c_str(), FILE_ATTRIBUTE_READONLY); // what makes Windows read its desktop.ini
            return folder;
        };
        const std::wstring user = folderNaming(QStringLiteral("named-by-copy"), copy);
        const std::wstring system = folderNaming(QStringLiteral("named-by-windows"), source);
        QVERIFY(!user.empty() && !system.empty());
        wchar_t module[MAX_PATH] {};
        int id = 0;
        QVERIFY(SUCCEEDED(::SHGetLocalizedName(user.c_str(), module, MAX_PATH, &id))); // Windows would read it
        QCOMPARE(dn::localizedName(user), std::wstring());
        QCOMPARE(dn::localizedName(system), name);
    }
};

QTEST_GUILESS_MAIN(TestDialogNav)
#include "tst_dialognav.moc"
