#include "ScrollInput.h"

#include "WindowSnapshot.h"

#include <dwmapi.h>
#include <ole2.h> // before uiautomation.h: WIN32_LEAN_AND_MEAN leaves it out of windows.h
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;
using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

QString exeName(HWND window)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    wchar_t path[MAX_PATH] {};
    DWORD size = MAX_PATH;
    const bool ok = ::QueryFullProcessImageNameW(process, 0, path, &size);
    ::CloseHandle(process);
    if (!ok)
        return {};
    const QString full = QString::fromWCharArray(path, int(size));
    return full.mid(full.lastIndexOf(u'\\') + 1);
}

// The deepest visible, enabled, not transparent child of `top` under `at`.
HWND deepestChild(HWND top, POINT at)
{
    HWND window = top;
    for (int depth = 0; depth < 32; ++depth) {
        POINT client = at;
        ::ScreenToClient(window, &client);
        const HWND child = ::ChildWindowFromPointEx(window, client, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == window)
            break;
        window = child;
    }
    return window;
}

// Whether `content`, or a window it sits in up to `top`, is one of Chromium's. UI Automation must not
// be asked there (WindowSnapshot::isChromiumClass): the browser or VS Code would turn its whole
// accessibility mode on. The wheel ways above it work in these programs anyway.
bool inChromium(HWND top, HWND content)
{
    for (HWND w = content; w; w = (w == top ? nullptr : ::GetParent(w))) {
        wchar_t name[64] {};
        ::GetClassNameW(w, name, int(std::size(name)));
        if (WindowSnapshot::isChromiumClass(name))
            return true;
    }
    return false;
}

bool down(int key)
{
    return (::GetAsyncKeyState(key) & 0x8000) != 0;
}

bool modifiersDown()
{
    return down(VK_CONTROL) || down(VK_SHIFT) || down(VK_MENU) || down(VK_LWIN) || down(VK_RWIN);
}

// A posted wheel goes before the program's input still waiting, so it reads
// a key let go a moment ago as still down: Word zooms out for the Ctrl of
// Ctrl+L (live test 2026-10-22, 150 to 140 %). The keys are up this long first.
constexpr ULONGLONG kKeysUpMs = 150;

} // namespace

QString methodName(ScrollMethod method)
{
    switch (method) {
    case ScrollMethod::PostWheel:
        return u"post wheel"_s;
    case ScrollMethod::SendWheel:
        return u"send wheel"_s;
    case ScrollMethod::Uia:
        return u"ui automation"_s;
    case ScrollMethod::Manual:
        return u"manual"_s;
    }
    return {};
}

QString codeName(int code)
{
    switch (code) {
    case ScrollOk:
        return u"ok"_s;
    case ScrollAccessDenied:
        return u"access denied"_s;
    case ScrollNoWindow:
        return u"no window"_s;
    case ScrollCursorOutside:
        return u"mouse outside"_s;
    case ScrollKeysHeld:
        return u"keys held"_s;
    case ScrollNoPattern:
        return u"no scroll pattern"_s;
    case ScrollFailed:
        return u"failed"_s;
    case ScrollAtEnd:
        return u"at end"_s;
    }
    return u"code %1"_s.arg(code);
}

// ---- the ladder ----

namespace {

constexpr int kStillToEnd = 2; // steps in a row a way did not move, for it to count at the end
constexpr int kStillToDrop = 2; // the same, for a way that never moved anything
constexpr double kFirstNotches = 1;
constexpr double kFirstView = 0.25;

bool wheel(ScrollMethod method)
{
    return method == ScrollMethod::PostWheel || method == ScrollMethod::SendWheel;
}

} // namespace

ScrollLadder::ScrollLadder(bool elevated)
{
    m_ways[0].method = ScrollMethod::PostWheel;
    m_ways[1].method = ScrollMethod::SendWheel;
    m_ways[2].method = ScrollMethod::Uia;
    if (elevated) {
        for (Way& way : m_ways)
            way.alive = false;
        m_hint = Hint::Elevated;
    }
}

