// WinShunCaptureTest.exe: an unattended screen-capture soak test for another
// PC (GitHub #325: black screen / frozen picture for ~2 s as the overlay
// comes, the pointer lost after circling the mouse, on AMD with FreeSync).
//
// Runs the capture plugin stand-alone (plugin\WinShunCapture.exe with the Qt
// of runtime\, its own data folder: nothing installed is touched), opens a
// capture through the plugin protocol after short and long idle gaps, moves
// the real mouse in circles over it and drags a selection, and meanwhile:
//   - watches the primary monitor frame by frame (DXGI desktop duplication):
//     black frames, gaps with no update while the mouse moves, mode switches
//     (ACCESS_LOST), the pointer as DWM reports it;
//   - reads the system cursor every 2 ms (hidden, none, another program's
//     window under it, handle changes);
//   - takes F9 from the person watching: "I saw it go wrong now".
// Everything, with the plugin's own journal (stall stacks, pointer records),
// ends up in one zip next to the program.
//
//   WinShunCaptureTest.exe [--gaps 2,3,80,...] [--flip] [--no-drag] [--yes]

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

// ---- time, text, output ------------------------------------------------------

LARGE_INTEGER g_freq {}, g_start {};

double nowMs()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - g_start.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

double qpcToMs(LARGE_INTEGER t)
{
    return double(t.QuadPart - g_start.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string json(const std::string& s)
{
    std::string out = "\"";
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
    }
    return out + "\"";
}

std::string fmt(const char* f, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buf, sizeof buf, f, args);
    va_end(args);
    return buf;
}

std::mutex g_logLock;
std::ofstream g_log; // log.txt in the results

// One line to the console (as UTF-16, whatever its code page) and to log.txt.
void say(const std::string& line)
{
    std::lock_guard lock(g_logLock);
    const std::wstring w = wide(line) + L"\r\n";
    DWORD written = 0;
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!WriteConsoleW(out, w.data(), DWORD(w.size()), &written, nullptr)) {
        const std::string s = line + "\n";
        WriteFile(out, s.data(), DWORD(s.size()), &written, nullptr);
    }
    if (g_log.is_open()) {
        g_log << fmt("%9.0f ", nowMs()) << line << '\n';
        g_log.flush();
    }
}

std::atomic<bool> g_stop {false};

BOOL WINAPI onCtrl(DWORD)
{
    g_stop = true;
    return TRUE;
}

// Sleeps up to `ms`, false when asked to stop.
bool rest(double ms)
{
    const double until = nowMs() + ms;
    while (nowMs() < until) {
        if (g_stop)
            return false;
        Sleep(DWORD(std::clamp(until - nowMs(), 1.0, 50.0)));
    }
    return !g_stop;
}

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* name)
{
    wchar_t buf[1024] {};
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

std::optional<DWORD> regDword(HKEY root, const wchar_t* key, const wchar_t* name)
{
    DWORD value = 0, size = sizeof value;
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

// ---- windows and the pointer ---------------------------------------------------

DWORD ownerAt(POINT p)
{
    const HWND hit = WindowFromPoint(p);
    const HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
    DWORD pid = 0;
    if (root)
        GetWindowThreadProcessId(root, &pid);
    return pid;
}

std::string ownerNameAt(POINT p)
{
    const HWND hit = WindowFromPoint(p);
    const HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
    if (!root)
        return "(none)";
    wchar_t cls[256] {};
    GetClassNameW(root, cls, 256);
    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    std::wstring exe = L"?";
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH] {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &size))
            exe = fs::path(path).filename().wstring();
        CloseHandle(h);
    }
    return utf8(exe) + "|" + utf8(cls);
}

POINT cursorPos()
{
    POINT p {};
    GetCursorPos(&p);
    return p;
}

void moveTo(double x, double y)
{
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in {};
    in.type = INPUT_MOUSE;
    in.mi.dx = LONG(std::lround((x - vx) * 65535.0 / std::max(1, vw - 1)));
    in.mi.dy = LONG(std::lround((y - vy) * 65535.0 / std::max(1, vh - 1)));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof in);
}

void leftButton(bool down)
{
    INPUT in {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof in);
}

// ---- the display ---------------------------------------------------------------

struct PathInfo {
    std::string monitor;
    std::string connection;
    double hz = 0;
    int width = 0, height = 0;
    bool primary = false;
};

std::string technology(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t)
{
    switch (t) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return "HDMI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return "DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return "eDP (built-in)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return "internal";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return "DVI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return "VGA";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED: return "UDI (built-in)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return "Miracast";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED: return "indirect";
    default: return fmt("type %d", int(t));
    }
}

