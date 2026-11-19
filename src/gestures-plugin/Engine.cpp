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
constexpr UINT kSelectionWaitMs = 250; // let go before the selection is known: waited for this long at most
constexpr int kAskSeconds = 5; // 关机 and the like: this long to change one's mind
constexpr int kEscapeHotkey = 0x4753;

enum : UINT {
    // From the hook thread to the window thread.
    kStart = WM_APP + 1, // w: point, l: the window under it
    kMove, // w: point, l: the gesture so far
    kEnd, // w: point, l: the gesture
    kCancel,
    kClick, // the press held back and its release, sent again
    kPressThrough, // the press held back, sent again (the rest goes through)
    kWheeledUp, // w: point, l: the window under it
    kWheeledDown,
    kTrigger, // w: point, l: 0-3 a corner (kCorners), 4-7 an edge (kEdges)
    kPressed, // w: point: a button pressed anywhere while the bubble is up
    // Within the window thread, or from other threads.
    kSelected, // w: which gesture, l: new std::wstring, the words selected
    kRun, // l: new std::function<void()>
    kLook,
    // To the hook thread.
    kAbort, // Esc while drawing
};
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT_PTR kPendingTimer = 2;
constexpr UINT_PTR kScreenOffTimer = 3;

Engine* g_engine = nullptr;

WPARAM packPoint(POINT p)
{
    return static_cast<WPARAM>(static_cast<std::uint32_t>(p.x)) | (static_cast<WPARAM>(static_cast<std::uint32_t>(p.y)) << 32);
}

POINT unpackPoint(WPARAM w)
{
    return {static_cast<LONG>(static_cast<std::int32_t>(w & 0xffffffff)), static_cast<LONG>(static_cast<std::int32_t>(w >> 32))};
}

// Up to 8 letters in the 64 bits: directions (more than kMaxDirections is
// too long whatever follows) or a circle's "cw" / "ccw".
static_assert(kMaxDirections < 8);
LPARAM packGesture(const std::string& d)
{
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < d.size() && i < 8; ++i)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(d[i])) << (8 * i);
    return static_cast<LPARAM>(v);
}

std::string unpackGesture(LPARAM l)
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

bool isCircle(std::string_view gesture)
{
    return gesture == kClockwise || gesture == kCounterClockwise;
}

bool tooLongGesture(std::string_view gesture)
{
    return !isCircle(gesture) && gesture.size() > kMaxDirections;
}

double scaleOf(HMONITOR monitor)
{
    UINT x = 96;
    UINT y = 96;
    if (FAILED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)))
        x = 96;
    return x / 96.0;
}

double scaleAt(POINT p)
{
    return scaleOf(::MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST));
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

// Whether the process runs above this one (one that cannot be asked counts
// as above).
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

// The program's file name in lower case, as the rules name programs.
std::wstring programOf(HANDLE process)
{
    wchar_t path[MAX_PATH * 2] {};
    DWORD size = MAX_PATH * 2;
    if (!process || !::QueryFullProcessImageNameW(process, 0, path, &size))
        return {};
    std::wstring name(path, size);
    if (const std::size_t slash = name.find_last_of(L"\\/"); slash != std::wstring::npos)
        name.erase(0, slash + 1);
    for (wchar_t& c : name) {
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return name;
}

std::wstring programOfWindow(HWND root)
{
    DWORD pid = 0;
    if (!root || !::GetWindowThreadProcessId(root, &pid) || pid == 0)
        return {};
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    std::wstring name = programOf(process);
    if (process)
        ::CloseHandle(process);
    return name;
}

// A window covering its whole monitor, taskbar and all: a game, a slide
// show, a video in full screen.
bool fullScreen(HWND root)
{
    if (!root || shellWindow(root))
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

bool anyButtonDown()
{
    return (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) || (::GetAsyncKeyState(VK_MBUTTON) & 0x8000);
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

// The first words of a selection, on one line, for the hint.
std::wstring snippet(const std::wstring& words, std::size_t length = 10)
{
    // Spaces and line breaks become one space between words, none at the ends.
    std::wstring out;
    bool gap = false;
    for (const wchar_t c : words) {
        if (c == L' ' || c == L'\r' || c == L'\n' || c == L'\t' || c == 0x3000 || c == 0xA0) {
            gap = !out.empty();
            continue;
        }
        if (out.size() >= length) {
            out += L"…";
            break;
        }
        if (std::exchange(gap, false))
            out += L' ';
        out += c;
    }
    return out;
}

std::wstring titleOf(Builtin builtin, bool chinese)
{
    return chinese ? info(builtin).zh : info(builtin).en;
}

} // namespace

Engine::Engine(EngineOptions options)
    : m_options(std::move(options))
    , m_shared(std::make_shared<Shared>())
    , m_reader(m_options.selectedText)
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
    next->corners = anyFor(settings.rules, TriggerKind::Corner);
    next->edges = anyFor(settings.rules, TriggerKind::Edge);
    next->wheel = anyFor(settings.rules, TriggerKind::Wheel);
    next->settings = std::move(settings);
    next->look = look;
    {
        std::lock_guard lock(m_mutex);
        m_shared = std::move(next);
    }
    if (m_window)
        ::PostMessageW(m_window, kLook, 0, 0);
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
        if (msg.hwnd == nullptr && msg.message == WM_TIMER && msg.wParam == m_holdTimer) {
            onHoldTimer();
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == WM_TIMER && msg.wParam == m_dwellTimer) {
            onDwellTimer();
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == kAbort) {
            // Esc while drawing: nothing on release.
            if (m_state == State::Pressed || m_state == State::Gesturing) {
                endPress();
                m_state = State::Canceled;
                ::PostMessageW(m_window, kCancel, 0, 0);
            }
            continue;
        }
        ::DispatchMessageW(&msg);
    }
    for (UINT_PTR* timer : {&m_holdTimer, &m_dwellTimer}) {
        if (*timer)
            ::KillTimer(nullptr, std::exchange(*timer, 0));
    }
    ::UnhookWindowsHookEx(hook);
}

