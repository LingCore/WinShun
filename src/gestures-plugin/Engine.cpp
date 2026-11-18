#include "Engine.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
using std::max;
using std::min;
#include <gdiplus.h>

namespace gestures {

namespace {

constexpr ULONG_PTR kOwnInput = 0x57534753; // "WSGS": input this plugin sends
constexpr ULONG_PTR kTouchMask = 0xFFFFFF00; // mouse messages made from touch or a pen
constexpr ULONG_PTR kTouchSignature = 0xFF515700;
constexpr UINT kHoldMs = 350; // held still this long: the press goes through
constexpr int kStartDistance = 6; // logical pixels moved: a gesture
constexpr int kStroke = 24; // logical pixels along a direction: it counts

enum : UINT {
    kStart = WM_APP + 1, // w: point, l: the window under it
    kMove, // w: point, l: directions
    kEnd, // w: point, l: directions
    kCancel,
    kClick, // the press held back and its release, sent again
    kPressThrough, // the press held back, sent again (the rest goes through)
};
constexpr UINT_PTR kFrameTimer = 1;

Engine* g_engine = nullptr;

WPARAM packPoint(POINT p)
{
    return static_cast<WPARAM>(static_cast<std::uint32_t>(p.x)) | (static_cast<WPARAM>(static_cast<std::uint32_t>(p.y)) << 32);
}

POINT unpackPoint(WPARAM w)
{
    return {static_cast<LONG>(static_cast<std::int32_t>(w & 0xffffffff)), static_cast<LONG>(static_cast<std::int32_t>(w >> 32))};
}

// Up to 8 directions in the 64 bits; more than kMaxDirections (6) is too
// long whatever follows.
static_assert(kMaxDirections < 8);
LPARAM packDirections(const std::string& d)
{
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < d.size() && i < 8; ++i)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(d[i])) << (8 * i);
    return static_cast<LPARAM>(v);
}

std::string unpackDirections(LPARAM l)
{
    const auto v = static_cast<std::uint64_t>(l);
    std::string d;
    for (int i = 0; i < 8; ++i) {
        const char c = static_cast<char>((v >> (8 * i)) & 0xff);
        if (c == 0)
            break;
        d += c;
    }
    return d;
}

double scaleAt(POINT p)
{
    UINT x = 96;
    UINT y = 96;
    if (FAILED(::GetDpiForMonitor(::MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &x, &y)))
        x = 96;
    return x / 96.0;
}

std::wstring className(HWND window)
{
    wchar_t name[64] {};
    ::GetClassNameW(window, name, 64);
    return name;
}

bool shellWindow(HWND root)
{
    const std::wstring c = className(root);
    return c == L"Progman" || c == L"WorkerW" || c == L"Shell_TrayWnd" || c == L"Shell_SecondaryTrayWnd";
}

std::optional<DWORD> levelOf(HANDLE token)
{
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[64];
    DWORD size = 0;
    if (!::GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof buffer, &size))
        return std::nullopt;
    const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
    const UCHAR count = *::GetSidSubAuthorityCount(sid);
    if (count == 0)
        return std::nullopt;
    return *::GetSidSubAuthority(sid, count - 1);
}

DWORD ownLevel()
{
    static const DWORD level = [] {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            return DWORD(SECURITY_MANDATORY_MEDIUM_RID);
        const DWORD l = levelOf(token).value_or(DWORD(SECURITY_MANDATORY_MEDIUM_RID));
        ::CloseHandle(token);
        return l;
    }();
    return level;
}

// The process of `pid`: whether it runs above this one (one that cannot be
// asked counts as above), and its program's file name.
bool aboveUs(HANDLE process)
{
    if (ownLevel() >= SECURITY_MANDATORY_HIGH_RID)
        return false;
    HANDLE token = nullptr;
    if (!process || !::OpenProcessToken(process, TOKEN_QUERY, &token))
        return true;
    const std::optional<DWORD> level = levelOf(token);
    ::CloseHandle(token);
    return !level || *level > ownLevel();
}

std::wstring programOf(HANDLE process)
{
    wchar_t path[MAX_PATH * 2] {};
    DWORD size = MAX_PATH * 2;
    if (!process || !::QueryFullProcessImageNameW(process, 0, path, &size))
        return {};
    return std::wstring(path, size);
}