std::vector<PathInfo> displayPaths()
{
    std::vector<PathInfo> out;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS)
        return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS)
        return out;
    paths.resize(np);
    for (const auto& p : paths) {
        PathInfo info;
        DISPLAYCONFIG_TARGET_DEVICE_NAME name {};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        name.header.size = sizeof name;
        name.header.adapterId = p.targetInfo.adapterId;
        name.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS)
            info.monitor = utf8(name.monitorFriendlyDeviceName);
        info.connection = technology(p.targetInfo.outputTechnology);
        if (p.targetInfo.refreshRate.Denominator)
            info.hz = double(p.targetInfo.refreshRate.Numerator) / p.targetInfo.refreshRate.Denominator;
        const UINT32 si = p.sourceInfo.modeInfoIdx;
        if (si < modes.size() && modes[si].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const auto& sm = modes[si].sourceMode;
            info.width = int(sm.width);
            info.height = int(sm.height);
            info.primary = sm.position.x == 0 && sm.position.y == 0;
        }
        out.push_back(info);
    }
    return out;
}

double primaryHz()
{
    for (const auto& p : displayPaths())
        if (p.primary)
            return p.hz;
    return 0;
}

// ---- the frame watcher (desktop duplication) -------------------------------------

constexpr int kCols = 24, kRows = 16; // the sample grid for "is it black"

struct Frame {
    double t; // ms
    bool picture; // the picture changed (not just the pointer)
    bool pointerMoved;
    bool pointerVisible;
    int black; // samples (of kCols * kRows) that are black; -1 for no picture
};

struct Note {
    double t;
    std::string what;
};

struct Watcher {
    std::mutex lock;
    std::vector<Frame> frames;
    std::vector<Note> notes; // access lost, made again, display changes
    std::atomic<int> lastBlack {-1};
    std::atomic<bool> ok {false};
    std::string mode; // what it watches
    std::thread thread;

    void run();
    void note(const std::string& what)
    {
        std::lock_guard l(lock);
        notes.push_back({nowMs(), what});
    }
};

void Watcher::run()
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        note("no DXGI factory");
        return;
    }
    const HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> o;
        for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC d {};
            o->GetDesc(&d);
            if (d.Monitor == primary) {
                o.As(&output);
                break;
            }
        }
        if (!output)
            adapter.Reset();
    }
    if (!output) {
        note("primary monitor's output not found");
        return;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, &context))) {
        note("no D3D11 device");
        return;
    }
    ComPtr<IDXGIOutputDuplication> dup;
    ComPtr<ID3D11Texture2D> staging;
    DXGI_OUTDUPL_DESC dd {};
    double lostAt = -1;
    while (!g_stop) {
        if (!dup) {
            const HRESULT hr = output->DuplicateOutput(device.Get(), &dup);
            if (FAILED(hr)) {
                if (lostAt < 0)
                    note(fmt("duplication not available (%08lx)", unsigned(hr)));
                Sleep(20);
                continue;
            }
            dup->GetDesc(&dd);
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = kCols;
            desc.Height = kRows;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = dd.ModeDesc.Format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.Reset();
            device->CreateTexture2D(&desc, nullptr, &staging);
            const double hz = dd.ModeDesc.RefreshRate.Denominator
                ? double(dd.ModeDesc.RefreshRate.Numerator) / dd.ModeDesc.RefreshRate.Denominator : 0;
            std::string m = fmt("%ux%u %.2f Hz format %d", dd.ModeDesc.Width, dd.ModeDesc.Height, hz, int(dd.ModeDesc.Format));
            if (lostAt >= 0)
                note(fmt("duplication back after %.0f ms: %s", nowMs() - lostAt, m.c_str()));
            mode = m;
            lostAt = -1;
            ok = true;
        }
        DXGI_OUTDUPL_FRAME_INFO info {};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = dup->AcquireNextFrame(50, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            continue;
        if (FAILED(hr)) {
            // ACCESS_LOST: a mode change, a full-screen switch, the secure desktop.
            lostAt = nowMs();
            note(fmt("duplication lost (%08lx)", unsigned(hr)));
            dup.Reset();
            continue;
        }
        Frame f {nowMs(), info.LastPresentTime.QuadPart != 0, info.LastMouseUpdateTime.QuadPart != 0,
            info.PointerPosition.Visible != FALSE, -1};
        if (f.picture) {
            f.t = qpcToMs(info.LastPresentTime);
            ComPtr<ID3D11Texture2D> texture;
            if (staging && SUCCEEDED(resource.As(&texture))) {
                for (int r = 0; r < kRows; ++r) {
                    for (int c = 0; c < kCols; ++c) {
                        const UINT x = (2 * c + 1) * dd.ModeDesc.Width / (2 * kCols);
                        const UINT y = (2 * r + 1) * dd.ModeDesc.Height / (2 * kRows);
                        const D3D11_BOX box {x, y, 0, x + 1, y + 1, 1};
                        context->CopySubresourceRegion(staging.Get(), 0, c, r, 0, texture.Get(), 0, &box);
                    }
                }
                D3D11_MAPPED_SUBRESOURCE mapped {};
                if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    int black = 0;
                    for (int r = 0; r < kRows; ++r) {
                        const auto* row = static_cast<const unsigned char*>(mapped.pData) + size_t(mapped.RowPitch) * r;
                        for (int c = 0; c < kCols; ++c) {
                            const unsigned char* p = row + c * 4;
                            if (p[0] <= 2 && p[1] <= 2 && p[2] <= 2)
                                ++black;
                        }
                    }
                    context->Unmap(staging.Get(), 0);
                    f.black = black;
                    lastBlack = black;
                }
            }
        }
        {
            std::lock_guard l(lock);
            frames.push_back(f);
        }
        resource.Reset();
        dup->ReleaseFrame();
    }
}

