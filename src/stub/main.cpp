// WinShunSearch.exe: the button on the taskbar that opens Win顺 the way
// Windows' own search opens (see src/app/platform/TaskbarSearch.h).
//
// Win顺 runs elevated, and a pinned program that needs elevation asks UAC on
// every click; this one runs with the user's normal rights, so it can be
// pinned. A click starts it: it passes on the right to take the foreground
// (the click gave it that) and sets the event Win顺 waits on, then exits.
// About 10 ms: no Qt, no C runtime DLL, nothing else to load.
//
// --screenclip starts Windows' screen capture as Explorer's Win+Shift+S does,
// for Win顺 once it has taken Win+S over (Explorer then lets go of every
// Win+S key): Win顺 cannot start it itself, the capture would run elevated.
//
// --pin <shortcut> asks Windows to pin this program to the taskbar; the user
// says yes in Windows' own dialog (TaskbarManager, open to desktop programs
// since Windows 11 KB5074105). Win顺 starts it so from its settings, on the
// user's click: Windows wants the program asking in front, asking right
// after a click, and in the Start menu as itself (`shortcut`, which Win顺
// makes sure of, has the AppUserModelID this program sets). Elevated, Win顺
// could not ask for itself, and it is this program that goes on the taskbar.

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Shell.h>

#include <cwchar>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kEventName[] = L"Local\\WinShun.TaskbarSearch"; // taskbar::kEventName
constexpr wchar_t kMessageWindow[] = L"WinShun.MessageWindow"; // MessageWindow.cpp
constexpr wchar_t kAutostartTask[] = L"WinShun"; // autostart:: in Shell.cpp
// Its AppUserModelID, the same on its Start menu shortcut (taskbar::kButtonAppId):
// how Windows knows that the program asking to be pinned is that entry.
constexpr wchar_t kAppId[] = L"LingCore.WinShun.Search";

// What Explorer starts the capture with on Win+Shift+S.
constexpr wchar_t kScreenClip[] = L"ms-screenclip:///?source=HotKey";

struct Bstr {
    explicit Bstr(const wchar_t* text) : value(::SysAllocString(text)) {}
    ~Bstr() { ::SysFreeString(value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR value;
};

std::wstring ownFolder()
{
    wchar_t path[MAX_PATH] {};
    const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    std::wstring folder(path, n);
    folder.resize(folder.find_last_of(L'\\'));
    return folder;
}

// The running Win顺: lets it come to the front and asks it to show or hide
// its window. False when it is not running (or not far enough yet).
bool signalRunning()
{
    const HANDLE event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, kEventName);
    if (!event)
        return false;
    DWORD pid = 0;
    if (const HWND window = ::FindWindowW(kMessageWindow, nullptr))
        ::GetWindowThreadProcessId(window, &pid);
    ::AllowSetForegroundWindow(pid ? pid : ASFW_ANY);
    const bool set = ::SetEvent(event);
    ::CloseHandle(event);
    return set;
}

// Win顺's logon task starts it elevated without asking UAC, and the user may
// run it on demand.
bool runAutostartTask()
{
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    ComPtr<IRegisteredTask> task;
    ComPtr<IRunningTask> running;
    const Bstr rootPath(L"\\");
    const Bstr taskName(kAutostartTask);
    const VARIANT none {};
    return SUCCEEDED(::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)))
        && SUCCEEDED(service->Connect(none, none, none, none)) && SUCCEEDED(service->GetFolder(rootPath.value, &root))
        && SUCCEEDED(root->GetTask(taskName.value, &task)) && SUCCEEDED(task->Run(none, &running));
}

// Not running: started (through its logon task, else the copy next to this
// one, which asks UAC), then asked once it is up. It reads its index first.
void startWinShun()
{
    if (!runAutostartTask()) {
        const std::wstring folder = ownFolder();
        const std::wstring exe = folder + L"\\WinShun.exe";
        if (folder.empty()
            || reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, nullptr, exe.c_str(), L"--background", folder.c_str(),
                   SW_SHOWNORMAL))
                <= 32)
            return;
    }
    for (int waited = 0; waited < 30000; waited += 100) {
        if (signalRunning())
            return;
        ::Sleep(100);
    }
}

// The class Snipping Tool registered for ms-screenclip: ("AppX…"), from its
// package's registration. Started through it, not through the user's choice
// for the protocol, which can be broken: Windows then asks which app to
// open it with (seen on 26200). Empty if Snipping Tool is not installed.
std::wstring screenClipClass()
{
    HKEY packages = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
            0, KEY_READ, &packages)
        != ERROR_SUCCESS)
        return {};
    std::wstring found;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD length = static_cast<DWORD>(std::size(name));
        if (::RegEnumKeyExW(packages, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        if (std::wcsncmp(name, L"Microsoft.ScreenSketch_", 23) != 0)
            continue;
        wchar_t progId[128] {};
        DWORD bytes = sizeof progId - sizeof(wchar_t);
        const std::wstring capabilities = std::wstring(name) + L"\\App\\Capabilities\\URLAssociations";
        if (::RegGetValueW(packages, capabilities.c_str(), L"ms-screenclip", RRF_RT_REG_SZ, nullptr, progId, &bytes)
                == ERROR_SUCCESS
            && progId[0])
            found = progId; // while an update installs, the last one listed
    }
    ::RegCloseKey(packages);
    return found;
}