LRESULT CALLBACK Engine::mouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_engine && g_engine->onMouse(wParam, *reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam)))
        return 1;
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

// Whether the right button pressed at `pt` may make a gesture; the window
// under it and its program.
bool Engine::eligible(POINT pt, HWND& under, std::wstring& program)
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
    if (pid == 0 || pid == ::GetCurrentProcessId() || pid == m_ownProcess)
        return false;
    const std::shared_ptr<const Shared> s = shared();
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    const bool above = aboveUs(process);
    program = above ? std::wstring() : programOf(process);
    if (process)
        ::CloseHandle(process);
    if (above || (!s->settings.excluded.empty() && listed(s->settings.excluded, program)))
        return false;
    if (!s->settings.fullScreen && root == ::GetForegroundWindow() && fullScreen(root))
        return false;
    return true;
}

// Whether a corner or an edge may do its rule over the window in front.
bool Engine::frontEligible(std::wstring* program)
{
    if (m_paused || anyButtonDown())
        return false;
    const HWND root = ::GetAncestor(::GetForegroundWindow(), GA_ROOT);
    const std::shared_ptr<const Shared> s = shared();
    if (!s->settings.fullScreen && fullScreen(root))
        return false;
    const std::wstring name = programOfWindow(root);
    if (!s->settings.excluded.empty() && listed(s->settings.excluded, name))
        return false;
    if (program)
        *program = name;
    return true;
}

void Engine::endPress()
{
    if (m_holdTimer)
        ::KillTimer(nullptr, std::exchange(m_holdTimer, 0));
}