// ---- the cursor sampler --------------------------------------------------------

struct CursorStats {
    int samples = 0, hidden = 0, none = 0, foreign = 0, handleChanges = 0;
    std::vector<std::string> foreignWindows;
};

struct Sampler {
    std::atomic<bool> watching {false};
    std::atomic<DWORD> pluginPid {0};
    std::mutex lock;
    CursorStats stats;
    HCURSOR last = nullptr;
    std::thread thread;

    void run()
    {
        while (!g_stop) {
            if (watching) {
                CURSORINFO ci {sizeof ci};
                if (GetCursorInfo(&ci)) {
                    const DWORD owner = ownerAt(ci.ptScreenPos);
                    std::lock_guard l(lock);
                    ++stats.samples;
                    if (owner && owner != pluginPid) {
                        ++stats.foreign;
                        if (stats.foreignWindows.size() < 5) {
                            const std::string name = ownerNameAt(ci.ptScreenPos);
                            if (std::find(stats.foreignWindows.begin(), stats.foreignWindows.end(), name) == stats.foreignWindows.end())
                                stats.foreignWindows.push_back(name);
                        }
                    } else {
                        if (!(ci.flags & CURSOR_SHOWING) || (ci.flags & CURSOR_SUPPRESSED))
                            ++stats.hidden;
                        else if (!ci.hCursor)
                            ++stats.none;
                    }
                    if (last && ci.hCursor != last)
                        ++stats.handleChanges;
                    last = ci.hCursor;
                }
            }
            Sleep(2);
        }
    }

    CursorStats take()
    {
        std::lock_guard l(lock);
        CursorStats s = stats;
        stats = {};
        last = nullptr;
        return s;
    }
};

// ---- F9 and display changes ----------------------------------------------------

struct Listener {
    std::mutex lock;
    std::vector<double> marks;
    std::string key = "F9";
    std::atomic<bool> ready {false};
    Watcher* watcher = nullptr;
    std::thread thread;
    DWORD threadId = 0;

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_DISPLAYCHANGE) {
            auto* self = reinterpret_cast<Listener*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self && self->watcher)
                self->watcher->note(fmt("display changed: %dx%d, %d bpp", LOWORD(lp), HIWORD(lp), int(wp)));
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void run()
    {
        threadId = GetCurrentThreadId();
        WNDCLASSW wc {};
        wc.lpfnWndProc = &Listener::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WinShunCaptureTestListener";
        RegisterClassW(&wc);
        // Top-level and hidden: broadcasts (WM_DISPLAYCHANGE) reach it, a message-only window gets none.
        const HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
            wc.hInstance, nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        const UINT keys[] = {VK_F9, VK_F10, VK_F8};
        const char* names[] = {"F9", "F10", "F8"};
        key.clear();
        for (int i = 0; i < 3; ++i) {
            if (RegisterHotKey(nullptr, 1, MOD_NOREPEAT, keys[i])) {
                key = names[i];
                break;
            }
        }
        ready = true;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_HOTKEY) {
                {
                    std::lock_guard l(lock);
                    marks.push_back(nowMs());
                }
                MessageBeep(MB_OK);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        UnregisterHotKey(nullptr, 1);
        DestroyWindow(hwnd);
    }
};

