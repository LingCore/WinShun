#include "FileManagers.h"

#include "PathText.h"
#include "Shell.h"
#include "Win32Util.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <ole2.h> // before exdisp.h: WIN32_LEAN_AND_MEAN keeps it out of windows.h

#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace ws::filemanager {

namespace {

constexpr UINT kTotalCommanderAsk = WM_USER + 50; // its interface for other programs

// A window's own text, as Windows keeps it: no message to the program.
std::wstring caption(HWND hwnd)
{
    std::wstring text(static_cast<std::size_t>(::GetWindowTextLengthW(hwnd)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(::GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()))));
    return text;
}

bool isFolder(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool isFile(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring quoted(const std::wstring& path)
{
    return L'"' + path + L'"';
}

std::wstring folderOf(const std::wstring& file)
{
    const std::size_t slash = file.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : file.substr(0, slash);
}

// The program file of the process a window belongs to.
std::wstring programOf(HWND window)
{
    DWORD pid = 0;
    if (!window || !::GetWindowThreadProcessId(window, &pid) || pid == 0)
        return {};
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!process.valid() || !::QueryFullProcessImageNameW(process.get(), 0, path.data(), &size))
        return {};
    path.resize(size);
    return path;
}

// A text value, %VARIABLES% expanded; empty when there is none. `flags`:
// which registry view (RRF_SUBKEY_WOW6432KEY for a 32-bit program's).
std::wstring registryText(HKEY root, const wchar_t* key, const wchar_t* value, DWORD flags = 0)
{
    flags |= RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    DWORD size = 0;
    if (::RegGetValueW(root, key, value, flags, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t))
        return {};
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, key, value, flags, nullptr, text.data(), &size) != ERROR_SUCCESS)
        return {};
    text.resize(::wcsnlen(text.c_str(), text.size()));
    return text;
}

std::wstring totalCommanderProgram()
{
    if (const HWND running = ::FindWindowW(L"TTOTAL_CMD", nullptr)) {
        if (std::wstring program = programOf(running); !program.empty())
            return program;
    }
    constexpr const wchar_t* kKey = L"Software\\Ghisler\\Total Commander";
    for (const std::wstring& dir : {registryText(HKEY_CURRENT_USER, kKey, L"InstallDir"),
             registryText(HKEY_LOCAL_MACHINE, kKey, L"InstallDir", RRF_SUBKEY_WOW6464KEY),
             registryText(HKEY_LOCAL_MACHINE, kKey, L"InstallDir", RRF_SUBKEY_WOW6432KEY)}) {
        if (dir.empty())
            continue;
        for (const wchar_t* name : {L"\\TOTALCMD64.EXE", L"\\TOTALCMD.EXE"}) {
            if (isFile(dir + name))
                return dir + name;
        }
    }
    return {};
}

std::wstring opusProgram()
{
    std::wstring dopus = programOf(::FindWindowW(L"dopus.lister", nullptr));
    constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\dopus.exe";
    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        if (dopus.empty())
            dopus = registryText(root, kKey, nullptr);
    }
    if (dopus.size() >= 2 && dopus.front() == L'"' && dopus.back() == L'"')
        dopus = dopus.substr(1, dopus.size() - 2);
    const std::wstring rt = folderOf(dopus) + L"\\dopusrt.exe";
    return !dopus.empty() && isFile(rt) ? rt : std::wstring();
}

// Starts `program` with the user's normal rights. `wait`: until it has
// handed its command over, so that the next one comes after it (a program
// that keeps running, a Total Commander started this way, is waited for a
// few seconds).
bool run(const std::wstring& program, const std::wstring& args, bool wait)
{
    const win32::UniqueHandle process = shell::startUnelevated(program, args);
    if (!process.valid()) {
        qWarning() << "Could not start" << QString::fromStdWString(program);
        return false;
    }
    if (wait)
        ::WaitForSingleObject(process.get(), 3000);
    return true;
}

// The folder on disk that an Explorer tab shows; empty for Home, This PC, a
// search and the like.
std::wstring folderPath(IShellBrowser* browser)
{
    ComPtr<IShellView> view;
    ComPtr<IFolderView> folderView;
    ComPtr<IPersistFolder2> folder;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folderView))
        || FAILED(folderView->GetFolder(IID_PPV_ARGS(&folder))) || FAILED(folder->GetCurFolder(&pidl)) || !pidl)
        return {};
    std::wstring path;
    PWSTR name = nullptr;
    if (SUCCEEDED(::SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &name))) {
        path = name;
        ::CoTaskMemFree(name);
    }
    ::ILFree(pidl);
    return path;
}

