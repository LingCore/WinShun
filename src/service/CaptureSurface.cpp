#include "CaptureSurface.h"

#include "CaptureSurfaceProtocol.h"
#include "Log.h"
#include "Win32Util.h"

#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ws::service {
namespace {

using win32::UniqueHandle;
using namespace capsurface;

constexpr wchar_t kSurfaceClass[] = L"WinShun.CaptureSurface";
constexpr UINT_PTR kWatchdogTimerId = 1;
constexpr UINT kWatchdogIntervalMs = 100;
constexpr DWORD kConnectWaitMs = 1500;
constexpr DWORD kWriteTimeoutMs = 500;
constexpr DWORD kDesktopBand = 1;
constexpr DWORD kUiAccessBand = 2;
constexpr UINT kToUnicodeKeepState = 0x4;
constexpr std::size_t kMaxCachedCursors = 16;

void enablePerMonitorDpi()
{
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto setContext = reinterpret_cast<SetContextFn>(reinterpret_cast<void*>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")));
    if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return;
    ::SetProcessDPIAware();
}

DWORD windowBand(HWND window)
{
    using GetBandFn = BOOL(WINAPI*)(HWND, DWORD*);
    static const auto fn = reinterpret_cast<GetBandFn>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetWindowBand")));
    DWORD band = 0;
    return fn && window && fn(window, &band) ? band : 0;
}

std::vector<std::byte> tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS kind)
{
    DWORD size = 0;
    ::GetTokenInformation(token, kind, nullptr, 0, &size);
    std::vector<std::byte> bytes(size);
    if (!size || !::GetTokenInformation(token, kind, bytes.data(), size, &size))
        bytes.clear();
    return bytes;
}

std::optional<DWORD> integrityOfPid(DWORD pid)
{
    const UniqueHandle proc(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE raw = nullptr;
    if (!proc.valid() || !::OpenProcessToken(proc.get(), TOKEN_QUERY, &raw))
        return std::nullopt;
    const UniqueHandle token(raw);
    const auto label = tokenInfo(token.get(), TokenIntegrityLevel);
    if (label.empty())
        return std::nullopt;
    const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
    return *::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1);
}

bool isSessionHelperToken()
{
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw))
        return false;
    const UniqueHandle token(raw);
    DWORD uiAccess = 0;
    DWORD size = 0;
    const auto label = tokenInfo(token.get(), TokenIntegrityLevel);
    if (!::GetTokenInformation(token.get(), TokenUIAccess, &uiAccess, sizeof uiAccess, &size) || !uiAccess
        || label.empty())
        return false;
    const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
    return *::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1) >= SECURITY_MANDATORY_HIGH_RID;
}

bool sameSession(DWORD pid)
{
    DWORD theirSession = 0;
    DWORD ourSession = 0;
    return pid && ::ProcessIdToSessionId(pid, &theirSession)
        && ::ProcessIdToSessionId(::GetCurrentProcessId(), &ourSession) && theirSession == ourSession;
}

