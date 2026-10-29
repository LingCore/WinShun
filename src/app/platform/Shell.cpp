#include "Shell.h"

#include "AutostartRun.h"
#include "CommandLaunch.h"

#include "DragOut.h"
#include "FileManagers.h"
#include "Favorites.h"
#include "Integrity.h"
#include "SystemCatalog.h"
#include "Win32Util.h"

#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QSettings>
#include <QThread>
#include <QUrl>

#include <ole2.h> // before exdisp.h: WIN32_LEAN_AND_MEAN keeps it out of windows.h

#include <exdisp.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace ws {

namespace {

// Owner of a BSTR.
class Bstr {
public:
    Bstr() = default;
    explicit Bstr(const wchar_t* s)
        : m_p(s ? ::SysAllocString(s) : nullptr)
    {
    }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    ~Bstr() { ::SysFreeString(m_p); }
    operator BSTR() const noexcept { return m_p; }
    BSTR* out() noexcept { return &m_p; }

private:
    BSTR m_p = nullptr;
};

} // namespace

namespace shell {

namespace {

// Where open() takes folders and reveal() shows items (setFileManager).
std::atomic<filemanager::Kind> g_fileManager {filemanager::Kind::Explorer};

thread_local const std::atomic_bool* t_pickerCancelled = nullptr;

class PickerEvents final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IFileDialogEvents> {
public:
    IFACEMETHODIMP OnFolderChange(IFileDialog* dialog) override
    {
        if (t_pickerCancelled && t_pickerCancelled->load())
            dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        return S_OK;
    }
    IFACEMETHODIMP OnFileOk(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP OnSelectionChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override
    {
        *response = FDESVR_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP OnTypeChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override
    {
        *response = FDEOR_DEFAULT;
        return S_OK;
    }
};

HRESULT showPicker(IFileDialog* dialog, HWND owner)
{
    if (t_pickerCancelled && t_pickerCancelled->load())
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const auto events = Microsoft::WRL::Make<PickerEvents>();
    DWORD cookie = 0;
    const HRESULT advised = dialog->Advise(events.Get(), &cookie);
    const HRESULT result = dialog->Show(owner);
    if (SUCCEEDED(advised))
        dialog->Unadvise(cookie);
    return result;
}

template <typename Fn> QString runPicker(Fn&& pick)
{
    auto* app = QCoreApplication::instance();
    if (!app || QThread::currentThread() != app->thread())
        return pick();
    // Keep the native dialog's drag loop off Qt's GUI thread. The GUI keeps
    // processing its search bar and folder menu while the caller waits.
    QEventLoop loop;
    QString result;
    std::atomic_bool cancelled {false};
    std::atomic<DWORD> threadId {0};
    const auto closing = QObject::connect(app, &QCoreApplication::aboutToQuit, &loop, [&] {
        cancelled.store(true);
        if (const DWORD id = threadId.load())
            ::EnumThreadWindows(
                id,
                [](HWND window, LPARAM) -> BOOL {
                    ::PostMessageW(window, WM_CLOSE, 0, 0);
                    return TRUE;
                },
                0);
    });
    std::jthread worker([&] {
        threadId.store(::GetCurrentThreadId());
        t_pickerCancelled = &cancelled;
        if (!cancelled.load())
            result = pick();
        QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
    });
    loop.exec();
    worker.join();
    QObject::disconnect(closing);
    return result;
}

// Runs `fn` on a detached thread with COM initialised (the shell needs an STA).
template <typename Fn> void runOnShellThread(Fn&& fn)
{
    std::thread([fn = std::forward<Fn>(fn)]() mutable {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        fn();
        if (SUCCEEDED(hr))
            ::CoUninitialize();
    }).detach();
}

// The Application object of an Explorer window: its shell view -> the view's
// background -> Application. Null for a window without a folder view.
ComPtr<IShellDispatch2> applicationOf(IDispatch* window)
{
    ComPtr<IServiceProvider> services;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background;
    ComPtr<IShellFolderViewDual> folderView;
    ComPtr<IDispatch> application;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(window->QueryInterface(IID_PPV_ARGS(&services)))
        || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))
        || FAILED(browser->QueryActiveShellView(&view))
        || FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background)))
        || FAILED(background.As(&folderView)) || FAILED(folderView->get_Application(&application))
        || FAILED(application.As(&shell)))
        return nullptr;
    return shell;
}

// Explorer's own Application object: the desktop's, else any folder
// window's. An Explorer that has run for long can drop its desktop from
// ShellWindows while its folder windows stay (see pitfalls). Null when
// there is no Explorer window at all.
ComPtr<IShellDispatch2> explorerApplication()
{
    ComPtr<IShellWindows> windows;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))))
        return nullptr;
    VARIANT desktop {};
    desktop.vt = VT_I4;
    desktop.lVal = CSIDL_DESKTOP;
    VARIANT none {};
    long hwnd = 0;
    ComPtr<IDispatch> found;
    if (windows->FindWindowSW(&desktop, &none, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &found) == S_OK && found)
        if (ComPtr<IShellDispatch2> shell = applicationOf(found.Get()))
            return shell;
    long count = 0;
    if (FAILED(windows->get_Count(&count)))
        return nullptr;
    for (long i = 0; i < count; ++i) {
        VARIANT index {};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> window;
        if (windows->Item(index, &window) == S_OK && window)
            if (ComPtr<IShellDispatch2> shell = applicationOf(window.Get()))
                return shell;
    }
    return nullptr;
}

