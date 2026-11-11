#include "TaskbarSearchBox.h"

#include "MonoLogo.h"
#include "TaskbarSearch.h"

#include <ole2.h> // before uiautomation.h: WIN32_LEAN_AND_MEAN leaves it out of windows.h

#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <uiautomation.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <QDebug>
#include <QString>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ws::taskbar {

namespace {

constexpr wchar_t kBoxClass[] = L"WinShun.TaskbarSearchBox";
constexpr wchar_t kHelperClass[] = L"WinShun.TaskbarSearchBoxHelper";

// To the box's thread, through its helper window.
constexpr UINT kStateMessage = WM_APP + 1; // the look or the text changed
constexpr UINT kMeasuredMessage = WM_APP + 2; // a Layout is waiting (Impl::measured)
constexpr UINT kReattachMessage = WM_APP + 3;
constexpr UINT kQuitMessage = WM_APP + 4;

constexpr UINT_PTR kMeasureTimer = 1; // its taskbar not put together yet: again
constexpr UINT_PTR kAttachTimer = 2;
constexpr UINT_PTR kCaretTimer = 3;

// Logical pixels, at the taskbar's scale.
constexpr double kMaxWidth = 240; // the whole box: this wide or not at all, a narrower form then
constexpr double kGap = 8; // from the icons, the tray, the widgets
constexpr double kPadding = 12; // inside, at either end
constexpr double kIconSize = 16;
constexpr double kIconGap = 10;
constexpr float kFontSize = 14;
constexpr double kMargin = 6; // round the box in its window
// Its icon alone is a taskbar button, as Windows' own search icon is
// (measured 2026-10-16 at 150%, dark taskbar: a slot 44 DIPs wide, the plate
// 40 × 40 with 4 DIP corners, 4 from the taskbar's top and bottom and 2 from
// the slot's ends, shown on hover only; the glyph 24 DIPs).
constexpr double kButtonSide = 2; // the plate from the slot's ends
constexpr double kButtonEdge = 4; // ... from the taskbar's top and bottom
constexpr double kButtonPlate = 40; // tallest
constexpr double kButtonRadius = 4;
constexpr double kButtonGlyph = 24;
constexpr double kTipGap = 12; // its tooltip's bottom above the taskbar, as Windows' own buttons' (measured over Start)

// Measuring (measureLoop). The taskbar says it moved something (a trigger)
// as it starts to, not while its icons slide (Windows 11's each about 333
// ms, one 16 ms after another), and what UI Automation says of them then
// runs ahead of the screen. So the box goes by a reading once the taskbar
// has been quiet a while and two readings agree (settled); by the ones
// before only where icons or the tray have come over it.
constexpr double kReadEveryMs = 125; // the readings before: no more often (never each frame), often enough to see icons come over the box
constexpr double kQuietMs = 1000; // since the last trigger: the icons done sliding (Start, last as a button goes, about 800 ms after), and a margin
constexpr double kConfirmMs = 250; // ... and the reading the same as one this much earlier, both since then
constexpr double kBusyMaxMs = 4000; // triggers coming on and on: the latest reading taken as settled this long after the first
constexpr int kAttachTries = 60; // a second apart: Explorer starting
constexpr int kUnmeasuredTries = 20; // likewise: its new taskbar still being put together

constexpr unsigned kBoxFrames = 1;

DWORD windowsBuild()
{
    wchar_t build[16] {};
    DWORD bytes = sizeof build - sizeof(wchar_t);
    if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber",
            RRF_RT_REG_SZ, nullptr, build, &bytes)
        != ERROR_SUCCESS)
        return 0;
    return static_cast<DWORD>(std::wcstol(build, nullptr, 10));
}

bool isWindows11() { return windowsBuild() >= 22000; }

