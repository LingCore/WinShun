#include "Shell.h"

#include "Win32Util.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QSettings>
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

#include <thread>

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

// WinShun always runs elevated, and ShellExecute would start the target
// elevated too. Explorer runs with the user's normal rights, so ask it to
// launch instead: the desktop window -> its shell view -> the view's
// Application object -> ShellExecute. False when there is no Explorer desktop.
bool executeAsUser(const std::wstring& file, const std::wstring& dir)
{
    ComPtr<IShellWindows> windows;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))))
        return false;
    VARIANT desktop {};
    desktop.vt = VT_I4;
    desktop.lVal = CSIDL_DESKTOP;
    VARIANT none {};
    long hwnd = 0;
    ComPtr<IDispatch> found;
    if (windows->FindWindowSW(&desktop, &none, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &found) != S_OK || !found)
        return false;
    ComPtr<IServiceProvider> services;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background;
    ComPtr<IShellFolderViewDual> folderView;
    ComPtr<IDispatch> application;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(found.As(&services)) || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))
        || FAILED(browser->QueryActiveShellView(&view))
        || FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) || FAILED(background.As(&folderView))
        || FAILED(folderView->get_Application(&application)) || FAILED(application.As(&shell)))
        return false;

    const Bstr target(file.c_str());
    const Bstr directory(dir.empty() ? nullptr : dir.c_str());
    VARIANT args {};
    VARIANT workDir {};
    if (directory) {
        workDir.vt = VT_BSTR;
        workDir.bstrVal = directory; // still owned by `directory`
    }
    VARIANT verb {};
    VARIANT show {};
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    return SUCCEEDED(shell->ShellExecute(target, args, workDir, verb, show));
}

// Without an Explorer desktop to ask (another shell is in use), start the
// target with the desktop shell's own token: the user's normal rights. The
// unelevated rundll32 hands it to ShellExecute, which opens documents,
// folders, programs and links alike. False when there is no shell at all.
bool executeWithShellToken(const std::wstring& file, const std::wstring& dir)
{
    if (file.find(L'"') != std::wstring::npos)
        return false; // cannot be quoted on the command line (no path has one)
    DWORD pid = 0;
    const HWND shellWindow = ::GetShellWindow();
    if (!shellWindow || !::GetWindowThreadProcessId(shellWindow, &pid) || pid == 0)
        return false;
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE shellToken = nullptr;
    if (!process.valid() || !::OpenProcessToken(process.get(), TOKEN_DUPLICATE, &shellToken))
        return false;
    const win32::UniqueHandle source(shellToken);
    HANDLE primaryToken = nullptr;
    if (!::DuplicateTokenEx(source.get(),
            TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, nullptr,
            SecurityImpersonation, TokenPrimary, &primaryToken))
        return false;
    const win32::UniqueHandle token(primaryToken);

    wchar_t system[MAX_PATH] = {};
    const UINT n = ::GetSystemDirectoryW(system, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    std::wstring command
        = L"\"" + std::wstring(system, n) + L"\\rundll32.exe\" shell32.dll,ShellExec_RunDLL \"" + file + L'"';
    STARTUPINFOW startup {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION started {};
    if (!::CreateProcessWithTokenW(token.get(), 0, nullptr, command.data(), 0, nullptr,
            dir.empty() ? nullptr : dir.c_str(), &startup, &started))
        return false;
    ::CloseHandle(started.hThread);
    ::CloseHandle(started.hProcess);
    return true;
}

// Starts `file` with the user's normal rights, never elevated: through
// Explorer (waiting a few seconds if it is restarting), else with the token
// of another shell. False when neither works.
bool executeUnelevated(const std::wstring& file, const std::wstring& dir)
{
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (executeAsUser(file, dir))
            return true;
        if (attempt == 0 && executeWithShellToken(file, dir))
            return true;
        ::Sleep(300);
    }
    return false;
}

bool executeAsAdministrator(const std::wstring& file, const std::wstring& dir)
{
    SHELLEXECUTEINFOW sei {sizeof(SHELLEXECUTEINFOW)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_LOG_USAGE;
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ::ShellExecuteExW(&sei);
}

} // namespace

void open(const QString& path, bool asAdministrator, std::function<void(bool)> done)
{
    ::AllowSetForegroundWindow(ASFW_ANY); // the launched app may take focus from us
    const QString native = QDir::toNativeSeparators(path);
    const QFileInfo info(native);
    const QString workDir = info.isDir() ? QString() : QDir::toNativeSeparators(info.absolutePath());
    runOnShellThread(
        [file = native.toStdWString(), dir = workDir.toStdWString(), asAdministrator, done = std::move(done)] {
            const bool ok = asAdministrator ? executeAsAdministrator(file, dir) : executeUnelevated(file, dir);
            if (done)
                done(ok);
        });
}

void openUrl(const QString& url)
{
    ::AllowSetForegroundWindow(ASFW_ANY);
    runOnShellThread([target = url.toStdWString()] { executeUnelevated(target, {}); });
}

void launchApp(const QString& launchPath, bool asAdministrator, std::function<void(bool)> done)
{
    ::AllowSetForegroundWindow(ASFW_ANY);
    runOnShellThread([target = launchPath.toStdWString(), asAdministrator, done = std::move(done)] {
        // Store apps refuse to start from an elevated process; Explorer can
        // start them. "runas" is the app's own verb, as in the Start menu.
        const bool ok = asAdministrator ? executeAsAdministrator(target, {}) : executeUnelevated(target, {});
        if (done)
            done(ok);
    });
}

void reveal(const QString& path)
{
    ::AllowSetForegroundWindow(ASFW_ANY);
    runOnShellThread([file = QDir::toNativeSeparators(path).toStdWString()] {
        if (PIDLIST_ABSOLUTE pidl = ::ILCreateFromPathW(file.c_str())) {
            ::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ::ILFree(pidl);
        }
    });
}

void recycle(const QString& path, HWND owner, std::function<void(bool)> done)
{
    runOnShellThread([file = QDir::toNativeSeparators(path).toStdWString(), owner, done = std::move(done)] {
        bool ok = false;
        {
            ComPtr<IFileOperation> op;
            ComPtr<IShellItem> item;
            if (SUCCEEDED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))
                && SUCCEEDED(::SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                // The user already confirmed in the launcher; still warn if the
                // item is too big for the Recycle Bin and would be destroyed.
                op->SetOperationFlags(
                    FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT);
                if (owner)
                    op->SetOwnerWindow(owner);
                BOOL aborted = FALSE;
                ok = SUCCEEDED(op->DeleteItem(item.Get(), nullptr)) && SUCCEEDED(op->PerformOperations())
                    && SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && !aborted;
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
    auto* mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString& p : paths)
        urls.append(QUrl::fromLocalFile(p));
    mime->setUrls(urls);
    mime->setText(paths.join(u'\n'));
    // Tell Explorer this is a copy, not a cut.
    const DWORD effect = DROPEFFECT_COPY;
    mime->setData(u"application/x-qt-windows-mime;value=\"Preferred DropEffect\""_s,
        QByteArray(reinterpret_cast<const char*>(&effect), sizeof effect));
    QGuiApplication::clipboard()->setMimeData(mime);
}

bool canRunAsAdministrator(const QString& path)
{
    static const QStringList kRunnable {u"exe"_s, u"bat"_s, u"cmd"_s, u"msi"_s, u"ps1"_s, u"lnk"_s, u"com"_s};
    return kRunnable.contains(QFileInfo(path).suffix(), Qt::CaseInsensitive);
}

int popupMenu(HWND owner, const std::vector<MenuItem>& items, POINT screenPos)
{
    HMENU menu = ::CreatePopupMenu();
    if (!menu)
        return 0;
    for (const MenuItem& item : items) {
        if (item.id == 0 && item.text.isEmpty()) {
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        UINT flags = MF_STRING;
        if (item.checked)
            flags |= MF_CHECKED;
        if (!item.enabled)
            flags |= MF_GRAYED;
        ::AppendMenuW(menu, flags, static_cast<UINT_PTR>(item.id), reinterpret_cast<LPCWSTR>(item.text.utf16()));
        if (item.isDefault)
            ::SetMenuDefaultItem(menu, static_cast<UINT>(item.id), FALSE);
    }
    // Documented requirement for menus owned by a background window (tray).
    ::SetForegroundWindow(owner);
    const int chosen = static_cast<int>(::TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPos.x, screenPos.y, owner, nullptr));
    ::PostMessageW(owner, WM_NULL, 0, 0);
    ::DestroyMenu(menu);
    return chosen;
}

QString pickFolder(HWND owner, const QString& title)
{
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    QString result;
    {
        ComPtr<IFileOpenDialog> dialog;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        DWORD options = 0;
        if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))
            && SUCCEEDED(dialog->GetOptions(&options))
            && SUCCEEDED(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST))
            && SUCCEEDED(dialog->SetTitle(reinterpret_cast<LPCWSTR>(title.utf16()))) && SUCCEEDED(dialog->Show(owner))
            && SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            result = QString::fromWCharArray(path);
            ::CoTaskMemFree(path);
        }
    } // released before COM is uninitialised
    if (SUCCEEDED(init))
        ::CoUninitialize();
    return result;
}

} // namespace shell