// Verifies that `target` is genuinely a full-screen Band > 1 Store app
// (ApplicationFrameWindow) in this session and NOT an elevated/SYSTEM window.
bool isFullScreenStoreTarget(HWND target, bool requireForeground)
{
    if (!target || !::IsWindow(target) || ::GetAncestor(target, GA_ROOT) != target || !::IsWindowVisible(target))
        return false;
    if (requireForeground) {
        const HWND fg = ::GetForegroundWindow();
        if (!fg || ::GetAncestor(fg, GA_ROOT) != target)
            return false;
    }
    wchar_t cls[64] {};
    if (!::GetClassNameW(target, cls, static_cast<int>(std::size(cls)))
        || std::wstring_view(cls) != L"ApplicationFrameWindow")
        return false;
    const DWORD band = windowBand(target);
    if (band == 0 || band == kDesktopBand)
        return false;
    RECT frame {};
    MONITORINFO monitor {sizeof monitor};
    if (!::GetWindowRect(target, &frame)
        || !::GetMonitorInfoW(::MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &monitor)
        || frame.left > monitor.rcMonitor.left || frame.top > monitor.rcMonitor.top
        || frame.right < monitor.rcMonitor.right || frame.bottom < monitor.rcMonitor.bottom)
        return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(target, &pid);
    if (!sameSession(pid))
        return false;
    const auto level = integrityOfPid(pid);
    if (!level || *level >= SECURITY_MANDATORY_HIGH_RID)
        return false;
    // Also check any hosted CoreWindow child process is not elevated/SYSTEM.
    struct ChildCheck {
        bool elevated = false;
    } check;
    ::EnumChildWindows(
        target,
        [](HWND child, LPARAM lp) -> BOOL {
            DWORD childPid = 0;
            ::GetWindowThreadProcessId(child, &childPid);
            if (childPid) {
                const auto childLevel = integrityOfPid(childPid);
                if (!childLevel || *childLevel >= SECURITY_MANDATORY_HIGH_RID) {
                    reinterpret_cast<ChildCheck*>(lp)->elevated = true;
                    return FALSE;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&check));
    return !check.elevated;
}

std::uint8_t currentModifiers()
{
    std::uint8_t mods = 0;
    if (::GetKeyState(VK_SHIFT) & 0x8000)
        mods |= kModShift;
    if (::GetKeyState(VK_CONTROL) & 0x8000)
        mods |= kModCtrl;
    if (::GetKeyState(VK_MENU) & 0x8000)
        mods |= kModAlt;
    return mods;
}

std::uint8_t buttonsFromWParam(WPARAM wp)
{
    std::uint8_t b = 0;
    if (wp & MK_LBUTTON)
        b |= kBtnLeft;
    if (wp & MK_RBUTTON)
        b |= kBtnRight;
    if (wp & MK_MBUTTON)
        b |= kBtnMiddle;
    if (wp & MK_XBUTTON1)
        b |= kBtnX1;
    if (wp & MK_XBUTTON2)
        b |= kBtnX2;
    return b;
}

HCURSOR standardCursorForQtShape(std::uint32_t shape)
{
    const wchar_t* id = IDC_ARROW;
    switch (shape) {
    case 0: id = IDC_ARROW; break; // ArrowCursor
    case 1: id = IDC_UPARROW; break; // UpArrowCursor
    case 2: id = IDC_CROSS; break; // CrossCursor
    case 3: id = IDC_WAIT; break; // WaitCursor
    case 4: id = IDC_IBEAM; break; // IBeamCursor
    case 5: id = IDC_SIZENS; break; // SizeVerCursor
    case 6: id = IDC_SIZEWE; break; // SizeHorCursor
    case 7: id = IDC_SIZENESW; break; // SizeBDiagCursor
    case 8: id = IDC_SIZENWSE; break; // SizeFDiagCursor
    case 9: id = IDC_SIZEALL; break; // SizeAllCursor
    case 10: return nullptr; // BlankCursor
    case 13: id = IDC_HAND; break; // PointingHandCursor
    case 14: id = IDC_NO; break; // ForbiddenCursor
    case 17: id = IDC_HAND; break; // OpenHandCursor
    case 18: id = IDC_SIZEALL; break; // ClosedHandCursor
    default: id = IDC_ARROW; break;
    }
    return ::LoadCursorW(nullptr, id);
}

HCURSOR createRgbaCursor(std::uint16_t w, std::uint16_t h, std::int16_t hotX, std::int16_t hotY,
    const std::vector<std::uint8_t>& bgra)
{
    if (w == 0 || h == 0 || w > kMaxCursorDim || h > kMaxCursorDim
        || bgra.size() != std::size_t {w} * std::size_t {h} * 4u)
        return nullptr;
    BITMAPV5HEADER bi {};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = w;
    bi.bV5Height = -static_cast<LONG>(h); // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HBITMAP color = ::CreateDIBSection(screen, reinterpret_cast<const BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (!color || !bits) {
        if (color)
            ::DeleteObject(color);
        return nullptr;
    }
    std::memcpy(bits, bgra.data(), bgra.size());
    HBITMAP mask = ::CreateBitmap(w, h, 1, 1, nullptr);
    if (!mask) {
        ::DeleteObject(color);
        return nullptr;
    }
    ICONINFO ii {};
    ii.fIcon = FALSE; // cursor
    ii.xHotspot = static_cast<DWORD>(std::clamp<int>(hotX, 0, w - 1));
    ii.yHotspot = static_cast<DWORD>(std::clamp<int>(hotY, 0, h - 1));
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HCURSOR cursor = ::CreateIconIndirect(&ii);
    ::DeleteObject(color);
    ::DeleteObject(mask);
    return cursor;
}

void setCloaked(HWND hwnd, bool cloak)
{
    const BOOL value = cloak ? TRUE : FALSE;
    ::DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof value);
}

class SurfaceSession {
public:
    SurfaceSession(DWORD pluginPid, UniqueHandle pluginProc, HWND fullScreenTarget, UniqueHandle pipe)
        : m_pluginPid(pluginPid)
        , m_pluginProc(std::move(pluginProc))
        , m_fullScreenTarget(fullScreenTarget)
        , m_pipe(std::move(pipe))
        , m_readEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
        , m_writeEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
        , m_currentCursor(::LoadCursorW(nullptr, IDC_ARROW))
    {
    }

