#include "Shell.h"

#include <QClipboard>
#include <QCoreApplication>
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

namespace qf::shell {

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

// QuickFind always runs elevated, and ShellExecute would start the target
// elevated too. Explorer runs with the user's normal rights, so ask it to
// launch instead: the desktop window -> its shell view -> the view's
// Application object -> ShellExecute. False when there is no Explorer desktop.
bool executeAsUser(const std::wstring& file, const std::wstring& dir)
{
    using Microsoft::WRL::ComPtr;
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

    BSTR target = ::SysAllocString(file.c_str());
    VARIANT args {};
    VARIANT workDir {};
    if (!dir.empty()) {
        workDir.vt = VT_BSTR;
        workDir.bstrVal = ::SysAllocString(dir.c_str());
    }
    VARIANT verb {};
    VARIANT show {};
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    const HRESULT hr = shell->ShellExecute(target, args, workDir, verb, show);
    ::SysFreeString(target);
    ::VariantClear(&workDir);
    return SUCCEEDED(hr);
}

} // namespace

void open(const QString& path, bool asAdministrator)
{
    ::AllowSetForegroundWindow(ASFW_ANY); // the launched app may take focus from us
    const QString native = QDir::toNativeSeparators(path);
    const QFileInfo info(native);
    const QString workDir = info.isDir() ? QString() : QDir::toNativeSeparators(info.absolutePath());
    runOnShellThread([file = native.toStdWString(), dir = workDir.toStdWString(), asAdministrator] {
        if (!asAdministrator && executeAsUser(file, dir))
            return;
        // Run as administrator, or no Explorer to delegate to (another shell).
        SHELLEXECUTEINFOW sei {sizeof(SHELLEXECUTEINFOW)};
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_LOG_USAGE;
        sei.lpVerb = asAdministrator ? L"runas" : nullptr;
        sei.lpFile = file.c_str();
        sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
        sei.nShow = SW_SHOWNORMAL;
        ::ShellExecuteExW(&sei);
    });
}

void openUrl(const QString& url)
{
    ::AllowSetForegroundWindow(ASFW_ANY);
    runOnShellThread([target = url.toStdWString()] {
        if (executeAsUser(target, {}))
            return;
        ::ShellExecuteW(nullptr, nullptr, target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
}

void launchApp(const QString& launchPath, bool asAdministrator)
{
    ::AllowSetForegroundWindow(ASFW_ANY);
    runOnShellThread([target = launchPath.toStdWString(), asAdministrator] {
        // Store apps refuse to start from an elevated process; Explorer can start them.
        if (!asAdministrator && executeAsUser(target, {}))
            return;
        SHELLEXECUTEINFOW sei {sizeof(SHELLEXECUTEINFOW)};
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_LOG_USAGE;
        sei.lpVerb = asAdministrator ? L"runas" : nullptr; // the app's own verb, as in the Start menu
        sei.lpFile = target.c_str();
        sei.nShow = SW_SHOWNORMAL;
        ::ShellExecuteExW(&sei);
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
        IFileOperation* op = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
            // The user already confirmed in the launcher; still warn if the
            // item is too big for the Recycle Bin and would be destroyed.
            op->SetOperationFlags(
                FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT);
            if (owner)
                op->SetOwnerWindow(owner);
            IShellItem* item = nullptr;
            if (SUCCEEDED(::SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                BOOL aborted = FALSE;
                ok = SUCCEEDED(op->DeleteItem(item, nullptr)) && SUCCEEDED(op->PerformOperations())
                    && SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && !aborted;
                item->Release();
            }
            op->Release();
        }
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
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(reinterpret_cast<LPCWSTR>(title.utf16()));
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = QString::fromWCharArray(path);
                ::CoTaskMemFree(path);
            }
            item->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(init))
        ::CoUninitialize();
    return result;
}

} // namespace qf::shell

namespace qf::autostart {

namespace {

using Microsoft::WRL::ComPtr;

// Windows skips Run-key entries of programs that need elevation, so autostart
// is a logon task that runs with highest privileges (no UAC prompt). Older
// versions used the Run key; migrate() moves that setting over.
constexpr auto kRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kTaskName[] = L"QuickFind";

struct ComScope {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope()
    {
        if (SUCCEEDED(hr))
            ::CoUninitialize();
    }
};

class Bstr {
public:
    Bstr() = default;
    explicit Bstr(const wchar_t* s)
        : m_p(::SysAllocString(s))
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

std::wstring exePath()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
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
    run.remove(u"QuickFind"_s);
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
        info->put_Description(Bstr(L"快搜：登录后在后台启动"));
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

void migrate()
{
    const QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    if (run.contains(u"QuickFind"_s))
        setEnabled(true);
}

} // namespace qf::autostart