// ---- the plugin ------------------------------------------------------------------

struct Plugin {
    PROCESS_INFORMATION pi {};
    HANDLE in = nullptr, out = nullptr, err = nullptr;
    std::mutex sendLock;
    std::thread outThread, errThread;
    std::ofstream errLog;
    int serial = 100;

    void send(const std::string& line)
    {
        std::lock_guard l(sendLock);
        const std::string s = line + "\n";
        DWORD written = 0;
        WriteFile(in, s.data(), DWORD(s.size()), &written, nullptr);
    }

    void action(const char* id)
    {
        send(fmt("{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"action\",\"params\":{\"item\":{\"id\":\"%s\"}}}", ++serial, id));
    }

    // The plugin's requests to the host are answered with an empty result.
    void pumpOut()
    {
        std::string buffer;
        char chunk[4096];
        DWORD n = 0;
        while (ReadFile(out, chunk, sizeof chunk, &n, nullptr) && n) {
            buffer.append(chunk, n);
            size_t nl;
            while ((nl = buffer.find('\n')) != std::string::npos) {
                const std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                const size_t m = line.find("\"method\"");
                const size_t i = line.find("\"id\"");
                if (m == std::string::npos || i == std::string::npos)
                    continue;
                size_t a = line.find(':', i);
                if (a == std::string::npos)
                    continue;
                ++a;
                while (a < line.size() && line[a] == ' ')
                    ++a;
                size_t b = a;
                if (b < line.size() && line[b] == '"') {
                    b = line.find('"', b + 1);
                    if (b == std::string::npos)
                        continue;
                    ++b;
                } else {
                    while (b < line.size() && line[b] != ',' && line[b] != '}' && line[b] != ' ')
                        ++b;
                }
                send("{\"jsonrpc\":\"2.0\",\"id\":" + line.substr(a, b - a) + ",\"result\":{}}");
            }
        }
    }

    void pumpErr()
    {
        std::string buffer;
        char chunk[4096];
        DWORD n = 0;
        while (ReadFile(err, chunk, sizeof chunk, &n, nullptr) && n) {
            buffer.append(chunk, n);
            size_t nl;
            while ((nl = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                std::lock_guard l(g_logLock);
                errLog << fmt("%9.0f ", nowMs()) << line << '\n';
                errLog.flush();
            }
        }
    }

    bool start(const fs::path& exe, const fs::path& runtime, const fs::path& data, const fs::path& errFile, bool flip)
    {
        SECURITY_ATTRIBUTES sa {sizeof sa, nullptr, TRUE};
        HANDLE inRead, outWrite, errWrite;
        if (!CreatePipe(&inRead, &in, &sa, 0) || !CreatePipe(&out, &outWrite, &sa, 0) || !CreatePipe(&err, &errWrite, &sa, 0))
            return false;
        SetHandleInformation(in, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(err, HANDLE_FLAG_INHERIT, 0);
        SetEnvironmentVariableW(L"WS_APP_DIR", runtime.c_str());
        SetEnvironmentVariableW(L"WS_DATA_DIR", data.c_str());
        SetEnvironmentVariableW(L"QT_FORCE_STDERR_LOGGING", L"1");
        SetEnvironmentVariableW(L"WINSHUN_CAPTURE_FLIP", flip ? L"1" : nullptr);
        STARTUPINFOW si {sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = errWrite;
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
            exe.parent_path().c_str(), &si, &pi);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        CloseHandle(errWrite);
        if (!ok)
            return false;
        errLog.open(errFile, std::ios::binary);
        outThread = std::thread([this] { pumpOut(); });
        errThread = std::thread([this] { pumpErr(); });
        send("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"dataDir\":" + json(utf8(data.wstring()))
            + ",\"locale\":\"zh\",\"appearance\":{\"renderer\":\"d3d11\"},\"capabilities\":{\"hostHotkeys\":true}}}");
        return true;
    }

    bool alive() const { return pi.hProcess && WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT; }

    void stop()
    {
        if (!pi.hProcess)
            return;
        send(fmt("{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"shutdown\",\"params\":{}}", ++serial));
        if (WaitForSingleObject(pi.hProcess, 6000) != WAIT_OBJECT_0)
            TerminateProcess(pi.hProcess, 1);
        CloseHandle(in);
        in = nullptr;
        if (outThread.joinable())
            outThread.join();
        if (errThread.joinable())
            errThread.join();
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        pi = {};
    }
};

// ---- one round -------------------------------------------------------------------

struct Round {
    int index = 0;
    double gapS = 0;
    double started = 0, ended = 0;
    double upMs = -1; // from the action to the overlay under the pointer
    double circleFrom = 0, circleTo = 0;
    double hzBefore = 0, hzAfter = 0;
    std::string circle = "not run";
    std::string drag = "not run";
};

int countOf(const std::string& text, const std::string& what)
{
    int n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size()))
        ++n;
    return n;
}