// Launch through the desktop shell, as opening an item in Explorer does.
// Besides keeping normal rights when we are elevated, this gives apps the
// shell as their caller (Foxit warns about an unsigned direct caller, #64).
// This preserves the shell's file associations and security checks.
bool executeAsUser(const std::wstring& file, const std::wstring& args, const std::wstring& dir)
{
    const ComPtr<IShellDispatch2> shell = explorerApplication();
    if (!shell)
        return false;

    const Bstr target(file.c_str());
    const Bstr arguments(args.empty() ? nullptr : args.c_str());
    const Bstr directory(dir.empty() ? nullptr : dir.c_str());
    VARIANT parameters {};
    if (arguments) {
        parameters.vt = VT_BSTR;
        parameters.bstrVal = arguments; // still owned by `arguments`
    }
    VARIANT workDir {};
    if (directory) {
        workDir.vt = VT_BSTR;
        workDir.bstrVal = directory; // still owned by `directory`
    }
    VARIANT verb {};
    VARIANT show {};
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    return SUCCEEDED(shell->ShellExecute(target, parameters, workDir, verb, show));
}

// Starts `command` with the desktop shell's own token: the user's normal
// rights. Invalid when there is no shell to take them from.
win32::UniqueHandle startWithShellToken(std::wstring command, const std::wstring& dir, DWORD flags = 0)
{
    DWORD pid = 0;
    const HWND shellWindow = ::GetShellWindow();
    if (!shellWindow || !::GetWindowThreadProcessId(shellWindow, &pid) || pid == 0)
        return {};
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE shellToken = nullptr;
    if (!process.valid() || !::OpenProcessToken(process.get(), TOKEN_DUPLICATE, &shellToken))
        return {};
    const win32::UniqueHandle source(shellToken);
    HANDLE primaryToken = nullptr;
    if (!::DuplicateTokenEx(source.get(),
            TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
            nullptr, SecurityImpersonation, TokenPrimary, &primaryToken))
        return {};
    const win32::UniqueHandle token(primaryToken);
    STARTUPINFOW startup {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION started {};
    if (!::CreateProcessWithTokenW(token.get(), 0, nullptr, command.data(), flags, nullptr,
            dir.empty() ? nullptr : dir.c_str(), &startup, &started))
        return {};
    ::CloseHandle(started.hThread);
    return win32::UniqueHandle(started.hProcess);
}

// A folder, shell location or URI ("shell:AppsFolder\<id>", "::{CLSID}",
// "ms-settings:display"), not a file or program: what ShellExecute hands to
// the running Explorer instead of starting it itself.
bool isShellTarget(const std::wstring& file)
{
    if (file.starts_with(L"::"))
        return true;
    const std::size_t colon = file.find(L':');
    if (colon != std::wstring::npos && colon >= 2 // "C:\..." has its colon second
        && std::all_of(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(colon), [](wchar_t c) {
               return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-'
                   || c == L'.' || c == L'+';
           }))
        return true;
    const DWORD attributes = ::GetFileAttributesW(file.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Without an Explorer window to ask (another shell is in use, or Explorer
// lists none), start the target with the desktop shell's own token: the
// user's normal rights. Folders, shell locations and URIs go to a new
// explorer.exe, which opens them itself when the running Explorer takes
// nothing more. rundll32's ShellExecute hands them to that Explorer all the
// same, opens nothing and still says it did (see pitfalls). Files and
// commands go to rundll32, which starts them itself, in `dir` (explorer.exe
// would start them in System32). False when there is no shell at all.
bool executeWithShellToken(const std::wstring& file, const std::wstring& args, const std::wstring& dir)
{
    if (file.find(L'"') != std::wstring::npos)
        return false; // cannot be quoted on the command line (no path has one)
    wchar_t folder[MAX_PATH] = {};
    if (args.empty() && isShellTarget(file)) {
        const UINT n = ::GetWindowsDirectoryW(folder, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return false;
        // "D:\" stays bare: a backslash before a closing quote would escape it.
        const std::wstring target = file.ends_with(L'\\') ? file : L'"' + file + L'"';
        return startWithShellToken(L'"' + std::wstring(folder, n) + L"\\explorer.exe\" " + target, {}).valid();
    }
    const UINT n = ::GetSystemDirectoryW(folder, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    std::wstring command
        = L'"' + std::wstring(folder, n) + L"\\rundll32.exe\" shell32.dll,ShellExec_RunDLL \"" + file + L'"';
    if (!args.empty())
        command += L' ' + args;
    return startWithShellToken(std::move(command), dir).valid();
}

// Starts through Explorer with the user's normal rights. Without Explorer:
// ShellExecute from a normal process, or the shell token from an elevated
// one. Only the latter waits briefly for the desktop shell to restart.
bool executeUnelevated(const std::wstring& file, const std::wstring& args, const std::wstring& dir)
{
    if (executeAsUser(file, args, dir))
        return true;
    if (!integrity::processElevated()) {
        SHELLEXECUTEINFOW sei {sizeof(SHELLEXECUTEINFOW)};
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_LOG_USAGE;
        sei.lpFile = file.c_str();
        sei.lpParameters = args.empty() ? nullptr : args.c_str();
        sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
        sei.nShow = SW_SHOWNORMAL;
        return ::ShellExecuteExW(&sei);
    }
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (attempt > 0)
            ::Sleep(300);
        if ((attempt > 0 && executeAsUser(file, args, dir)) || executeWithShellToken(file, args, dir))
            return true;
    }
    return false;
}

// Starts `file` as administrator: UAC asks, unless Win顺 is elevated already.
// `process`, when wanted, gets a handle to wait on (null if the shell made
// no process of its own for it). False when it did not start: the prompt
// declined (ERROR_CANCELLED), or the file missing.
bool executeAsAdministrator(const std::wstring& file, const std::wstring& args, const std::wstring& dir,
    win32::UniqueHandle* process = nullptr, int show = SW_SHOWNORMAL)
{
    SHELLEXECUTEINFOW sei {sizeof(SHELLEXECUTEINFOW)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_LOG_USAGE | (process ? SEE_MASK_NOCLOSEPROCESS : 0);
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = show;
    if (!::ShellExecuteExW(&sei))
        return false;
    if (process)
        *process = win32::UniqueHandle(sei.hProcess);
    return true;
}

// A custom command (GitHub #112), on the shell thread. Silent: a console
// program, a .bat or a .ps1 is created with CREATE_NO_WINDOW, so that no
// console exists to be seen (see commandlaunch::Kind); the rest starts as
// it would without the option. Never through cmd.exe or PowerShell unless
// the file is a script of theirs. False when it did not start.
bool startHidden(const commandlaunch::Plan& plan, const std::wstring& dir)
{
    std::wstring line = plan.commandLine.toStdWString();
    if (integrity::processElevated()) // the user's normal rights: the shell's token
        return startWithShellToken(std::move(line), dir, CREATE_NO_WINDOW).valid();
    STARTUPINFOW startup {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION started {};
    if (!::CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            dir.empty() ? nullptr : dir.c_str(), &startup, &started))
        return false;
    ::CloseHandle(started.hThread);
    ::CloseHandle(started.hProcess);
    return true;
}

// As administrator and without a window: UAC asks for Win顺's own exe, which
// has no console (it is of the Windows subsystem) and creates the program
// with CREATE_NO_WINDOW (hiddenHelper). ShellExecute's own SW_HIDE would
// leave the console the elevated program gets to flash.
bool startViaHelperAsAdministrator(const commandlaunch::Plan& plan, const std::wstring& dir, bool hidden)
{
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    const char16_t* const mode = hidden ? commandlaunch::kHiddenSwitch : commandlaunch::kShownSwitch;
    const std::wstring parameters = std::wstring(reinterpret_cast<const wchar_t*>(mode)) + L' ' + plan.commandLine.toStdWString();
    return executeAsAdministrator(exe, parameters, dir, nullptr, SW_HIDE);
}

bool launchCustom(const QString& command, const QString& directory, const QString& query, CommandOptions options)
{
    const auto [file, arguments] = splitCommand(command);
    if (file.isEmpty())
        return false;
    const std::wstring dir = win32::expandEnvironment(directory.toStdWString());
    commandlaunch::Plan plan;
    bool script = false; // .bat, .cmd, .ps1
    if (options.silent || options.administrator) {
        const QString program = commandlaunch::resolveProgram(file, QString::fromStdWString(dir));
        const commandlaunch::Kind kind = program.isEmpty() ? commandlaunch::Kind::Other : commandlaunch::kindOf(program);
        // The template is expanded before the words are put in: a %NAME%
        // typed after the keyword stays text.
        const QString expandedArguments = QString::fromStdWString(win32::expandEnvironment(arguments.toStdWString()));
        script = kind == commandlaunch::Kind::Batch || kind == commandlaunch::Kind::PowerShell;
        if (kind == commandlaunch::Kind::Batch && !query.isEmpty() && !commandlaunch::safeForCmd(query))
            return false; // cmd.exe would read & | < > ^ % in them as its own
        plan = commandlaunch::silentPlan(program, commandArguments(expandedArguments, query), kind, commandlaunch::systemTools(),
            !options.silent);
    }
    bool ok;
    if (plan.ok && options.silent) {
        ok = options.administrator ? startViaHelperAsAdministrator(plan, dir, true) : startHidden(plan, dir);
    } else if (plan.ok && script) {
        // As administrator with its window: the interpreter asked for, not the file (see kShownSwitch).
        ok = startViaHelperAsAdministrator(plan, dir, false);
    } else if (options.administrator) {
        ok = executeAsAdministrator(file.toStdWString(), commandArguments(arguments, query).toStdWString(), dir);
    } else {
        ok = executeUnelevated(file.toStdWString(), commandArguments(arguments, query).toStdWString(), dir);
    }
    // Declining the UAC prompt is an answer, not a failure to report. Read at once: nothing in between has run.
    const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
    return ok || (options.administrator && error == ERROR_CANCELLED);
}

} // namespace

void open(const QString& path, bool asAdministrator, std::function<void(bool)> done)
{
    ::AllowSetForegroundWindow(ASFW_ANY); // the launched app may take focus from us
    const QString native = QDir::toNativeSeparators(path);
    const QFileInfo info(native);
    const QString workDir = info.isDir() ? QString() : QDir::toNativeSeparators(info.absolutePath());
    // A folder goes to the file manager chosen (setFileManager); Explorer
    // takes it if that does not start.
    const filemanager::Kind manager
        = info.isDir() && !asAdministrator ? g_fileManager.load() : filemanager::Kind::Explorer;
    runOnShellThread(
        [file = native.toStdWString(), dir = workDir.toStdWString(), asAdministrator, manager, done = std::move(done)] {
            const bool ok = asAdministrator
                ? executeAsAdministrator(file, {}, dir)
                : (manager != filemanager::Kind::Explorer && filemanager::openFolder(manager, file))
                    || executeUnelevated(file, {}, dir);
            if (done)
                done(ok);
        });
}

void openUrl(const QString& url)
{
    ::AllowSetForegroundWindow(ASFW_ANY);
    runOnShellThread([target = url.toStdWString()] { executeUnelevated(target, {}, {}); });
}

void openTerminal(const QString& folder, std::function<void(bool)> done)
{
    ::AllowSetForegroundWindow(ASFW_ANY);
    const QString native = QDir::toNativeSeparators(folder);
    runOnShellThread([dir = native.toStdWString(), done = std::move(done)] {
        wchar_t system[MAX_PATH] = {};
        const UINT n = ::GetSystemDirectoryW(system, MAX_PATH);
        bool ok = false;
        if (n > 0 && n < MAX_PATH) {
            const std::wstring cmd = std::wstring(system, n) + L"\\cmd.exe";
            if (dir.starts_with(L"\\\\")) {
                // Without a backslash at the end: before the closing quote it would escape it.
                std::wstring target = dir;
                while (target.ends_with(L'\\'))
                    target.pop_back();
                ok = executeUnelevated(cmd, L"/k pushd \"" + target + L'"', {});
            } else {
                ok = executeUnelevated(cmd, {}, dir);
            }
        }
        if (done)
            done(ok);
    });
}

void run(const QString& command, std::function<void(bool)> done)
{
    ::AllowSetForegroundWindow(ASFW_ANY);
    const auto [file, args] = splitCommand(command);
    runOnShellThread([file = file.toStdWString(), args = args.toStdWString(), done = std::move(done)] {
        const bool ok = executeUnelevated(file, args, {});
        if (done)
            done(ok);
    });
}

void launchApp(const QString& launchPath, bool asAdministrator, std::function<void(bool)> done)
{
    ::AllowSetForegroundWindow(ASFW_ANY);
    runOnShellThread([target = launchPath.toStdWString(), asAdministrator, done = std::move(done)] {
        // Store apps refuse to start from an elevated process; Explorer can
        // start them. "runas" is the app's own verb, as in the Start menu.
        const bool ok = asAdministrator ? executeAsAdministrator(target, {}, {}) : executeUnelevated(target, {}, {});
        if (done)
            done(ok);
    });
}

void runCustom(const QString& command, const QString& directory, const QString& query, std::function<void(bool)> done,
    CommandOptions options)
{
    ::AllowSetForegroundWindow(ASFW_ANY);
    runOnShellThread([command, directory, query, options, done = std::move(done)] {
        const bool ok = launchCustom(command, directory, query, options);
        if (done)
            done(ok);
    });
}

int hiddenHelper()
{
    // "WinShun.exe --run-hidden <command line>" (or --run-shown): the rest of the line, as it is.
    const wchar_t* line = ::GetCommandLineW();
    if (line[0] == L'"') {
        ++line;
        while (*line && *line != L'"')
            ++line;
        if (*line)
            ++line;
    } else {
        while (*line && *line != L' ')
            ++line;
    }
    while (*line == L' ')
        ++line;
    std::wstring_view prefix(reinterpret_cast<const wchar_t*>(commandlaunch::kHiddenSwitch));
    DWORD flags = CREATE_NO_WINDOW;
    if (std::wstring_view(line).substr(0, prefix.size()) != prefix || line[prefix.size()] != L' ') {
        prefix = std::wstring_view(reinterpret_cast<const wchar_t*>(commandlaunch::kShownSwitch));
        flags = 0; // its own console window, as it would have without the helper
        if (std::wstring_view(line).substr(0, prefix.size()) != prefix || line[prefix.size()] != L' ')
            return -1;
    }
    std::wstring rest = line + prefix.size() + 1;
    STARTUPINFOW startup {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION started {};
    if (!::CreateProcessW(nullptr, rest.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup, &started))
        return 1;
    ::CloseHandle(started.hThread);
    ::CloseHandle(started.hProcess);
    return 0;
}

bool startElevated(const std::wstring& program, const std::wstring& args, const std::wstring& cwd)
{
    return executeAsAdministrator(program, args, cwd);
}

void runElevated(const std::wstring& program, const std::wstring& args,
    std::function<void(std::optional<unsigned long>)> done, int show)
{
    runOnShellThread([program, args, done = std::move(done), show] {
        win32::UniqueHandle process;
        std::optional<unsigned long> code;
        if (executeAsAdministrator(program, args, {}, &process, show)) {
            DWORD exitCode = 0;
            // Long enough for the user to answer UAC and the service to start.
            if (process.valid() && ::WaitForSingleObject(process.get(), 120'000) == WAIT_OBJECT_0
                && ::GetExitCodeProcess(process.get(), &exitCode))
                code = exitCode;
            else
                code = static_cast<unsigned long>(-1); // still running, or no process to wait on
        }
        done(code);
    });
}

namespace {

// Explorer windows, one per folder, with all of its items selected. On the
// shell thread.
void selectInExplorer(const std::vector<std::wstring>& paths)
{
    // By folder, in the order the folders first come up.
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> folders;
    for (const std::wstring& path : paths) {
        const QFileInfo info(QString::fromStdWString(path));
        if (info.isRoot()) { // a drive: nothing above it to select it in
            if (PIDLIST_ABSOLUTE pidl = ::ILCreateFromPathW(path.c_str())) {
                ::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ::ILFree(pidl);
            }
            continue;
        }
        const std::wstring key = QDir::toNativeSeparators(info.path()).toStdWString();
        auto it = std::find_if(
            folders.begin(), folders.end(), [&](const auto& f) { return _wcsicmp(f.first.c_str(), key.c_str()) == 0; });
        if (it == folders.end())
            it = folders.insert(folders.end(), {key, {}});
        it->second.push_back(path);
    }
    for (const auto& [folder, files] : folders) {
        PIDLIST_ABSOLUTE parent = ::ILCreateFromPathW(folder.c_str());
        if (!parent)
            continue;
        std::vector<PIDLIST_ABSOLUTE> items;
        std::vector<PCUITEMID_CHILD> children;
        for (const std::wstring& file : files) {
            if (PIDLIST_ABSOLUTE pidl = ::ILCreateFromPathW(file.c_str())) {
                items.push_back(pidl);
                children.push_back(::ILFindLastID(pidl));
            }
        }
        if (!children.empty())
            ::SHOpenFolderAndSelectItems(parent, static_cast<UINT>(children.size()), children.data(), 0);
        for (PIDLIST_ABSOLUTE pidl : items)
            ::ILFree(pidl);
        ::ILFree(parent);
    }
}

// In `manager`, or in Explorer when that is Explorer or does not start.
void revealIn(const QStringList& paths, filemanager::Kind manager)
{
    std::vector<std::wstring> natives;
    for (const QString& path : paths)
        natives.push_back(QDir::toNativeSeparators(path).toStdWString());
    if (natives.empty())
        return;
    ::AllowSetForegroundWindow(ASFW_ANY);
    runOnShellThread([natives = std::move(natives), manager] {
        if (manager == filemanager::Kind::Explorer || !filemanager::reveal(manager, natives))
            selectInExplorer(natives);
    });
}

} // namespace

void reveal(const QString& path)
{
    revealIn({path}, g_fileManager.load());
}

void reveal(const QStringList& paths)
{
    revealIn(paths, g_fileManager.load());
}

void revealInExplorer(const QString& path)
{
    revealIn({path}, filemanager::Kind::Explorer);
}

void setFileManager(filemanager::Kind kind)
{
    g_fileManager = kind;
}

filemanager::Kind fileManager()
{
    return g_fileManager.load();
}

win32::UniqueHandle startUnelevated(const std::wstring& program, const std::wstring& args, const std::wstring& cwd)
{
    if (program.find(L'"') != std::wstring::npos)
        return {}; // cannot be quoted on the command line (no path has one)
    std::wstring command = L'"' + program + L'"';
    if (!args.empty())
        command += L' ' + args;
    if (integrity::processElevated())
        return startWithShellToken(std::move(command), cwd);
    // Not elevated: our own rights are the user's (CreateProcessWithTokenW
    // needs a privilege only administrators have, error 1314).
    STARTUPINFOW startup {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION started {};
    if (!::CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            cwd.empty() ? nullptr : cwd.c_str(), &startup, &started))
        return {};
    ::CloseHandle(started.hThread);
    return win32::UniqueHandle(started.hProcess);
}

void recycle(const QStringList& paths, HWND owner, std::function<void(bool)> done)
{
    std::vector<std::wstring> files;
    for (const QString& path : paths)
        files.push_back(QDir::toNativeSeparators(path).toStdWString());
    runOnShellThread([files = std::move(files), owner, done = std::move(done)] {
        bool ok = false;
        {
            ComPtr<IFileOperation> op;
            if (SUCCEEDED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
                // The user already confirmed in the launcher; still warn if an
                // item is too big for the Recycle Bin and would be destroyed.
                op->SetOperationFlags(
                    FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT);
                if (owner)
                    op->SetOwnerWindow(owner);
                bool queued = false;
                for (const std::wstring& file : files) {
                    ComPtr<IShellItem> item;
                    if (SUCCEEDED(::SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item)))
                        && SUCCEEDED(op->DeleteItem(item.Get(), nullptr)))
                        queued = true;
                }
                BOOL aborted = FALSE;
                ok = queued && SUCCEEDED(op->PerformOperations()) && SUCCEEDED(op->GetAnyOperationsAborted(&aborted))
                    && !aborted;
            }
        } // released before COM is uninitialised
        done(ok);
    });
}

void copyText(const QString& text)
{
    QGuiApplication::clipboard()->setText(text);
}

void copyFiles(const QStringList& paths)
{
    // As Explorer copies files: the files only. Qt's way added the paths as
    // text and as file:// URLs, which WPS pasted instead of a copied picture
    // (GitHub #130); 复制完整路径 is there for the text.
    const ComPtr<IDataObject> data = dragout::fileNamesObject(paths);
    if (!data)
        return;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (SUCCEEDED(::OleSetClipboard(data.Get()))) {
            ::OleFlushClipboard(); // on the clipboard itself: kept when Win顺 exits
            return;
        }
        ::Sleep(20); // another program has the clipboard open for a moment
    }
    qWarning("copyFiles: the clipboard stayed busy");
}

bool canRunAsAdministrator(const QString& path)
{
    static const QStringList kRunnable {u"exe"_s, u"bat"_s, u"cmd"_s, u"msi"_s, u"ps1"_s, u"lnk"_s, u"com"_s};
    return kRunnable.contains(QFileInfo(path).suffix(), Qt::CaseInsensitive);
}

namespace {

thread_local std::function<void()> t_menuShown; // popupMenu()'s `shown`

void CALLBACK onMenuShown(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
{
    if (auto shown = std::exchange(t_menuShown, {}))
        shown();
}

} // namespace

namespace {

// A popup menu of `items`, submenus included (destroyed with it).
HMENU buildMenu(const std::vector<MenuItem>& items)
{
    HMENU menu = ::CreatePopupMenu();
    if (!menu)
        return nullptr;
    for (const MenuItem& item : items) {
        if (item.id == 0 && item.text.isEmpty() && item.children.empty()) {
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        UINT flags = MF_STRING;
        if (item.checked)
            flags |= MF_CHECKED;
        if (!item.enabled)
            flags |= MF_GRAYED;
        if (!item.children.empty()) {
            if (HMENU sub = buildMenu(item.children)) {
                ::AppendMenuW(menu, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(sub),
                    reinterpret_cast<LPCWSTR>(item.text.utf16()));
                continue;
            }
        }
        ::AppendMenuW(menu, flags, static_cast<UINT_PTR>(item.id), reinterpret_cast<LPCWSTR>(item.text.utf16()));
        if (item.isDefault)
            ::SetMenuDefaultItem(menu, static_cast<UINT>(item.id), FALSE);
    }
    return menu;
}

} // namespace

int popupMenu(HWND owner, const std::vector<MenuItem>& items, POINT screenPos, std::function<void()> shown)
{
    HMENU menu = buildMenu(items);
    if (!menu)
        return 0;
    // Delivered from the menu's own message loop, as it opens.
    HWINEVENTHOOK hook = nullptr;
    if (shown) {
        t_menuShown = std::move(shown);
        hook = ::SetWinEventHook(EVENT_SYSTEM_MENUPOPUPSTART, EVENT_SYSTEM_MENUPOPUPSTART, nullptr, &onMenuShown,
            ::GetCurrentProcessId(), ::GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
    }
    // Documented requirement for menus owned by a background window (tray).
    ::SetForegroundWindow(owner);
    const int chosen = static_cast<int>(::TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPos.x, screenPos.y, owner, nullptr));
    if (hook)
        ::UnhookWinEvent(hook);
    t_menuShown = {};
    ::PostMessageW(owner, WM_NULL, 0, 0);
    ::DestroyMenu(menu);
    return chosen;
}

void prepareMenuHooks()
{
    // Nilesoft Shell's icon overlay handler hooks TrackPopupMenuEx in every
    // process it is loaded into, and reads its settings on the first call: the
    // tray menu's first open took 350-470 ms instead of 20 (see pitfalls).
    static std::once_flag once;
    std::call_once(once, [] {
        runOnShellThread([] {
            const HMENU menu = ::CreatePopupMenu();
            if (!menu)
                return;
            ::AppendMenuW(menu, MF_STRING, 1, L"-");
            // No owner window: Windows refuses (ERROR_INVALID_WINDOW_HANDLE)
            // once the hooks have run, and nothing is shown.
            ::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_NOANIMATION, 0, 0, nullptr, nullptr);
            ::DestroyMenu(menu);
        });
    });
}

QString pickFolder(HWND owner, const QString& title)
{
    return runPicker([&] {
        const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        QString result;
        {
            ComPtr<IFileOpenDialog> dialog;
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            DWORD options = 0;
            if (SUCCEEDED(
                    ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))
                && SUCCEEDED(dialog->GetOptions(&options))
                && SUCCEEDED(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST))
                && SUCCEEDED(dialog->SetTitle(reinterpret_cast<LPCWSTR>(title.utf16())))
                && SUCCEEDED(showPicker(dialog.Get(), owner)) && SUCCEEDED(dialog->GetResult(&item))
                && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = QString::fromWCharArray(path);
                ::CoTaskMemFree(path);
            }
        } // released before COM is uninitialised
        if (SUCCEEDED(init))
            ::CoUninitialize();
        return result;
    });
}

QString pickFile(HWND owner, const QString& title, const std::vector<std::pair<QString, QString>>& filters)
{
    return runPicker([&] {
        const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        QString result;
        {
            std::vector<std::wstring> names;
            std::vector<std::wstring> patterns;
            for (const auto& [name, pattern] : filters) {
                names.push_back(name.toStdWString());
                patterns.push_back(pattern.toStdWString());
            }
            std::vector<COMDLG_FILTERSPEC> specs;
            for (std::size_t i = 0; i < names.size(); ++i)
                specs.push_back({names[i].c_str(), patterns[i].c_str()});
            ComPtr<IFileOpenDialog> dialog;
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            DWORD options = 0;
            if (SUCCEEDED(
                    ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))
                && SUCCEEDED(dialog->GetOptions(&options))
                && SUCCEEDED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST))
                && (specs.empty() || SUCCEEDED(dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data())))
                && SUCCEEDED(dialog->SetTitle(reinterpret_cast<LPCWSTR>(title.utf16())))
                && SUCCEEDED(showPicker(dialog.Get(), owner)) && SUCCEEDED(dialog->GetResult(&item))
                && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = QString::fromWCharArray(path);
                ::CoTaskMemFree(path);
            }
        } // released before COM is uninitialised
        if (SUCCEEDED(init))
            ::CoUninitialize();
        return result;
    });
}