// A window covering its whole monitor, taskbar and all: a game, a slide
// show, a video in full screen.
bool fullScreen(HWND root)
{
    if (shellWindow(root))
        return false;
    RECT window {};
    if (!::GetWindowRect(root, &window))
        return false;
    MONITORINFO monitor {sizeof monitor};
    if (!::GetMonitorInfoW(::MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    return window.left <= monitor.rcMonitor.left && window.top <= monitor.rcMonitor.top
        && window.right >= monitor.rcMonitor.right && window.bottom >= monitor.rcMonitor.bottom;
}

bool extendedKey(std::uint16_t vk)
{
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_DIVIDE: case VK_NUMLOCK:
    case VK_SNAPSHOT: case VK_RCONTROL: case VK_RMENU:
        return true;
    default:
        return false;
    }
}

Gdiplus::Color color(std::uint32_t argb)
{
    return Gdiplus::Color(static_cast<BYTE>(argb >> 24), static_cast<BYTE>(argb >> 16), static_cast<BYTE>(argb >> 8), static_cast<BYTE>(argb));
}

std::wstring fromUtf8(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

} // namespace

Engine::Engine(EngineOptions options)
    : m_options(std::move(options))
    , m_shared(std::make_shared<Shared>())
{
}

Engine::~Engine()
{
    stop();
}

std::shared_ptr<const Engine::Shared> Engine::shared() const
{
    std::lock_guard lock(m_mutex);
    return m_shared;
}

void Engine::configure(Settings settings, Look look)
{
    auto next = std::make_shared<Shared>();
    next->settings = std::move(settings);
    next->look = look;
    std::lock_guard lock(m_mutex);
    m_shared = std::move(next);
}

void Engine::start()
{
    if (g_engine)
        return;
    g_engine = this;
    std::promise<bool> windowReady;
    auto windowStarted = windowReady.get_future();
    m_windowThread = std::thread([this, &windowReady] { windowThread(&windowReady); });
    if (!windowStarted.get()) {
        m_options.log("cannot make the trail window");
        return;
    }
    std::promise<bool> hookReady;
    auto hookStarted = hookReady.get_future();
    m_hookThread = std::thread([this, &hookReady] { hookThread(&hookReady); });
    if (!hookStarted.get())
        m_options.log("cannot hook the mouse: error " + std::to_string(::GetLastError()));
}

void Engine::stop()
{
    if (m_hookThread.joinable()) {
        ::PostThreadMessageW(m_hookThreadId, WM_QUIT, 0, 0);
        m_hookThread.join();
    }
    if (m_windowThread.joinable()) {
        ::PostMessageW(m_window, WM_CLOSE, 0, 0);
        m_windowThread.join();
    }
    if (g_engine == this)
        g_engine = nullptr;
}

// ---- the hook thread ----

void Engine::hookThread(std::promise<bool>* ready)
{
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    m_hookThreadId = ::GetCurrentThreadId();
    MSG msg;
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the queue, before anyone posts
    const HHOOK hook = ::SetWindowsHookExW(WH_MOUSE_LL, &Engine::mouseProc, ::GetModuleHandleW(nullptr), 0);
    ready->set_value(hook != nullptr);
    if (!hook)
        return;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == nullptr && msg.wParam == m_holdTimer) {
            onHoldTimer();
            continue;
        }
        ::DispatchMessageW(&msg);
    }
    if (m_holdTimer)
        ::KillTimer(nullptr, m_holdTimer);
    ::UnhookWindowsHookEx(hook);
}

LRESULT CALLBACK Engine::mouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_engine && g_engine->onMouse(wParam, *reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam)))
        return 1;
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

// Whether the right button pressed at `pt` may make a gesture.
bool Engine::eligible(POINT pt, HWND& under)
{
    if (m_paused)
        return false;
    for (const int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN}) {
        if (::GetAsyncKeyState(key) & 0x8000)
            return false;
    }
    under = ::WindowFromPoint(pt);
    const HWND root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;
    if (!root)
        return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(root, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId())
        return false;
    const std::shared_ptr<const Shared> s = shared();
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    const bool above = aboveUs(process);
    const std::wstring program = above ? std::wstring() : programOf(process);
    if (process)
        ::CloseHandle(process);
    if (above || (!s->settings.excluded.empty() && listed(s->settings.excluded, program)))
        return false;
    if (!s->settings.fullScreen && root == ::GetForegroundWindow() && fullScreen(root))
        return false;
    return true;
}