std::string systemFacts()
{
    std::ostringstream o;
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    // The registry still says "Windows 10" on 11: the build tells.
    const std::wstring build = regString(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild");
    std::string product = utf8(regString(HKEY_LOCAL_MACHINE, nt, L"ProductName"));
    if (_wtoi(build.c_str()) >= 22000 && product.find("Windows 10") == 0)
        product.replace(0, 10, "Windows 11");
    o << "Windows: " << product << " " << utf8(regString(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion")) << " build "
      << utf8(build) << "." << regDword(HKEY_LOCAL_MACHINE, nt, L"UBR").value_or(0) << "\n";
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> adapter;
        std::vector<LONGLONG> seen; // one card can be listed once per output
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            DXGI_ADAPTER_DESC1 d {};
            adapter->GetDesc1(&d);
            const LONGLONG luid = (LONGLONG(d.AdapterLuid.HighPart) << 32) | d.AdapterLuid.LowPart;
            if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || std::find(seen.begin(), seen.end(), luid) != seen.end())
                continue;
            seen.push_back(luid);
            LARGE_INTEGER v {};
            std::string driver = "?";
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v)))
                driver = fmt("%u.%u.%u.%u", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart), LOWORD(v.LowPart));
            o << "Graphics: " << utf8(d.Description) << " (driver " << driver << ")\n";
        }
    }
    for (const auto& p : displayPaths())
        o << "Display: " << (p.monitor.empty() ? "(no name)" : p.monitor) << ", " << p.connection << ", " << p.width << "x"
          << p.height << " @ " << fmt("%.2f", p.hz) << " Hz" << (p.primary ? ", primary" : "") << "\n";
    UINT dx = 0, dy = 0;
    GetDpiForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dx, &dy);
    o << "Scale: " << dx * 100 / 96 << "%\n";
    const auto hags = regDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode");
    o << "Hardware-accelerated GPU scheduling: " << (hags ? (*hags == 2 ? "on" : "off") : "not set") << "\n";
    const auto mpo = regDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\Dwm", L"OverlayTestMode");
    o << "MPO (OverlayTestMode): " << (mpo ? std::to_string(*mpo) + (*mpo == 5 ? " (MPO off)" : "") : "not set") << "\n";
    o << "DirectX user settings (windowed games): "
      << utf8(regString(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", L"DirectXUserGlobalSettings"))
      << "\n";
    BOOL shadow = FALSE;
    SystemParametersInfoW(SPI_GETCURSORSHADOW, 0, &shadow, 0);
    int trails = 0;
    SystemParametersInfoW(SPI_GETMOUSETRAILS, 0, &trails, 0);
    o << "Pointer: size " << regDword(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize").value_or(32)
      << ", shadow " << (shadow ? "on" : "off") << ", trails " << trails << ", scheme "
      << utf8(regString(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"")) << "\n";
    return o.str();
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(onCtrl, TRUE);
    // 1 ms sleeps for the 2 ms cursor reads and the mouse moves. Windows 11
    // ignores a timer request from a process whose windows are hidden or
    // covered (the console sits under the overlay): opt out of that first.
    PROCESS_POWER_THROTTLING_STATE throttling {PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION | PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling);
    timeBeginPeriod(1);

    std::vector<double> gaps {2, 3, 80, 3, 95, 3, 110, 3, 85, 3, 100, 3};
    bool flip = false, drag = true, yes = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--gaps" && i + 1 < argc) {
            gaps.clear();
            std::wstringstream s(argv[++i]);
            std::wstring item;
            while (std::getline(s, item, L','))
                gaps.push_back(_wtof(item.c_str()));
        } else if (a == L"--flip") {
            flip = true;
        } else if (a == L"--no-drag") {
            drag = false;
        } else if (a == L"--yes") {
            yes = true;
        }
    }

    wchar_t selfPath[MAX_PATH] {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    const fs::path here = fs::path(selfPath).parent_path();
    const fs::path pluginExe = here / L"plugin" / L"WinShunCapture.exe";
    const fs::path runtime = here / L"runtime";
    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::wstring stamp = wide(fmt("%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond));
    const fs::path results = here / (L"results-" + stamp);
    std::error_code ec;
    fs::create_directories(results / L"data", ec);
    g_log.open(results / L"log.txt", std::ios::binary);

    double total = 0;
    for (double g : gaps)
        total += g + 12;
    say("Win顺 截图测试（黑屏、画面定住、指针不见）");
    say("");
    if (!fs::exists(pluginExe) || !fs::exists(runtime / L"Qt6Core.dll")) {
        say("找不到 plugin 或 runtime 文件夹：请把整个压缩包解压后再运行。");
        std::getchar();
        return 2;
    }
    say(fmt("会自动截图 %d 次，大约 %.0f 分钟。每次截图时程序会自己移动鼠标转圈、拖一个选区，然后取消；不会点到别的程序。",
        int(gaps.size()), std::ceil(total / 60)));
    say("期间请不要动鼠标和键盘，但请看着屏幕：");
    say("  看到屏幕变黑、画面定住、或者鼠标指针不见了，就按一下 F9（会“叮”一声），不用动鼠标。");
    say("  想提前结束：按 Ctrl+C。");
    say("");
    if (!yes) {
        say("准备好后按回车开始。");
        std::getchar();
    }

    std::ofstream(results / L"system.txt", std::ios::binary) << systemFacts();
    say(systemFacts());

    Watcher watcher;
    Sampler sampler;
    Listener listener;
    listener.watcher = &watcher;
    watcher.thread = std::thread([&] { watcher.run(); });
    sampler.thread = std::thread([&] { sampler.run(); });
    listener.thread = std::thread([&] { listener.run(); });
    while (!listener.ready)
        Sleep(10);
    if (listener.key.empty())
        say("注意：F8、F9、F10 都被别的程序占着，没法按键标记，只能事后告诉我大概几点看到的。");
    else if (listener.key != "F9")
        say("注意：F9 被别的程序占着，标记改用 " + listener.key + "。");

    Plugin plugin;
    if (!plugin.start(pluginExe, runtime, results / L"data", results / L"plugin-stderr.log", flip)) {
        say("截图插件启动失败。");
        g_stop = true;
    }
    sampler.pluginPid = plugin.pi.dwProcessId;
    say("截图插件已启动，等它准备好……");
    rest(5000);

    const HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi {sizeof mi};
    GetMonitorInfoW(primary, &mi);
    const double cx = (mi.rcMonitor.left + mi.rcMonitor.right) / 2.0;
    const double cy = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2.0;
    const double radius = std::min(mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top) * 0.4;
    const POINT home = cursorPos();

    std::ofstream rounds(results / L"rounds.jsonl", std::ios::binary);
    std::vector<std::string> table;
    int problems = 0;
    for (size_t r = 0; r < gaps.size() && !g_stop; ++r) {
        Round round;
        round.index = int(r + 1);
        round.gapS = gaps[r];
        say(fmt("第 %d/%d 次：先等 %.0f 秒（隔得久才会出问题）", round.index, int(gaps.size()), gaps[r]));
        double waited = 0;
        while (waited < gaps[r] * 1000 && !g_stop) {
            const double step = std::min(10000.0, gaps[r] * 1000 - waited);
            if (!rest(step))
                break;
            waited += step;
        }
        if (g_stop || !plugin.alive())
            break;
        moveTo(cx, cy);
        rest(300);
        round.hzBefore = primaryHz();
        round.started = nowMs();
        sampler.take();
        plugin.action("start");
        while (nowMs() - round.started < 6000 && !g_stop) {
            if (ownerAt(cursorPos()) == plugin.pi.dwProcessId) {
                round.upMs = nowMs() - round.started;
                break;
            }
            Sleep(2);
        }
        if (round.upMs < 0) {
            say("  截图界面没出来（6 秒内），跳过这一次。");
            plugin.action("start");
            rest(1500);
            continue;
        }
        sampler.watching = true;
        // Circles: three in six seconds, the real mouse, a move every ~2 ms.
        round.circleFrom = nowMs();
        double away = -1;
        round.circle = "done";
        while (!g_stop) {
            const double t = nowMs() - round.circleFrom;
            if (t > 6000)
                break;
            const double angle = 2 * 3.14159265358979 * 3 * t / 6000;
            moveTo(cx + radius * std::cos(angle), cy + radius * std::sin(angle));
            Sleep(2);
            if (ownerAt(cursorPos()) != plugin.pi.dwProcessId) {
                if (away < 0)
                    away = nowMs();
                else if (nowMs() - away > 400) {
                    round.circle = "stopped: the capture was not under the pointer";
                    break;
                }
            } else {
                away = -1;
            }
        }
        round.circleTo = nowMs();
        rest(1500); // still: does the pointer stay?
        // A selection dragged with the left button (the reporter also saw a stall here).
        if (drag && !g_stop && ownerAt(cursorPos()) == plugin.pi.dwProcessId) {
            const double x0 = cx - radius * 0.6, y0 = cy - radius * 0.4;
            moveTo(x0, y0);
            Sleep(30);
            if (ownerAt(cursorPos()) == plugin.pi.dwProcessId) {
                leftButton(true);
                round.drag = "done";
                const double from = nowMs();
                while (nowMs() - from < 500) {
                    const double k = (nowMs() - from) / 500;
                    moveTo(x0 + radius * 1.2 * k, y0 + radius * 0.8 * k);
                    Sleep(4);
                }
                leftButton(false);
                rest(1200);
            } else {
                round.drag = "skipped: the capture was not under the pointer";
            }
        }
        sampler.watching = false;
        plugin.action("start"); // cancels
        const double cancelAt = nowMs();
        while (nowMs() - cancelAt < 3000 && ownerAt(cursorPos()) == plugin.pi.dwProcessId)
            Sleep(10);
        rest(500);
        round.ended = nowMs();
        round.hzAfter = primaryHz();
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)
            leftButton(false);

        // What happened in this round.
        const CursorStats cs = sampler.take();
        std::vector<Frame> frames;
        std::vector<Note> notes;
        int baseline = -1;
        {
            std::lock_guard l(watcher.lock);
            for (const Frame& f : watcher.frames) {
                if (f.t < round.started && f.picture)
                    baseline = f.black;
                if (f.t >= round.started && f.t <= round.ended)
                    frames.push_back(f);
            }
            for (const Note& n : watcher.notes)
                if (n.t >= round.started - 1000 && n.t <= round.ended)
                    notes.push_back(n);
        }
        int pictures = 0, blackFrames = 0, invisible = 0;
        double longestBlack = 0, blackSince = -1, maxGap = 0, lastUpdate = round.circleFrom;
        const int half = kCols * kRows / 2;
        const int jump = std::max(0, baseline) + kCols * kRows / 4;
        for (const Frame& f : frames) {
            if (f.picture && f.black >= 0) {
                ++pictures;
                const bool black = f.black > half && f.black > jump;
                if (black) {
                    ++blackFrames;
                    if (blackSince < 0)
                        blackSince = f.t;
                } else if (blackSince >= 0) {
                    longestBlack = std::max(longestBlack, f.t - blackSince);
                    blackSince = -1;
                }
            }
            if (f.t >= round.circleFrom && f.t <= round.circleTo) {
                maxGap = std::max(maxGap, f.t - lastUpdate);
                lastUpdate = f.t;
                if (f.pointerMoved && !f.pointerVisible)
                    ++invisible;
            }
        }
        if (blackSince >= 0)
            longestBlack = std::max(longestBlack, round.ended - blackSince);
        if (round.circleTo > lastUpdate)
            maxGap = std::max(maxGap, round.circleTo - lastUpdate);
        std::vector<double> marks;
        {
            std::lock_guard l(listener.lock);
            for (double m : listener.marks)
                if (m >= round.started - 2000 && m <= round.ended + 3000)
                    marks.push_back(m - round.started);
        }
        int lost = 0;
        std::string notesJson = "[";
        for (const Note& n : notes) {
            lost += n.what.rfind("duplication lost", 0) == 0;
            notesJson += (notesJson.size() > 1 ? "," : "") + fmt("{\"ms\":%.0f,\"what\":", n.t - round.started) + json(n.what) + "}";
        }
        notesJson += "]";
        std::string marksJson = "[";
        for (double m : marks)
            marksJson += (marksJson.size() > 1 ? "," : "") + fmt("%.0f", m);
        marksJson += "]";
        std::string foreignJson = "[";
        for (const auto& w : cs.foreignWindows)
            foreignJson += (foreignJson.size() > 1 ? "," : "") + json(w);
        foreignJson += "]";
        rounds << "{" << fmt("\"round\":%d,\"gap_s\":%.0f,\"overlay_up_ms\":%.0f,\"hz_before\":%.2f,\"hz_after\":%.2f,",
                             round.index, round.gapS, round.upMs, round.hzBefore, round.hzAfter)
               << fmt("\"frames\":%d,\"pictures\":%d,\"black_frames\":%d,\"longest_black_ms\":%.0f,\"baseline_black\":%d,",
                      int(frames.size()), pictures, blackFrames, longestBlack, baseline)
               << fmt("\"longest_no_update_while_circling_ms\":%.0f,\"pointer_invisible_updates\":%d,\"duplication_lost\":%d,",
                      maxGap, invisible, lost)
               << fmt("\"cursor_samples\":%d,\"cursor_hidden\":%d,\"cursor_none\":%d,\"cursor_over_other_program\":%d,"
                      "\"cursor_handle_changes\":%d,",
                      cs.samples, cs.hidden, cs.none, cs.foreign, cs.handleChanges)
               << "\"other_windows\":" << foreignJson << ",\"circle\":" << json(round.circle) << ",\"drag\":" << json(round.drag)
               << ",\"marks_ms\":" << marksJson << ",\"notes\":" << notesJson << "}\n";
        rounds.flush();
        const bool bad = blackFrames > 0 || maxGap > 300 || invisible > 0 || lost > 0 || cs.hidden > 0 || cs.none > 0
            || !marks.empty() || round.upMs > 1500;
        problems += bad;
        const std::string line = fmt("  界面出来 %.0f ms；黑帧 %d（最长 %.0f ms）；转圈时最长没刷新 %.0f ms；显示模式切换 %d；指针隐藏 %d、无指针 %d、"
                                     "在别的窗口上 %d；F9 %d 次%s",
            round.upMs, blackFrames, longestBlack, maxGap, lost, cs.hidden, cs.none, cs.foreign, int(marks.size()),
            bad ? "  ← 有异常" : "");
        say(line);
        table.push_back(fmt("第 %2d 次（等 %3.0f 秒）", round.index, round.gapS) + line);
    }

    sampler.watching = false;
    say("");
    say("测试结束，正在收尾……");
    plugin.stop();
    moveTo(home.x, home.y);
    g_stop = true;
    if (listener.threadId)
        PostThreadMessageW(listener.threadId, WM_QUIT, 0, 0);
    if (watcher.thread.joinable())
        watcher.thread.join();
    if (sampler.thread.joinable())
        sampler.thread.join();
    if (listener.thread.joinable())
        listener.thread.join();

    // The plugin's journal: what the overlay itself saw.
    std::string journal;
    {
        std::ifstream in(results / L"data" / L"diagnostics" / L"events.jsonl", std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        journal = s.str();
    }
    std::ostringstream summary;
    summary << "Win顺 截图测试结果\n\n" << systemFacts() << "\n";
    summary << "duplication: " << watcher.mode << "\n";
    summary << fmt("overlays presented by: %s\n", flip ? "flip (--flip)" : "copy (default)");
    summary << "（“界面出来”从发出截图指令算起，含插件自己等的 200 ms）\n\n";
    for (const auto& line : table)
        summary << line << "\n";
    summary << "\n插件日志：capture " << countOf(journal, "\"event\":\"capture\"") << " 次，capture-stall "
            << countOf(journal, "\"event\":\"capture-stall\"") << "，ui-stall " << countOf(journal, "\"event\":\"ui-stall\"")
            << "，指针被盖住 " << countOf(journal, "\"event\":\"capture-pointer-covered\"") << "，指针隐藏 "
            << countOf(journal, "\"event\":\"capture-cursor-hidden\"") << "\n";
    {
        std::lock_guard l(listener.lock);
        summary << "F9 共 " << listener.marks.size() << " 次\n";
    }
    std::ofstream(results / L"summary.txt", std::ios::binary) << summary.str();
    say(summary.str());
    g_log.close();

    // One zip next to the program, made with Windows' own tar.
    const fs::path zip = here / (L"WinShun截图测试结果-" + stamp + L".zip");
    std::wstring cmd = L"tar.exe -a -c -f \"" + zip.wstring() + L"\" -C \"" + here.wstring() + L"\" \"" + results.filename().wstring() + L"\"";
    STARTUPINFOW si {sizeof si};
    PROCESS_INFORMATION pi {};
    bool zipped = false;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        zipped = code == 0 && fs::exists(zip);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    std::wstring done = zipped ? L"完成。请把这个文件发给我：\r\n" + zip.wstring() + L"\r\n"
                               : L"完成。请把这个文件夹打包发给我：\r\n" + results.wstring() + L"\r\n";
    DWORD written = 0;
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), done.data(), DWORD(done.size()), &written, nullptr);
    if (!yes) {
        const std::wstring bye = L"按回车关闭。\r\n";
        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), bye.data(), DWORD(bye.size()), &written, nullptr);
        std::getchar();
    }
    timeEndPeriod(1);
    return problems > 0 ? 1 : 0;
}