QString pickSaveFile(
    HWND owner, const QString& title, const QString& fileName, const QString& filterName, const QString& extension)
{
    return runPicker([&] {
        const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        QString result;
        {
            const std::wstring name = filterName.toStdWString();
            const std::wstring pattern = (u"*."_s + extension).toStdWString();
            const COMDLG_FILTERSPEC spec {name.c_str(), pattern.c_str()};
            ComPtr<IFileSaveDialog> dialog;
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            DWORD options = 0;
            if (SUCCEEDED(
                    ::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))
                && SUCCEEDED(dialog->GetOptions(&options))
                && SUCCEEDED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT))
                && SUCCEEDED(dialog->SetFileTypes(1, &spec))
                && SUCCEEDED(dialog->SetDefaultExtension(reinterpret_cast<LPCWSTR>(extension.utf16())))
                && SUCCEEDED(dialog->SetFileName(reinterpret_cast<LPCWSTR>(fileName.utf16())))
                && SUCCEEDED(dialog->SetTitle(reinterpret_cast<LPCWSTR>(title.utf16())))
                && SUCCEEDED(showPicker(dialog.Get(), owner)) && SUCCEEDED(dialog->GetResult(&item))
                && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = QString::fromWCharArray(path);
                ::CoTaskMemFree(path);
            }
        } // released before COM is uninitialised
        if (SUCCEEDED(init))
            ::CoUninitialize();
        return result;
    });
}

} // namespace shell