bool Engine::onMouse(WPARAM message, const MSLLHOOKSTRUCT& info)
{
    // Sent by a program (this one's own clicks again among them), or made
    // from touch or a pen: never ours.
    if ((info.flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) || (info.dwExtraInfo & kTouchMask) == kTouchSignature)
        return false;
    if (m_bubbleShown && (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN))
        ::PostMessageW(m_window, kPressed, packPoint(info.pt), 0);
    switch (message) {
    case WM_RBUTTONDOWN: {
        if (m_state != State::Idle && m_state != State::Through) {
            // A release never seen (the hook was late): start over.
            endPress();
            ::PostMessageW(m_window, kCancel, 0, 0);
        }
        m_state = State::Idle;
        m_cornerWatch.reset();
        m_rubWatch.reset();
        HWND under = nullptr;
        std::wstring program;
        if (!eligible(info.pt, under, program))
            return false;
        const double scale = scaleAt(info.pt);
        m_state = State::Pressed;
        m_down = info.pt;
        m_under = under;
        m_program = std::move(program);
        m_wheelDelta = 0;
        m_startDistance = static_cast<int>(std::lround(kStartDistance * scale));
        m_recognizer.reset(info.pt.x, info.pt.y, static_cast<int>(std::lround(kStroke * scale)));
        m_holdTimer = ::SetTimer(nullptr, 0, kHoldMs, nullptr);
        return true;
    }
    case WM_MOUSEMOVE:
        if (m_state == State::Idle) {
            watchScreen(info.pt);
            return false;
        }
        if (m_state == State::Pressed) {
            const int dx = info.pt.x - m_down.x;
            const int dy = info.pt.y - m_down.y;
            if (dx * dx + dy * dy < m_startDistance * m_startDistance)
                return false;
            endPress();
            m_state = State::Gesturing;
            ::PostMessageW(m_window, kStart, packPoint(m_down), reinterpret_cast<LPARAM>(m_under));
        }
        if (m_state == State::Gesturing) {
            m_recognizer.add(info.pt.x, info.pt.y);
            ::PostMessageW(m_window, kMove, packPoint(info.pt), packGesture(m_recognizer.gesture()));
        }
        return false;
    case WM_MOUSEWHEEL: {
        if (m_state != State::Pressed && m_state != State::Gesturing && m_state != State::Wheeling)
            return false;
        const std::shared_ptr<const Shared> s = shared();
        if (!s->wheel)
            return false;
        const int delta = static_cast<short>(HIWORD(info.mouseData));
        if (delta == 0)
            return false;
        const bool up = delta > 0;
        // Only when the wheel has a rule that way here; else it scrolls as ever.
        const Rule* rule = match(s->settings.rules, up ? gestures::kWheelUp : gestures::kWheelDown, {.program = m_program});
        if (!rule || rule->action.kind == Action::Kind::Nothing)
            return false;
        if (m_state != State::Wheeling) {
            endPress();
            if (m_state == State::Gesturing)
                ::PostMessageW(m_window, kCancel, 0, 0); // the trail goes
            m_state = State::Wheeling;
            m_wheelDelta = 0;
        }
        if (m_wheelDelta != 0 && (m_wheelDelta > 0) != up)
            m_wheelDelta = 0; // turned the other way: count afresh
        m_wheelDelta += delta;
        // A notch at a time (smooth wheels send parts of one).
        while (std::abs(m_wheelDelta) >= WHEEL_DELTA) {
            ::PostMessageW(m_window, up ? kWheeledUp : kWheeledDown, packPoint(info.pt), reinterpret_cast<LPARAM>(m_under));
            m_wheelDelta += up ? -WHEEL_DELTA : WHEEL_DELTA;
        }
        return true;
    }
    case WM_LBUTTONDOWN:
        // The left button while drawing: no gesture after all.
        if (m_state == State::Pressed || m_state == State::Gesturing) {
            endPress();
            m_state = State::Canceled;
            ::PostMessageW(m_window, kCancel, 0, 0);
        }
        return false;
    case WM_RBUTTONUP:
        endPress();
        switch (std::exchange(m_state, State::Idle)) {
        case State::Pressed:
            ::PostMessageW(m_window, kClick, 0, 0);
            return true;
        case State::Gesturing:
            m_recognizer.add(info.pt.x, info.pt.y);
            ::PostMessageW(m_window, kEnd, packPoint(info.pt), packGesture(m_recognizer.gesture()));
            return true;
        case State::Wheeling:
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
    endPress();
    if (m_state != State::Pressed)
        return;
    m_state = State::Through;
    ::PostMessageW(m_window, kPressThrough, 0, 0);
}

// The pointer moved with no button held for a gesture: into a corner, along
// an edge? Only the sides that end the desktop count (no screen beyond).
void Engine::watchScreen(POINT pt)
{
    const std::shared_ptr<const Shared> s = shared();
    if (!s->corners && !s->edges)
        return;
    if (m_paused || anyButtonDown()) {
        m_cornerWatch.reset();
        m_rubWatch.reset();
        return;
    }
    if (const HMONITOR monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST); monitor != m_monitor) {
        MONITORINFO mi {sizeof mi};
        if (!::GetMonitorInfoW(monitor, &mi))
            return;
        m_monitor = monitor;
        m_monitorRect = {mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom};
        m_monitorScale = scaleOf(monitor);
    }
    Screen screen;
    screen.bounds = m_monitorRect;
    screen.scale = m_monitorScale;
    const Rect& r = m_monitorRect;
    const int reach = static_cast<int>(std::lround(8 * m_monitorScale));
    const auto nothingAt = [](LONG x, LONG y) { return ::MonitorFromPoint({x, y}, MONITOR_DEFAULTTONULL) == nullptr; };
    if (pt.x - r.left < reach)
        screen.hardLeft = nothingAt(r.left - 1, pt.y);
    if (r.right - 1 - pt.x < reach)
        screen.hardRight = nothingAt(r.right, pt.y);
    if (pt.y - r.top < reach)
        screen.hardTop = nothingAt(pt.x, r.top - 1);
    if (r.bottom - 1 - pt.y < reach)
        screen.hardBottom = nothingAt(pt.x, r.bottom);
    const auto now = static_cast<std::int64_t>(::GetTickCount64());
    if (s->corners && m_cornerWatch.move(pt.x, pt.y, screen, now)) {
        if (m_dwellTimer)
            ::KillTimer(nullptr, m_dwellTimer);
        m_dwellTimer = ::SetTimer(nullptr, 0, CornerWatch::kDwellMs + 20, nullptr);
    }
    if (s->edges) {
        const std::string_view edge = m_rubWatch.move(pt.x, pt.y, screen, now);
        if (!edge.empty() && frontEligible(nullptr)) {
            const auto index = std::ranges::find(kEdges, edge) - std::begin(kEdges);
            ::PostMessageW(m_window, kTrigger, packPoint(pt), static_cast<LPARAM>(4 + index));
        }
    }
}

void Engine::onDwellTimer()
{
    if (m_dwellTimer)
        ::KillTimer(nullptr, std::exchange(m_dwellTimer, 0));
    const std::string_view corner = m_cornerWatch.due(static_cast<std::int64_t>(::GetTickCount64()));
    if (corner.empty() || m_state != State::Idle || !frontEligible(nullptr))
        return;
    POINT at {};
    ::GetCursorPos(&at);
    const auto index = std::ranges::find(kCorners, corner) - std::begin(kCorners);
    ::PostMessageW(m_window, kTrigger, packPoint(at), static_cast<LPARAM>(index));
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
    if (m_window && m_cards.create())
        m_cards.setLook(shared()->look);
    ready->set_value(m_window != nullptr);
    if (m_window) {
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&msg);
    }
    m_cards.destroy();
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
    case kStart:
        began(unpackPoint(wParam), reinterpret_cast<HWND>(lParam));
        return 0;
    case kMove: {
        m_points.push_back(unpackPoint(wParam));
        m_gesture = unpackGesture(lParam);
        if (!shared()->settings.trail)
            return 0;
        m_dirty = true;
        if (!m_timer) {
            render(); // the first frame at once, then at most one a frame
            m_timer = ::SetTimer(window, kFrameTimer, 15, nullptr) != 0;
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == kFrameTimer) {
            if (m_dirty) {
                render();
            } else {
                ::KillTimer(window, kFrameTimer);
                m_timer = false;
            }
        } else if (wParam == kPendingTimer) {
            // The selection did not come in time: as if there were none.
            ::KillTimer(window, kPendingTimer);
            if (m_pending) {
                m_selectionKnown = true;
                finish(m_pendingGesture, m_pendingEnd);
            }
        } else if (wParam == kScreenOffTimer) {
            ::KillTimer(window, kScreenOffTimer);
            ::DefWindowProcW(window, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
        }
        return 0;
    case kEnd: {
        hideTrail();
        const std::string gesture = unpackGesture(lParam);
        const POINT end = unpackPoint(wParam);
        if (m_selectionAsked && !m_selectionKnown) {
            m_pending = true;
            m_pendingGesture = gesture;
            m_pendingEnd = end;
            ::SetTimer(window, kPendingTimer, kSelectionWaitMs, nullptr);
        } else {
            finish(gesture, end);
        }
        return 0;
    }
    case kCancel:
        hideTrail();
        m_pending = false;
        return 0;
    case kSelected: {
        std::unique_ptr<std::wstring> words(reinterpret_cast<std::wstring*>(lParam));
        if (static_cast<int>(wParam) != m_ask || m_selectionKnown)
            return 0; // an earlier gesture's
        m_selectionKnown = true;
        m_selection = std::move(*words);
        m_target.context.selection = !m_selection.empty();
        if (m_pending) {
            ::KillTimer(window, kPendingTimer);
            finish(m_pendingGesture, m_pendingEnd);
        } else if (m_drawing && shared()->settings.trail) {
            render(); // the hint may say something else now
        }
        return 0;
    }
    case kClick:
        sendButton(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0);
        return 0;
    case kPressThrough:
        sendButton(MOUSEEVENTF_RIGHTDOWN, 0, 0);
        return 0;
    case kWheeledUp:
    case kWheeledDown:
        wheel(message == kWheeledUp, unpackPoint(wParam), reinterpret_cast<HWND>(lParam));
        return 0;
    case kTrigger: {
        const auto index = static_cast<std::size_t>(lParam);
        if (index < 4)
            triggered(kCorners[index], unpackPoint(wParam));
        else if (index < 8)
            triggered(kEdges[index - 4], unpackPoint(wParam));
        return 0;
    }
    case kPressed:
        m_cards.pressedAt(unpackPoint(wParam));
        m_bubbleShown = m_cards.bubbleShown();
        return 0;
    case kRun: {
        std::unique_ptr<std::function<void()>> work(reinterpret_cast<std::function<void()>*>(lParam));
        (*work)();
        return 0;
    }
    case kLook:
        m_cards.setLook(shared()->look);
        return 0;
    case WM_HOTKEY:
        if (wParam == kEscapeHotkey) {
            ::UnregisterHotKey(window, kEscapeHotkey);
            m_escape = false;
            ::PostThreadMessageW(m_hookThreadId, kAbort, 0, 0);
        }
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

// A gesture began at `down` over `under`: where it is, which program's, and
// (when a rule could care) the words selected there, asked for at once so
// the hint can say what will happen before the button is let go.
void Engine::began(POINT down, HWND under)
{
    const std::shared_ptr<const Shared> s = shared();
    m_target = {};
    m_target.under = under;
    m_target.root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;
    m_target.at = down;
    m_target.context.program = programOfWindow(m_target.root);
    MONITORINFO mi {sizeof mi};
    if (::GetMonitorInfoW(::MonitorFromPoint(down, MONITOR_DEFAULTTONEAREST), &mi))
        m_target.context.zone = zoneAt(down.x, down.y, {mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom});
    m_points.assign(1, down);
    m_gesture.clear();
    m_scale = scaleAt(down);
    m_drawing = true;
    m_pending = false;
    m_selection.clear();
    ++m_ask;
    const auto wantsWords = [&] {
        if (selectionMatters(s->settings.rules, "", m_target.context.program))
            return true;
        return std::ranges::any_of(s->settings.rules, [&](const Rule& r) {
            return r.action.kind == Action::Kind::Builtin && takesSelection(r.action.builtin)
                && (r.apps.empty() || listed(r.apps, m_target.context.program));
        });
    };
    m_selectionAsked = m_target.root && m_target.root == ::GetAncestor(::GetForegroundWindow(), GA_ROOT) && wantsWords();
    m_selectionKnown = !m_selectionAsked;
    if (m_selectionAsked) {
        const HWND window = m_window;
        const int ask = m_ask;
        m_reader.ask(m_target.root, [window, ask](std::wstring words) {
            auto* carried = new std::wstring(std::move(words));
            if (!::PostMessageW(window, kSelected, static_cast<WPARAM>(ask), reinterpret_cast<LPARAM>(carried)))
                delete carried;
        });
    }
    // Esc while drawing: nothing on release.
    m_escape = ::RegisterHotKey(m_window, kEscapeHotkey, MOD_NOREPEAT, VK_ESCAPE) != 0;
}

void Engine::hideTrail()
{
    if (m_escape) {
        ::UnregisterHotKey(m_window, kEscapeHotkey);
        m_escape = false;
    }
    m_drawing = false;
    if (m_timer) {
        ::KillTimer(m_window, kFrameTimer);
        m_timer = false;
    }
    m_dirty = false;
    m_points.clear();
    if (m_shown) {
        ::ShowWindow(m_window, SW_HIDE);
        m_shown = false;
    }
}

// What the hint says for the gesture so far: its arrows, the corner it
// counts for, and what letting go now will do (with the words it will do it
// on).
Engine::Hint Engine::hintFor(const std::string& gesture) const
{
    const std::shared_ptr<const Shared> s = shared();
    const bool zh = s->look.chinese;
    Hint hint;
    hint.arrows = fromUtf8(arrows(gesture.substr(0, kMaxDirections)));
    if (tooLongGesture(gesture)) {
        hint.action = zh ? L"太长了，松开不做什么" : L"Too long: nothing on release";
        return hint;
    }
    const Rule* rule = match(s->settings.rules, gesture, m_target.context);
    if (!rule) {
        hint.action = zh ? L"没有设置" : L"Not set";
        return hint;
    }
    if (rule->where != Zone::Anywhere)
        hint.zone = zoneTitle(rule->where, zh);
    const Action& a = rule->action;
    if (a.kind == Action::Kind::Nothing) {
        hint.action = zh ? L"不做什么" : L"Does nothing";
        return hint;
    }
    hint.action = actionTitle(a, zh);
    if (a.kind == Action::Kind::Builtin && !m_selection.empty()
        && (takesSelection(a.builtin) || a.builtin == Builtin::Copy || a.builtin == Builtin::Cut))
        hint.words = L"「" + snippet(m_selection) + L"」";
    hint.known = a.kind != Action::Kind::Invalid;
    return hint;
}

namespace {

void roundedRect(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, Gdiplus::REAL radius)
{
    radius = std::min({radius, r.Width / 2, r.Height / 2});
    if (radius <= 0.5f) {
        path.AddRectangle(r);
        return;
    }
    const Gdiplus::REAL d = 2 * radius;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

Gdiplus::REAL widthOf(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::Font& font)
{
    if (text.empty())
        return 0;
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF box;
    g.MeasureString(text.c_str(), static_cast<INT>(text.size()), &font, Gdiplus::PointF(0, 0), &format, &box);
    return box.Width;
}

} // namespace

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

    // The hint: a card by the pointer (the background colour, a 1 px edge, a
    // soft shadow) holding the arrows on a chip (the accent when letting go
    // does something, grey when not), then the corner in the caption colour,
    // what it does in the text colour and the words in the caption colour.
    const bool hinted = !m_gesture.empty();
    const Hint hint = hinted ? hintFor(m_gesture) : Hint {};
    const auto px = [this](double logical) { return static_cast<Gdiplus::REAL>(std::round(logical * m_scale)); };
    Gdiplus::FontFamily textFamily(look.chinese ? L"Microsoft YaHei UI" : L"Segoe UI");
    Gdiplus::FontFamily arrowFamily(L"Segoe UI Symbol");
    Gdiplus::Font textFont(&textFamily, px(13), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font strongFont(&textFamily, px(13), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font arrowFont(arrowFamily.IsAvailable() ? &arrowFamily : &textFamily, px(14), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    const Gdiplus::REAL height = px(32);
    const Gdiplus::REAL inset = px(4);
    const Gdiplus::REAL gap = px(8);
    const Gdiplus::REAL shadow = px(8); // room for the shadow all round
    Gdiplus::REAL arrowsWidth = 0;
    Gdiplus::REAL zoneWidth = 0;
    Gdiplus::REAL actionWidth = 0;
    Gdiplus::REAL wordsWidth = 0;
    RECT label {};
    if (hinted) {
        Gdiplus::Bitmap probe(1, 1, PixelFormat32bppPARGB);
        Gdiplus::Graphics measure(&probe);
        measure.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        arrowsWidth = widthOf(measure, hint.arrows, arrowFont);
        zoneWidth = widthOf(measure, hint.zone, textFont);
        actionWidth = widthOf(measure, hint.action, hint.known ? strongFont : textFont);
        wordsWidth = widthOf(measure, hint.words, textFont);
        const Gdiplus::REAL chip = std::max(arrowsWidth + 2 * px(8), height - 2 * inset);
        Gdiplus::REAL width = inset + chip + gap + actionWidth + px(12);
        if (zoneWidth > 0)
            width += zoneWidth + px(6);
        if (wordsWidth > 0)
            width += wordsWidth + px(2);
        const POINT last = m_points.back();
        const int offset = static_cast<int>(px(18));
        label.left = last.x + offset;
        label.top = last.y + offset;
        label.right = label.left + static_cast<int>(std::ceil(width));
        label.bottom = label.top + static_cast<int>(height);
        // On the screen the pointer is on: to the other side near an edge.
        MONITORINFO mi {sizeof mi};
        if (::GetMonitorInfoW(::MonitorFromPoint(last, MONITOR_DEFAULTTONEAREST), &mi)) {
            if (label.right > mi.rcWork.right)
                ::OffsetRect(&label, -(label.right - label.left) - 2 * offset, 0);
            if (label.bottom > mi.rcWork.bottom)
                ::OffsetRect(&label, 0, -(label.bottom - label.top) - 2 * offset);
        }
        RECT withShadow = label;
        ::InflateRect(&withShadow, static_cast<int>(shadow), static_cast<int>(shadow));
        ::UnionRect(&box, &box, &withShadow);
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
        if (hinted) {
            const Gdiplus::RectF card(static_cast<Gdiplus::REAL>(label.left - box.left), static_cast<Gdiplus::REAL>(label.top - box.top),
                static_cast<Gdiplus::REAL>(label.right - label.left), static_cast<Gdiplus::REAL>(label.bottom - label.top));
            // A pill when the corners are round, the controls' corner when not.
            const Gdiplus::REAL radius = look.rounded ? card.Height / 2 : px(look.controlRadius);
            const Gdiplus::REAL chipRadius = look.rounded ? (card.Height - 2 * inset) / 2 : std::max<Gdiplus::REAL>(0, px(look.controlRadius) - inset / 2);
            // The shadow: rings of a little black, wider and fainter, a
            // touch below the card (cheap enough for every frame).
            const int rings = 6;
            const int darkness = look.dark ? 26 : 9;
            for (int i = rings; i >= 1; --i) {
                const Gdiplus::REAL grow = shadow * i / rings;
                Gdiplus::RectF ring = card;
                ring.Inflate(grow * 0.75f, grow * 0.75f);
                ring.Offset(0, px(2));
                Gdiplus::GraphicsPath p;
                roundedRect(p, ring, radius + grow * 0.75f);
                Gdiplus::SolidBrush b(Gdiplus::Color(static_cast<BYTE>(darkness * (rings - i + 1) / rings), 0, 0, 0));
                g.FillPath(&b, &p);
            }
            Gdiplus::GraphicsPath outline;
            roundedRect(outline, card, radius);
            Gdiplus::SolidBrush surface(color(look.background));
            g.FillPath(&surface, &outline);
            Gdiplus::Pen edge(color(look.edge), 1.0f);
            Gdiplus::RectF edgeRect = card;
            edgeRect.Inflate(-0.5f, -0.5f);
            Gdiplus::GraphicsPath edgePath;
            roundedRect(edgePath, edgeRect, radius - 0.5f);
            g.DrawPath(&edge, &edgePath);

            const Gdiplus::REAL chipWidth = std::max(arrowsWidth + 2 * px(8), card.Height - 2 * inset);
            const Gdiplus::RectF chip(card.X + inset, card.Y + inset, chipWidth, card.Height - 2 * inset);
            Gdiplus::GraphicsPath chipPath;
            roundedRect(chipPath, chip, chipRadius);
            Gdiplus::SolidBrush chipFill(color(hint.known ? look.accentFill : look.muted));
            g.FillPath(&chipFill, &chipPath);

            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            Gdiplus::StringFormat centered(Gdiplus::StringFormat::GenericTypographic());
            centered.SetAlignment(Gdiplus::StringAlignmentCenter);
            centered.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::StringFormat oneLine(Gdiplus::StringFormat::GenericTypographic());
            oneLine.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            oneLine.SetFormatFlags(oneLine.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
            Gdiplus::SolidBrush onChip(color(look.onAccent));
            g.DrawString(hint.arrows.c_str(), static_cast<INT>(hint.arrows.size()), &arrowFont, chip, &centered, &onChip);
            Gdiplus::SolidBrush ink(color(look.text));
            Gdiplus::SolidBrush caption(color(look.subtext));
            Gdiplus::REAL x = chip.X + chip.Width + gap;
            const auto draw = [&](const std::wstring& text, const Gdiplus::Font& font, const Gdiplus::Brush& brush, Gdiplus::REAL width, Gdiplus::REAL after) {
                if (text.empty())
                    return;
                g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, Gdiplus::RectF(x, card.Y, width + px(4), card.Height), &oneLine, &brush);
                x += width + after;
            };
            draw(hint.zone, textFont, caption, zoneWidth, px(6));
            draw(hint.action, hint.known ? strongFont : textFont, hint.known ? static_cast<const Gdiplus::Brush&>(ink) : caption, actionWidth, px(2));
            draw(hint.words, textFont, caption, wordsWidth, 0);
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

// Words typed where the caret is, character by character as an input
// method would (a line break as Enter).
void Engine::typeText(const std::wstring& words)
{
    std::vector<INPUT> in;
    const auto add = [&](WORD vk, WORD scan, DWORD flags) {
        INPUT i {};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.wScan = scan;
        i.ki.dwFlags = flags;
        i.ki.dwExtraInfo = kOwnInput;
        in.push_back(i);
    };
    for (const wchar_t c : words) {
        if (c == L'\r')
            continue;
        if (c == L'\n' || c == L'\t') {
            const WORD vk = c == L'\n' ? VK_RETURN : VK_TAB;
            const WORD scan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            add(vk, scan, 0);
            add(vk, scan, KEYEVENTF_KEYUP);
            continue;
        }
        add(0, c, KEYEVENTF_UNICODE);
        add(0, c, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
    }
    if (!in.empty())
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

void Engine::keysTo(HWND root, const std::vector<std::uint16_t>& keys)
{
    if (!root || !bringToFront(root)) {
        const auto describe = [](HWND w) {
            std::string c;
            for (const wchar_t ch : w ? className(w) : std::wstring(L"none"))
                c += ch < 0x80 ? static_cast<char>(ch) : '?';
            return c;
        };
        m_options.log("keys: " + describe(root) + " would not come to the front over " + describe(::GetForegroundWindow()));
        return;
    }
    sendKeys(keys);
}

void Engine::post(std::function<void()> work)
{
    auto* carried = new std::function<void()>(std::move(work));
    if (!m_window || !::PostMessageW(m_window, kRun, 0, reinterpret_cast<LPARAM>(carried)))
        delete carried;
}

namespace {

// Where a trigger is, for the note about it: a corner's diagonal, an edge's
// side (the wheel: up or down), else the pointer.
NoteIcon triggerIcon(std::string_view on)
{
    if (on == "corner:tl")
        return NoteIcon::UpLeft;
    if (on == "corner:tr")
        return NoteIcon::UpRight;
    if (on == "corner:bl")
        return NoteIcon::DownLeft;
    if (on == "corner:br")
        return NoteIcon::DownRight;
    if (on == "edge:top" || on == kWheelUp)
        return NoteIcon::Up;
    if (on == "edge:bottom" || on == kWheelDown)
        return NoteIcon::Down;
    if (on == "edge:left")
        return NoteIcon::Left;
    if (on == "edge:right")
        return NoteIcon::Right;
    return NoteIcon::Cursor;
}

} // namespace

// `lead` (a trigger and " · ") is said quietly before `text`.
void Engine::note(POINT at, NoteIcon icon, const std::wstring& text, const std::wstring& lead)
{
    if (shared()->settings.trail && !text.empty())
        m_cards.flash(at, icon, lead, text);
}

void Engine::finish(const std::string& gesture, POINT end)
{
    m_pending = false;
    if (gesture.empty() || tooLongGesture(gesture))
        return;
    m_target.at = end;
    m_target.words = m_selection;
    const std::shared_ptr<const Shared> s = shared();
    if (const Rule* rule = match(s->settings.rules, gesture, m_target.context))
        perform(*rule, m_target);
}

// The wheel, a corner, an edge: nothing was asked for beforehand, so the
// words selected in the window in front are read now when the rule acts on
// them (a moment later), else it acts at once.
void Engine::performWithWords(const Rule& rule, const Target& target)
{
    const Action& a = rule.action;
    if (a.kind != Action::Kind::Builtin || !takesSelection(a.builtin)) {
        perform(rule, target);
        return;
    }
    const HWND root = ::GetAncestor(::GetForegroundWindow(), GA_ROOT);
    if (!root || root != target.root) {
        perform(rule, target); // the selection is the front window's: none here
        return;
    }
    const HWND window = m_window;
    ++m_ask; // a gesture's answer still on its way is dropped
    m_reader.ask(root, [this, window, rule, target](std::wstring words) {
        auto* work = new std::function<void()>([this, rule, target, words = std::move(words)] {
            Target t = target;
            t.words = words;
            perform(rule, t);
        });
        if (!::PostMessageW(window, kRun, 0, reinterpret_cast<LPARAM>(work)))
            delete work;
    });
}

void Engine::wheel(bool up, POINT at, HWND under)
{
    Target t;
    t.under = under;
    t.root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;
    t.at = at;
    t.context.program = programOfWindow(t.root);
    const std::shared_ptr<const Shared> s = shared();
    if (const Rule* rule = match(s->settings.rules, up ? gestures::kWheelUp : gestures::kWheelDown, t.context))
        performWithWords(*rule, t);
}

// A corner pushed into or an edge rubbed: its rule over the window in front,
// with a note of what was done where (the card that asks says it itself).
void Engine::triggered(std::string_view on, POINT at)
{
    Target t;
    t.root = ::GetAncestor(::GetForegroundWindow(), GA_ROOT);
    t.under = t.root;
    t.at = at;
    t.context.program = programOfWindow(t.root);
    const std::shared_ptr<const Shared> s = shared();
    const Rule* rule = match(s->settings.rules, on, t.context);
    if (!rule || rule->action.kind == Action::Kind::Nothing)
        return;
    const bool asks = rule->action.kind == Action::Kind::Builtin && asksFirst(rule->action.builtin);
    if (!asks)
        note(at, triggerIcon(on), actionTitle(rule->action, s->look.chinese), triggerTitle(on, s->look.chinese) + L" · ");
    performWithWords(*rule, t);
}

void Engine::perform(const Rule& rule, const Target& target)
{
    const Action& a = rule.action;
    switch (a.kind) {
    case Action::Kind::Nothing:
    case Action::Kind::Invalid:
        return;
    case Action::Kind::Keys:
        keysTo(target.root, a.keys);
        break;
    case Action::Kind::Open: {
        wchar_t expanded[2048] {};
        const DWORD n = ::ExpandEnvironmentStringsW(a.target.c_str(), expanded, 2048);
        m_options.open(n > 0 && n <= 2048 ? std::wstring(expanded) : a.target);
        break;
    }
    case Action::Kind::Type:
        if (target.root && bringToFront(target.root))
            typeText(a.target);
        break;
    case Action::Kind::Builtin:
        performBuiltin(a.builtin, target);
        break;
    }
    ++m_performed;
}

void Engine::performBuiltin(Builtin builtin, const Target& target)
{
    const bool zh = shared()->look.chinese;
    const HWND root = target.root;
    const auto window = [&] { return root && !shellWindow(root); }; // never the desktop's or the taskbar's
    switch (builtin) {
    case Builtin::Back:
    case Builtin::Forward: {
        // As the mouse's own back and forward buttons do: those buttons at
        // the pointer when it is still on that window, else the command they
        // make (WM_APPCOMMAND) to the window drawn on.
        const bool back = builtin == Builtin::Back;
        const HWND now = ::WindowFromPoint(target.at);
        if (root && now && ::GetAncestor(now, GA_ROOT) == root)
            sendButton(MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, back ? XBUTTON1 : XBUTTON2);
        else if (root && target.under)
            ::PostMessageW(target.under, WM_APPCOMMAND, reinterpret_cast<WPARAM>(target.under),
                MAKELPARAM(0, (back ? APPCOMMAND_BROWSER_BACKWARD : APPCOMMAND_BROWSER_FORWARD) | FAPPCOMMAND_MOUSE));
        break;
    }
    case Builtin::Refresh: keysTo(root, {VK_F5}); break;
    case Builtin::Top: keysTo(root, {VK_CONTROL, VK_HOME}); break;
    case Builtin::Bottom: keysTo(root, {VK_CONTROL, VK_END}); break;
    case Builtin::NewTab: keysTo(root, {VK_CONTROL, 'T'}); break;
    case Builtin::CloseTab: keysTo(root, {VK_CONTROL, 'W'}); break;
    case Builtin::ReopenTab: keysTo(root, {VK_CONTROL, VK_SHIFT, 'T'}); break;
    case Builtin::PrevTab: keysTo(root, {VK_CONTROL, VK_SHIFT, VK_TAB}); break;
    case Builtin::NextTab: keysTo(root, {VK_CONTROL, VK_TAB}); break;
    case Builtin::Close:
        if (window())
            ::PostMessageW(root, WM_SYSCOMMAND, SC_CLOSE, 0);
        break;
    case Builtin::Minimize:
        if (window())
            ::PostMessageW(root, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        break;
    case Builtin::Maximize:
        if (window())
            ::PostMessageW(root, WM_SYSCOMMAND, ::IsZoomed(root) ? SC_RESTORE : SC_MAXIMIZE, 0);
        break;
    case Builtin::Topmost: {
        if (!window())
            break;
        const bool on = (::GetWindowLongPtrW(root, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        if (::SetWindowPos(root, on ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
            note(target.at, NoteIcon::Check, on ? (zh ? L"取消置顶" : L"No longer on top") : (zh ? L"已置顶" : L"Kept on top"));
        break;
    }
    case Builtin::Desktop: sendKeys({VK_LWIN, 'D'}); break;
    case Builtin::TaskView: sendKeys({VK_LWIN, VK_TAB}); break;
    case Builtin::LastWindow: sendKeys({VK_MENU, VK_TAB}); break;
    case Builtin::Copy:
        keysTo(root, {VK_CONTROL, 'C'});
        note(target.at, NoteIcon::Check, zh ? L"已复制" : L"Copied");
        break;
    case Builtin::Cut:
        keysTo(root, {VK_CONTROL, 'X'});
        note(target.at, NoteIcon::Check, zh ? L"已剪切" : L"Cut");
        break;
    case Builtin::Paste: keysTo(root, {VK_CONTROL, 'V'}); break;
    case Builtin::Undo: keysTo(root, {VK_CONTROL, 'Z'}); break;
    case Builtin::Redo: keysTo(root, {VK_CONTROL, 'Y'}); break;
    case Builtin::SelectAll: keysTo(root, {VK_CONTROL, 'A'}); break;
    case Builtin::WebSearch:
    case Builtin::FindInWinShun:
    case Builtin::Translate:
        textAction(builtin, target);
        break;
    case Builtin::Search: m_options.showSearch(); break;
    case Builtin::Mute: sendKeys({VK_VOLUME_MUTE}); break;
    case Builtin::VolumeUp: sendKeys({VK_VOLUME_UP}); break;
    case Builtin::VolumeDown: sendKeys({VK_VOLUME_DOWN}); break;
    case Builtin::PlayPause: sendKeys({VK_MEDIA_PLAY_PAUSE}); break;
    case Builtin::NextTrack: sendKeys({VK_MEDIA_NEXT_TRACK}); break;
    case Builtin::PrevTrack: sendKeys({VK_MEDIA_PREV_TRACK}); break;
    case Builtin::Lock: ::LockWorkStation(); break;
    case Builtin::ScreenOff:
        // A moment later: the hand is still on the mouse, which would wake it.
        ::SetTimer(m_window, kScreenOffTimer, 500, nullptr);
        break;
    case Builtin::Sleep:
    case Builtin::Shutdown:
    case Builtin::Restart:
    case Builtin::SignOut:
        askFirst(builtin, target);
        break;
    }
}

// Searched, found or translated: the words selected where the gesture was
// drawn (read when it began). None: a note says so.
void Engine::textAction(Builtin builtin, const Target& target)
{
    const bool zh = shared()->look.chinese;
    const std::wstring& words = target.words;
    if (words.empty()) {
        note(target.at, NoteIcon::Info, zh ? L"没有选中的文字" : L"Nothing selected");
        return;
    }
    if (builtin == Builtin::WebSearch) {
        m_options.webSearch(words);
        return;
    }
    if (builtin == Builtin::FindInWinShun) {
        m_options.findInWinShun(words);
        return;
    }
    // The bubble comes at once; the translation when it is back. A click
    // copies it, or (none to copy) opens what is missing.
    auto result = std::make_shared<Translation>();
    result->state = Translation::State::Failed;
    m_cards.bubble(target.at, [this, result] {
        if (result->state == Translation::State::Ok)
            m_options.copy(result->text);
        else if (result->state == Translation::State::NoService || result->state == Translation::State::NoPlugin)
            m_options.translateSetup();
    });
    m_bubbleShown = true;
    const HWND window = m_window;
    m_options.translate(words, [this, window, result, zh](Translation t) {
        auto* work = new std::function<void()>([this, result, zh, t] {
            *result = t;
            switch (t.state) {
            // The click that fixes it is in the bubble's header: what is said is only what is wrong.
            case Translation::State::Ok:
                m_cards.update(BubbleState::Result, t.text);
                break;
            case Translation::State::NoService:
                m_cards.update(BubbleState::NoService, zh ? L"翻译插件还没设好翻译服务" : L"The translate plugin has no service yet");
                break;
            case Translation::State::NoPlugin:
                m_cards.update(BubbleState::NoPlugin, zh ? L"还没有翻译插件，可以在插件库里装上" : L"No translate plugin yet, but the plugin library has one");
                break;
            case Translation::State::Failed:
                m_cards.update(BubbleState::Failed, (zh ? L"没翻译成：" : L"Not translated: ") + t.text);
                break;
            }
            m_bubbleShown = m_cards.bubbleShown();
        });
        if (!::PostMessageW(window, kRun, 0, reinterpret_cast<LPARAM>(work)))
            delete work;
    });
}

// 关机 重启 注销 睡眠: a card counts down first; Esc or 取消 and nothing
// happens. Programs with work not saved can still hold Windows up.
void Engine::askFirst(Builtin builtin, const Target& target)
{
    const bool zh = shared()->look.chinese;
    const std::wstring title = titleOf(builtin, zh);
    const std::wstring now = zh ? L"现在" + title : title + L" now";
    const HWND window = m_window;
    const auto power = m_options.power;
    const ConfirmIcon icon = builtin == Builtin::Restart   ? ConfirmIcon::Restart
        : builtin == Builtin::SignOut                       ? ConfirmIcon::SignOut
        : builtin == Builtin::Sleep                         ? ConfirmIcon::Sleep
                                                            : ConfirmIcon::Power;
    m_cards.confirm(target.at, icon, title, now, kAskSeconds, [this, builtin, window, power, zh](bool go) {
        if (!go)
            return;
        if (builtin == Builtin::SignOut) {
            if (!::ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
                m_options.log("sign out: error " + std::to_string(::GetLastError()));
            return;
        }
        const std::string what = info(builtin).id;
        // Win顺 does it; asking waits for its answer, so not on this thread.
        std::thread([this, window, power, what, zh] {
            const std::wstring error = power ? power(what) : std::wstring(L"-");
            if (error.empty())
                return;
            auto* work = new std::function<void()>([this, error, zh] {
                POINT at {};
                ::GetCursorPos(&at);
                note(at, NoteIcon::Info, (zh ? L"没能做到：" : L"Could not: ") + error);
            });
            if (!::PostMessageW(window, kRun, 0, reinterpret_cast<LPARAM>(work)))
                delete work;
        }).detach();
    });
}

} // namespace gestures