DWORD explorerValue(const wchar_t* key, const wchar_t* name, DWORD fallback)
{
    DWORD value = fallback;
    DWORD size = sizeof value;
    if (::RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return fallback;
    return value;
}

bool iconsCentred() // Windows 11's default
{
    return explorerValue(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarAl", 1) != 0;
}

std::wstring take(BSTR text)
{
    std::wstring value = text ? text : L"";
    ::SysFreeString(text);
    return value;
}

std::wstring printed(const wchar_t* pattern, ...)
{
    wchar_t text[512];
    va_list args;
    va_start(args, pattern);
    std::vswprintf(text, 512, pattern, args);
    va_end(args);
    return text;
}

std::wstring rectText(const RECT& r)
{
    return printed(L"(%ld,%ld %ldx%ld)", r.left, r.top, r.right - r.left, r.bottom - r.top);
}

// The window classes inside the taskbar, for the log: another taskbar
// (StartAllBack, ExplorerPatcher) shows there.
std::wstring childClasses(HWND tray)
{
    std::wstring classes;
    ::EnumChildWindows(
        tray,
        [](HWND child, LPARAM param) -> BOOL {
            auto& out = *reinterpret_cast<std::wstring*>(param);
            wchar_t name[128] {};
            ::GetClassNameW(child, name, 128);
            out += (out.empty() ? L"" : L", ") + std::wstring(name);
            return out.size() < 400;
        },
        reinterpret_cast<LPARAM>(&classes));
    return classes.empty() ? L"none" : classes;
}

// Into WinShun.log (warnings and up go there): why the box is or is not on
// a user's taskbar is otherwise not to be found out.
void log(const std::wstring& line)
{
    qWarning().noquote() << "Taskbar box:" << QString::fromStdWString(line);
}

bool sameRect(const std::optional<RECT>& a, const std::optional<RECT>& b)
{
    return a.has_value() == b.has_value() && (!a || ::EqualRect(&*a, &*b));
}

double nowMs()
{
    static const double perMs = [] {
        LARGE_INTEGER frequency;
        ::QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart / 1000.0;
    }();
    LARGE_INTEGER counter;
    ::QueryPerformanceCounter(&counter);
    return counter.QuadPart / perMs;
}

// Until the compositor's next frame. Not DCompositionWaitForCompositorClock,
// which can wait in the kernel for good after a GPU reset or an ended remote
// session (zed-industries/zed#36934).
void waitForFrame()
{
    static thread_local int quick = 0;
    const double start = nowMs();
    if (FAILED(::DwmFlush()) || nowMs() - start < 0.5) {
        if (++quick >= 3)
            ::Sleep(15); // returning at once again and again (no display): no spinning
    } else {
        quick = 0;
    }
}

D2D1_COLOR_F gray(float level, float alpha) { return D2D1::ColorF(level, level, level, alpha); }

D2D1_COLOR_F colorOf(COLORREF color, float alpha = 1)
{
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, alpha);
}

// 0xAARRGGBB; none for alpha 0 (Look::selection, caret: the box's own then).
std::optional<D2D1_COLOR_F> argbOf(std::uint32_t argb)
{
    if ((argb >> 24) == 0)
        return std::nullopt;
    const auto channel = [argb](int shift) { return static_cast<float>((argb >> shift) & 0xFF) / 255.0f; };
    return D2D1::ColorF(channel(16), channel(8), channel(0), channel(24));
}

D2D1_COLOR_F mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t)
{
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

D2D1_COLOR_F faded(D2D1_COLOR_F color, float alpha)
{
    color.a *= alpha;
    return color;
}

// ---- motion ----

// An easing curve, as CSS's cubic-bezier().
struct Curve {
    float x1, y1, x2, y2;

    float operator()(float t) const
    {
        if (t <= 0)
            return 0;
        if (t >= 1)
            return 1;
        const auto at = [](float s, float a, float b) {
            const float u = 1 - s;
            return 3 * u * u * s * a + 3 * u * s * s * b + s * s * s;
        };
        float lo = 0;
        float hi = 1;
        float s = t;
        for (int i = 0; i < 24; ++i) { // x grows with s: halved down to a ten-thousandth
            const float x = at(s, x1, x2);
            if (std::abs(x - t) < 1e-4f)
                break;
            (x < t ? lo : hi) = s;
            s = (lo + hi) / 2;
        }
        return at(s, y1, y2);
    }
};

// Windows 11's own (learn.microsoft.com/windows/apps/design/signature-experiences/motion).
constexpr Curve kFastIn {0, 0, 0, 1}; // entrances; exits too, with a fade
constexpr Curve kSoftOut {1, 0, 1, 1}; // gentle exits
constexpr Curve kLinear {0, 0, 1, 1};

// A value going somewhere along a curve in a set time.
struct Tween {
    float value = 0;
    float from = 0;
    float to = 0;
    double start = 0;
    double duration = 0;
    Curve curve = kLinear;
    bool running = false;

    void snap(double v)
    {
        value = from = to = static_cast<float>(v);
        running = false;
    }
    void go(double goal, double now, double ms, Curve c)
    {
        const float target = static_cast<float>(goal);
        if (running ? to == target : value == target)
            return;
        from = value;
        to = target;
        start = now;
        duration = ms;
        curve = c;
        running = ms > 0;
        if (!running)
            value = target;
    }
    void step(double now)
    {
        if (!running)
            return;
        const float t = std::clamp(static_cast<float>((now - start) / duration), 0.0f, 1.0f);
        value = from + (to - from) * curve(t);
        if (t >= 1) {
            value = to;
            running = false;
        }
    }
};

// A damped spring: picks up where it is, at the speed it has, when given a new goal.
struct Spring {
    double value = 0;
    double velocity = 0; // per second
    double target = 0;
    double omega = 20; // stiffness, radians a second
    double zeta = 1; // damping: 1 none past the goal, less a little bounce
    double rest = 0.002; // close enough to stop
    bool moving = false;

    void snap(double v)
    {
        value = target = v;
        velocity = 0;
        moving = false;
    }
    void to(double goal, double stiffness, double damping)
    {
        target = goal;
        omega = stiffness;
        zeta = damping;
        moving = true;
    }
    void step(double ms)
    {
        if (!moving)
            return;
        for (double left = std::min(ms, 64.0) / 1000.0; left > 0;) { // after a stall, no leap
            const double h = std::min(left, 0.004);
            velocity += (-omega * omega * (value - target) - 2 * zeta * omega * velocity) * h;
            value += velocity * h;
            left -= h;
        }
        if (std::abs(value - target) < rest && std::abs(velocity) < rest * 20) {
            value = target;
            velocity = 0;
            moving = false;
        }
    }
};

// What UI Automation found on the taskbar, on the screen.
struct Layout {
    HWND tray = nullptr;
    int generation = 0;
    bool measured = false; // its frame was found
    std::wstring trouble; // ... else what went wrong (for the log)
    std::optional<RECT> icons; // Start, Windows' search, the apps
    std::vector<RECT> obstacles; // the tray, the widgets
    bool classic = false; // Windows 10's taskbar (classicTaskList): Start among the obstacles
    bool centred = false; // its icons centred (TaskbarAl; on a classic taskbar, StartAllBack's own)
    bool settled = false; // the taskbar quiet a while, and the same as a reading before (measureLoop)
    double sinceBusy = 0; // ms from the trigger it began changing with to this reading (for the log)
};

bool sameLayout(const Layout& a, const Layout& b)
{
    return a.measured == b.measured && a.classic == b.classic && a.centred == b.centred && sameRect(a.icons, b.icons)
        && a.obstacles.size() == b.obstacles.size()
        && std::equal(a.obstacles.begin(), a.obstacles.end(), b.obstacles.begin(),
            [](const RECT& x, const RECT& y) { return ::EqualRect(&x, &y) != FALSE; });
}

// The taskbar's UI Automation tree (Windows 11): a frame with Start, the
// apps and the rest as children, the tray's icons in it or beside it. The
// children are read in one go (a cache request): a few milliseconds; the
// tray's icons outside the frame are a search of the whole tree, 50 to 160 ms.
class Measurer {
public:
    // All of it, the tray's icons outside the frame included.
    Layout measure(HWND tray, int generation) { return read(tray, generation, true); }
    // The frame's children alone (the tray's icons as last found).
    Layout track(HWND tray, int generation) { return read(tray, generation, m_frameOf != tray || !m_frame); }

private:
    Layout read(HWND tray, int generation, bool full)
    {
        Layout layout;
        layout.tray = tray;
        layout.generation = generation;
        if (!ready()) {
            layout.trouble = L"UI Automation unavailable";
            return layout;
        }
        if (const HWND list = classicTaskList(tray))
            return readClassic(std::move(layout), list);
        ComPtr<IUIAutomationElement> root;
        if (full || m_frameOf != tray || !m_frame) {
            const HRESULT hr = m_uia->ElementFromHandle(tray, &root);
            if (FAILED(hr) || !root) {
                layout.trouble = printed(L"no UI Automation element for the taskbar (0x%08X)", static_cast<unsigned>(hr));
                return layout;
            }
        }
        if (m_frameOf != tray || !m_frame) { // a full walk: once per taskbar
            m_frame.Reset();
            m_outside.clear();
            root->FindFirst(TreeScope_Descendants, m_frameCondition.Get(), &m_frame);
            m_frameOf = tray;
        }
        ComPtr<IUIAutomationElementArray> children;
        if (!m_frame
            || FAILED(m_frame->FindAllBuildCache(TreeScope_Children, m_anyCondition.Get(), m_cache.Get(), &children))
            || !children) {
            layout.trouble = m_frame ? L"the TaskbarFrame's children unreadable" : L"no TaskbarFrame in UI Automation";
            m_frame.Reset(); // gone with its taskbar: looked for again next time
            return layout;
        }
        RECT bar {};
        ::GetWindowRect(tray, &bar);
        const LONG widest = (bar.right - bar.left) * 3 / 5; // wider: a container across it, not an icon
        int count = 0;
        children->get_Length(&count);
        for (int i = 0; i < count; ++i) {
            ComPtr<IUIAutomationElement> child;
            RECT r {};
            BSTR id = nullptr;
            if (FAILED(children->GetElement(i, &child)) || !child || FAILED(child->get_CachedBoundingRectangle(&r))
                || r.right <= r.left || r.bottom <= r.top)
                continue;
            child->get_CachedAutomationId(&id);
            const std::wstring automationId = take(id);
            if (automationId == L"SystemTrayIcon" || automationId == L"WidgetsButton") {
                layout.obstacles.push_back(r);
            } else if (r.right - r.left <= widest) {
                if (layout.icons)
                    ::UnionRect(&*layout.icons, &*layout.icons, &r);
                else
                    layout.icons = r;
            }
        }
        if (full && root) { // on some builds outside the frame
            m_outside.clear();
            ComPtr<IUIAutomationElementArray> trayIcons;
            if (SUCCEEDED(root->FindAllBuildCache(
                    TreeScope_Descendants, m_trayIconCondition.Get(), m_cache.Get(), &trayIcons))
                && trayIcons) {
                trayIcons->get_Length(&count);
                for (int i = 0; i < count; ++i) {
                    ComPtr<IUIAutomationElement> icon;
                    RECT r {};
                    if (SUCCEEDED(trayIcons->GetElement(i, &icon)) && icon
                        && SUCCEEDED(icon->get_CachedBoundingRectangle(&r)) && r.right > r.left && r.bottom > r.top)
                        m_outside.push_back(r);
                }
            }
        }
        layout.obstacles.insert(layout.obstacles.end(), m_outside.begin(), m_outside.end());
        layout.centred = iconsCentred();
        layout.measured = true;
        return layout;
    }

    // Windows 10's taskbar (StartAllBack, ExplorerPatcher): the task buttons
    // are UI Automation children of their window (MSTaskListWClass), all the
    // rest windows of their own, read as they are each time. Start is at the
    // left end even with the icons centred, so it is in the way, not one of
    // them.
    Layout readClassic(Layout layout, HWND list)
    {
        layout.classic = true;
        layout.centred = classicIconsCentred();
        if (m_listOf != list || !m_list) {
            m_list.Reset();
            m_listOf = nullptr;
            const HRESULT hr = m_uia->ElementFromHandle(list, &m_list);
            if (FAILED(hr) || !m_list) {
                layout.trouble = printed(L"no UI Automation element for the task buttons (0x%08X)", static_cast<unsigned>(hr));
                return layout;
            }
            m_listOf = list;
        }
        ComPtr<IUIAutomationElementArray> buttons;
        if (FAILED(m_list->FindAllBuildCache(TreeScope_Children, m_anyCondition.Get(), m_cache.Get(), &buttons))
            || !buttons) {
            layout.trouble = L"the task buttons (MSTaskListWClass) unreadable";
            m_list.Reset();
            return layout;
        }
        int count = 0;
        buttons->get_Length(&count);
        for (int i = 0; i < count; ++i) {
            ComPtr<IUIAutomationElement> button;
            RECT r {};
            if (FAILED(buttons->GetElement(i, &button)) || !button || FAILED(button->get_CachedBoundingRectangle(&r))
                || r.right <= r.left || r.bottom <= r.top)
                continue;
            if (layout.icons)
                ::UnionRect(&*layout.icons, &*layout.icons, &r);
            else
                layout.icons = r;
        }
        RECT listRect {};
        ::GetWindowRect(list, &listRect);
        if (!layout.icons) { // none pinned, none running: where the first would come
            const LONG x = layout.centred ? (listRect.left + listRect.right) / 2 : listRect.left;
            layout.icons = RECT {x, listRect.top, x, listRect.bottom};
        }
        const auto inTheWay = [&layout](HWND window) {
            RECT r {};
            if (::IsWindowVisible(window) && ::GetWindowRect(window, &r) && r.right > r.left && r.bottom > r.top)
                layout.obstacles.push_back(r);
        };
        const auto classOf = [](HWND window) {
            wchar_t name[64] {};
            ::GetClassNameW(window, name, 64);
            return std::wstring(name);
        };
        // Start, the tray, buttons of the taskbar's own (Task View) or of the
        // tool's; in the rebar, toolbars beside the task buttons.
        for (HWND child = nullptr; (child = ::FindWindowExW(layout.tray, child, nullptr, nullptr));) {
            const std::wstring name = classOf(child);
            if (name == kBoxClass)
                continue;
            if (name != L"ReBarWindow32") {
                inTheWay(child);
                continue;
            }
            for (HWND band = nullptr; (band = ::FindWindowExW(child, band, nullptr, nullptr));) {
                if (classOf(band) != L"MSTaskSwWClass")
                    inTheWay(band);
            }
        }
        layout.measured = true;
        return layout;
    }

    bool ready()
    {
        if (m_uia)
            return true;
        if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia)))
            && FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia))))
            return false;
        ComPtr<IUIAutomation2> timed; // a hung Explorer: given up on soon
        if (SUCCEEDED(m_uia.As(&timed))) {
            timed->put_ConnectionTimeout(2000);
            timed->put_TransactionTimeout(2000);
        }
        const auto byId = [this](const wchar_t* id, ComPtr<IUIAutomationCondition>& condition) {
            VARIANT value;
            ::VariantInit(&value);
            value.vt = VT_BSTR;
            value.bstrVal = ::SysAllocString(id);
            m_uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, &condition);
            ::VariantClear(&value);
        };
        byId(L"TaskbarFrame", m_frameCondition);
        byId(L"SystemTrayIcon", m_trayIconCondition);
        m_uia->CreateTrueCondition(&m_anyCondition);
        // Only what is read, all at once, no live elements kept.
        if (SUCCEEDED(m_uia->CreateCacheRequest(&m_cache))) {
            m_cache->AddProperty(UIA_BoundingRectanglePropertyId);
            m_cache->AddProperty(UIA_AutomationIdPropertyId);
            m_cache->put_AutomationElementMode(AutomationElementMode_None);
        }
        if (!m_frameCondition || !m_trayIconCondition || !m_anyCondition || !m_cache) {
            m_uia.Reset();
            return false;
        }
        return true;
    }

    ComPtr<IUIAutomation> m_uia;
    ComPtr<IUIAutomationCondition> m_frameCondition;
    ComPtr<IUIAutomationCondition> m_trayIconCondition;
    ComPtr<IUIAutomationCondition> m_anyCondition;
    ComPtr<IUIAutomationCacheRequest> m_cache;
    HWND m_frameOf = nullptr;
    ComPtr<IUIAutomationElement> m_frame;
    std::vector<RECT> m_outside; // the tray's icons outside the frame, as last found
    HWND m_listOf = nullptr; // a classic taskbar's (readClassic)
    ComPtr<IUIAutomationElement> m_list;
};

// Joined if it ends soon; one stuck with a hung Explorer is left to end by itself.
void finish(std::thread& thread)
{
    if (!thread.joinable())
        return;
    if (::WaitForSingleObject(thread.native_handle(), 2000) == WAIT_OBJECT_0)
        thread.join();
    else
        thread.detach();
}

} // namespace

struct SearchBox::Impl {
    Callbacks callbacks;
    std::atomic<bool> stopping {false};
    HANDLE ready = nullptr; // the helper window is up
    HANDLE quit = nullptr; // for the measuring thread
    std::thread boxThread;
    std::thread measureThread;
    std::thread clockThread;

    // Shared with the other threads (mutex).
    mutable std::mutex mutex;
    Look look;
    Text text;
    std::optional<RECT> shownRect;
    bool shownWide = false; // ... the whole box, not its label or icon alone
    std::optional<RECT> caretRect;
    std::atomic<HWND> helper {nullptr};
    std::atomic<HWND> box {nullptr};

    // The measuring thread's work (measureMutex).
    std::mutex measureMutex;
    HANDLE measureRequest = nullptr; // auto-reset
    HWND measureTray = nullptr;
    int measureGeneration = 0;
    bool measureWhole = false; // the tray's icons outside the frame too (requestMeasure)
    double measureAskedAt = 0; // the last trigger (nowMs), not when the thread got to it
    std::unique_ptr<Layout> measured; // the latest, for the box's thread
    bool measuredPosted = false;

    // Frames (clockMutex): the clock thread sets boxFrame once a frame while
    // the box's thread wants them (clockUsers).
    std::mutex clockMutex;
    std::condition_variable clockWake;
    std::atomic<unsigned> clockUsers {0};
    bool clockQuit = false;
    HANDLE boxFrame = nullptr; // auto-reset