namespace autostart {

namespace {

// Autostart is a logon task: it can start Win顺 elevated without a UAC
// prompt (the 管理员直读 mode, GitHub #21; Windows skips Run-key entries of
// programs that need elevation), and the taskbar button and the MCP relay
// start Win顺 by running it. Older versions, still called QuickFind, used a
// Run-key value and later a logon task of that name; migrateFromQuickFind()
// moves either over. Where Task Scheduler will not take the task (another
// account's has the name, a policy forbids tasks: GitHub #52), the Run key
// holds a "WinShun" value instead (AutostartRun.h); both are read, both removed.
constexpr auto kRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kTaskName[] = L"WinShun";
constexpr wchar_t kOldTaskName[] = L"QuickFind";
constexpr auto kOldRunValue = u"QuickFind";

struct ComScope {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope()
    {
        if (SUCCEEDED(hr))
            ::CoUninitialize();
    }
};

std::wstring exePath() // also before QCoreApplication exists (migrateFromQuickFind)
{
    wchar_t path[MAX_PATH * 2] {};
    ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return path;
}

std::wstring currentUser() // DOMAIN\user
{
    return (qEnvironmentVariable("USERDOMAIN") + u'\\' + qEnvironmentVariable("USERNAME")).toStdWString();
}

ComPtr<ITaskFolder> taskFolder(ComPtr<ITaskService>& service)
{
    VARIANT none {};
    ComPtr<ITaskFolder> folder;
    if (FAILED(::CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)))
        || FAILED(service->Connect(none, none, none, none)) || FAILED(service->GetFolder(Bstr(L"\\"), &folder)))
        return nullptr;
    return folder;
}