bool Engine::onMouse(WPARAM message, const MSLLHOOKSTRUCT& info)
{
    // Sent by a program (this one's own clicks again among them), or made
    // from touch or a pen: never ours.
    if ((info.flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) || (info.dwExtraInfo & kTouchMask) == kTouchSignature)
        return false;
    switch (message) {
    case WM_RBUTTONDOWN: {
        if (m_state != State::Idle && m_state != State::Through) {
            // A release never seen (the hook was late): start over.
            if (m_holdTimer)
                ::KillTimer(nullptr, std::exchange(m_holdTimer, 0));
            ::PostMessageW(m_window, kCancel, 0, 0);
        }
        m_state = State::Idle;
        HWND under = nullptr;
        if (!eligible(info.pt, under))
            return false;
        const double scale = scaleAt(info.pt);
        m_state = State::Pressed;
        m_down = info.pt;
        m_under = under;
        m_startDistance = static_cast<int>(std::lround(kStartDistance * scale));
        m_recognizer.reset(info.pt.x, info.pt.y, static_cast<int>(std::lround(kStroke * scale)));
        m_sentDirections.clear();
        m_holdTimer = ::SetTimer(nullptr, 0, kHoldMs, nullptr);
        return true;
    }
    case WM_MOUSEMOVE:
        if (m_state == State::Pressed) {
            const int dx = info.pt.x - m_down.x;
            const int dy = info.pt.y - m_down.y;
            if (dx * dx + dy * dy < m_startDistance * m_startDistance)
                return false;
            if (m_holdTimer)
                ::KillTimer(nullptr, std::exchange(m_holdTimer, 0));
            m_state = State::Gesturing;
            ::PostMessageW(m_window, kStart, packPoint(m_down), reinterpret_cast<LPARAM>(m_under));
        }
        if (m_state == State::Gesturing) {
            m_recognizer.add(info.pt.x, info.pt.y);
            ::PostMessageW(m_window, kMove, packPoint(info.pt), packDirections(m_recognizer.directions()));
        }
        return false;
    case WM_LBUTTONDOWN:
        // The left button while drawing: no gesture after all.
        if (m_state == State::Pressed || m_state == State::Gesturing) {
            if (m_holdTimer)
                ::KillTimer(nullptr, std::exchange(m_holdTimer, 0));
            m_state = State::Canceled;
            ::PostMessageW(m_window, kCancel, 0, 0);
        }
        return false;
    case WM_RBUTTONUP:
        if (m_holdTimer)
            ::KillTimer(nullptr, std::exchange(m_holdTimer, 0));
        switch (std::exchange(m_state, State::Idle)) {
        case State::Pressed:
            ::PostMessageW(m_window, kClick, 0, 0);
            return true;
        case State::Gesturing:
            m_recognizer.add(info.pt.x, info.pt.y);
            ::PostMessageW(m_window, kEnd, packPoint(info.pt), packDirections(m_recognizer.directions()));
            return true;
        case State::Canceled:
            return true;
        case State::Idle:
        case State::Through:
            return false;
        }
        return false;
    default:
        return false;
    }
}

void Engine::onHoldTimer()
{
    if (m_holdTimer)
        ::KillTimer(nullptr, std::exchange(m_holdTimer, 0));
    if (m_state != State::Pressed)
        return;
    m_state = State::Through;
    ::PostMessageW(m_window, kPressThrough, 0, 0);
}

// ---- the window thread ----

void Engine::windowThread(std::promise<bool>* ready)
{
    ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&m_gdiplus, &input, nullptr);
    WNDCLASSEXW wc {sizeof wc};
    wc.lpfnWndProc = &Engine::windowProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WinShun.Gestures.Trail";
    ::RegisterClassExW(&wc);
    m_window = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, this);
    ready->set_value(m_window != nullptr);
    if (m_window) {
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&msg);
    }
    if (m_memory)
        ::DeleteDC(m_memory);
    if (m_bitmap)
        ::DeleteObject(m_bitmap);
    m_memory = nullptr;
    m_bitmap = nullptr;
    Gdiplus::GdiplusShutdown(m_gdiplus);
}