    ~SurfaceSession()
    {
        teardown();
        for (auto& [key, cursor] : m_rgbaCursors) {
            if (cursor)
                ::DestroyIcon(cursor);
        }
        m_rgbaCursors.clear();
    }

    int run()
    {
        if (!m_readEvent.valid() || !m_writeEvent.valid())
            return ERROR_OUTOFMEMORY;
        WNDCLASSW wc {};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &SurfaceSession::wndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kSurfaceClass;
        ::RegisterClassW(&wc);

        if (!beginRead())
            return ERROR_BROKEN_PIPE;

        // Wait for Init frame and then drive the Win32 message loop alongside
        // overlapped pipe reads on the same thread.
        const ULONGLONG initDeadline = ::GetTickCount64() + 5000;
        while (!m_done) {
            if (!m_armed && ::GetTickCount64() > initDeadline) {
                finish(CloseReason::Error);
                break;
            }
            HANDLE handles[2] {m_readEvent.get(), m_pluginProc.get()};
            const DWORD timeout = m_armed ? kWatchdogIntervalMs : 50;
            const DWORD wait = ::MsgWaitForMultipleObjectsEx(2, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0) {
                if (!completeRead()) {
                    finish(CloseReason::Requested);
                    break;
                }
            } else if (wait == WAIT_OBJECT_0 + 1) {
                finish(CloseReason::Requested);
                break;
            } else if (wait == WAIT_OBJECT_0 + 2 || wait == WAIT_TIMEOUT) {
                MSG msg;
                while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        m_done = true;
                        break;
                    }
                    ::TranslateMessage(&msg);
                    ::DispatchMessageW(&msg);
                }
                if (!m_done && m_armed)
                    watchdogCheck();
            } else {
                finish(CloseReason::Error);
                break;
            }
        }
        teardown();
        return 0;
    }