void removeRunKey()
{
    QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    run.remove(QStringView(kOldRunValue).toString());
}

QString runValueName()
{
    return QString::fromWCharArray(kRunValueName);
}

// The program the Run value "WinShun" starts; nothing without one.
std::optional<std::wstring> runValuePath()
{
    const QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    const QString command = run.value(runValueName()).toString();
    if (command.isEmpty())
        return std::nullopt;
    const std::wstring program = runCommandProgram(command.toStdWString());
    return program.empty() ? std::nullopt : std::optional(program);
}

void writeRunValue(bool on)
{
    QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    if (on)
        run.setValue(runValueName(), QString::fromStdWString(runCommand(exePath())));
    else
        run.remove(runValueName());
    run.sync();
}

// The autostart task as registered: the program it starts, and whether it
// starts it elevated. Nothing without an enabled task (or one that cannot
// be read: another user's, registered by the installer of old).
struct Task {
    std::wstring path;
    bool elevated = false;
};

std::optional<Task> readTask()
{
    const ComScope com;
    ComPtr<ITaskService> service;
    const ComPtr<ITaskFolder> folder = taskFolder(service);
    ComPtr<IRegisteredTask> task;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    ComPtr<ITaskDefinition> definition;
    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> exec;
    ComPtr<IPrincipal> principal;
    TASK_RUNLEVEL_TYPE level = TASK_RUNLEVEL_LUA;
    Bstr path;
    if (!folder || FAILED(folder->GetTask(Bstr(kTaskName), &task)) || FAILED(task->get_Enabled(&enabled)) || !enabled
        || FAILED(task->get_Definition(&definition)) || FAILED(definition->get_Actions(&actions))
        || FAILED(actions->get_Item(1, &action)) || FAILED(action.As(&exec)) || FAILED(exec->get_Path(path.out()))
        || !path)
        return std::nullopt;
    if (SUCCEEDED(definition->get_Principal(&principal)) && principal)
        principal->get_RunLevel(&level);
    return Task {std::wstring(path), level == TASK_RUNLEVEL_HIGHEST};
}