struct ExplorerTab {
    HWND window;
    HWND tab;
    std::wstring path;
};

// Each tab of each Explorer window that shows a folder on disk.
std::vector<ExplorerTab> explorerTabs()
{
    ComPtr<IShellWindows> windows;
    long count = 0;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))
        || FAILED(windows->get_Count(&count)))
        return {};
    std::vector<ExplorerTab> tabs;
    for (long i = 0; i < count; ++i) {
        VARIANT index {};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> browser;
        HWND tab = nullptr;
        if (windows->Item(index, &item) != S_OK || !item || FAILED(item.As(&services))
            || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))
            || FAILED(browser->GetWindow(&tab)) || !tab)
            continue;
        std::wstring path = folderPath(browser.Get());
        if (!path.empty())
            tabs.push_back({::GetAncestor(tab, GA_ROOT), tab, std::move(path)});
    }
    return tabs;
}

// The folders a Total Commander window shows: the panel with the focus first.
std::vector<std::wstring> totalCommanderFolders(HWND window)
{
    DWORD_PTR active = 0; // 1: the left panel, 2: the right one
    if (!::SendMessageTimeoutW(window, kTotalCommanderAsk, 1000, 0, SMTO_ABORTIFHUNG, 1000, &active))
        return {};
    std::vector<std::wstring> folders;
    // The text over the left file list (9), the right one (10): "c:\Windows\*.*".
    for (const WPARAM side : active == 2 ? std::initializer_list<WPARAM> {10, 9} : std::initializer_list<WPARAM> {9, 10}) {
        DWORD_PTR label = 0;
        if (!::SendMessageTimeoutW(window, kTotalCommanderAsk, side, 0, SMTO_ABORTIFHUNG, 1000, &label) || !label)
            continue;
        const std::wstring folder = pathtext::folderFromTotalCommander(
            QString::fromStdWString(caption(reinterpret_cast<HWND>(label)))).toStdWString();
        if (!folder.empty() && isFolder(folder)) // not inside an archive
            folders.push_back(folder);
    }
    return folders;
}

// The tabs of Directory Opus's windows; none while it has no window open:
// asking would start it.
QList<pathtext::OpusTab> opusTabs()
{
    if (!::FindWindowW(L"dopus.lister", nullptr))
        return {};
    const std::wstring rt = opusProgram();
    wchar_t temp[MAX_PATH + 1] {};
    const DWORD n = ::GetTempPathW(MAX_PATH + 1, temp);
    if (rt.empty() || n == 0 || n > MAX_PATH)
        return {};
    static std::atomic<unsigned> asked {0};
    const std::wstring file = std::wstring(temp, n) + L"WinShun-opus-" + std::to_wstring(::GetCurrentProcessId()) + L'-'
        + std::to_wstring(++asked) + L".xml";
    // It answers in a file, written by the time dopusrt.exe ends (some 50 ms).
    const win32::UniqueHandle process = shell::startUnelevated(rt, L"/info " + quoted(file) + L",paths");
    if (!process.valid())
        return {};
    if (::WaitForSingleObject(process.get(), 3000) != WAIT_OBJECT_0) {
        ::TerminateProcess(process.get(), 1);
        ::DeleteFileW(file.c_str());
        qWarning() << "Directory Opus did not say which folders it shows";
        return {};
    }
    QFile answer(QString::fromStdWString(file));
    const QByteArray xml = answer.open(QIODevice::ReadOnly) ? answer.readAll() : QByteArray();
    answer.close();
    answer.remove();
    QList<pathtext::OpusTab> tabs = pathtext::opusTabs(xml);
    tabs.removeIf([](const pathtext::OpusTab& tab) { return !isFolder(tab.path.toStdWString()); }); // archives
    return tabs;
}

} // namespace

std::optional<Kind> kindOf(HWND window)
{
    wchar_t name[32] {};
    if (!window || ::GetClassNameW(window, name, static_cast<int>(std::size(name))) == 0)
        return std::nullopt;
    if (::wcscmp(name, L"CabinetWClass") == 0)
        return Kind::Explorer;
    if (::wcscmp(name, L"TTOTAL_CMD") == 0)
        return Kind::TotalCommander;
    if (::wcscmp(name, L"dopus.lister") == 0)
        return Kind::DirectoryOpus;
    return std::nullopt;
}

