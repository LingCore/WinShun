// The shell's file dialogs, popup menus and the clipboard's copy of files and
// text: built into WinShun and into the screen capture plugin
// (src/capture-plugin), which needs these and none of the rest of Shell.cpp.

#include "Shell.h"

#include "DragOut.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QGuiApplication>
#include <QThread>

#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <atomic>
#include <functional>
#include <thread>
#include <utility>

using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace ws::shell {

namespace {

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

} // namespace

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

QString pickFolder(HWND owner, const QString& title, const QString& start)
{
    return runPicker([&] {
        const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        QString result;
        {
            ComPtr<IFileOpenDialog> dialog;
            ComPtr<IShellItem> item;
            ComPtr<IShellItem> folder;
            PWSTR path = nullptr;
            DWORD options = 0;
            const auto startIn = [&] { // a folder gone: where it was last
                if (!start.isEmpty()
                    && SUCCEEDED(::SHCreateItemFromParsingName(
                        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(start).utf16()), nullptr, IID_PPV_ARGS(&folder))))
                    dialog->SetFolder(folder.Get());
                return true;
            };
            if (SUCCEEDED(
                    ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))
                && SUCCEEDED(dialog->GetOptions(&options))
                && SUCCEEDED(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST))
                && SUCCEEDED(dialog->SetTitle(reinterpret_cast<LPCWSTR>(title.utf16()))) && startIn()
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

} // namespace ws::shell