std::optional<std::wstring> taskPath()
{
    const std::optional<Task> task = readTask();
    return task ? std::optional(task->path) : std::nullopt;
}

// What starts at logon: the task's program, else the Run value's.
std::optional<std::wstring> startupPath()
{
    const std::optional<std::wstring> task = taskPath();
    return task ? task : runValuePath();
}

bool startsThisCopy(const std::optional<std::wstring>& path)
{
    return path && ::_wcsicmp(path->c_str(), exePath().c_str()) == 0; // a copy elsewhere (another build) does not count
}

// What writeTask() puts for the run level (setElevated); the user's normal
// rights until told otherwise, so that whatever runs before the settings
// are read (migrateFromQuickFind) can write it.
std::atomic<bool> g_elevated {false};

// isEnabled() answers from here: asking Task Scheduler takes 30 ms or more
// the first time (its client loads and connects), too long for the tray
// menu to wait. -1 until known. `g_changes` counts setEnabled() calls, so a
// refresh that read the task before one does not overwrite it.
std::atomic<int> g_enabled {-1};
std::atomic<int> g_changes {0};

bool queryEnabled()
{
    const int changes = g_changes;
    const bool enabled = startsThisCopy(startupPath());
    int unknown = -1;
    if (g_changes == changes)
        g_enabled = enabled;
    else
        g_enabled.compare_exchange_strong(unknown, enabled);
    return enabled;
}

} // namespace