std::vector<Folder> openFolders()
{
    struct Search {
        std::vector<ExplorerTab> explorer;
        QList<pathtext::OpusTab> opus;
        std::vector<Folder> onTop; // each window's folder in front
        std::vector<Folder> behind; // the others
    } search {explorerTabs(), opusTabs(), {}, {}};
    // Top-level windows come front to back, and so do a window's children.
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(param);
            // Explorer: Windows 11 has a child per tab (all of them visible), the tab on top first.
            bool first = true;
            for (HWND child = ::FindWindowExW(hwnd, nullptr, L"ShellTabWindowClass", nullptr); child;
                child = ::FindWindowExW(hwnd, child, L"ShellTabWindowClass", nullptr)) {
                for (const ExplorerTab& tab : search.explorer) {
                    if (tab.tab == child)
                        (first ? search.onTop : search.behind).push_back({tab.path, Kind::Explorer});
                }
                first = false;
            }
            for (const ExplorerTab& tab : search.explorer) { // a window without such children
                if (tab.window == hwnd && tab.tab == hwnd)
                    search.onTop.push_back({tab.path, Kind::Explorer});
            }
            const std::optional<Kind> kind = kindOf(hwnd);
            if (kind == Kind::TotalCommander && ::IsWindowVisible(hwnd)) {
                first = true;
                for (std::wstring& folder : totalCommanderFolders(hwnd))
                    (std::exchange(first, false) ? search.onTop : search.behind).push_back({std::move(folder), *kind});
            } else if (kind == Kind::DirectoryOpus) {
                // The tab in front on the side with the focus, the other side's, the tabs behind.
                for (const int state : {1, 2, 0}) {
                    for (const pathtext::OpusTab& tab : std::as_const(search.opus)) {
                        if (tab.lister == reinterpret_cast<quintptr>(hwnd) && tab.state == state)
                            (state == 1 ? search.onTop : search.behind).push_back({tab.path.toStdWString(), *kind});
                    }
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    std::vector<Folder> folders;
    for (const auto* list : {&search.onTop, &search.behind}) {
        for (const Folder& folder : *list) {
            if (std::ranges::none_of(folders, [&](const Folder& f) { return ::_wcsicmp(f.path.c_str(), folder.path.c_str()) == 0; }))
                folders.push_back(folder);
        }
    }
    return folders;
}

std::wstring program(Kind kind)
{
    switch (kind) {
    case Kind::TotalCommander:
        return totalCommanderProgram();
    case Kind::DirectoryOpus:
        return opusProgram();
    case Kind::Explorer:
        break;
    }
    return {};
}

bool installed(Kind kind)
{
    return kind == Kind::Explorer || !program(kind).empty();
}

bool openFolder(Kind kind, const std::wstring& folder)
{
    const std::wstring exe = program(kind);
    if (exe.empty())
        return false;
    switch (kind) {
    case Kind::TotalCommander: // in the copy that runs (/O), the panel with the focus (/S: /L is that one)
        return run(exe, L"/O /T /S /L=" + quoted(folder), false);
    case Kind::DirectoryOpus:
        return run(exe, L"/acmd Go " + quoted(folder) + L" NEWTAB=findexisting,tofront", false);
    case Kind::Explorer:
        break;
    }
    return false;
}

bool reveal(Kind kind, const std::vector<std::wstring>& paths)
{
    const std::wstring exe = program(kind);
    if (exe.empty() || kind == Kind::Explorer)
        return false;
    std::vector<std::wstring> folders; // shown already (Total Commander)
    bool shown = false;
    for (const std::wstring& path : paths) {
        const QFileInfo info(QString::fromStdWString(path));
        const bool root = info.isRoot(); // a drive: nothing above it
        std::wstring args;
        if (kind == Kind::TotalCommander) {
            const std::wstring folder = QDir::toNativeSeparators(info.path()).toStdWString();
            if (std::ranges::any_of(folders, [&](const std::wstring& f) { return ::_wcsicmp(f.c_str(), folder.c_str()) == 0; }))
                continue;
            folders.push_back(folder);
            // A file: its folder with the cursor on it. A folder: /P, the one above with the cursor on it.
            args = std::wstring(L"/O /T /S ") + (info.isDir() && !root ? L"/P " : L"") + L"/L=" + quoted(path);
        } else {
            args = L"/acmd Go " + quoted(path) + (root ? L"" : L" OPENCONTAINER") + L" NEWTAB=findexisting,tofront";
        }
        if (!run(exe, args, paths.size() > 1))
            return shown;
        shown = true;
    }
    return shown;
}

} // namespace ws::filemanager