namespace autostart {

namespace {

// Windows skips Run-key entries of programs that need elevation, so autostart
// is a logon task that runs with highest privileges (no UAC prompt). Older
// versions, still called QuickFind, used a Run-key value and later a logon
// task of that name; migrateFromQuickFind() moves either over.
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

} // namespace

bool isEnabled()
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
    Bstr path;
    if (!folder || FAILED(folder->GetTask(Bstr(kTaskName), &task)) || FAILED(task->get_Enabled(&enabled)) || !enabled
        || FAILED(task->get_Definition(&definition)) || FAILED(definition->get_Actions(&actions))
        || FAILED(actions->get_Item(1, &action)) || FAILED(action.As(&exec)) || FAILED(exec->get_Path(path.out())) || !path)
        return false;
    return ::_wcsicmp(path, exePath().c_str()) == 0; // a copy elsewhere (another build) does not count
}

void setEnabled(bool enabled)
{
    const ComScope com;
    removeRunKey();
    ComPtr<ITaskService> service;
    const ComPtr<ITaskFolder> folder = taskFolder(service);
    if (!folder)
        return;
    if (!enabled) {
        folder->DeleteTask(Bstr(kTaskName), 0);
        return;
    }

    const std::wstring user = currentUser();
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
        || FAILED(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST)) || FAILED(definition->get_Triggers(&triggers))
        || FAILED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) || FAILED(trigger.As(&logon))
        || FAILED(logon->put_UserId(Bstr(user.c_str()))) || FAILED(definition->get_Actions(&actions))
        || FAILED(actions->Create(TASK_ACTION_EXEC, &action)) || FAILED(action.As(&exec))
        || FAILED(exec->put_Path(Bstr(exePath().c_str()))) || FAILED(exec->put_Arguments(Bstr(L"--background"))))
        return;
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
    folder->RegisterTaskDefinition(Bstr(kTaskName), definition.Get(), TASK_CREATE_OR_UPDATE, userId, none,
        TASK_LOGON_INTERACTIVE_TOKEN, none, &registered);
    ::VariantClear(&userId);
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