LRESULT CALLBACK Engine::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    auto* self = reinterpret_cast<Engine*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    return self ? self->onWindowMessage(window, message, wParam, lParam) : ::DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Engine::onWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case kStart: {
        const POINT p = unpackPoint(wParam);
        m_target = reinterpret_cast<HWND>(lParam);
        m_points.assign(1, p);
        m_directions.clear();
        m_scale = scaleAt(p);
        return 0;
    }
    case kMove:
        m_points.push_back(unpackPoint(wParam));
        m_directions = unpackDirections(lParam);
        if (!shared()->settings.trail)
            return 0;
        m_dirty = true;
        if (!m_timer) {
            render(); // the first frame at once, then at most one a frame
            m_timer = ::SetTimer(window, kFrameTimer, 15, nullptr) != 0;
        }
        return 0;
    case WM_TIMER:
        if (wParam == kFrameTimer) {
            if (m_dirty) {
                render();
            } else {
                ::KillTimer(window, kFrameTimer);
                m_timer = false;
            }
        }
        return 0;
    case kEnd:
        hideTrail();
        perform(unpackDirections(lParam), unpackPoint(wParam));
        return 0;
    case kCancel:
        hideTrail();
        return 0;
    case kClick:
        sendButton(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0);
        return 0;
    case kPressThrough:
        sendButton(MOUSEEVENTF_RIGHTDOWN, 0, 0);
        return 0;
    case WM_CLOSE:
        ::DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

void Engine::hideTrail()
{
    if (m_timer) {
        ::KillTimer(m_window, kFrameTimer);
        m_timer = false;
    }
    m_dirty = false;
    m_points.clear();
    m_directions.clear();
    if (m_shown) {
        ::ShowWindow(m_window, SW_HIDE);
        m_shown = false;
    }
}