private:
    struct Window {
        HWND hwnd = nullptr;
        HWND overlayHwnd = nullptr;
        HTHUMBNAIL thumb = nullptr;
        RECT rect {};
        bool trackingLeave = false;
    };

    bool isOwnSurface(HWND hwnd) const
    {
        if (!hwnd)
            return false;
        const HWND root = ::GetAncestor(hwnd, GA_ROOT);
        for (const Window& w : m_windows) {
            if (w.hwnd == hwnd || w.hwnd == root)
                return true;
        }
        return false;
    }

    Window* findWindow(HWND hwnd)
    {
        for (Window& w : m_windows) {
            if (w.hwnd == hwnd)
                return &w;
        }
        return nullptr;
    }

    bool sendFrame(std::span<const std::byte> frame)
    {
        if (m_done || !m_pipe.valid())
            return false;
        std::size_t sent = 0;
        while (sent < frame.size()) {
            OVERLAPPED ov {};
            ov.hEvent = m_writeEvent.get();
            ::ResetEvent(m_writeEvent.get());
            DWORD written = 0;
            const BOOL ok = ::WriteFile(m_pipe.get(), frame.data() + sent,
                static_cast<DWORD>(frame.size() - sent), &written, &ov);
            if (!ok) {
                const DWORD err = ::GetLastError();
                if (err != ERROR_IO_PENDING)
                    return false;
                if (::WaitForSingleObject(m_writeEvent.get(), kWriteTimeoutMs) != WAIT_OBJECT_0) {
                    ::CancelIoEx(m_pipe.get(), &ov);
                    ::GetOverlappedResult(m_pipe.get(), &ov, &written, TRUE);
                    return false;
                }
                if (!::GetOverlappedResult(m_pipe.get(), &ov, &written, FALSE))
                    return false;
            }
            if (written == 0)
                return false;
            sent += written;
        }
        return true;
    }

    bool beginRead()
    {
        while (!m_done && !m_readPending) {
            m_readOv = {};
            m_readOv.hEvent = m_readEvent.get();
            ::ResetEvent(m_readEvent.get());
            DWORD got = 0;
            const BOOL ok = ::ReadFile(m_pipe.get(), m_readChunk, sizeof m_readChunk, &got, &m_readOv);
            if (ok) {
                if (!onBytesRead(got))
                    return false;
                continue;
            }
            const DWORD err = ::GetLastError();
            if (err == ERROR_IO_PENDING) {
                m_readPending = true;
                return true;
            }
            return false;
        }
        return true;
    }

    bool completeRead()
    {
        if (!m_readPending)
            return beginRead();
        DWORD got = 0;
        if (!::GetOverlappedResult(m_pipe.get(), &m_readOv, &got, FALSE))
            return false;
        m_readPending = false;
        if (!onBytesRead(got))
            return false;
        return m_done || beginRead();
    }

    bool onBytesRead(DWORD got)
    {
        if (got == 0)
            return false;
        m_rxBuf.insert(m_rxBuf.end(), m_readChunk, m_readChunk + got);
        while (!m_done && m_rxBuf.size() >= kHeaderBytes) {
            Type type {};
            std::uint32_t len = 0;
            std::span<const std::byte, kHeaderBytes> hdr(m_rxBuf.data(), kHeaderBytes);
            if (!parseHeader(hdr, type, len))
                return false;
            if (m_rxBuf.size() < kHeaderBytes + len)
                break;
            std::span<const std::byte> payload(m_rxBuf.data() + kHeaderBytes, len);
            const bool ok = dispatch(type, payload);
            m_rxBuf.erase(m_rxBuf.begin(), m_rxBuf.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + len));
            if (!ok)
                return false;
        }
        return true;
    }

    bool dispatch(Type type, std::span<const std::byte> payload)
    {
        switch (type) {
        case Type::Init: {
            InitMsg msg;
            if (m_armed || !decode(payload, msg))
                return false;
            return handleInit(msg);
        }
        case Type::SetCursor: {
            CursorMsg msg;
            if (!decode(payload, msg))
                return false;
            handleCursor(msg);
            return true;
        }
        case Type::SetIme: {
            ImeStateMsg msg;
            if (!decode(payload, msg))
                return false;
            handleImeState(msg);
            return true;
        }
        case Type::Close:
            finish(CloseReason::Requested);
            return true;
        default:
            return false;
        }
    }

    bool handleInit(const InitMsg& msg)
    {
        auto sendFailure = [&](DWORD err) {
            ReadyMsg fail {0, err, {}};
            sendFrame(encode(fail));
            finish(CloseReason::Error);
            return false;
        };

        // Re-check right before creating Band-2 windows: full-screen Store app
        // must still be in front and in band > 1.
        if (!isFullScreenStoreTarget(m_fullScreenTarget, true))
            return sendFailure(ERROR_NOT_READY);

        std::vector<std::uint64_t> hwnds;
        hwnds.reserve(msg.monitors.size());
        const HINSTANCE inst = ::GetModuleHandleW(nullptr);
        for (const MonitorSpec& spec : msg.monitors) {
            const HWND src = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(spec.overlayHwnd));
            DWORD ownerPid = 0;
            if (!::IsWindow(src) || ::GetAncestor(src, GA_ROOT) != src
                || !::GetWindowThreadProcessId(src, &ownerPid) || ownerPid != m_pluginPid)
                return sendFailure(ERROR_INVALID_WINDOW_HANDLE);

            Window w;
            w.overlayHwnd = src;
            w.rect = {spec.x, spec.y, spec.x + spec.w, spec.y + spec.h};
            w.hwnd = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kSurfaceClass, L"", WS_POPUP,
                spec.x, spec.y, spec.w, spec.h, nullptr, nullptr, inst, this);
            if (!w.hwnd)
                return sendFailure(::GetLastError() ? ::GetLastError() : ERROR_GEN_FAILURE);

            setCloaked(w.hwnd, true);
            ::ImmAssociateContextEx(w.hwnd, nullptr, 0); // IME off until TextEdit has focus
            ::SetWindowPos(w.hwnd, HWND_TOPMOST, spec.x, spec.y, spec.w, spec.h, SWP_NOACTIVATE);
            if (windowBand(w.hwnd) != kUiAccessBand) {
                ::DestroyWindow(w.hwnd);
                return sendFailure(ERROR_NOT_SUPPORTED);
            }
            HTHUMBNAIL thumb = nullptr;
            const HRESULT hrReg = ::DwmRegisterThumbnail(w.hwnd, src, &thumb);
            if (FAILED(hrReg) || !thumb) {
                ::DestroyWindow(w.hwnd);
                return sendFailure(static_cast<DWORD>(hrReg));
            }
            DWM_THUMBNAIL_PROPERTIES props {};
            props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            props.rcDestination = {0, 0, spec.w, spec.h};
            props.fVisible = TRUE;
            props.opacity = 255;
            props.fSourceClientAreaOnly = TRUE;
            const HRESULT hrUpd = ::DwmUpdateThumbnailProperties(thumb, &props);
            if (FAILED(hrUpd)) {
                ::DwmUnregisterThumbnail(thumb);
                ::DestroyWindow(w.hwnd);
                return sendFailure(static_cast<DWORD>(hrUpd));
            }
            w.thumb = thumb;
            hwnds.push_back(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(w.hwnd)));
            m_windows.push_back(w);
        }

        for (std::size_t i = 0; i < m_windows.size(); ++i) {
            const bool active = i == msg.activeIndex;
            ::ShowWindow(m_windows[i].hwnd, active ? SW_SHOW : SW_SHOWNOACTIVATE);
            ::SetWindowPos(m_windows[i].hwnd, HWND_TOPMOST, m_windows[i].rect.left, m_windows[i].rect.top,
                m_windows[i].rect.right - m_windows[i].rect.left, m_windows[i].rect.bottom - m_windows[i].rect.top,
                active ? SWP_SHOWWINDOW : (SWP_NOACTIVATE | SWP_SHOWWINDOW));
        }

        const HWND activeHwnd = m_windows[msg.activeIndex].hwnd;
        ::SetForegroundWindow(activeHwnd);
        ::SetFocus(activeHwnd);
        if (!isOwnSurface(::GetForegroundWindow()))
            return sendFailure(ERROR_ACCESS_DENIED);

        // Let DWM bind the thumbnail before revealing the Band-2 window so the
        // first visible frame already carries the frozen screenshot.
        ::DwmFlush();
        for (const Window& w : m_windows)
            setCloaked(w.hwnd, false);

        m_armed = true;
        ::SetTimer(activeHwnd, kWatchdogTimerId, kWatchdogIntervalMs, nullptr);
        ReadyMsg ready {1, 0, std::move(hwnds)};
        if (!sendFrame(encode(ready))) {
            finish(CloseReason::Error);
            return false;
        }
        // Apply current cursor immediately if the pointer is already over us.
        POINT pt {};
        if (::GetCursorPos(&pt) && isOwnSurface(::WindowFromPoint(pt)))
            ::SetCursor(m_currentCursor);
        return true;
    }

    void handleCursor(const CursorMsg& msg)
    {
        if (msg.kind == CursorKind::Standard) {
            m_currentCursor = standardCursorForQtShape(msg.qtShape);
        } else {
            auto it = m_rgbaCursors.find(msg.cacheKey);
            if (it == m_rgbaCursors.end() && msg.width > 0 && msg.height > 0) {
                if (m_rgbaCursors.size() >= kMaxCachedCursors) {
                    if (m_rgbaCursors.begin()->second)
                        ::DestroyIcon(m_rgbaCursors.begin()->second);
                    m_rgbaCursors.erase(m_rgbaCursors.begin());
                }
                if (HCURSOR created = createRgbaCursor(msg.width, msg.height, msg.hotX, msg.hotY, msg.bgra))
                    it = m_rgbaCursors.emplace(msg.cacheKey, created).first;
            }
            if (it != m_rgbaCursors.end())
                m_currentCursor = it->second;
        }
        POINT pt {};
        if (m_armed && ::GetCursorPos(&pt) && isOwnSurface(::WindowFromPoint(pt)))
            ::SetCursor(m_currentCursor);
    }

    void handleImeState(const ImeStateMsg& msg)
    {
        if (!m_armed || m_windows.empty())
            return;
        m_imeEnabled = msg.enabled;
        const std::size_t idx = msg.monitorIndex < m_windows.size() ? msg.monitorIndex : 0;
        const HWND target = m_windows[idx].hwnd;
        for (const Window& w : m_windows) {
            const bool enableHere = msg.enabled && w.hwnd == target;
            ::ImmAssociateContextEx(w.hwnd, nullptr, enableHere ? IACE_DEFAULT : 0);
        }
        if (msg.enabled && target) {
            if (HIMC himc = ::ImmGetContext(target)) {
                COMPOSITIONFORM cf {};
                cf.dwStyle = CFS_POINT;
                cf.ptCurrentPos = {msg.caretX, msg.caretY + msg.caretH};
                ::ImmSetCompositionWindow(himc, &cf);
                CANDIDATEFORM cand {};
                cand.dwIndex = 0;
                cand.dwStyle = CFS_EXCLUDE;
                cand.ptCurrentPos = {msg.caretX, msg.caretY + msg.caretH};
                cand.rcArea = {msg.caretX, msg.caretY, msg.caretX + std::max(1, msg.caretW),
                    msg.caretY + std::max(1, msg.caretH)};
                ::ImmSetCandidateWindow(himc, &cand);
                ::ImmReleaseContext(target, himc);
            }
        }
    }

    void watchdogCheck()
    {
        if (!m_armed || m_done)
            return;
        if (::WaitForSingleObject(m_pluginProc.get(), 0) != WAIT_TIMEOUT) {
            finish(CloseReason::Requested);
            return;
        }
        if (!isFullScreenStoreTarget(m_fullScreenTarget, false)) {
            finish(CloseReason::FullScreenGone);
            return;
        }
        if (!isOwnSurface(::GetForegroundWindow())) {
            finish(CloseReason::Deactivated);
            return;
        }
    }

    void finish(CloseReason reason)
    {
        if (m_done)
            return;
        m_done = true;
        m_armed = false;
        if (reason != CloseReason::Requested) {
            // Best-effort notice before teardown.
            ClosedMsg msg {reason};
            const auto frame = encode(msg);
            if (m_pipe.valid()) {
                OVERLAPPED ov {};
                ov.hEvent = m_writeEvent.get();
                ::ResetEvent(m_writeEvent.get());
                DWORD written = 0;
                if (!::WriteFile(m_pipe.get(), frame.data(), static_cast<DWORD>(frame.size()), &written, &ov)
                    && ::GetLastError() == ERROR_IO_PENDING) {
                    ::WaitForSingleObject(m_writeEvent.get(), 150);
                    ::CancelIoEx(m_pipe.get(), &ov);
                }
            }
        }
        teardown();
    }

    void teardown()
    {
        m_armed = false;
        if (m_readPending && m_pipe.valid()) {
            ::CancelIoEx(m_pipe.get(), &m_readOv);
            DWORD unused = 0;
            ::GetOverlappedResult(m_pipe.get(), &m_readOv, &unused, TRUE);
            m_readPending = false;
        }
        for (Window& w : m_windows) {
            if (w.thumb) {
                ::DwmUnregisterThumbnail(w.thumb);
                w.thumb = nullptr;
            }
            if (w.hwnd) {
                ::KillTimer(w.hwnd, kWatchdogTimerId);
                ::ShowWindow(w.hwnd, SW_HIDE);
                ::DestroyWindow(w.hwnd);
                w.hwnd = nullptr;
            }
        }
        m_windows.clear();
    }

    void relayMouse(HWND hwnd, MouseKind kind, MouseButton button, WPARAM wp, LPARAM lp, bool lParamIsScreen = false,
        std::int16_t wheelDelta = 0)
    {
        if (!m_armed || m_done)
            return;
        POINT pt {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!lParamIsScreen)
            ::ClientToScreen(hwnd, &pt);
        MouseMsg msg;
        msg.kind = kind;
        msg.button = button;
        msg.buttons = buttonsFromWParam(wp);
        msg.modifiers = currentModifiers();
        msg.globalX = pt.x;
        msg.globalY = pt.y;
        msg.wheelDelta = wheelDelta;
        if (!sendFrame(encode(msg)))
            finish(CloseReason::Error);
    }

    void relayKey(bool press, WPARAM wp, LPARAM lp)
    {
        if (!m_armed || m_done)
            return;
        KeyMsg msg;
        msg.press = press;
        msg.repeat = press && ((lp & 0x40000000) != 0);
        msg.extended = (lp & 0x01000000) != 0;
        msg.modifiers = currentModifiers();
        msg.vk = static_cast<std::uint32_t>(wp);
        msg.scan = static_cast<std::uint32_t>((lp >> 16) & 0xFF);
        if (press && msg.vk != VK_PROCESSKEY) {
            BYTE state[256] {};
            if (::GetKeyboardState(state)) {
                wchar_t chars[8] {};
                const int n = ::ToUnicodeEx(msg.vk, msg.scan, state, chars, static_cast<int>(std::size(chars)),
                    kToUnicodeKeepState, ::GetKeyboardLayout(0));
                if (n > 0)
                    msg.text.assign(reinterpret_cast<const char16_t*>(chars), static_cast<std::size_t>(n));
            }
        }
        if (!sendFrame(encode(msg)))
            finish(CloseReason::Error);
    }

    void relayImeString(HWND hwnd, DWORD index, ImeKind kind)
    {
        HIMC himc = ::ImmGetContext(hwnd);
        if (!himc)
            return;
        const LONG bytes = ::ImmGetCompositionStringW(himc, index, nullptr, 0);
        std::u16string text;
        if (bytes > 0 && static_cast<std::size_t>(bytes) <= std::size_t {kMaxTextChars} * sizeof(char16_t)) {
            text.resize(static_cast<std::size_t>(bytes) / sizeof(char16_t));
            ::ImmGetCompositionStringW(himc, index, text.data(), static_cast<DWORD>(bytes));
        }
        std::int32_t cursor = static_cast<std::int32_t>(text.size());
        if (kind == ImeKind::Preedit) {
            const LONG c = ::ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
            if (c >= 0)
                cursor = static_cast<std::int32_t>(c);
        }
        ::ImmReleaseContext(hwnd, himc);
        ImeEventMsg msg {kind, cursor, std::move(text)};
        if (!sendFrame(encode(msg)))
            finish(CloseReason::Error);
    }

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps {};
            ::BeginPaint(hwnd, &ps);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                ::SetCursor(m_currentCursor);
                return TRUE;
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && m_armed && !m_done) {
                const HWND next = reinterpret_cast<HWND>(lp);
                if (!isOwnSurface(next)) {
                    finish(CloseReason::Deactivated);
                    return 0;
                }
            }
            return ::DefWindowProcW(hwnd, msg, wp, lp);
        case WM_CLOSE:
            finish(CloseReason::Deactivated);
            return 0;
        case WM_TIMER:
            if (wp == kWatchdogTimerId) {
                watchdogCheck();
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (Window* w = findWindow(hwnd); w && !w->trackingLeave) {
                TRACKMOUSEEVENT tme {sizeof tme, TME_LEAVE, hwnd, 0};
                w->trackingLeave = ::TrackMouseEvent(&tme) != FALSE;
            }
            relayMouse(hwnd, MouseKind::Move, MouseButton::None, wp, lp);
            return 0;
        case WM_MOUSELEAVE:
            if (Window* w = findWindow(hwnd))
                w->trackingLeave = false;
            if (::GetCapture() != hwnd) {
                POINT pt {};
                ::GetCursorPos(&pt);
                relayMouse(hwnd, MouseKind::Leave, MouseButton::None, 0, MAKELPARAM(pt.x, pt.y), true);
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (::GetCapture() != hwnd)
                ::SetCapture(hwnd);
            relayMouse(hwnd, MouseKind::Press, MouseButton::Left, wp, lp);
            return 0;
        case WM_LBUTTONUP:
            relayMouse(hwnd, MouseKind::Release, MouseButton::Left, wp, lp);
            if (buttonsFromWParam(wp) == 0 && ::GetCapture() == hwnd)
                ::ReleaseCapture();
            return 0;
        case WM_LBUTTONDBLCLK:
            if (::GetCapture() != hwnd)
                ::SetCapture(hwnd);
            relayMouse(hwnd, MouseKind::DblClick, MouseButton::Left, wp, lp);
            return 0;
        case WM_RBUTTONDOWN:
            if (::GetCapture() != hwnd)
                ::SetCapture(hwnd);
            relayMouse(hwnd, MouseKind::Press, MouseButton::Right, wp, lp);
            return 0;
        case WM_RBUTTONUP:
            relayMouse(hwnd, MouseKind::Release, MouseButton::Right, wp, lp);
            if (buttonsFromWParam(wp) == 0 && ::GetCapture() == hwnd)
                ::ReleaseCapture();
            return 0;
        case WM_RBUTTONDBLCLK:
            relayMouse(hwnd, MouseKind::DblClick, MouseButton::Right, wp, lp);
            return 0;
        case WM_MBUTTONDOWN:
            if (::GetCapture() != hwnd)
                ::SetCapture(hwnd);
            relayMouse(hwnd, MouseKind::Press, MouseButton::Middle, wp, lp);
            return 0;
        case WM_MBUTTONUP:
            relayMouse(hwnd, MouseKind::Release, MouseButton::Middle, wp, lp);
            if (buttonsFromWParam(wp) == 0 && ::GetCapture() == hwnd)
                ::ReleaseCapture();
            return 0;
        case WM_XBUTTONDOWN:
            if (::GetCapture() != hwnd)
                ::SetCapture(hwnd);
            relayMouse(hwnd, MouseKind::Press,
                GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? MouseButton::X1 : MouseButton::X2, wp, lp);
            return TRUE;
        case WM_XBUTTONUP:
            relayMouse(hwnd, MouseKind::Release,
                GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? MouseButton::X1 : MouseButton::X2, wp, lp);
            if (buttonsFromWParam(wp) == 0 && ::GetCapture() == hwnd)
                ::ReleaseCapture();
            return TRUE;
        case WM_MOUSEWHEEL:
            relayMouse(hwnd, MouseKind::Wheel, MouseButton::None, GET_KEYSTATE_WPARAM(wp), lp, true,
                static_cast<std::int16_t>(GET_WHEEL_DELTA_WPARAM(wp)));
            return 0;
        case WM_MOUSEHWHEEL:
            relayMouse(hwnd, MouseKind::HWheel, MouseButton::None, GET_KEYSTATE_WPARAM(wp), lp, true,
                static_cast<std::int16_t>(GET_WHEEL_DELTA_WPARAM(wp)));
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_PROCESSKEY)
                return ::DefWindowProcW(hwnd, msg, wp, lp);
            if (msg == WM_SYSKEYDOWN && wp == VK_F4) {
                finish(CloseReason::Deactivated);
                return 0;
            }
            relayKey(true, wp, lp);
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wp == VK_PROCESSKEY)
                return ::DefWindowProcW(hwnd, msg, wp, lp);
            relayKey(false, wp, lp);
            return 0;
        case WM_IME_COMPOSITION:
            if (m_imeEnabled) {
                if (lp & GCS_RESULTSTR)
                    relayImeString(hwnd, GCS_RESULTSTR, ImeKind::Commit);
                if (lp & GCS_COMPSTR)
                    relayImeString(hwnd, GCS_COMPSTR, ImeKind::Preedit);
                return 0;
            }
            break;
        case WM_IME_ENDCOMPOSITION:
            if (m_imeEnabled && m_armed && !m_done) {
                ImeEventMsg ev {ImeKind::End, 0, {}};
                if (!sendFrame(encode(ev)))
                    finish(CloseReason::Error);
            }
            break;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        auto* self = reinterpret_cast<SurfaceSession*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self ? self->handleMessage(hwnd, msg, wp, lp) : ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    const DWORD m_pluginPid;
    UniqueHandle m_pluginProc;
    const HWND m_fullScreenTarget;
    UniqueHandle m_pipe;
    UniqueHandle m_readEvent;
    UniqueHandle m_writeEvent;
    OVERLAPPED m_readOv {};
    bool m_readPending = false;
    std::byte m_readChunk[4096] {};
    std::vector<std::byte> m_rxBuf;
    std::vector<Window> m_windows;
    HCURSOR m_currentCursor = nullptr;
    std::unordered_map<std::uint64_t, HCURSOR> m_rgbaCursors;
    bool m_imeEnabled = false;
    bool m_armed = false;
    bool m_done = false;
};