    // The box's thread alone from here on.
    HWND tray = nullptr;
    HWINEVENTHOOK hook = nullptr;
    HWND classicList = nullptr; // a classic taskbar's task buttons (classicTaskList)...
    HWINEVENTHOOK buttonsHook = nullptr; // ... coming and going
    bool destroying = false; // the box goes because we said so
    int generation = 0;
    int attachTriesLeft = 0;
    int unmeasuredTries = 0;
    bool retrying = false; // kMeasureTimer set: one try a second, however many readings come meanwhile
    Hidden hidden = Hidden::Pending; // as told (shownChanged)
    bool classicShown = false; // as told (classicChanged)
    std::wstring attachTrouble; // why attach() last failed (for the log)
    std::wstring skipped; // why measureRoom() last skipped (for the log)
    UINT dpi = 96;
    // The reading the box was last placed by (placeFrom), not the latest:
    // most of those in between are left alone (apply).
    Layout taskbarLayout;
    bool provisional = false; // ... one with icons come over the box, not settled: nothing wider by it (refit)
    bool decided = false; // ... since it went onto this taskbar (else the next is placed by as it is)
    RECT placedBar {}; // ... the taskbar then: moved, resized or rescaled since, the next likewise

    // As wide as the stretch at its end of the taskbar allows: the whole
    // box, which what is typed goes into; its icon and a short label; its
    // icon alone.
    enum class Form { Box, Label, Icon };
    // Where the box is: its pill (the window has a margin round it), on the
    // screen, physical pixels.
    struct Target {
        double x = 0;
        double w = 0;
        LONG y = 0;
        LONG h = 0;
        Form form = Form::Box;
    };
    struct Shape {
        Form form;
        double width; // physical pixels
    };
    // The stretch the box goes in (measureRoom), physical pixels.
    struct Room {
        RECT bar {};
        UINT dpi = 96; // the taskbar's
        bool leftOfIcons = false; // the box at the stretch's left end; else right of the icons, at its right end (stretchFor)
        double from = 0, to = 0; // the stretch, the gaps at its ends kept
        LONG y = 0, h = 0; // the box's, and the label form's
        LONG buttonY = 0, buttonH = 0; // the icon form's plate
        double buttonW = 0; // ... and its slot
        double gap = 0; // kept from the icons and the ends: a button's slot goes right up to them
    };
    enum class Fit { Skip, None, Fits }; // not now (the taskbar sliding), nowhere, somewhere
    // Why the box went where it is (for the log): the first reading of the
    // taskbar as it is, a settled one, one with icons or the tray over the
    // box, the label or the style changed.
    enum class Why { First, Settled, Crowded, Look };
    std::optional<Target> target;
    bool placed = false; // its window is up, at pillX...
    bool hiding = false; // ... and fading out
    double pillX = 0; // whole pixels
    double pillW = 0;
    LONG pillY = 0;
    LONG pillH = 0;
    POINT windowPos {}; // in the taskbar
    SIZE windowSize {};
    int marginX = 0;
    int marginY = 0;
    LONG barH = 0; // the taskbar's height, for marginY
    double lastTick = 0;

    Look drawnLook;
    Text drawnText;
    std::wstring oldPlaceholder; // fading out (placeholderT)
    bool dark = false;
    bool highContrast = false;
    bool animations = true;
    bool tracking = false; // TrackMouseEvent for the leave
    bool hover = false;
    Tween hoverT;
    Tween activeT;
    Tween shownT;
    Tween placeholderT;
    Spring pressS; // 1 held down
    bool caretOn = true;
    float scroll = 0; // the text, moved left to keep the caret in view
    bool selecting = false; // the button held down in the text
    int selectAnchor = 0;
    int lastSelect = -1;
    bool iconDown = false; // the button held down on the icon: a click if let go on the box
    bool iconPressed = false; // ... the press shows on the icon alone while active
    HWND tip = nullptr; // its tooltip (makeTip)

    Form form = Form::Box; // as drawn: its target's
    Form drawnForm = Form::Box; // in the last frame
    double labelWidth = 0; // the label form's, physical pixels (labelForm)
    std::wstring labelMeasured;
    UINT labelDpi = 0;
    bool wideShown = false; // as told (wideChanged)

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1DCRenderTarget> renderTarget;
    ComPtr<IDWriteTextFormat> format;
    UINT formatDpi = 0;
    ComPtr<ID2D1Bitmap> icon;
    UINT iconDpi = 0;
    std::optional<COLORREF> iconInk; // one colour (monoIcon): this one; else its own colours
    ComPtr<ID2D1Bitmap> buttonIcon; // the program logo at the icon-only button's size
    int buttonIconPx = 0; // its size (buttonGlyph)
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    SIZE bitmapSize {};
    ComPtr<IDWriteTextLayout> textLayout; // the last one drawn while active: where presses land
    float textLeft = 0; // in the pill

    static thread_local Impl* t_current; // for the location event's callback

    ~Impl() // the last of its threads is done with it
    {
        for (const HANDLE event : {ready, quit, measureRequest, boxFrame}) {
            if (event)
                ::CloseHandle(event);
        }
    }

    template <typename F, typename... Args> void call(const F& f, Args&&... args)
    {
        if (f && !stopping.load())
            f(std::forward<Args>(args)...);
    }

    double scale() const { return dpi / 96.0; }

    // ---- frames ----

    void wantFrames(unsigned who, bool on)
    {
        const unsigned before = on ? clockUsers.fetch_or(who) : clockUsers.fetch_and(~who);
        if (on && !(before & who)) {
            std::lock_guard lock(clockMutex);
            clockWake.notify_one();
        }
    }

    void clockLoop()
    {
        std::unique_lock lock(clockMutex);
        for (;;) {
            clockWake.wait(lock, [this] { return clockUsers.load() != 0 || clockQuit; });
            if (clockQuit)
                return;
            lock.unlock();
            waitForFrame();
            if (clockUsers.load() & kBoxFrames)
                ::SetEvent(boxFrame);
            lock.lock();
        }
    }

    // ---- the box's thread ----

