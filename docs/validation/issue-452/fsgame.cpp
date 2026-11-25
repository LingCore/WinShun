// A stand-in for a game in exclusive full screen (GitHub #452): covers the
// primary monitor, asks DXGI for exclusive full screen, and when another
// program's window is shown over it leaves full screen and minimizes itself,
// as such games do. Where DXGI gives no exclusive full screen (a VM), it
// watches for a topmost window over it instead and minimizes just the same.
// Everything is written to the log given as the first argument.
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <shellapi.h>
#include <cstdarg>
#include <cstdio>
#include <initializer_list>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

static FILE* g_log = nullptr;
static ULONGLONG g_start = 0;
static HWND g_window = nullptr;
static IDXGISwapChain* g_chain = nullptr;
static bool g_exclusive = false;
static bool g_minimized = false;
static HWND g_before[256];
static int g_beforeCount = -1; // -1: still collecting what was there as it started

static void say(const char* format, ...)
{
    if (!g_log)
        return;
    std::fprintf(g_log, "%6llu ", GetTickCount64() - g_start);
    va_list args;
    va_start(args, format);
    std::vfprintf(g_log, format, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

static void leave(const char* why)
{
    if (g_minimized)
        return;
    g_minimized = true;
    say("leaving full screen: %s", why);
    if (g_chain && g_exclusive)
        g_chain->SetFullscreenState(FALSE, nullptr);
    ShowWindow(g_window, SW_MINIMIZE);
}

// A visible, uncloaked topmost window of another program over this one.
static BOOL CALLBACK overUs(HWND hwnd, LPARAM found)
{
    if (hwnd == g_window || !IsWindowVisible(hwnd) || !(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId())
        return TRUE;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
        return TRUE;
    wchar_t name[64] {};
    GetClassNameW(hwnd, name, 64);
    if (!wcscmp(name, L"Shell_TrayWnd") || !wcscmp(name, L"Shell_SecondaryTrayWnd"))
        return TRUE;
    RECT rect {};
    if (!GetWindowRect(hwnd, &rect) || rect.right - rect.left < 50 || rect.bottom - rect.top < 50)
        return TRUE;
    if (g_beforeCount < 0) { // there before it: not shown over it
        HWND* list = reinterpret_cast<HWND*>(found);
        for (int i = 0; i < 256; ++i)
            if (!list[i]) { list[i] = hwnd; break; }
        return TRUE;
    }
    for (int i = 0; i < g_beforeCount; ++i)
        if (g_before[i] == hwnd)
            return TRUE;
    *reinterpret_cast<HWND*>(found) = hwnd;
    return FALSE;
}

static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_ACTIVATE:
        say("WM_ACTIVATE %d", static_cast<int>(LOWORD(wParam)));
        break;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            say("minimized");
        else if (wParam == SIZE_RESTORED && g_minimized) {
            say("restored");
            g_minimized = false;
        }
        break;
    case WM_CHAR:
        say("WM_CHAR %u", static_cast<unsigned>(wParam));
        break;
    case WM_KEYDOWN:
        say("WM_KEYDOWN %u", static_cast<unsigned>(wParam));
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    g_start = GetTickCount64();
    if (count > 1)
        _wfopen_s(&g_log, arguments[1], L"w");
    const bool simulateOnly = count > 2 && !wcscmp(arguments[2], L"sim");

    WNDCLASSW cls {};
    cls.lpfnWndProc = proc;
    cls.hInstance = instance;
    cls.lpszClassName = L"FsGame";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = CreateSolidBrush(RGB(20, 90, 40));
    RegisterClassW(&cls);
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    g_window = CreateWindowExW(0, L"FsGame", L"FsGame", WS_POPUP | WS_VISIBLE, 0, 0, width, height, nullptr, nullptr,
        instance, nullptr);
    SetForegroundWindow(g_window);
    say("window %dx%d", width, height);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    if (!simulateOnly) {
        DXGI_SWAP_CHAIN_DESC desc {};
        desc.BufferCount = 2;
        desc.BufferDesc.Width = static_cast<UINT>(width);
        desc.BufferDesc.Height = static_cast<UINT>(height);
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = g_window;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        HRESULT hr = E_FAIL;
        for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &g_chain,
                &device, nullptr, &context);
            say("device type %d: 0x%08lX", static_cast<int>(type), static_cast<unsigned long>(hr));
            if (SUCCEEDED(hr))
                break;
        }
        if (SUCCEEDED(hr)) {
            hr = g_chain->SetFullscreenState(TRUE, nullptr);
            BOOL full = FALSE;
            g_chain->GetFullscreenState(&full, nullptr);
            g_exclusive = SUCCEEDED(hr) && full;
            say("SetFullscreenState: 0x%08lX, full screen %d", static_cast<unsigned long>(hr), static_cast<int>(full));
            g_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
            ID3D11Texture2D* back = nullptr;
            if (SUCCEEDED(g_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
                device->CreateRenderTargetView(back, nullptr, &view);
                back->Release();
            }
        }
    }
    QUERY_USER_NOTIFICATION_STATE state {};
    SHQueryUserNotificationState(&state);
    say("exclusive %d, notification state %d (3 = D3D full screen)", static_cast<int>(g_exclusive), static_cast<int>(state));
    EnumWindows(overUs, reinterpret_cast<LPARAM>(g_before));
    g_beforeCount = 0;
    while (g_beforeCount < 256 && g_before[g_beforeCount])
        ++g_beforeCount;
    say("ready, %d topmost windows there already", g_beforeCount);

    MSG message {};
    for (;;) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT)
                return 0;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!g_minimized) {
            if (g_exclusive) {
                BOOL full = FALSE;
                g_chain->GetFullscreenState(&full, nullptr);
                if (!full)
                    leave("DXGI took it out of full screen");
            }
            HWND over = nullptr;
            EnumWindows(overUs, reinterpret_cast<LPARAM>(&over));
            if (over && !g_minimized) {
                wchar_t name[64] {};
                GetClassNameW(over, name, 64);
                say("a topmost window over it: %ls", name);
                if (!g_exclusive)
                    leave("a window shown over it (simulated)");
            }
        }
        if (view && !g_minimized) {
            const float colour[4] {0.08f, 0.35f, 0.16f, 1.0f};
            context->OMSetRenderTargets(1, &view, nullptr);
            context->ClearRenderTargetView(view, colour);
            g_chain->Present(1, 0);
        } else {
            Sleep(10);
        }
    }
}