int runCaptureSurfaceImpl(DWORD pluginPid, HWND fullScreenTarget, const std::wstring& pipeName)
{
    enablePerMonitorDpi();
    if (!isSessionHelperToken())
        return ERROR_ACCESS_DENIED;
    if (!isValidPipeName(pipeName, pluginPid))
        return ERROR_INVALID_PARAMETER;
    if (!sameSession(pluginPid))
        return ERROR_ACCESS_DENIED;
    const auto pluginIl = integrityOfPid(pluginPid);
    if (!pluginIl || *pluginIl > SECURITY_MANDATORY_MEDIUM_RID)
        return ERROR_ACCESS_DENIED;
    UniqueHandle pluginProc(::OpenProcess(SYNCHRONIZE, FALSE, pluginPid));
    if (!pluginProc.valid())
        return ERROR_INVALID_PARAMETER;
    if (!isFullScreenStoreTarget(fullScreenTarget, true))
        return ERROR_NOT_READY;

    const std::wstring fullPipe = L"\\\\.\\pipe\\" + pipeName;
    UniqueHandle pipe;
    const ULONGLONG deadline = ::GetTickCount64() + kConnectWaitMs;
    while (::GetTickCount64() < deadline) {
        const HANDLE h = ::CreateFileW(fullPipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            pipe.reset(h);
            break;
        }
        const DWORD err = ::GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND)
            return static_cast<int>(err);
        ::Sleep(15);
    }
    if (!pipe.valid())
        return ERROR_TIMEOUT;

    ULONG serverPid = 0;
    if (!::GetNamedPipeServerProcessId(pipe.get(), &serverPid) || serverPid != pluginPid)
        return ERROR_ACCESS_DENIED;

    SurfaceSession session(pluginPid, std::move(pluginProc), fullScreenTarget, std::move(pipe));
    return session.run();
}

} // namespace

int runCaptureSurface(DWORD pluginPid, HWND fullScreenTarget, const std::wstring& pipeName)
{
    try {
        return runCaptureSurfaceImpl(pluginPid, fullScreenTarget, pipeName);
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

} // namespace ws::service