bool isEnabled()
{
    const int known = g_enabled;
    return known >= 0 ? known != 0 : queryEnabled();
}

void refresh(std::function<void()> done)
{
    std::thread([done = std::move(done)] {
        queryEnabled(); // taskPath() initialises COM
        if (done)
            done();
    }).detach();
}

bool isSetUp()
{
    return startupPath().has_value();
}

void adoptIfOrphaned()
{
    const std::optional<std::wstring> path = startupPath();
    if (path && !startsThisCopy(path) && ::GetFileAttributesW(path->c_str()) == INVALID_FILE_ATTRIBUTES)
        setEnabled(true);
    else
        g_enabled = startsThisCopy(path); // known from the start
}

void setElevated(bool elevated)
{
    g_elevated = elevated;
}

bool elevated()
{
    return g_elevated;
}

std::optional<bool> taskElevated()
{
    const std::optional<Task> task = readTask();
    if (!task || !startsThisCopy(task->path))
        return std::nullopt;
    return task->elevated;
}

namespace {

// False when the task could not be registered (enabled) or removed.
bool writeTask(bool enabled)
{
    const ComScope com;
    removeRunKey();
    ComPtr<ITaskService> service;
    const ComPtr<ITaskFolder> folder = taskFolder(service);
    if (!folder)
        return false;
    if (!enabled) {
        // Refused (E_ACCESSDENIED) for a task that starts Win顺 elevated to a
        // process with normal rights (removeAsAdministrator), and for another
        // user's; none at all is fine.
        const HRESULT hr = folder->DeleteTask(Bstr(kTaskName), 0);
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            qWarning("WinShun: cannot remove the autostart task (0x%08lx)", static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    }

    const std::wstring user = currentUser();
    // Asked for by a process without administrator rights, the highest level
    // would be refused outright: the task is written with normal rights
    // instead, so that there is one, and the copy that runs elevated raises
    // it when it next starts (App::reconcileAutostart).
    const bool highest = g_elevated && integrity::processElevated();
    ComPtr<ITaskDefinition> definition;
    ComPtr<IPrincipal> principal;
    ComPtr<ITriggerCollection> triggers;
    ComPtr<ITrigger> trigger;
    ComPtr<ILogonTrigger> logon;
    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> exec;
    if (FAILED(service->NewTask(0, &definition)) || FAILED(definition->get_Principal(&principal))
        || FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN))
        || FAILED(principal->put_RunLevel(highest ? TASK_RUNLEVEL_HIGHEST : TASK_RUNLEVEL_LUA))
        || FAILED(definition->get_Triggers(&triggers)) || FAILED(triggers->Create(TASK_TRIGGER_LOGON, &trigger))
        || FAILED(trigger.As(&logon)) || FAILED(logon->put_UserId(Bstr(user.c_str())))
        || FAILED(definition->get_Actions(&actions)) || FAILED(actions->Create(TASK_ACTION_EXEC, &action))
        || FAILED(action.As(&exec)) || FAILED(exec->put_Path(Bstr(exePath().c_str())))
        || FAILED(exec->put_Arguments(Bstr(L"--background"))))
        return false;
    ComPtr<IRegistrationInfo> info;
    if (SUCCEEDED(definition->get_RegistrationInfo(&info)))
        info->put_Description(Bstr(L"Win顺：登录后在后台启动"));
    ComPtr<ITaskSettings> settings;
    if (SUCCEEDED(definition->get_Settings(&settings))) {
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_ExecutionTimeLimit(Bstr(L"PT0S")); // no time limit: it stays in the tray
        settings->put_Priority(4); // normal; tasks default to below normal
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
    }