ScrollLadder::Way* ScrollLadder::current()
{
    Way& way = m_ways[size_t(m_current)];
    return way.alive ? &way : nullptr;
}

const ScrollLadder::Way* ScrollLadder::current() const
{
    const Way& way = m_ways[size_t(m_current)];
    return way.alive ? &way : nullptr;
}

ScrollMethod ScrollLadder::method() const
{
    const Way* way = current();
    return way && !m_atEnd ? way->method : ScrollMethod::Manual;
}

bool ScrollLadder::worked(ScrollMethod method) const
{
    for (const Way& way : m_ways)
        if (way.method == method)
            return way.worked;
    return false;
}

double ScrollLadder::amount(int px) const
{
    const Way* way = current();
    if (!way)
        return 0;
    if (wheel(way->method)) {
        if (way->perUnit <= 0)
            return kFirstNotches;
        return std::clamp(std::round(px / way->perUnit), 1.0, 10.0);
    }
    if (way->perUnit <= 0)
        return kFirstView;
    return std::clamp(px / way->perUnit, 0.05, 0.9);
}

int ScrollLadder::expected(double amount) const
{
    const Way* way = current();
    return way && way->perUnit > 0 ? int(std::lround(amount * way->perUnit)) : 0;
}

void ScrollLadder::next()
{
    for (int i = 1; i <= int(m_ways.size()); ++i) {
        const int at = (m_current + i) % int(m_ways.size());
        if (m_ways[size_t(at)].alive) {
            m_current = at;
            return;
        }
    }
    // None left (the current one is not alive either).
    if (m_hint != Hint::Elevated)
        m_hint = Hint::NothingWorks;
}

void ScrollLadder::checkEnd()
{
    bool anyWorked = false;
    bool allStill = true;
    bool anyAlive = false;
    for (const Way& way : m_ways) {
        anyWorked = anyWorked || way.worked;
        if (!way.alive || (way.method == ScrollMethod::SendWheel && m_mouseOutside))
            continue;
        anyAlive = true;
        allStill = allStill && way.still >= kStillToEnd;
    }
    if (anyWorked && anyAlive && allStill)
        m_atEnd = true;
}

void ScrollLadder::refused(int code)
{
    Way* way = current();
    if (!way)
        return;
    switch (code) {
    case ScrollOk:
        return;
    case ScrollAccessDenied:
        // UIPI: the program runs with higher rights; no way of ours reaches it.
        for (Way& w : m_ways)
            w.alive = false;
        m_hint = Hint::Elevated;
        return;
    case ScrollKeysHeld:
        return; // the same way again in a moment
    case ScrollCursorOutside: {
        m_mouseOutside = true;
        const int was = m_current;
        next();
        // Only this one left: it waits for the mouse.
        m_hint = m_current == was && way->alive ? Hint::MouseOutside : m_hint;
        return;
    }
    case ScrollNoPattern:
        way->alive = false;
        next();
        checkEnd();
        return;
    case ScrollAtEnd:
        if (way->worked) {
            m_atEnd = true;
            return;
        }
        // Before it moved anything: it may be right, if the others agree.
        way->still = std::max(way->still, kStillToEnd);
        checkEnd();
        if (!m_atEnd)
            next();
        return;
    default: // no window, failed
        moved(0, 0);
        return;
    }
}

void ScrollLadder::moved(int rows, double amount)
{
    Way* way = current();
    if (!way)
        return;
    if (rows != 0) {
        way->worked = true;
        if (amount > 0) {
            const double unit = std::abs(rows) / amount;
            way->perUnit = way->perUnit > 0 ? (way->perUnit + unit) / 2 : unit;
        }
        for (Way& w : m_ways)
            w.still = 0;
        m_mouseOutside = false;
        if (m_hint == Hint::MouseOutside)
            m_hint = Hint::None;
        return;
    }
    ++way->still;
    if (!way->worked && way->still >= kStillToDrop)
        way->alive = false;
    checkEnd();
    if (!m_atEnd)
        next();
}

// ---- the ways ----

struct ScrollInput::Uia {
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationScrollPattern> pattern;
    bool looked = false;
    bool percent = true; // SetScrollPercent works here, until it fails once
};