    void run()
    {
        t_current = this;
        ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
        ::DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc {sizeof wc};
        wc.lpfnWndProc = helperProc;
        wc.hInstance = instance;
        wc.lpszClassName = kHelperClass;
        ::RegisterClassExW(&wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = boxProc;
        wc.lpszClassName = kBoxClass;
        ::RegisterClassExW(&wc);
        // Top-level and never shown: broadcasts (the taskbar's theme) reach only those.
        const HWND window = ::CreateWindowExW(
            WS_EX_TOOLWINDOW, kHelperClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (window)
            ::ChangeWindowMessageFilterEx(window, WM_SETTINGCHANGE, MSGFLT_ALLOW, nullptr);
        helper = window;
        ::SetEvent(ready);
        if (!window || !d2d || !dwrite || stopping.load()) {
            helper = nullptr;
            if (window)
                ::DestroyWindow(window);
            return;
        }
        {
            std::lock_guard lock(mutex);
            drawnLook = look;
            drawnText = text;
        }
        readTheme();
        startAttaching();
        loop();
        wantFrames(kBoxFrames, false);
        detach();
        helper = nullptr;
        ::DestroyWindow(window);
        releaseSurface();
    }

    // Messages, and while anything moves a tick on every frame.
    void loop()
    {
        bool framing = false;
        for (;;) {
            const bool frames = animating();
            if (frames != framing) {
                wantFrames(kBoxFrames, frames);
                framing = frames;
                lastTick = nowMs();
            }
            const DWORD woke
                = ::MsgWaitForMultipleObjectsEx(frames ? 1 : 0, &boxFrame, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (frames && woke == WAIT_OBJECT_0)
                tick();
            MSG msg;
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT)
                    return;
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
        }
    }

    bool animating() const
    {
        return hoverT.running || activeT.running || shownT.running || placeholderT.running || pressS.moving;
    }

    void tick()
    {
        const double now = nowMs();
        const double ms = now - lastTick;
        lastTick = now;
        for (Tween* t : {&hoverT, &activeT, &shownT, &placeholderT})
            t->step(now);
        pressS.step(ms);
        if (hiding && !shownT.running) {
            finishHiding();
            return;
        }
        place();
        render();
    }

    void readTheme()
    {
        dark = explorerValue(
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", 1)
            == 0;
        HIGHCONTRASTW contrast {sizeof contrast};
        highContrast = ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0)
            && (contrast.dwFlags & HCF_HIGHCONTRASTON);
        BOOL on = TRUE;
        animations = !::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
        themeTip();
    }

    void startAttaching()
    {
        attachTriesLeft = kAttachTries;
        if (!attach())
            ::SetTimer(helper, kAttachTimer, 1000, nullptr);
    }

    bool attach()
    {
        const HWND bar = ::FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!bar || !::IsWindowVisible(bar)) {
            attachTrouble = bar ? L"the taskbar (Shell_TrayWnd) is not visible" : L"no taskbar (Shell_TrayWnd)";
            return false;
        }
        // Hidden until measured.
        const HWND window = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_NOPARENTNOTIFY, kBoxClass, drawnLook.name.c_str(),
            WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, bar, nullptr, ::GetModuleHandleW(nullptr), this);
        if (!window) {
            attachTrouble = printed(L"its window not made in the taskbar (error %lu)", ::GetLastError());
            return false;
        }
        RECT barRect {};
        ::GetWindowRect(bar, &barRect);
        classicList = classicTaskList(bar);
        log(printed(L"onto the %ls taskbar %ls, Windows build %lu, icons %ls, %u dpi",
            classicList ? L"classic (Windows 10's: StartAllBack, ExplorerPatcher)" : L"Windows 11", rectText(barRect).c_str(),
            windowsBuild(), (classicList ? classicIconsCentred() : iconsCentred()) ? L"centred" : L"left",
            ::GetDpiForWindow(bar)));
        tray = bar;
        box = window;
        makeTip(window);
        ++generation;
        windowPos = {};
        windowSize = {};
        DWORD process = 0;
        const DWORD thread = ::GetWindowThreadProcessId(tray, &process);
        // Its own thread's location changes: its buttons moving, it moving.
        hook = ::SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, locationChanged,
            process, thread, WINEVENT_OUTOFCONTEXT);
        // A classic taskbar says nothing of its buttons moving, only of one
        // made or gone (the rest slide over to make room or close the gap).
        if (classicList)
            buttonsHook = ::SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_DESTROY, nullptr, buttonsChanged, process,
                thread, WINEVENT_OUTOFCONTEXT);
        unmeasuredTries = 0;
        requestMeasure();
        return true;
    }

    // Its name over the label or the icon alone, as the taskbar's own buttons
    // have theirs (the whole box has its placeholder to say it). Owned by no
    // window: the taskbar is another process's. Always shown, though no
    // window of this thread is active; its text asked for each time (tipText).
    void makeTip(HWND window)
    {
        tip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, ::GetModuleHandleW(nullptr),
            nullptr);
        if (!tip)
            return;
        TTTOOLINFOW tool {};
        tool.cbSize = sizeof tool;
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = window;
        tool.uId = reinterpret_cast<UINT_PTR>(window);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        ::SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        themeTip();
    }

    // Dark on a dark taskbar, as the taskbar's own tooltips are (the common
    // controls' dark theme, as Explorer's own windows use it).
    void themeTip() const
    {
        if (tip)
            ::SetWindowTheme(tip, dark && !highContrast ? L"DarkMode_Explorer" : nullptr, nullptr);
    }

    // Above the taskbar, centred over the box, as the taskbar's own buttons
    // have theirs (else it is at the pointer, over the taskbar).
    bool placeTip() const
    {
        const HWND window = box.load();
        RECT tipRect {}, barRect {}, boxRect {};
        if (!window || !::GetWindowRect(tip, &tipRect) || !::GetWindowRect(tray, &barRect) || !::GetWindowRect(window, &boxRect))
            return false;
        MONITORINFO monitor {sizeof monitor};
        ::GetMonitorInfoW(::MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST), &monitor);
        const LONG w = tipRect.right - tipRect.left;
        const LONG h = tipRect.bottom - tipRect.top;
        const LONG x = std::clamp<LONG>((boxRect.left + boxRect.right - w) / 2, monitor.rcMonitor.left,
            std::max(monitor.rcMonitor.left, monitor.rcMonitor.right - w));
        const LONG y = barRect.top - std::lround(kTipGap * scale()) - h;
        return ::SetWindowPos(tip, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
    }

    // None (so none shown) on the whole box, while the launcher is open over
    // it, or held down.
    const wchar_t* tipText() const
    {
        if (form == Form::Box || drawnText.active || iconDown)
            return L"";
        return drawnLook.name.c_str();
    }

    void detach()
    {
        if (hook)
            ::UnhookWinEvent(std::exchange(hook, nullptr));
        if (buttonsHook)
            ::UnhookWinEvent(std::exchange(buttonsHook, nullptr));
        classicList = nullptr;
        if (const HWND window = box.load()) {
            destroying = true;
            ::DestroyWindow(window);
            destroying = false;
        }
        tray = nullptr;
        forget();
    }

    // No window on a taskbar any more.
    void forget()
    {
        ::KillTimer(helper, kMeasureTimer);
        retrying = false;
        placed = false;
        hiding = false;
        decided = false;
        target.reset();
        taskbarLayout = {};
        iconDown = selecting = false;
        setShown(Hidden::Pending);
    }

    // A trigger: the measuring thread reads the taskbar soon, and on until
    // it has settled (measureLoop). `whole`: the tray's icons outside the
    // frame looked for at once too, a slow search; not when the taskbar
    // moved something (the settled readings look for them).
    void requestMeasure(bool whole = true)
    {
        if (!tray)
            return;
        {
            std::lock_guard lock(measureMutex);
            measureTray = tray;
            measureGeneration = generation;
            measureWhole = measureWhole || whole;
            measureAskedAt = nowMs();
        }
        ::SetEvent(measureRequest);
    }

    static void CALLBACK locationChanged(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD)
    {
        Impl* self = t_current;
        if (!self || window == self->box.load() || object == OBJID_CURSOR || object == OBJID_CARET)
            return;
        self->requestMeasure(false); // as it starts: its icons may slide for a while after
    }

    static void CALLBACK buttonsChanged(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD)
    {
        Impl* self = t_current;
        if (self && window && window == self->classicList && object == OBJID_CLIENT && child != CHILDID_SELF)
            self->requestMeasure(false);
    }

    void measuredArrived()
    {
        std::unique_ptr<Layout> next;
        {
            std::lock_guard lock(measureMutex);
            next = std::move(measured);
            measuredPosted = false;
        }
        if (next)
            apply(*next);
    }

    // A reading (measureLoop). The box goes by the first of the taskbar as
    // it is (just gone onto it; or moved, resized or rescaled since) and by
    // settled ones. By the others only where in them the icons or the tray
    // have come over it: narrower or along at once, out of their way. Else
    // they are left alone, the box neither widened nor moved early: on the
    // screen the icons are still where they were.
    void apply(const Layout& next)
    {
        const HWND window = box.load();
        if (next.generation != generation || next.tray != tray || !window)
            return; // from before the box went onto another taskbar
        if (!next.measured) {
            if (retrying)
                return; // asked again already: a try a second
            if (++unmeasuredTries <= kUnmeasuredTries) {
                retrying = true;
                ::SetTimer(helper, kMeasureTimer, 1000, nullptr);
            } else if (!placed) { // given up on until the taskbar moves something
                setShown(Hidden::Unreadable, next.trouble + L"; in the taskbar: " + childClasses(tray));
            }
            return;
        }
        unmeasuredTries = 0;
        if (next.classic != classicShown) {
            classicShown = next.classic;
            call(callbacks.classicChanged, next.classic);
        }
        Room room;
        switch (measureRoom(next, room)) {
        case Fit::Skip:
            if (!placed)
                setShown(Hidden::OffScreen, skipped);
            return;
        case Fit::None:
            taskbarLayout = next;
            decided = true;
            placedBar = room.bar;
            hide(Hidden::Vertical);
            return;
        case Fit::Fits:
            break;
        }
        if (!decided || room.dpi != dpi || !::EqualRect(&room.bar, &placedBar))
            placeFrom(next, room, Why::First);
        else if (next.settled)
            placeFrom(next, room, Why::Settled);
        else if (crowded(next))
            placeFrom(next, room, Why::Crowded, target->form); // no wider: that waits for a settled reading
        // Over the taskbar's own content (its XAML island, a sibling).
        if (placed && ::GetWindow(window, GW_HWNDPREV))
            ::SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // In `layout`, the icons or what is in the way have come over the box
    // where it is now. Only over it: touching is not (a button's slot is
    // right against them), nor coming into the gaps it keeps.
    bool crowded(const Layout& layout) const
    {
        if (!target || !placed)
            return false;
        const double left = target->x;
        const double right = target->x + target->w;
        const auto over = [&](const RECT& r) { return r.left < right && r.right > left; };
        return (layout.icons && over(*layout.icons))
            || std::any_of(layout.obstacles.begin(), layout.obstacles.end(), over);
    }

    // The box by `layout` (`room` measured from it), at once: shown, moved,
    // another form, or hidden. `widest`: no wider a form than that.
    void placeFrom(const Layout& layout, const Room& room, Why why, Form widest = Form::Box)
    {
        taskbarLayout = layout;
        if (why != Why::Look)
            provisional = why == Why::Crowded;
        decided = true;
        placedBar = room.bar;
        if (room.dpi != dpi) {
            dpi = room.dpi;
            windowSize = {}; // drawn again at the new size
        }
        barH = room.bar.bottom - room.bar.top;
        marginX = static_cast<int>(std::lround(kMargin * scale()));
        const std::wstring because = whyText(why, layout);
        if (const std::optional<Target> t = targetIn(room, widest))
            setTarget(*t, because);
        else
            hide(Hidden::NoRoom,
                printed(L"%.0f px %ls the icons, box %ld px high; ", room.to - room.from,
                    room.leftOfIcons ? L"left of" : L"right of", room.h)
                    + because);
    }

    // For the log: why, and how long after the taskbar began to change.
    static std::wstring whyText(Why why, const Layout& layout)
    {
        static const wchar_t* const names[] = {L"first", L"settled", L"crowded", L"look changed"};
        std::wstring text = names[static_cast<int>(why)];
        if (why != Why::Look)
            text += printed(L", %.0f ms after the change", layout.sinceBusy);
        return text;
    }

    // Again by the reading last placed by: the label form's width, or the
    // forms the user's style allows, changed. No wider by a crowded one:
    // that waits for a settled reading.
    void refit()
    {
        Room room;
        if (box.load() && taskbarLayout.measured && measureRoom(taskbarLayout, room) == Fit::Fits)
            placeFrom(taskbarLayout, room, Why::Look, provisional && target ? target->form : Form::Box);
    }

    // The stretch the box goes in by `layout`, at an end of the taskbar out
    // of the icons' way (stretchFor). Left of them when they are centred and
    // the user leaves the side to us, from the taskbar's left end or what is
    // there (the widgets' button; Start on a classic taskbar), the box at
    // its left end; else right of them up to what is there (the tray, the
    // widgets), the box at its right end. Never between icons: icons coming
    // and going leave the box where it is; only what is at its end moves it.
    Fit measureRoom(const Layout& layout, Room& room)
    {
        MONITORINFO monitor {sizeof monitor};
        if (!::GetWindowRect(tray, &room.bar)
            || !::GetMonitorInfoW(::MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST), &monitor))
            return Fit::Skip;
        const RECT& bar = room.bar;
        room.dpi = ::GetDpiForWindow(tray);
        const double s = room.dpi / 96.0;
        const LONG barWidth = bar.right - bar.left;
        const LONG barHeight = bar.bottom - bar.top;
        // Auto-hidden, or sliding in or out: nothing on it is where it will be.
        RECT visible {};
        ::IntersectRect(&visible, &bar, &monitor.rcMonitor);
        const LONG tolerance = std::lround(2 * s);
        if (visible.right - visible.left < barWidth - tolerance || visible.bottom - visible.top < barHeight - tolerance) {
            skipped = L"the taskbar " + rectText(bar) + L" not all on its screen " + rectText(monitor.rcMonitor);
            return Fit::Skip;
        }
        if (barHeight > barWidth) // at a side: Windows 10's
            return Fit::None;
        const Stretch stretch = stretchFor(bar, layout.icons, layout.centred, layout.obstacles, drawnLook.side);
        room.leftOfIcons = stretch.leftOfIcons;
        room.gap = kGap * s;
        room.from = stretch.from + room.gap;
        room.to = stretch.to - room.gap;
        room.h = std::lround(barHeight * 2.0 / 3.0); // as Windows' own box
        room.y = bar.top + (barHeight - room.h) / 2;
        // As tall as the taskbar's own buttons: 4 DIPs off its edges, smaller
        // buttons (a lower taskbar) included.
        room.buttonH = buttonPlate(barHeight, s);
        room.buttonY = bar.top + (barHeight - room.buttonH) / 2;
        room.buttonW = room.buttonH + 2 * std::round(kButtonSide * s);
        return Fit::Fits;
    }

    // The widest form a stretch `width` wide takes (no wider than the user's
    // style, nor `widest`), and its width: the whole box at kMaxWidth, else
    // the label form as wide as its label needs, else the icon alone, a
    // taskbar button's slot.
    std::optional<Shape> shapeFor(double width, const Room& room, Form widest)
    {
        const auto may = [&](Form f) { return allowed(f) && static_cast<int>(f) >= static_cast<int>(widest); };
        if (const double whole = std::lround(kMaxWidth * scale()); may(Form::Box) && width >= whole)
            return Shape {Form::Box, whole};
        if (const double label = labelForm(); may(Form::Label) && label > 0 && width >= label)
            return Shape {Form::Label, label};
        if (width + 2 * ends(Form::Icon, room) >= room.buttonW)
            return Shape {Form::Icon, room.buttonW};
        return std::nullopt;
    }

    // The icon form's plate, as high as the taskbar's own buttons: 4 DIPs off
    // its edges, at most 40 (smaller buttons, a lower taskbar: smaller), at
    // the scale `s`.
    static LONG buttonPlate(LONG barHeight, double s)
    {
        return std::max<LONG>(1, std::min<LONG>(std::lround(kButtonPlate * s), barHeight - 2 * std::lround(kButtonEdge * s)));
    }

    // Its magnifier: 24 DIPs as Windows' own, but no more than 3/5 of a
    // lower plate (Windows' smaller taskbar buttons keep a margin round theirs).
    int buttonGlyph() const
    {
        const long full = std::lround(kButtonGlyph * scale());
        return static_cast<int>(barH > 0 ? std::min(full, std::lround(buttonPlate(barH, scale()) * 0.6)) : full);
    }

    // The user's style lets the box take this form.
    bool allowed(Form f) const { return static_cast<int>(f) >= static_cast<int>(drawnLook.style); }

    // How far into the gaps kept at a stretch's ends a form goes: a button's
    // slot right up to the icons and the ends, as the taskbar's own buttons
    // sit (its plate is inset in it); the other forms keep the gaps.
    static double ends(Form f, const Room& room) { return f == Form::Icon ? room.gap : 0; }

    // The label form's width: the icon and the short label, padded.
    double labelForm()
    {
        ensureFormat();
        if (labelDpi == dpi && labelMeasured == drawnLook.label)
            return labelWidth;
        labelDpi = dpi;
        labelMeasured = drawnLook.label;
        labelWidth = 0;
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics {};
        if (format && !labelMeasured.empty()
            && SUCCEEDED(dwrite->CreateTextLayout(labelMeasured.c_str(), static_cast<UINT32>(labelMeasured.size()),
                format.Get(), 100000.0f, 100.0f, &layout))
            && SUCCEEDED(layout->GetMetrics(&metrics))) {
            const double s = scale();
            labelWidth = std::ceil(std::round(kPadding * s) + std::round(kIconSize * s) + std::round(kIconGap * s)
                + metrics.widthIncludingTrailingWhitespace + std::round(kPadding * s));
        }
        return labelWidth;
    }

    // The box in `room`: at the stretch's end away from the icons, the
    // widest form that fits (none wider than `widest`); none where not even
    // the icon does.
    std::optional<Target> targetIn(const Room& room, Form widest)
    {
        const std::optional<Shape> shape = shapeFor(room.to - room.from, room, widest);
        if (!shape)
            return std::nullopt;
        const double into = ends(shape->form, room);
        const bool button = shape->form == Form::Icon;
        Target t;
        t.x = std::round(room.leftOfIcons ? room.from - into : room.to + into - shape->width);
        t.w = shape->width;
        t.y = button ? room.buttonY : room.y;
        t.h = button ? room.buttonH : room.h;
        t.form = shape->form;
        return t;
    }

    static RECT rectOf(const Target& t)
    {
        return {std::lround(t.x), t.y, std::lround(t.x + t.w), t.y + t.h};
    }

    static const wchar_t* formName(Form f)
    {
        switch (f) {
        case Form::Box:
            return L"box";
        case Form::Label:
            return L"label";
        case Form::Icon:
            break;
        }
        return L"icon";
    }

    // There at once: no glide, no hold; shown if it was not. `because` for
    // the log, written when the box goes on the taskbar or changes there.
    void setTarget(const Target& t, const std::wstring& because)
    {
        const bool changed = !target || target->form != t.form || target->x != t.x || target->w != t.w
            || target->y != t.y || target->h != t.h;
        target = t;
        pillX = t.x;
        pillW = t.w;
        pillY = t.y;
        pillH = t.h;
        form = t.form;
        const std::wstring line = formName(t.form) + (L" " + rectText(rectOf(t))) + L", " + because;
        if (!placed) {
            appear(line);
        } else {
            if (hiding) { // back before it was gone
                hiding = false;
                shownT.go(1, nowMs(), 250, kFastIn);
                setShown(Hidden::None, line);
            } else if (changed) {
                log(L"now " + line);
            }
            if (!animating()) {
                place();
                render();
            }
        }
        publishRect();
    }

    void appear(const std::wstring& line)
    {
        const HWND window = box.load();
        if (!window)
            return;
        placed = true;
        hiding = false;
        shownT.snap(animations ? 0 : 1);
        if (animations)
            shownT.go(1, nowMs(), 333, kFastIn);
        place();
        render();
        ::SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ::ShowWindow(window, SW_SHOWNA);
        setShown(Hidden::None, line);
    }

    // No room for it, or no taskbar: fading out.
    void hide(Hidden why, const std::wstring& detail = {})
    {
        target.reset();
        setShown(why, detail);
        if (!placed || hiding)
            return;
        if (!animations) {
            finishHiding();
            return;
        }
        hiding = true;
        shownT.go(0, nowMs(), 167, kSoftOut);
    }

    void finishHiding()
    {
        if (const HWND window = box.load())
            ::ShowWindow(window, SW_HIDE);
        placed = false;
        hiding = false;
        iconDown = selecting = false;
    }

    void setShown(Hidden why, const std::wstring& detail = {})
    {
        const bool on = why == Hidden::None;
        if (!on) {
            std::lock_guard lock(mutex);
            shownRect.reset();
            shownWide = false;
            caretRect.reset();
            wideShown = false;
        }
        if (hidden == why)
            return;
        hidden = why;
        static const wchar_t* const names[] = {L"on the taskbar", L"going onto the taskbar", L"no taskbar",
            L"taskbar unreadable", L"taskbar off its screen", L"taskbar at a side", L"no room"};
        std::wstring line = names[static_cast<int>(why)];
        if (!detail.empty())
            line += L": " + detail; // on the taskbar: its form, where and why (setTarget)
        log(line);
        call(callbacks.shownChanged, why);
    }

    void publishRect()
    {
        if (!target || hidden != Hidden::None)
            return;
        const RECT r = rectOf(*target);
        const bool wide = target->form == Form::Box;
        {
            std::lock_guard lock(mutex);
            shownRect = r;
            shownWide = wide;
        }
        if (wide != wideShown) {
            wideShown = wide;
            call(callbacks.wideChanged, wide);
        }
    }

    // The window round the pill where it is now.
    void place()
    {
        const HWND window = box.load();
        if (!window || !tray || !placed)
            return;
        marginY = static_cast<int>(std::clamp<LONG>((barH - pillH) / 2, 0, std::lround(kMargin * scale())));
        POINT pos {std::lround(pillX) - marginX, pillY - marginY};
        ::ScreenToClient(tray, &pos);
        const SIZE size {std::lround(pillW) + 2 * marginX, pillH + 2 * marginY};
        if (pos.x == windowPos.x && pos.y == windowPos.y && size.cx == windowSize.cx && size.cy == windowSize.cy)
            return;
        ::SetWindowPos(window, nullptr, pos.x, pos.y, size.cx, size.cy, SWP_NOACTIVATE | SWP_NOZORDER);
        windowPos = pos;
        windowSize = size;
    }

    void stateChanged()
    {
        const double now = nowMs();
        Text next;
        bool relabelled = false;
        {
            std::lock_guard lock(mutex);
            if (look.name != drawnLook.name && box.load())
                ::SetWindowTextW(box.load(), look.name.c_str());
            if (look.placeholder != drawnLook.placeholder && placed && animations && form == Form::Box) {
                oldPlaceholder = drawnLook.placeholder; // fades out as the new one fades in
                placeholderT.snap(0);
                placeholderT.go(1, now, 200, kFastIn);
            }
            relabelled = look.label != drawnLook.label || look.style != drawnLook.style || look.side != drawnLook.side;
            drawnLook = look;
            next = text;
        }
        if (relabelled)
            refit(); // the label form's width, or the forms it may take
        if (next.active != drawnText.active) {
            scroll = 0;
            if (animations) {
                activeT.go(next.active ? 1 : 0, now, next.active ? 250 : 167, kFastIn); // its fill and line alone: no ring, no hop
            } else {
                activeT.snap(next.active ? 1 : 0);
            }
        }
        drawnText = std::move(next);
        caretOn = true;
        if (drawnText.active) {
            const UINT blink = ::GetCaretBlinkTime();
            if (blink != INFINITE && blink != 0)
                ::SetTimer(helper, kCaretTimer, blink, nullptr);
        } else {
            ::KillTimer(helper, kCaretTimer);
            textLayout.Reset();
        }
        render();
    }

    void setHover(bool on)
    {
        if (hover == on)
            return;
        hover = on;
        if (animations)
            hoverT.go(on ? 1 : 0, nowMs(), on ? 100 : 200, kFastIn);
        else
            hoverT.snap(on ? 1 : 0);
        if (!animating())
            render();
    }

    void setPressed(bool on)
    {
        if (!animations) {
            pressS.snap(on ? 1 : 0);
            render();
            return;
        }
        if (on)
            pressS.to(1, 45, 0.9); // in quickly
        else
            pressS.to(0, 22, 1); // back, no bounce
    }

    // ---- the pointer ----

    // The window's own pixels to the pill's.
    float localX(int x) const { return static_cast<float>(x - marginX); }
    float localY(int y) const { return static_cast<float>(y - marginY); }

    // (x, y), its window's pixels, on the pill, not in the margin round it.
    bool onPill(int x, int y) const
    {
        const float px = localX(x);
        const float py = localY(y);
        return px >= 0 && py >= 0 && px < static_cast<float>(pillW) && py < static_cast<float>(pillH);
    }

    bool onIcon(int x) const
    {
        if (form != Form::Box)
            return true; // no text to press in: all of it
        const double s = scale();
        return localX(x) < static_cast<float>((kPadding + kIconSize + kIconGap / 2) * s);
    }

    // Where in the field's text the box is pressed at (x, y) (its window's pixels).
    std::optional<int> positionAt(int x, int y) const
    {
        if (!drawnText.active || form != Form::Box)
            return std::nullopt;
        if (!textLayout)
            return 0;
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS hit {};
        if (FAILED(textLayout->HitTestPoint(localX(x) - textLeft + scroll, localY(y), &trailing, &inside, &hit)))
            return std::nullopt;
        int at = static_cast<int>(hit.textPosition + (trailing ? hit.length : 0));
        // Back to the field's text, which has no composition in it.
        const int cursor = drawnText.cursor;
        const int composing = static_cast<int>(drawnText.composition.size());
        if (at > cursor)
            at = at >= cursor + composing ? at - composing : cursor;
        return std::clamp(at, 0, static_cast<int>(drawnText.text.size()));
    }

    void reportPress(const std::optional<int>& at, bool word)
    {
        Press press;
        if (target)
            press.anchor = {std::lround(target->x + target->w / 2), target->y};
        press.wide = target && target->form == Form::Box;
        press.at = at;
        press.word = word;
        call(callbacks.pressed, press);
    }

    // ---- drawing ----

    struct Palette {
        // edge: the line round it while typing, the grey of our windows' edge
        // (Theme.windowEdge): it stands for the launcher's field then.
        D2D1_COLOR_F fill, hoverFill, pressedFill, activeFill, border, edge, text, placeholder, selection, caret;
        // The icon form's plate (a taskbar button's): WinUI's SubtleFillColorSecondary
        // and Tertiary, as measured on Windows' own search icon (5.8 % white on dark).
        D2D1_COLOR_F buttonHover, buttonPressed, buttonTop;
    };

    Palette palette() const
    {
        const D2D1_COLOR_F accent = colorOf(drawnLook.accent);
        if (highContrast) {
            const D2D1_COLOR_F window = colorOf(::GetSysColor(COLOR_WINDOW));
            const D2D1_COLOR_F ink = colorOf(::GetSysColor(COLOR_WINDOWTEXT));
            const D2D1_COLOR_F highlight = colorOf(::GetSysColor(COLOR_HIGHLIGHT));
            return {window, window, window, window, ink, highlight, ink, colorOf(::GetSysColor(COLOR_GRAYTEXT)),
                highlight, ink, gray(0, 0), gray(0, 0), gray(0, 0)};
        }
        // On the taskbar, the taskbar's light or dark (not Win顺's), its
        // translucent fills, as Windows' own search box. What stands for the
        // launcher's field while typing follows the 界面风格 where it has a
        // colour of its own: the selection and the caret (Look::selection, caret).
        D2D1_COLOR_F selection = accent;
        selection.a = dark ? 0.45f : 0.32f;
        if (const std::optional<D2D1_COLOR_F> styled = argbOf(drawnLook.selection))
            selection = *styled;
        Palette p = dark
            ? Palette {gray(1, 0.06f), gray(1, 0.11f), gray(1, 0.03f), gray(0.11f, 0.94f), gray(1, 0.09f),
                  gray(0x6A / 255.0f, 1), gray(1, 1), gray(1, 0.66f), selection, gray(1, 1), gray(1, 0.058f), gray(1, 0.039f),
                  gray(1, 0.045f)}
            : Palette {gray(1, 0.6f), gray(1, 0.82f), gray(1, 0.42f), gray(1, 0.98f), gray(0, 0.08f), gray(0xB4 / 255.0f, 1),
                  gray(0, 0.9f), gray(0, 0.6f), selection, gray(0, 0.9f), gray(0, 0.037f), gray(0, 0.024f), gray(0, 0)};
        if (const std::optional<D2D1_COLOR_F> caret = argbOf(drawnLook.caret))
            p.caret = *caret;
        // The style's field (brand's white box, tactile's slot), on a taskbar
        // of the same light or dark: hover and press a step towards the ink;
        // the line while typing stays our windows' grey (p.edge).
        if (const std::optional<D2D1_COLOR_F> fill = argbOf(drawnLook.fill); fill && drawnLook.styleDark == dark) {
            p.fill = *fill;
            p.hoverFill = mix(*fill, p.text, 0.04f);
            p.pressedFill = mix(*fill, p.text, 0.08f);
            p.activeFill = *fill;
            if (const std::optional<D2D1_COLOR_F> edge = argbOf(drawnLook.edge))
                p.border = *edge;
        }
        return p;
    }

    // The words in the box: the placeholder (or what to search, while typing
    // there); a short label where it is narrow; none round the icon alone.
    std::wstring labelOf(Form f) const
    {
        switch (f) {
        case Form::Box:
            return drawnLook.placeholder;
        case Form::Label:
            return drawnLook.label;
        case Form::Icon:
            break;
        }
        return {};
    }

    void releaseSurface()
    {
        renderTarget.Reset();
        icon.Reset();
        buttonIcon.Reset();
        if (memory) {
            ::SelectObject(memory, oldBitmap);
            ::DeleteDC(std::exchange(memory, nullptr));
        }
        if (bitmap)
            ::DeleteObject(std::exchange(bitmap, nullptr));
        bitmapSize = {};
    }

    // A bitmap as big as the window gets: the whole box's, kept as the form changes.
    bool ensureSurface()
    {
        if (renderTarget && bitmapSize.cx >= windowSize.cx && bitmapSize.cy >= windowSize.cy)
            return true;
        releaseSurface();
        const SIZE size {std::max<LONG>(windowSize.cx, std::lround(kMaxWidth * scale()) + 2 * marginX + 2),
            windowSize.cy};
        BITMAPINFO info {};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), size.cx, -size.cy, 1, 32, BI_RGB};
        void* bits = nullptr;
        const HDC screen = ::GetDC(nullptr);
        memory = ::CreateCompatibleDC(screen);
        bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        ::ReleaseDC(nullptr, screen);
        if (!memory || !bitmap) {
            releaseSurface();
            return false;
        }
        oldBitmap = ::SelectObject(memory, bitmap);
        bitmapSize = size;
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(d2d->CreateDCRenderTarget(&properties, &renderTarget))) {
            releaseSurface();
            return false;
        }
        return true;
    }

    void ensureFormat()
    {
        if (format && formatDpi == dpi)
            return;
        format.Reset();
        formatDpi = dpi;
        wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"zh-CN";
        ::GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH); // fallback fonts for its script
        if (FAILED(dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, kFontSize * dpi / 96.0f, locale, &format)))
            return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // In one colour: the taskbar's text's (white on dark, near black on
    // light), or high contrast's.
    std::optional<COLORREF> wantedInk() const
    {
        if (!drawnLook.monoIcon)
            return std::nullopt;
        return highContrast ? ::GetSysColor(COLOR_WINDOWTEXT) : dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1B, 0x1B, 0x1B);
    }

    // The same program identity in every form, at each form's exact size.
    void ensureIcon()
    {
        const std::optional<COLORREF> ink = wantedInk();
        const int glyph = buttonGlyph();
        if (icon && buttonIcon && iconDpi == dpi && iconInk == ink && buttonIconPx == glyph)
            return;
        icon.Reset();
        buttonIcon.Reset();
        iconDpi = dpi;
        iconInk = ink;
        icon = logoBitmap(static_cast<int>(std::lround(kIconSize * dpi / 96.0)), ink);
        buttonIconPx = glyph;
        buttonIcon = logoBitmap(glyph, ink);
    }

    ComPtr<ID2D1Bitmap> logoBitmap(int px, std::optional<COLORREF> ink)
    {
        if (!ink)
            return iconBitmap(px);
        ComPtr<ID2D1Bitmap> result;
        const QImage image = monologo::image(px, QColor(GetRValue(*ink), GetGValue(*ink), GetBValue(*ink)));
        renderTarget->CreateBitmap(D2D1::SizeU(px, px), image.constBits(), static_cast<UINT32>(image.bytesPerLine()),
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &result);
        return result;
    }

    // The program's colour icon at px, converted to premultiplied alpha.
    ComPtr<ID2D1Bitmap> iconBitmap(int px)
    {
        ComPtr<ID2D1Bitmap> result;
        const auto handle
            = static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr), L"IDI_ICON1", IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
        if (!handle)
            return result;
        ICONINFO info {};
        if (::GetIconInfo(handle, &info)) {
            BITMAPINFO bi {};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB};
            std::vector<std::uint32_t> pixels(static_cast<size_t>(px) * px);
            const HDC screen = ::GetDC(nullptr);
            const bool read = info.hbmColor && ::GetDIBits(screen, info.hbmColor, 0, px, pixels.data(), &bi, DIB_RGB_COLORS);
            ::ReleaseDC(nullptr, screen);
            if (read) {
                for (std::uint32_t& p : pixels) { // straight alpha to premultiplied
                    const std::uint32_t a = p >> 24;
                    const auto channel = [&](int shift) { return (((p >> shift) & 0xFF) * a + 127) / 255 << shift; };
                    p = (a << 24) | channel(16) | channel(8) | channel(0);
                }
                renderTarget->CreateBitmap(D2D1::SizeU(px, px), pixels.data(), px * 4,
                    D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                    &result);
            }
            if (info.hbmColor)
                ::DeleteObject(info.hbmColor);
            if (info.hbmMask)
                ::DeleteObject(info.hbmMask);
        }
        ::DestroyIcon(handle);
        return result;
    }

    void render()
    {
        const HWND window = box.load();
        if (!window || !placed || windowSize.cx <= 0 || windowSize.cy <= 0 || !ensureSurface())
            return;
        const RECT all {0, 0, windowSize.cx, windowSize.cy};
        if (FAILED(renderTarget->BindDC(memory, &all)))
            return;
        ensureFormat();
        ensureIcon();
        const float s = static_cast<float>(scale());
        const float w = static_cast<float>(pillW);
        const float h = static_cast<float>(pillH);
        const Palette p = palette();
        if (form != drawnForm) { // its words change with it
            if (animations) {
                oldPlaceholder = labelOf(drawnForm);
                placeholderT.snap(0);
                placeholderT.go(1, nowMs(), 200, kFastIn);
            }
            drawnForm = form;
        }
        const bool whole = form == Form::Box; // else no typing shows here, the launcher has its field
        const bool active = drawnText.active;
        const float activeAmount = activeT.value;
        const float idle = 1 - activeAmount;
        const float press = static_cast<float>(std::clamp(pressS.value, -0.5, 1.0));
        // Its icon alone: a taskbar button, a plate under the glyph on hover
        // or while open, none at rest; pressed, only the glyph gives.
        const bool button = form == Form::Icon;
        const float pill = iconPressed || button ? 0 : press * idle; // the box itself gives a little when pressed

        // Pressed, appearing: about its middle.
        const float grow = (1 - 0.035f * pill) * (0.9f + 0.1f * shownT.value);
        const D2D1::Matrix3x2F base = D2D1::Matrix3x2F::Scale(grow, grow, D2D1::Point2F(w / 2, h / 2))
            * D2D1::Matrix3x2F::Translation(static_cast<float>(marginX), static_cast<float>(marginY));
        const bool still = grow == 1;
        const D2D1_DRAW_TEXT_OPTIONS textOptions = still ? D2D1_DRAW_TEXT_OPTIONS_NONE : D2D1_DRAW_TEXT_OPTIONS_NO_SNAP;

        renderTarget->BeginDraw();
        renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
        renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
        renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // on alpha: no ClearType
        renderTarget->SetTransform(base);
        ComPtr<ID2D1SolidColorBrush> brush;
        renderTarget->CreateSolidColorBrush(p.fill, &brush);
        const bool round = drawnLook.roundedCorners;
        // The box: a pill as Windows' own, or the style's field corner (never past a pill).
        const float boxRadius = drawnLook.radius >= 0 ? std::min(h / 2, std::round(static_cast<float>(drawnLook.radius) * s)) : h / 2;
        const float radius = round ? (button ? std::round(static_cast<float>(kButtonRadius) * s) : boxRadius) : 0;
        const float inset = button ? std::round(static_cast<float>(kButtonSide) * s) : 0; // the plate in the slot
        const auto rounded = [&](float out) {
            return D2D1_ROUNDED_RECT {D2D1::RectF(inset - out, -out, w - inset + out, h + out),
                round ? std::max(0.0f, radius + out) : 0, round ? std::max(0.0f, radius + out) : 0};
        };

        // The pill, with a line round it, 1 logical px as round our windows;
        // with high contrast the highlight's, thicker, while typing in it.
        // On whole pixels, rounded as Qt rounds our windows' (2 at 150 %):
        // 1.5 px drew one row at the top and left, two at the bottom and right.
        const float line = std::max(1.0f, std::round(s));
        const float stroke = highContrast ? std::round(line * (1 + activeAmount)) : line;
        // Square edges stay on physical pixels at fractional monitor scales.
        renderTarget->SetAntialiasMode(round ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
        if (button) {
            // As the taskbar's own buttons (Windows' subtle fills): shown on
            // hover or open, fainter pressed; a faint lighter top edge on
            // dark. Never quite clear: a layered window's pixels with no
            // alpha let the pointer through to the taskbar, and the plate is
            // what is pointed at.
            const float shown = std::max(hoverT.value, activeAmount);
            D2D1_COLOR_F plate = mix(p.buttonHover, p.buttonPressed, std::clamp(press, 0.0f, 1.0f) * hoverT.value);
            plate.a = std::max(plate.a * shown, 1.0f / 255);
            brush->SetColor(plate);
            renderTarget->FillRoundedRectangle(rounded(0), brush.Get());
            if (highContrast && shown > 0) {
                brush->SetColor(faded(p.edge, shown));
                renderTarget->DrawRoundedRectangle(rounded(-line / 2), brush.Get(), line);
            } else if (p.buttonTop.a > 0 && shown > 0) {
                ComPtr<ID2D1GradientStopCollection> stops;
                const D2D1_GRADIENT_STOP top[] = {{0, faded(p.buttonTop, shown)}, {1, faded(p.buttonTop, 0)}};
                ComPtr<ID2D1LinearGradientBrush> edge;
                if (SUCCEEDED(renderTarget->CreateGradientStopCollection(top, 2, &stops))
                    && SUCCEEDED(renderTarget->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 3 * s)), stops.Get(), &edge)))
                    renderTarget->DrawRoundedRectangle(rounded(-line / 2), edge.Get(), line);
            }
        } else {
            D2D1_COLOR_F fill = mix(p.fill, p.hoverFill, hoverT.value);
            fill = mix(fill, p.pressedFill, std::clamp(pill, 0.0f, 1.0f));
            // Open while narrow: lit, as a taskbar button whose window is open.
            // Under all of the line: half under it made its rows two shades.
            brush->SetColor(mix(fill, whole ? p.activeFill : p.hoverFill, activeAmount));
            renderTarget->FillRoundedRectangle(rounded(0), brush.Get());
            brush->SetColor(mix(p.border, whole ? p.edge : p.border, activeAmount));
            renderTarget->DrawRoundedRectangle(rounded(-stroke / 2), brush.Get(), stroke);
        }

        renderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float iconSize = button ? static_cast<float>(buttonIconPx) : std::round(static_cast<float>(kIconSize) * s);
        const float iconX = button ? std::round((w - iconSize) / 2)
                                   : std::min(std::round(static_cast<float>(kPadding) * s), std::round((w - iconSize) / 2)); // narrow: centred
        if (const ComPtr<ID2D1Bitmap>& glyph = button ? buttonIcon : icon) {
            const float y = std::round((h - iconSize) / 2);
            const float k = 1 - 0.14f * std::max(0.0f, press);
            renderTarget->SetTransform(
                D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(iconX + iconSize / 2, y + iconSize / 2)) * base);
            renderTarget->DrawBitmap(glyph.Get(), D2D1::RectF(iconX, y, iconX + iconSize, y + iconSize), 1.0f,
                still && k == 1 ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            renderTarget->SetTransform(base);
        }
        textLeft = iconX + iconSize + std::round(static_cast<float>(kIconGap) * s);
        const float textRight = std::max(textLeft, w - std::round(static_cast<float>(kPadding) * s));
        const float room = std::max(1.0f, textRight - textLeft);
        renderTarget->PushAxisAlignedClip(D2D1::RectF(textLeft, 0, textRight, h), D2D1_ANTIALIAS_MODE_ALIASED);

        std::optional<D2D1_RECT_F> caret; // in the pill
        const float caretWidth = std::max(1.0f, std::round(s));
        const std::wstring& typed = drawnText.text;
        const std::wstring& composition = drawnText.composition;
        if (format && (!whole || !active || (typed.empty() && composition.empty()))) {
            // The placeholder, cut short with "…" where it does not fit; a
            // new one comes in from the right as the old one goes left.
            const auto placeholder = [&](const std::wstring& value, float alpha, float dx) {
                ComPtr<IDWriteTextLayout> layout;
                if (alpha <= 0
                    || FAILED(dwrite->CreateTextLayout(
                        value.c_str(), static_cast<UINT32>(value.size()), format.Get(), room, h, &layout)))
                    return layout;
                ComPtr<IDWriteInlineObject> ellipsis;
                dwrite->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
                const DWRITE_TRIMMING trimming {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                layout->SetTrimming(&trimming, ellipsis.Get());
                brush->SetColor(faded(p.placeholder, alpha));
                renderTarget->DrawTextLayout(D2D1::Point2F(textLeft + dx, 0), layout.Get(), brush.Get(),
                    dx == 0 ? textOptions : D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
                return layout;
            };
            const float in = placeholderT.running ? placeholderT.value : 1;
            const float shift = 8 * s;
            if (in < 1)
                placeholder(oldPlaceholder, 1 - in, -shift * in);
            const ComPtr<IDWriteTextLayout> layout = placeholder(labelOf(form), in, shift * (1 - in));
            if (active && whole && layout) { // typing starts here, over the placeholder
                float x = 0;
                float y = 0;
                DWRITE_HIT_TEST_METRICS hit {};
                layout->HitTestTextPosition(0, FALSE, &x, &y, &hit);
                caret = D2D1::RectF(textLeft, std::round(hit.top), textLeft + caretWidth, std::round(hit.top + hit.height));
            }
            textLayout.Reset();
        } else if (format) {
            // The field's text, the composition at its cursor.
            const int cursor = std::clamp(drawnText.cursor, 0, static_cast<int>(typed.size()));
            const std::wstring shownText = typed.substr(0, cursor) + composition + typed.substr(cursor);
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite->CreateTextLayout(shownText.c_str(), static_cast<UINT32>(shownText.size()),
                    format.Get(), 100000.0f, h, &layout))) {
                if (!composition.empty())
                    layout->SetUnderline(
                        TRUE, {static_cast<UINT32>(cursor), static_cast<UINT32>(composition.size())});
                const auto xAt = [&](int index, float* top, float* height) {
                    float x = 0;
                    float y = 0;
                    DWRITE_HIT_TEST_METRICS hit {};
                    layout->HitTestTextPosition(static_cast<UINT32>(index), FALSE, &x, &y, &hit);
                    if (top)
                        *top = hit.top;
                    if (height)
                        *height = hit.height;
                    return x;
                };
                float lineTop = 0;
                float lineHeight = 0;
                const int caretIndex = cursor + static_cast<int>(composition.size());
                const float caretX = xAt(caretIndex, &lineTop, &lineHeight);
                DWRITE_TEXT_METRICS metrics {};
                layout->GetMetrics(&metrics);
                if (caretX - scroll > room - caretWidth)
                    scroll = caretX - room + caretWidth;
                if (caretX < scroll)
                    scroll = caretX;
                scroll = std::clamp(scroll, 0.0f, std::max(0.0f, metrics.widthIncludingTrailingWhitespace + caretWidth - room));
                const float originX = textLeft - scroll;
                const int from = std::min(drawnText.selectionStart, drawnText.selectionEnd);
                const int to = std::max(drawnText.selectionStart, drawnText.selectionEnd);
                if (composition.empty() && from < to) {
                    UINT32 count = 0;
                    layout->HitTestTextRange(static_cast<UINT32>(from), static_cast<UINT32>(to - from), 0, 0, nullptr, 0, &count);
                    std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
                    if (count
                        && SUCCEEDED(layout->HitTestTextRange(static_cast<UINT32>(from), static_cast<UINT32>(to - from),
                            0, 0, ranges.data(), count, &count))) {
                        brush->SetColor(p.selection);
                        for (UINT32 i = 0; i < count; ++i) {
                            const DWRITE_HIT_TEST_METRICS& r = ranges[i];
                            renderTarget->FillRectangle(D2D1::RectF(originX + r.left, r.top, originX + r.left + r.width,
                                                            r.top + r.height),
                                brush.Get());
                        }
                    }
                }
                brush->SetColor(p.text);
                renderTarget->DrawTextLayout(D2D1::Point2F(originX, 0), layout.Get(), brush.Get(), textOptions);
                const float x = std::round(originX + caretX);
                if (caretOn) {
                    brush->SetColor(p.caret);
                    renderTarget->FillRectangle(
                        D2D1::RectF(x, std::round(lineTop), x + caretWidth, std::round(lineTop + lineHeight)), brush.Get());
                }
                // For the input method's window: where the composition starts.
                const float imeX = std::round(originX + xAt(cursor, nullptr, nullptr));
                caret = D2D1::RectF(imeX, lineTop, imeX + 1, lineTop + lineHeight);
                textLayout = layout;
            }
        }
        if (caret && active && whole && caretOn && typed.empty() && composition.empty()) {
            brush->SetColor(faded(p.caret, activeAmount));
            renderTarget->FillRectangle(*caret, brush.Get());
        }
        renderTarget->PopAxisAlignedClip();
        if (renderTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
            releaseSurface();
            return;
        }
        POINT zero {};
        const BYTE alpha = static_cast<BYTE>(std::lround(255 * std::clamp(shownT.value, 0.0f, 1.0f)));
        BLENDFUNCTION blend {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        ::UpdateLayeredWindow(window, nullptr, nullptr, &windowSize, memory, &zero, 0, &blend, ULW_ALPHA);

        std::optional<RECT> onScreen; // where it settles, not where it is drawn mid-animation
        if (caret && target) {
            const double left = target->x;
            onScreen = RECT {static_cast<LONG>(std::lround(left + caret->left)), target->y + static_cast<LONG>(caret->top),
                static_cast<LONG>(std::lround(left + caret->right)), target->y + static_cast<LONG>(std::lround(caret->bottom))};
        }
        bool moved = false;
        {
            std::lock_guard lock(mutex);
            moved = !sameRect(caretRect, onScreen);
            caretRect = onScreen;
        }
        if (moved && onScreen)
            call(callbacks.caretMoved);
    }

    // ---- window procedures ----

    static Impl* of(HWND window, UINT msg, LPARAM lParam)
    {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return self;
        }
        return reinterpret_cast<Impl*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    static LRESULT CALLBACK helperProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        Impl* self = of(window, msg, lParam);
        if (!self)
            return ::DefWindowProcW(window, msg, wParam, lParam);
        switch (msg) {
        case kStateMessage:
            self->stateChanged();
            return 0;
        case kMeasuredMessage:
            self->measuredArrived();
            return 0;
        case kReattachMessage:
            self->detach();
            self->startAttaching();
            return 0;
        case kQuitMessage:
            ::PostQuitMessage(0);
            return 0;
        case WM_SETTINGCHANGE: // light or dark, high contrast, animations, the icons' alignment
            self->readTheme();
            self->requestMeasure();
            self->render();
            return 0;
        case WM_TIMER:
            switch (wParam) {
            case kMeasureTimer:
                ::KillTimer(window, kMeasureTimer);
                self->retrying = false;
                self->requestMeasure();
                break;
            case kAttachTimer:
                if (self->attach()) {
                    ::KillTimer(window, kAttachTimer);
                } else if (--self->attachTriesLeft <= 0) {
                    ::KillTimer(window, kAttachTimer);
                    self->setShown(Hidden::NoTaskbar, self->attachTrouble);
                }
                break;
            case kCaretTimer:
                self->caretOn = !self->caretOn;
                self->render();
                break;
            }
            return 0;
        }
        return ::DefWindowProcW(window, msg, wParam, lParam);
    }

    static LRESULT CALLBACK boxProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        Impl* self = of(window, msg, lParam);
        if (!self)
            return ::DefWindowProcW(window, msg, wParam, lParam);
        const int x = static_cast<short>(LOWORD(lParam));
        const int y = static_cast<short>(HIWORD(lParam));
        switch (msg) {
        case WM_MOUSEACTIVATE: // the launcher takes the keyboard, not the taskbar
            return MA_NOACTIVATE;
        case WM_SETCURSOR: {
            POINT pointer {};
            ::GetCursorPos(&pointer);
            ::ScreenToClient(window, &pointer);
            const wchar_t* shape = self->drawnText.active && self->form == Form::Box && !self->onIcon(pointer.x)
                ? IDC_IBEAM
                : IDC_ARROW;
            ::SetCursor(::LoadCursorW(nullptr, shape));
            return TRUE;
        }
        case WM_MOUSEMOVE:
            if (!self->tracking) {
                TRACKMOUSEEVENT track {sizeof track, TME_LEAVE, window, 0};
                self->tracking = ::TrackMouseEvent(&track);
            }
            self->setHover(true);
            if (self->selecting && (wParam & MK_LBUTTON)) { // selecting text in the box
                if (const std::optional<int> at = self->positionAt(x, y); at && *at != self->lastSelect) {
                    self->lastSelect = *at;
                    self->call(self->callbacks.dragged, self->selectAnchor, *at);
                }
            }
            return 0;
        case WM_MOUSELEAVE:
            self->tracking = false;
            self->setHover(false);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            ::SetCapture(window);
            if (self->onIcon(x)) { // a click, on release
                self->iconDown = true;
                self->iconPressed = self->drawnText.active;
                self->setPressed(true);
                return 0;
            }
            const std::optional<int> at = self->positionAt(x, y);
            self->selecting = at.has_value() && msg == WM_LBUTTONDOWN;
            self->selectAnchor = at.value_or(0);
            self->lastSelect = self->selectAnchor;
            self->iconPressed = false;
            if (!self->drawnText.active) // a text field gives no press to placing the caret
                self->setPressed(true);
            self->reportPress(at, msg == WM_LBUTTONDBLCLK);
            return 0;
        }
        case WM_LBUTTONUP: {
            const bool click = self->iconDown && self->onPill(x, y); // let go off it: no click
            if (::GetCapture() == window)
                ::ReleaseCapture(); // ends the press (WM_CAPTURECHANGED)
            if (click)
                self->reportPress(std::nullopt, false);
            return 0;
        }
        case WM_CAPTURECHANGED:
            self->iconDown = false;
            self->selecting = false;
            self->setPressed(false);
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            self->requestMeasure();
            return 0;
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->hwndFrom != self->tip)
                break;
            if (header->code == TTN_GETDISPINFOW) {
                reinterpret_cast<NMTTDISPINFOW*>(lParam)->lpszText = const_cast<wchar_t*>(self->tipText());
                return 0;
            }
            if (header->code == TTN_SHOW)
                return self->placeTip() ? TRUE : FALSE; // TRUE: where we put it
            break;
        }
        case WM_DESTROY:
            self->box = nullptr;
            if (self->tip)
                ::DestroyWindow(std::exchange(self->tip, nullptr));
            self->textLayout.Reset();
            if (!self->destroying) { // with its taskbar: Explorer ended
                if (self->hook)
                    ::UnhookWinEvent(std::exchange(self->hook, nullptr));
                if (self->buttonsHook)
                    ::UnhookWinEvent(std::exchange(self->buttonsHook, nullptr));
                self->classicList = nullptr;
                self->tray = nullptr;
                self->forget();
                self->startAttaching();
            }
            return 0;
        }
        return ::DefWindowProcW(window, msg, wParam, lParam);
    }

    // ---- the measuring thread ----

    void publish(Layout layout)
    {
        const HWND window = helper.load();
        if (!window || stopping.load())
            return;
        std::lock_guard lock(measureMutex);
        measured = std::make_unique<Layout>(std::move(layout));
        if (!measuredPosted)
            measuredPosted = ::PostMessageW(window, kMeasuredMessage, 0, 0) != FALSE;
    }

    // Asked (a trigger): the taskbar read soon, then every kReadEveryMs
    // until a reading has settled; nothing read while it is idle. It says
    // nothing more while its icons slide, and UI Automation runs ahead of
    // them: one not moving yet says where it will end, then where it is
    // again, then slides; Start, as a button goes, only begins about 430 ms
    // after. So a reading counts once the taskbar has been quiet kQuietMs
    // and one kConfirmMs later says the same; the ones before only for icons
    // come over the box (apply). Triggers on and on (something on it
    // animating): the latest kBusyMaxMs after the first. The tray's icons
    // outside the frame are a slow search: looked for at the quiet point,
    // at the ceiling and when asked for outright (requestMeasure); the
    // other readings, the frame's children alone.
    void measureLoop()
    {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            Measurer measurer;
            HWND bar = nullptr;
            int forGeneration = 0;
            bool whole = false; // the tray's icons outside the frame wanted
            bool busy = false; // a burst of triggers, read until settled
            double began = 0; // its first trigger
            double asked = 0; // its last
            double lastRead = 0;
            std::optional<Layout> quiet; // read since the quiet point, for the next to agree with
            double quietAt = 0;
            for (;;) {
                DWORD timeout = INFINITE;
                if (busy) {
                    const double due = quiet ? quietAt + kConfirmMs : lastRead + kReadEveryMs;
                    timeout = static_cast<DWORD>(std::clamp(std::ceil(due - nowMs()), 0.0, kBusyMaxMs));
                }
                const HANDLE handles[] = {quit, measureRequest};
                const DWORD woke = ::WaitForMultipleObjects(2, handles, FALSE, timeout);
                if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED || stopping.load())
                    break;
                if (woke == WAIT_OBJECT_0 + 1) {
                    int nextGeneration = 0;
                    {
                        std::lock_guard lock(measureMutex);
                        bar = measureTray;
                        nextGeneration = measureGeneration;
                        whole = std::exchange(measureWhole, false) || whole;
                        asked = measureAskedAt;
                    }
                    if (!busy || nextGeneration != forGeneration) { // the first: read at once
                        busy = true;
                        began = asked;
                        lastRead = asked - kReadEveryMs;
                    }
                    forGeneration = nextGeneration;
                    quiet.reset(); // the quiet point moved on
                    if (nowMs() < lastRead + kReadEveryMs)
                        continue; // read a moment ago: the next when due
                }
                const double now = nowMs();
                const bool settling = now - asked >= kQuietMs;
                const bool overdue = now - began >= kBusyMaxMs;
                const bool full = whole || overdue || (settling && !quiet);
                Layout layout = full ? measurer.measure(bar, forGeneration) : measurer.track(bar, forGeneration);
                if (full)
                    whole = false;
                lastRead = nowMs();
                layout.sinceBusy = now - began;
                if (!layout.measured) { // asked again by the box's thread (apply), a second apart
                    busy = false;
                    quiet.reset();
                    publish(std::move(layout));
                    continue;
                }
                if (settling) {
                    layout.settled = quiet && sameLayout(*quiet, layout);
                    quiet = layout;
                    quietAt = lastRead;
                }
                {
                    std::lock_guard lock(measureMutex);
                    if (measureAskedAt != asked)
                        layout.settled = false; // asked again as it was read: the taskbar moved something since
                }
                if (overdue)
                    layout.settled = true;
                if (layout.settled) {
                    busy = false;
                    quiet.reset();
                }
                publish(std::move(layout));
            }
        }
        if (SUCCEEDED(com))
            ::CoUninitialize();
    }
};

