#include "Open.h"

#include "Encoding.h"
#include "Messages.h"
#include "Win.h"

#include <ole2.h> // before exdisp.h: WIN32_LEAN_AND_MEAN keeps it out of windows.h

#include <exdisp.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace ws::cli {

namespace {

using Microsoft::WRL::ComPtr;

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

private:
    BSTR m_p = nullptr;
};

// Explorer's own Application object (the shell's IShellDispatch2) of one of
// its windows: the shell view -> the view's background -> Application.
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
        || FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) || FAILED(background.As(&folderView))
        || FAILED(folderView->get_Application(&application)) || FAILED(application.As(&shell)))
        return nullptr;
    return shell;
}

// The desktop's, else any folder window's (an Explorer that has run for long
// can drop its desktop from ShellWindows while its folder windows stay).
ComPtr<IShellDispatch2> explorerApplication()
{
    ComPtr<IShellWindows> windows;
    if (FAILED(::CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))))
        return nullptr;
    VARIANT desktop {};
    desktop.vt = VT_I4;
    desktop.lVal = CSIDL_DESKTOP;
    VARIANT none {};
    long hwnd = 0;
    ComPtr<IDispatch> found;
    if (windows->FindWindowSW(&desktop, &none, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &found) == S_OK && found) {
        if (ComPtr<IShellDispatch2> shell = applicationOf(found.Get()))
            return shell;
    }
    long count = 0;
    if (FAILED(windows->get_Count(&count)))
        return nullptr;
    for (long i = 0; i < count; ++i) {
        VARIANT index {};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> window;
        if (windows->Item(index, &window) == S_OK && window) {
            if (ComPtr<IShellDispatch2> shell = applicationOf(window.Get()))
                return shell;
        }
    }
    return nullptr;
}

// Starts `file` through the desktop shell: Explorer is the caller, with the
// user's normal rights, whatever token ours is.
bool executeThroughExplorer(const std::wstring& file, const std::wstring& dir)
{
    const ComPtr<IShellDispatch2> shell = explorerApplication();
    if (!shell)
        return false;
    const Bstr target(file.c_str());
    const Bstr directory(dir.empty() ? nullptr : dir.c_str());
    VARIANT parameters {};
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

std::wstring folderOf(std::wstring_view path)
{
    const std::size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring_view::npos ? std::wstring() : std::wstring(path.substr(0, slash));
}

bool sameText(std::wstring_view a, std::wstring_view b)
{
    while (a.size() > 3 && (a.back() == L'\\' || a.back() == L'/'))
        a.remove_suffix(1);
    while (b.size() > 3 && (b.back() == L'\\' || b.back() == L'/'))
        b.remove_suffix(1);
    return !a.empty() && ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

bool processElevated()
{
    // The tests: 1 pretends to be elevated, 0 not to be, whatever the token says.
    if (const std::wstring forced = environment(L"WS_TEST_ELEVATED"); !forced.empty())
        return forced != L"0";
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    const Handle owned(token);
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    return ::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated != 0;
}

Result<void> openPath(const std::wstring& path, bool reveal, bool asUser)
{
    const bool throughExplorer = asUser && !reveal && processElevated();
    // The tests': what would be done, written to this file.
    if (const std::wstring record = environment(L"WS_TEST_OPEN"); !record.empty()) {
        const Handle file(::CreateFileW(record.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        const std::string line = utf8((reveal ? L"reveal " : throughExplorer ? L"user-open " : L"open ") + path + L"\n");
        DWORD written = 0;
        if (!file || !::WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr))
            return Failure {Exit::Failed, fmt(msg::kOpenFailed, path, systemMessage(::GetLastError()))};
        return {};
    }
    // The shell wants COM (and no DDE) on the thread that asks it.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Result<void> result;
    if (reveal) {
        PIDLIST_ABSOLUTE item = nullptr;
        HRESULT hr = ::SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr);
        if (SUCCEEDED(hr)) {
            hr = ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            ::CoTaskMemFree(item);
        }
        if (FAILED(hr))
            result = Failure {Exit::Failed, fmt(msg::kOpenFailed, path, systemMessage(static_cast<DWORD>(hr)))};
    } else if (throughExplorer) {
        // A file a search found, and ws runs as administrator: not with our
        // token. A .lnk dropped into an indexed folder the user can write to
        // would otherwise run elevated on the first `ws open`.
        if (!executeThroughExplorer(path, folderOf(path)))
            result = Failure {Exit::Failed, fmt(msg::kOpenNotElevated, path)};
    } else {
        SHELLEXECUTEINFOW info {};
        info.cbSize = sizeof info;
        // ws ends right after: finish here. And never a dialog (an error box on
        // a desktop nobody looks at would hang an AI that runs ws).
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpFile = path.c_str();
        const std::wstring folder = folderOf(path);
        info.lpDirectory = folder.empty() ? nullptr : folder.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!::ShellExecuteExW(&info))
            result = Failure {Exit::Failed, fmt(msg::kOpenFailed, path, systemMessage(::GetLastError()))};
    }
    if (SUCCEEDED(com))
        ::CoUninitialize();
    return result;
}

bool isWinShunImage(std::wstring_view image, std::wstring_view wsExe, std::wstring_view installed)
{
    const std::size_t slash = image.find_last_of(L'\\');
    if (slash == std::wstring_view::npos)
        return false;
    if (!sameText(image.substr(slash + 1), L"WinShun.exe"))
        return false;
    const std::wstring_view imageFolder = image.substr(0, slash);
    // ws.exe is <install>\bin\ws.exe; Win顺 is <install>\WinShun.exe.
    const std::wstring binFolder = folderOf(wsExe);
    if (!binFolder.empty() && sameText(imageFolder, folderOf(binFolder)))
        return true;
    return !installed.empty() && sameText(imageFolder, installed);
}

std::wstring installedFolder()
{
    // The installer's uninstall entry (its AppId, installer/WinShun.iss).
    constexpr wchar_t kKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{C582F95C-EAF2-4AF2-961D-E35C4506652A}_is1";
    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t folder[MAX_PATH * 2] {};
        DWORD size = sizeof folder - sizeof(wchar_t);
        if (::RegGetValueW(root, kKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, folder, &size) == ERROR_SUCCESS && folder[0])
            return folder;
    }
    return {};
}

Result<void> checkWinShunServes(const pipe::LineClient& client)
{
    const DWORD pid = client.serverProcessId();
    if (pid == 0)
        return Failure {Exit::Failed, std::wstring(tr(msg::kServerUnknown))};
    const Handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    wchar_t image[MAX_PATH * 2] {};
    DWORD size = static_cast<DWORD>(sizeof image / sizeof image[0]);
    if (!process || !::QueryFullProcessImageNameW(process.get(), 0, image, &size))
        return Failure {Exit::Failed, std::wstring(tr(msg::kServerUnknown))};
    if (!isWinShunImage(std::wstring_view(image, size), modulePath(), installedFolder()))
        return Failure {Exit::Failed, fmt(msg::kNotWinShun, std::wstring_view(image, size))};
    return {};
}

} // namespace ws::cli