    VARIANT userId {};
    userId.vt = VT_BSTR;
    userId.bstrVal = ::SysAllocString(user.c_str());
    VARIANT none {};
    ComPtr<IRegisteredTask> registered;
    const HRESULT hr = folder->RegisterTaskDefinition(Bstr(kTaskName), definition.Get(), TASK_CREATE_OR_UPDATE, userId,
        none, TASK_LOGON_INTERACTIVE_TOKEN, none, &registered);
    ::VariantClear(&userId);
    // Refused to a process that is not elevated when it asks for the highest
    // level (E_ACCESSDENIED), and for a task another user registered.
    if (FAILED(hr))
        qWarning("WinShun: cannot register the autostart task (%s, 0x%08lx)", highest ? "elevated" : "normal rights",
            static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

} // namespace

void setEnabled(bool enabled)
{
    ++g_changes;
    // Off: both go. On: the task, and the Run value only when the task cannot be had.
    if (!enabled) {
        writeTask(false);
        writeRunValue(false);
    } else if (writeTask(true) || startsThisCopy(taskPath())) {
        // Refused over this copy's task that starts it elevated (GitHub #67):
        // that one starts it already, a Run value would start it twice.
        writeRunValue(false);
    } else {
        qWarning("WinShun: autostart through the Run key instead");
        writeRunValue(true);
    }
    queryEnabled(); // what the task says now, written or not
}

void removeAsAdministrator(std::function<void(bool)> done)
{
    wchar_t system[MAX_PATH] {};
    ::GetSystemDirectoryW(system, MAX_PATH);
    ++g_changes;
    shell::runElevated(
        std::wstring(system) + L"\\schtasks.exe", L"/Delete /TN " + std::wstring(kTaskName) + L" /F",
        [done = std::move(done)](std::optional<unsigned long> code) {
            queryEnabled(); // what the task says now
            done(code.has_value());
        },
        SW_HIDE);
}

void migrateFromQuickFind()
{
    const QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    bool wasEnabled = run.contains(QStringView(kOldRunValue).toString());
    {
        const ComScope com;
        ComPtr<ITaskService> service;
        const ComPtr<ITaskFolder> folder = taskFolder(service);
        ComPtr<IRegisteredTask> task;
        if (folder && SUCCEEDED(folder->GetTask(Bstr(kOldTaskName), &task))) {
            VARIANT_BOOL enabled = VARIANT_FALSE;
            wasEnabled = wasEnabled || (SUCCEEDED(task->get_Enabled(&enabled)) && enabled);
            folder->DeleteTask(Bstr(kOldTaskName), 0);
        }
    }
    if (wasEnabled)
        setEnabled(true); // also removes the Run-key value
}

} // namespace autostart

} // namespace ws