ScrollInput::ScrollInput(const QRect& region, bool horizontal)
    : m_region(region)
    , m_horizontal(horizontal)
    , m_middle {region.x() + region.width() / 2, region.y() + region.height() / 2}
{
    m_window = windowUnderMiddle();
    if (m_window) {
        // The content's program: a UWP program's frame is ApplicationFrameHost's.
        m_content = deepestChild(m_window, m_middle);
        m_program = exeName(m_content);
    }
}

ScrollInput::~ScrollInput() = default;

HWND ScrollInput::windowUnderMiddle() const
{
    // From the top of the Z order: the first window over the middle that is
    // not ours (the panel, the border) and not a click-through layer.
    for (HWND w = ::GetTopWindow(nullptr); w; w = ::GetWindow(w, GW_HWNDNEXT)) {
        if (!::IsWindowVisible(w) || ::IsIconic(w))
            continue;
        RECT r {};
        if (!::GetWindowRect(w, &r) || !::PtInRect(&r, m_middle))
            continue;
        const LONG_PTR ex = ::GetWindowLongPtrW(w, GWL_EXSTYLE);
        if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT))
            continue;
        BOOL cloaked = FALSE;
        if (SUCCEEDED(::DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
            continue;
        DWORD pid = 0;
        ::GetWindowThreadProcessId(w, &pid);
        if (pid == ::GetCurrentProcessId())
            continue;
        return w;
    }
    return nullptr;
}

bool ScrollInput::cursorInRegion() const
{
    POINT at {};
    return ::GetCursorPos(&at) && m_region.contains(at.x, at.y);
}

bool ScrollInput::keysHeld(bool buttons)
{
    if (modifiersDown() || (buttons && (down(VK_LBUTTON) || down(VK_RBUTTON) || down(VK_MBUTTON)))) {
        m_keysUpAt = 0;
        return true;
    }
    const ULONGLONG now = ::GetTickCount64();
    if (m_keysUpAt == 0)
        m_keysUpAt = now;
    return now - m_keysUpAt < kKeysUpMs;
}