// The trail and, once it has a direction, the hint: the arrows and what
// they do. The window covers only them, not the screen.
void Engine::render()
{
    m_dirty = false;
    if (m_points.size() < 2)
        return;
    const std::shared_ptr<const Shared> s = shared();
    const Look& look = s->look;
    const float pen = static_cast<float>(4 * m_scale);

    RECT box {m_points[0].x, m_points[0].y, m_points[0].x, m_points[0].y};
    for (const POINT& p : m_points) {
        box.left = std::min(box.left, p.x);
        box.top = std::min(box.top, p.y);
        box.right = std::max(box.right, p.x);
        box.bottom = std::max(box.bottom, p.y);
    }
    const int margin = static_cast<int>(std::ceil(pen)) + 2;
    ::InflateRect(&box, margin, margin);

    std::wstring hint;
    bool known = false;
    if (!m_directions.empty()) {
        hint = fromUtf8(arrows(m_directions.substr(0, kMaxDirections)));
        if (m_directions.size() > kMaxDirections) {
            hint += look.chinese ? L"  太长了，松开不做什么" : L"  Too long: nothing on release";
        } else if (const auto found = s->settings.gestures.find(m_directions); found != s->settings.gestures.end()) {
            hint += L"  " + actionTitle(found->second, look.chinese);
            known = found->second.kind != Action::Kind::Invalid;
        } else {
            hint += look.chinese ? L"  没有设置" : L"  Not set";
        }
    }

    Gdiplus::FontFamily family(look.chinese ? L"Microsoft YaHei UI" : L"Segoe UI");
    Gdiplus::Font font(&family, static_cast<Gdiplus::REAL>(13 * m_scale), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    RECT label {};
    Gdiplus::RectF textBox;
    if (!hint.empty()) {
        Gdiplus::Bitmap probe(1, 1, PixelFormat32bppPARGB);
        Gdiplus::Graphics measure(&probe);
        measure.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        measure.MeasureString(hint.c_str(), static_cast<INT>(hint.size()), &font, Gdiplus::PointF(0, 0), &textBox);
        const int padX = static_cast<int>(std::lround(10 * m_scale));
        const int padY = static_cast<int>(std::lround(5 * m_scale));
        const POINT last = m_points.back();
        const int offset = static_cast<int>(std::lround(18 * m_scale));
        label.left = last.x + offset;
        label.top = last.y + offset;
        label.right = label.left + static_cast<int>(std::ceil(textBox.Width)) + 2 * padX;
        label.bottom = label.top + static_cast<int>(std::ceil(textBox.Height)) + 2 * padY;
        // On the screen the pointer is on: to the other side near an edge.
        MONITORINFO mi {sizeof mi};
        if (::GetMonitorInfoW(::MonitorFromPoint(last, MONITOR_DEFAULTTONEAREST), &mi)) {
            if (label.right > mi.rcWork.right)
                ::OffsetRect(&label, -(label.right - label.left) - 2 * offset, 0);
            if (label.bottom > mi.rcWork.bottom)
                ::OffsetRect(&label, 0, -(label.bottom - label.top) - 2 * offset);
        }
        ::UnionRect(&box, &box, &label);
    }

    const SIZE size {box.right - box.left, box.bottom - box.top};
    if (size.cx <= 0 || size.cy <= 0 || size.cx > 16384 || size.cy > 16384)
        return;
    const HDC screen = ::GetDC(nullptr);
    if (!m_memory)
        m_memory = ::CreateCompatibleDC(screen);
    if (size.cx > m_capacity.cx || size.cy > m_capacity.cy) {
        const SIZE grown {std::max(size.cx, m_capacity.cx), std::max(size.cy, m_capacity.cy)};
        BITMAPINFO bi {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = grown.cx;
        bi.bmiHeader.biHeight = -grown.cy;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        const HBITMAP bitmap = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) {
            ::ReleaseDC(nullptr, screen);
            return;
        }
        ::SelectObject(m_memory, bitmap);
        if (m_bitmap)
            ::DeleteObject(m_bitmap);
        m_bitmap = bitmap;
        m_bits = bits;
        m_capacity = grown;
    }
    {
        const INT stride = m_capacity.cx * 4;
        Gdiplus::Bitmap canvas(size.cx, size.cy, stride, PixelFormat32bppPARGB, static_cast<BYTE*>(m_bits));
        Gdiplus::Graphics g(&canvas);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        std::vector<Gdiplus::PointF> points;
        points.reserve(m_points.size());
        for (const POINT& p : m_points)
            points.emplace_back(static_cast<Gdiplus::REAL>(p.x - box.left), static_cast<Gdiplus::REAL>(p.y - box.top));
        Gdiplus::Pen line(color((look.accent & 0x00ffffff) | 0xe0000000), pen);
        line.SetLineJoin(Gdiplus::LineJoinRound);
        line.SetStartCap(Gdiplus::LineCapRound);
        line.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLines(&line, points.data(), static_cast<INT>(points.size()));
        if (!hint.empty()) {
            const Gdiplus::REAL x = static_cast<Gdiplus::REAL>(label.left - box.left);
            const Gdiplus::REAL y = static_cast<Gdiplus::REAL>(label.top - box.top);
            const Gdiplus::REAL w = static_cast<Gdiplus::REAL>(label.right - label.left);
            const Gdiplus::REAL h = static_cast<Gdiplus::REAL>(label.bottom - label.top);
            const Gdiplus::REAL r = h / 2;
            Gdiplus::GraphicsPath pill;
            pill.AddArc(x, y, 2 * r, h, 90, 180);
            pill.AddArc(x + w - 2 * r, y, 2 * r, h, 270, 180);
            pill.CloseFigure();
            Gdiplus::SolidBrush fill(color(known ? look.accent : look.muted));
            g.FillPath(&fill, &pill);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            Gdiplus::SolidBrush ink(color(look.onAccent));
            Gdiplus::StringFormat format;
            format.SetAlignment(Gdiplus::StringAlignmentCenter);
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(hint.c_str(), static_cast<INT>(hint.size()), &font, Gdiplus::RectF(x, y, w, h), &format, &ink);
        }
    }
    POINT origin {box.left, box.top};
    POINT source {0, 0};
    BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    SIZE shown = size;
    ::UpdateLayeredWindow(m_window, screen, &origin, &shown, m_memory, &source, 0, &blend, ULW_ALPHA);
    ::ReleaseDC(nullptr, screen);
    if (!m_shown) {
        ::SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        m_shown = true;
    }
}

void Engine::sendButton(DWORD down, DWORD up, DWORD data)
{
    INPUT in[2] {};
    UINT n = 0;
    for (const DWORD flag : {down, up}) {
        if (!flag)
            continue;
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = flag;
        in[n].mi.mouseData = data;
        in[n].mi.dwExtraInfo = kOwnInput;
        ++n;
    }
    ::SendInput(n, in, sizeof(INPUT));
}

void Engine::sendKeys(const std::vector<std::uint16_t>& keys)
{
    std::vector<INPUT> in;
    const auto key = [&](std::uint16_t vk, bool up) {
        INPUT i {};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        i.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
        i.ki.dwExtraInfo = kOwnInput;
        in.push_back(i);
    };
    for (const std::uint16_t vk : keys)
        key(vk, false);
    for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        key(*it, true);
    ::SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
}

// `root` in front, for keys to go to it. Windows lets the process that sent
// the latest input set the foreground: an input that does nothing first,
// as Win顺 does (WindowEffects.cpp bringToFront).
bool Engine::bringToFront(HWND root)
{
    if (::GetForegroundWindow() == root)
        return true;
    if (!::AllowSetForegroundWindow(::GetCurrentProcessId())) {
        INPUT nothing {};
        nothing.type = INPUT_MOUSE;
        nothing.mi.dwExtraInfo = kOwnInput;
        ::SendInput(1, &nothing, sizeof nothing);
    }
    if (::IsIconic(root))
        ::ShowWindow(root, SW_RESTORE);
    ::SetForegroundWindow(root);
    for (int i = 0; i < 10 && ::GetForegroundWindow() != root; ++i)
        ::Sleep(5);
    return ::GetForegroundWindow() == root;
}

void Engine::perform(const std::string& directions, POINT end)
{
    const std::shared_ptr<const Shared> s = shared();
    const auto found = s->settings.gestures.find(directions);
    const HWND under = m_target;
    m_target = nullptr;
    if (directions.empty() || directions.size() > kMaxDirections || found == s->settings.gestures.end())
        return;
    const Action& a = found->second;
    // GetAncestor, not IsWindow: in the plugin's job IsWindow says no to
    // other programs' windows that are there (measured 2026-11-18).
    const HWND root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;
    const auto keysTo = [&](std::vector<std::uint16_t> keys) {
        if (!root || !bringToFront(root)) {
            const HWND front = ::GetForegroundWindow();
            const auto describe = [](HWND w) {
                std::string c;
                for (const wchar_t ch : w ? className(w) : std::wstring(L"none"))
                    c += ch < 0x80 ? static_cast<char>(ch) : '?';
                return c;
            };
            m_options.log("gesture " + directions + ": " + describe(root) + " would not come to the front for its keys over "
                + describe(front));
            return;
        }
        sendKeys(keys);
    };
    switch (a.kind) {
    case Action::Kind::Nothing:
    case Action::Kind::Invalid:
        return;
    case Action::Kind::Keys:
        keysTo(a.keys);
        break;
    case Action::Kind::Open: {
        wchar_t expanded[2048] {};
        const DWORD n = ::ExpandEnvironmentStringsW(a.target.c_str(), expanded, 2048);
        m_options.open(n > 0 && n <= 2048 ? std::wstring(expanded) : a.target);
        break;
    }
    case Action::Kind::Builtin:
        switch (a.builtin) {
        case Builtin::Back:
        case Builtin::Forward: {
            // As the mouse's own back and forward buttons do: those buttons
            // at the pointer when it is still on that window, else the
            // command they make (WM_APPCOMMAND) to the window drawn on.
            const bool back = a.builtin == Builtin::Back;
            const HWND now = ::WindowFromPoint(end);
            if (root && now && ::GetAncestor(now, GA_ROOT) == root)
                sendButton(MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, back ? XBUTTON1 : XBUTTON2);
            else if (root)
                ::PostMessageW(under, WM_APPCOMMAND, reinterpret_cast<WPARAM>(under),
                    MAKELPARAM(0, (back ? APPCOMMAND_BROWSER_BACKWARD : APPCOMMAND_BROWSER_FORWARD) | FAPPCOMMAND_MOUSE));
            break;
        }
        case Builtin::Top:
            keysTo({VK_CONTROL, VK_HOME});
            break;
        case Builtin::Bottom:
            keysTo({VK_CONTROL, VK_END});
            break;
        case Builtin::Refresh:
            keysTo({VK_F5});
            break;
        case Builtin::CloseTab:
            keysTo({VK_CONTROL, 'W'});
            break;
        case Builtin::Close:
        case Builtin::Minimize:
        case Builtin::Maximize:
            // Never the desktop's or the taskbar's: closing those asks to
            // shut Windows down.
            if (!root || shellWindow(root))
                return;
            if (a.builtin == Builtin::Close)
                ::PostMessageW(root, WM_SYSCOMMAND, SC_CLOSE, 0);
            else if (a.builtin == Builtin::Minimize)
                ::PostMessageW(root, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            else
                ::PostMessageW(root, WM_SYSCOMMAND, ::IsZoomed(root) ? SC_RESTORE : SC_MAXIMIZE, 0);
            break;
        case Builtin::Search:
            m_options.showSearch();
            break;
        }
        break;
    }
    ++m_performed;
}

} // namespace gestures
