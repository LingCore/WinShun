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

#include <windows.h>
#include <shellapi.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <cwchar>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kEventName[] = L"Local\\WinShun.TaskbarSearch"; // taskbar::kEventName
constexpr wchar_t kMessageWindow[] = L"WinShun.MessageWindow"; // MessageWindow.cpp
constexpr wchar_t kAutostartTask[] = L"WinShun"; // autostart:: in Shell.cpp

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

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (commandLine && std::wcsstr(commandLine, L"--screenclip"))
        screenClip();
    else if (!signalRunning())
        startWinShun();
    if (SUCCEEDED(com))
        ::CoUninitialize();
    return 0;
}