thread_local SearchBox::Impl* SearchBox::Impl::t_current = nullptr;

SearchBox::SearchBox(Callbacks callbacks, Look look) : m_impl(std::make_shared<Impl>())
{
    m_impl->callbacks = std::move(callbacks);
    m_impl->look = std::move(look);
    m_impl->ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_impl->quit = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_impl->measureRequest = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_impl->boxFrame = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_impl->clockThread = std::thread([impl = m_impl] { impl->clockLoop(); });
    m_impl->measureThread = std::thread([impl = m_impl] { impl->measureLoop(); });
    m_impl->boxThread = std::thread([impl = m_impl] { impl->run(); });
    if (m_impl->ready)
        ::WaitForSingleObject(m_impl->ready, 2000);
}

SearchBox::~SearchBox()
{
    m_impl->stopping = true;
    if (const HWND helper = m_impl->helper.load())
        ::PostMessageW(helper, kQuitMessage, 0, 0);
    if (m_impl->quit)
        ::SetEvent(m_impl->quit);
    {
        std::lock_guard lock(m_impl->clockMutex);
        m_impl->clockQuit = true;
    }
    m_impl->clockWake.notify_all();
    finish(m_impl->boxThread);
    finish(m_impl->measureThread);
    finish(m_impl->clockThread);
}

void SearchBox::setLook(Look look)
{
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->look = std::move(look);
    }
    if (const HWND helper = m_impl->helper.load())
        ::PostMessageW(helper, kStateMessage, 0, 0);
}

void SearchBox::setText(Text text)
{
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->text = std::move(text);
    }
    if (const HWND helper = m_impl->helper.load())
        ::PostMessageW(helper, kStateMessage, 0, 0);
}

void SearchBox::reattach()
{
    if (const HWND helper = m_impl->helper.load())
        ::PostMessageW(helper, kReattachMessage, 0, 0);
}

std::optional<RECT> SearchBox::rect() const
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->shownRect;
}

bool SearchBox::wide() const
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->shownWide;
}

std::optional<RECT> SearchBox::caret() const
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->caretRect;
}

bool SearchBox::underPointer() const
{
    const HWND window = m_impl->box.load();
    POINT pointer {};
    return window && ::GetCursorPos(&pointer) && ::WindowFromPoint(pointer) == window;
}

bool SearchBox::supported()
{
    static const bool windows11 = isWindows11();
    return windows11;
}

} // namespace ws::taskbar