void screenClip()
{
    const std::wstring progId = screenClipClass();
    SHELLEXECUTEINFOW info {sizeof info};
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpFile = kScreenClip;
    info.nShow = SW_SHOWNORMAL;
    if (!progId.empty()) {
        info.fMask |= SEE_MASK_CLASSNAME;
        info.lpClass = progId.c_str();
    }
    if (::ShellExecuteExW(&info))
        return;
    // Windows 10 without the new Snipping Tool: the old one.
    wchar_t system[MAX_PATH] {};
    const UINT n = ::GetSystemDirectoryW(system, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;
    const std::wstring exe = std::wstring(system, n) + L"\\SnippingTool.exe";
    ::ShellExecuteW(nullptr, nullptr, exe.c_str(), L"/clip", nullptr, SW_SHOWNORMAL);
}

// The result of `operation`, the thread's messages handled meanwhile: it is
// a single-threaded apartment with a window, and the answer may come
// through it. Given up (cancelled, and the default result) after `limitMs`:
// Windows' question is a notification, which the user may leave unanswered.
template <typename Operation> auto awaitResult(const Operation& operation, DWORD limitMs = 10'000)
{
    const ULONGLONG deadline = ::GetTickCount64() + limitMs;
    while (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
        if (::GetTickCount64() > deadline) {
            operation.Cancel();
            return decltype(operation.GetResults()) {};
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    return operation.GetResults();
}

// Where Windows does not ask (before Windows 11, pinning turned off by a
// policy, the Start menu not knowing the shortcut yet): the shortcut,
// selected in File Explorer, for the user to pin.
void showShortcut(const std::wstring& shortcut)
{
    if (PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(shortcut.c_str())) {
        ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ::ILFree(item);
    }
}

void pinToTaskbar(const std::wstring& shortcut)
{
    using winrt::Windows::UI::Shell::ITaskbarManagerDesktopAppSupportStatics;
    using winrt::Windows::UI::Shell::TaskbarManager;
    // In front, as Windows wants the program asking: a window of its own,
    // one transparent pixel under the pointer (the click on Win顺's button
    // gave this program the right to come to the front).
    POINT pointer {};
    ::GetCursorPos(&pointer);
    HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST, L"STATIC", L"Win顺",
        WS_POPUP, pointer.x, pointer.y, 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (window) {
        ::SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
        ::ShowWindow(window, SW_SHOW);
        ::SetForegroundWindow(window);
    }
    bool asked = false;
    try {
        if (winrt::try_get_activation_factory<TaskbarManager, ITaskbarManagerDesktopAppSupportStatics>()) {
            const TaskbarManager manager = TaskbarManager::GetDefault();
            if (awaitResult(manager.IsCurrentAppPinnedAsync())) {
                asked = true; // pinned already
            } else {
                // A shortcut just made, or just given its AppUserModelID, takes
                // the Start menu some 3 s to see; until then pinning is not allowed.
                bool allowed = manager.IsPinningAllowed();
                for (int waited = 0; !allowed && waited < 6000; waited += 250) {
                    ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
                    MSG msg;
                    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                        ::DispatchMessageW(&msg);
                    allowed = manager.IsPinningAllowed();
                }
                if (allowed) {
                    const auto request = manager.RequestPinCurrentAppAsync();
                    // Asked: the foreground goes back, the keyboard with it (the
                    // question waits in a notification, maybe for long).
                    if (window)
                        ::DestroyWindow(std::exchange(window, nullptr));
                    // "No", or no answer, is the user's: nothing more.
                    awaitResult(request, 120'000);
                    asked = true;
                }
            }
        }
    } catch (const winrt::hresult_error&) {
    }
    if (window)
        ::DestroyWindow(window);
    if (!asked)
        showShortcut(shortcut);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ::SetCurrentProcessExplicitAppUserModelID(kAppId);
    int count = 0;
    LPWSTR* args = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    const std::wstring option = count > 1 ? args[1] : L"";
    if (option == L"--screenclip")
        screenClip();
    else if (option == L"--pin" && count > 2)
        pinToTaskbar(args[2]);
    else if (!signalRunning())
        startWinShun();
    ::LocalFree(args);
    if (SUCCEEDED(com))
        ::CoUninitialize();
    return 0;
}