ScrollResult ScrollInput::scroll(ScrollMethod method, double amount)
{
    ScrollResult result;
    result.method = method;
    switch (method) {
    case ScrollMethod::PostWheel: {
        if (keysHeld(false)) {
            result.code = ScrollKeysHeld;
            return result;
        }
        const HWND top = windowUnderMiddle();
        if (!top) {
            result.code = ScrollNoWindow;
            return result;
        }
        const HWND target = deepestChild(top, m_middle);
        // Down is a negative delta; right a positive one.
        const long notches = std::clamp(std::lround(120.0 * amount), 1L, 1200L);
        const short delta = short(m_horizontal ? notches : -notches);
        if (!::PostMessageW(target, m_horizontal ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL, MAKEWPARAM(0, WORD(delta)), MAKELPARAM(WORD(short(m_middle.x)), WORD(short(m_middle.y))))) {
            result.detail = ::GetLastError();
            result.code = result.detail == ERROR_ACCESS_DENIED ? ScrollAccessDenied : ScrollFailed;
        }
        return result;
    }
    case ScrollMethod::SendWheel: {
        if (!cursorInRegion()) {
            result.code = ScrollCursorOutside;
            return result;
        }
        if (keysHeld(true)) {
            result.code = ScrollKeysHeld;
            return result;
        }
        INPUT input {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = m_horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
        const long notches = std::clamp(std::lround(120.0 * amount), 1L, 1200L);
        input.mi.mouseData = DWORD(m_horizontal ? notches : -notches);
        if (::SendInput(1, &input, sizeof input) != 1) {
            result.detail = ::GetLastError();
            result.code = ScrollFailed;
        }
        return result;
    }
    case ScrollMethod::Uia: {
        if (!m_uia)
            m_uia = std::make_unique<Uia>();
        Uia& u = *m_uia;
        if (!u.looked) {
            u.looked = true;
            if (m_window && inChromium(m_window, m_content)) {
                result.code = ScrollNoPattern;
                return result;
            }
            if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&u.automation)))
                && FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&u.automation)))) {
                result.code = ScrollNoPattern;
                return result;
            }
            ComPtr<IUIAutomation2> timed;
            if (SUCCEEDED(u.automation.As(&timed))) {
                timed->put_ConnectionTimeout(1000);
                timed->put_TransactionTimeout(1000);
            }
            // The element under the middle must be the program's under it
            // (a UWP program's content is not its frame's process).
            ComPtr<IUIAutomationElement> element;
            ComPtr<IUIAutomationTreeWalker> walker;
            const HWND top = windowUnderMiddle();
            DWORD owner = 0;
            if (top)
                ::GetWindowThreadProcessId(deepestChild(top, m_middle), &owner);
            int pid = 0;
            if (owner && SUCCEEDED(u.automation->ElementFromPoint(m_middle, &element)) && element
                && SUCCEEDED(element->get_CurrentProcessId(&pid)) && DWORD(pid) == owner
                && SUCCEEDED(u.automation->get_ControlViewWalker(&walker))) {
                // Up to the nearest that scrolls up and down (or sideways).
                for (int depth = 0; element && depth < 40; ++depth) {
                    ComPtr<IUIAutomationScrollPattern> p;
                    BOOL scrolls = FALSE;
                    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&p))) && p
                        && SUCCEEDED(m_horizontal ? p->get_CurrentHorizontallyScrollable(&scrolls) : p->get_CurrentVerticallyScrollable(&scrolls))
                        && scrolls) {
                        u.pattern = p;
                        break;
                    }
                    ComPtr<IUIAutomationElement> parent;
                    if (FAILED(walker->GetParentElement(element.Get(), &parent)))
                        break;
                    element = parent;
                }
            }
        }
        if (!u.pattern) {
            result.code = ScrollNoPattern;
            return result;
        }
        double at = 0;
        double view = 0;
        HRESULT hr = m_horizontal ? u.pattern->get_CurrentHorizontalScrollPercent(&at) : u.pattern->get_CurrentVerticalScrollPercent(&at);
        if (SUCCEEDED(hr))
            hr = m_horizontal ? u.pattern->get_CurrentHorizontalViewSize(&view) : u.pattern->get_CurrentVerticalViewSize(&view);
        if (FAILED(hr)) {
            result.code = ScrollFailed;
            result.detail = quint32(hr);
            return result;
        }
        if (at != UIA_ScrollPatternNoScroll && at >= 99.95) {
            result.code = ScrollAtEnd;
            return result;
        }
        // Where to is known (stage 6 probe: 602 px each time in Edge, 605 in
        // Settings), where a small step is 9 to 247 px depending on the program.
        if (u.percent && at != UIA_ScrollPatternNoScroll && view > 0 && view < 100) {
            const double to = std::min(100.0, at + view * std::max(0.01, amount));
            hr = m_horizontal ? u.pattern->SetScrollPercent(to, UIA_ScrollPatternNoScroll) : u.pattern->SetScrollPercent(UIA_ScrollPatternNoScroll, to);
            if (SUCCEEDED(hr))
                return result;
            u.percent = false;
        }
        const int steps = std::max(1, int(std::lround(amount * 8)));
        for (int i = 0; i < steps; ++i) {
            hr = m_horizontal ? u.pattern->Scroll(ScrollAmount_SmallIncrement, ScrollAmount_NoAmount) : u.pattern->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallIncrement);
            if (FAILED(hr)) {
                result.code = ScrollFailed;
                result.detail = quint32(hr);
                break;
            }
        }
        return result;
    }
    case ScrollMethod::Manual:
        break;
    }
    return result;
}

double ScrollInput::uiaPercent()
{
    if (!m_uia || !m_uia->pattern)
        return -1;
    double at = 0;
    const HRESULT hr = m_horizontal ? m_uia->pattern->get_CurrentHorizontalScrollPercent(&at) : m_uia->pattern->get_CurrentVerticalScrollPercent(&at);
    if (FAILED(hr) || at == UIA_ScrollPatternNoScroll)
        return -1;
    return at;
}

} // namespace ws::capture
