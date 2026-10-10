#include "TaskbarSearchBox.h"

#include <ole2.h> // before uiautomation.h: WIN32_LEAN_AND_MEAN leaves it out of windows.h

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
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
constexpr UINT_PTR kHoldTimer = 4; // a jump into the clear, waited out (see follow)
constexpr UINT_PTR kLiftTimer = 5; // the icon held down: lifted, to be dragged

// Logical pixels, at the taskbar's scale.
constexpr double kMaxWidth = 240;
constexpr double kMinWidth = 140;
constexpr double kGap = 8; // from the icons, the tray, the widgets
constexpr double kPadding = 12; // inside, at either end
constexpr double kIconSize = 16;
constexpr double kIconGap = 10;
constexpr float kFontSize = 14;
constexpr double kMargin = 6; // round the box in its window: for growing when lifted, its shadow, its glow
constexpr double kSnap = 16; // dropped this near either end of a stretch: right against it
constexpr double kFollowStep = 14; // a step this small: the taskbar animating (see follow)

constexpr UINT kHoldMs = 800; // a jump into the clear (see follow)
constexpr UINT kBriefHoldMs = 100; // ... into the box's way
constexpr UINT kLiftMs = 350;
// The taskbar's icons measured on every frame for this long after it last
// said it moved something, and until they have been still for kQuietMs.
constexpr double kTrackMs = 1500;
constexpr double kQuietMs = 300;
constexpr double kTrackMaxMs = 6000;
constexpr double kFullEveryMs = 250; // the tray's icons outside the frame: looked for no more often
constexpr int kAttachTries = 60; // a second apart: Explorer starting
constexpr int kUnmeasuredTries = 20; // likewise: its new taskbar still being put together

constexpr unsigned kBoxFrames = 1;
constexpr unsigned kMeasureFrames = 2;

bool isWindows11()
{
    wchar_t build[16] {};
    DWORD bytes = sizeof build - sizeof(wchar_t);
    return ::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber",
               RRF_RT_REG_SZ, nullptr, build, &bytes)
            == ERROR_SUCCESS
        && std::wcstol(build, nullptr, 10) >= 22000;
}

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
constexpr Curve kPointToPoint {0.55f, 0.55f, 0, 1}; // what is there already, moving
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
    std::optional<RECT> icons; // Start, Windows' search, the apps
    std::vector<RECT> obstacles; // the tray, the widgets
};

bool sameLayout(const Layout& a, const Layout& b)
{
    return a.measured == b.measured && sameRect(a.icons, b.icons) && a.obstacles.size() == b.obstacles.size()
        && std::equal(a.obstacles.begin(), a.obstacles.end(), b.obstacles.begin(),
            [](const RECT& x, const RECT& y) { return ::EqualRect(&x, &y) != FALSE; });
}

// The taskbar's UI Automation tree (Windows 11): a frame with Start, the
// apps and the rest as children, the tray's icons in it or beside it. The
// children are read in one go (a cache request): a few milliseconds, so
// they can be read on every frame while the icons slide.
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
        if (!ready())
            return layout;
        ComPtr<IUIAutomationElement> root;
        if (full || m_frameOf != tray || !m_frame) {
            if (FAILED(m_uia->ElementFromHandle(tray, &root)) || !root)
                return layout;
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
    Spot initialSpot;
    std::optional<RECT> shownRect;
    std::optional<RECT> caretRect;
    std::atomic<HWND> helper {nullptr};
    std::atomic<HWND> box {nullptr};

    // The measuring thread's work (measureMutex).
    std::mutex measureMutex;
    HANDLE measureRequest = nullptr; // auto-reset
    HWND measureTray = nullptr;
    int measureGeneration = 0;
    std::unique_ptr<Layout> measured; // the latest, for the box's thread
    bool measuredPosted = false;

    // Frames (clockMutex): the clock thread sets these events once a frame
    // for those who asked (clockUsers).
    std::mutex clockMutex;
    std::condition_variable clockWake;
    std::atomic<unsigned> clockUsers {0};
    bool clockQuit = false;
    HANDLE boxFrame = nullptr; // auto-reset
    HANDLE measureFrame = nullptr; // auto-reset

    // The box's thread alone from here on.
    HWND tray = nullptr;
    HWINEVENTHOOK hook = nullptr;
    bool destroying = false; // the box goes because we said so
    int generation = 0;
    int attachTriesLeft = 0;
    int unmeasuredTries = 0;
    bool shown = false; // as told (shownChanged)
    UINT dpi = 96;
    Layout taskbarLayout; // the latest measured
    Spot spot;

    // Where the box goes and where it is: its pill (the window has a margin
    // round it), on the screen, physical pixels.
    struct Target {
        double x = 0;
        double w = 0;
        LONG y = 0;
        LONG h = 0;
    };
    // The free stretches beside the icons, the gaps kept.
    struct Room {
        RECT bar {};
        bool haveIcons = false;
        double iconsMiddle = 0;
        double beforeFrom = 0, beforeTo = 0; // left of the icons
        double afterFrom = 0, afterTo = 0; // right of them
        LONG y = 0, h = 0;
        double width(bool before) const { return before ? beforeTo - beforeFrom : afterTo - afterFrom; }
    };
    enum class Fit { Skip, None, Fits }; // not now (the taskbar sliding), nowhere, somewhere
    enum class Move { Still, Glide, Spring, Drag };
    std::optional<Target> target;
    bool placed = false; // its window is up, at pillX...
    bool hiding = false; // ... and fading out
    double pillX = 0;
    double pillW = 0;
    LONG pillY = 0;
    LONG pillH = 0;
    Move move = Move::Still;
    Tween glide; // 0 to 1, from... to...
    double fromX = 0, fromW = 0, toX = 0, toW = 0;
    Spring springX;
    Spring springW;
    std::optional<Target> pending; // a jump, held (see follow)
    bool pendingInTheWay = false; // ... where the box is now would be in the way
    POINT windowPos {}; // in the taskbar
    SIZE windowSize {};
    int marginX = 0;
    int marginY = 0;
    float frac = 0; // the pill's left edge, between pixels
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
    Tween glowT; // 0 to 1 once, as it becomes active
    Tween shownT;
    Tween placeholderT;
    Tween liftT;
    Spring pressS; // 1 held down
    Spring iconS; // the icon's scale
    bool caretOn = true;
    float scroll = 0; // the text, moved left to keep the caret in view
    bool selecting = false; // the button held down in the text
    int selectAnchor = 0;
    int lastSelect = -1;
    bool iconDown = false; // the button held down on the icon: a click, or a drag
    bool iconPressed = false; // ... the press shows on the icon alone while active
    bool lifted = false; // being dragged
    POINT downAt {};
    double grab = 0; // the pointer's x in the pill

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1DCRenderTarget> renderTarget;
    ComPtr<IDWriteTextFormat> format;
    UINT formatDpi = 0;
    ComPtr<ID2D1Bitmap> icon;
    UINT iconDpi = 0;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    SIZE bitmapSize {};
    ComPtr<IDWriteTextLayout> textLayout; // the last one drawn while active: where presses land
    float textLeft = 0; // in the pill

    static thread_local Impl* t_current; // for the location event's callback

    ~Impl() // the last of its threads is done with it
    {
        for (const HANDLE event : {ready, quit, measureRequest, boxFrame, measureFrame}) {
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
            const unsigned users = clockUsers.load();
            if (users & kBoxFrames)
                ::SetEvent(boxFrame);
            if (users & kMeasureFrames)
                ::SetEvent(measureFrame);
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
            spot = initialSpot;
        }
        iconS.snap(1);
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
        return hoverT.running || activeT.running || glowT.running || shownT.running || placeholderT.running
            || liftT.running || pressS.moving || iconS.moving || move == Move::Glide || move == Move::Spring;
    }

    void tick()
    {
        const double now = nowMs();
        const double ms = now - lastTick;
        lastTick = now;
        for (Tween* t : {&hoverT, &activeT, &glowT, &shownT, &placeholderT, &liftT})
            t->step(now);
        pressS.step(ms);
        iconS.step(ms);
        if (move == Move::Glide) {
            glide.step(now);
            pillX = fromX + (toX - fromX) * glide.value;
            pillW = fromW + (toW - fromW) * glide.value;
            if (!glide.running)
                move = Move::Still;
        } else if (move == Move::Spring) {
            springX.step(ms);
            springW.step(ms);
            pillX = springX.value;
            pillW = springW.value;
            if (!springX.moving && !springW.moving)
                move = Move::Still;
        }
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
        if (!bar || !::IsWindowVisible(bar))
            return false;
        // Hidden until measured.
        const HWND window = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_NOPARENTNOTIFY, kBoxClass, drawnLook.name.c_str(),
            WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, bar, nullptr, ::GetModuleHandleW(nullptr), this);
        if (!window)
            return false;
        tray = bar;
        box = window;
        ++generation;
        windowPos = {};
        windowSize = {};
        DWORD process = 0;
        const DWORD thread = ::GetWindowThreadProcessId(tray, &process);
        // Its own thread's location changes: its buttons moving, it moving.
        hook = ::SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, locationChanged,
            process, thread, WINEVENT_OUTOFCONTEXT);
        unmeasuredTries = 0;
        requestMeasure();
        return true;
    }

    void detach()
    {
        ::KillTimer(helper, kMeasureTimer);
        ::KillTimer(helper, kHoldTimer);
        ::KillTimer(helper, kLiftTimer);
        if (hook)
            ::UnhookWinEvent(std::exchange(hook, nullptr));
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
        placed = false;
        hiding = false;
        move = Move::Still;
        target.reset();
        pending.reset();
        iconDown = lifted = selecting = false;
        liftT.snap(0);
        setShown(false);
    }

    void requestMeasure()
    {
        if (!tray)
            return;
        {
            std::lock_guard lock(measureMutex);
            measureTray = tray;
            measureGeneration = generation;
        }
        ::SetEvent(measureRequest);
    }

    static void CALLBACK locationChanged(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD)
    {
        Impl* self = t_current;
        if (!self || window == self->box.load() || object == OBJID_CURSOR || object == OBJID_CARET)
            return;
        self->requestMeasure(); // and every frame after, while they move
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

    void apply(const Layout& next)
    {
        const HWND window = box.load();
        if (next.generation != generation || next.tray != tray || !window)
            return; // from before the box went onto another taskbar
        if (!next.measured) {
            if (++unmeasuredTries <= kUnmeasuredTries)
                ::SetTimer(helper, kMeasureTimer, 1000, nullptr);
            return;
        }
        unmeasuredTries = 0;
        taskbarLayout = next;
        Room room;
        switch (measureRoom(room)) {
        case Fit::Skip:
            return;
        case Fit::None:
            setTarget(std::nullopt);
            return;
        case Fit::Fits:
            break;
        }
        setTarget(targetIn(room, spot));
        // Over the taskbar's own content (its XAML island, a sibling).
        if (placed && ::GetWindow(window, GW_HWNDPREV))
            ::SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    Fit measureRoom(Room& room)
    {
        RECT bar {};
        MONITORINFO monitor {sizeof monitor};
        if (!::GetWindowRect(tray, &bar)
            || !::GetMonitorInfoW(::MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST), &monitor))
            return Fit::Skip;
        const UINT barDpi = ::GetDpiForWindow(tray);
        if (barDpi != dpi) {
            dpi = barDpi;
            windowSize = {}; // drawn again at the new size
        }
        const double s = scale();
        const LONG barWidth = bar.right - bar.left;
        const LONG barHeight = bar.bottom - bar.top;
        // Auto-hidden, or sliding in or out: nothing on it is where it will be.
        RECT visible {};
        ::IntersectRect(&visible, &bar, &monitor.rcMonitor);
        const LONG tolerance = std::lround(2 * s);
        if (visible.right - visible.left < barWidth - tolerance || visible.bottom - visible.top < barHeight - tolerance)
            return Fit::Skip;
        if (barHeight > barWidth) // at a side: Windows 10's
            return Fit::None;
        const double gap = kGap * s;
        double beforeFrom = bar.left;
        const Layout& layout = taskbarLayout;
        double beforeTo = layout.icons ? layout.icons->left : bar.left;
        double afterFrom = layout.icons ? layout.icons->right : bar.left;
        double afterTo = bar.right;
        for (const RECT& o : layout.obstacles) {
            if (o.right <= beforeTo)
                beforeFrom = std::max<double>(beforeFrom, o.right);
            if (o.left >= afterFrom)
                afterTo = std::min<double>(afterTo, o.left);
        }
        room.bar = bar;
        room.haveIcons = layout.icons.has_value();
        room.iconsMiddle = layout.icons ? (layout.icons->left + layout.icons->right) / 2.0 : bar.left;
        room.beforeFrom = beforeFrom + gap;
        room.beforeTo = beforeTo - gap;
        room.afterFrom = afterFrom + gap;
        room.afterTo = afterTo - gap;
        room.h = std::lround(barHeight * 2.0 / 3.0); // as Windows' own box
        room.y = bar.top + (barHeight - room.h) / 2;
        marginX = static_cast<int>(std::lround(kMargin * s));
        marginY = static_cast<int>(std::min<LONG>(std::lround(kMargin * s), (barHeight - room.h) / 2));
        return Fit::Fits;
    }

    // The default side: before the icons when they are centred (by Start).
    bool defaultBefore(const Room& room) const { return room.haveIcons && iconsCentred(); }

    // Where `where` puts the box in `room`: its side, or the other where
    // that one is too narrow.
    std::optional<Target> targetIn(const Room& room, const Spot& where) const
    {
        const double s = scale();
        bool before = where.side == Spot::Side::Auto ? defaultBefore(room) : where.side == Spot::Side::Before;
        const double minWidth = std::lround(kMinWidth * s);
        if (room.width(before) < minWidth)
            before = !before;
        if (room.width(before) < minWidth)
            return std::nullopt;
        const double from = before ? room.beforeFrom : room.afterFrom;
        const double to = before ? room.beforeTo : room.afterTo;
        const double width = std::floor(std::min(to - from, static_cast<double>(std::lround(kMaxWidth * s))));
        const double offset = std::lround(where.offset * s);
        double x = 0;
        if (before)
            x = where.fromIcons ? to - offset - width : from + offset;
        else
            x = where.fromIcons ? from + offset : to - offset - width;
        Target t;
        t.x = std::round(std::clamp(x, from, to - width));
        t.w = width;
        t.y = room.y;
        t.h = room.h;
        return t;
    }

    // Where a box dropped at x, `width` wide, is meant to be.
    Spot spotAt(const Room& room, double x, double width) const
    {
        const double s = scale();
        bool before = room.haveIcons && x + width / 2 < room.iconsMiddle;
        if (room.width(before) < std::lround(kMinWidth * s))
            before = !before;
        const double from = before ? room.beforeFrom : room.afterFrom;
        const double to = before ? room.beforeTo : room.afterTo;
        width = std::min(width, to - from);
        x = std::clamp(x, from, std::max(from, to - width));
        const double fromIcons = before ? to - (x + width) : x - from;
        const double fromFar = before ? x - from : to - (x + width);
        const double snap = kSnap * s;
        Spot where;
        where.side = before ? Spot::Side::Before : Spot::Side::After;
        where.fromIcons = fromIcons <= fromFar;
        const double offset = std::max(0.0, where.fromIcons ? fromIcons : fromFar);
        where.offset = offset <= snap ? 0 : static_cast<int>(std::lround(offset / s));
        if (where.fromIcons && where.offset == 0 && before == defaultBefore(room))
            where.side = Spot::Side::Auto; // the default: keeps to the taskbar's alignment
        return where;
    }

    void setTarget(const std::optional<Target>& next)
    {
        if (!next) {
            cancelHold();
            target.reset();
            hide();
            return;
        }
        target = next;
        pillY = next->y;
        pillH = next->h;
        if (!placed) {
            pillX = next->x;
            pillW = next->w;
            move = Move::Still;
            appear();
        } else {
            if (hiding) { // back before it was gone
                hiding = false;
                shownT.go(1, nowMs(), 250, kFastIn);
                setShown(true);
            }
            if (move != Move::Drag) // dropped onto the latest
                follow(*next);
        }
        publishRect();
    }

    // To where the taskbar now leaves room. It slides its icons over a few
    // hundred milliseconds, each a little after the one before; measured on
    // every frame, a step at a time, the box goes with them. But some of
    // what UI Automation says is ahead of the screen: an icon whose turn to
    // slide has not come is already where it will end up, then back where it
    // was, then slides. When a button comes, that is Start for one reading;
    // when one goes, for about 400 ms (more with more buttons). So a jump is
    // held, and taken (gliding) only if nothing has said otherwise by then:
    // briefly where the box would be in the icons' way, a while where it
    // would be left in the clear.
    void follow(const Target& t)
    {
        if (!animations) {
            jump(t);
            return;
        }
        const double step = kFollowStep * scale();
        const auto nearby = [&](double x, double w) { return std::abs(t.x - x) <= step && std::abs(t.w - w) <= step; };
        if (move == Move::Glide && nearby(toX, toW)) { // on its way there already
            toX = t.x;
            toW = t.w;
            cancelHold();
            return;
        }
        if (move == Move::Spring && nearby(springX.target, springW.target)) {
            springX.target = t.x;
            springW.target = t.w;
            cancelHold();
            return;
        }
        if (nearby(pillX, pillW)) { // a step: the icons sliding
            if (move == Move::Still)
                jump(t);
            else
                glideTo(t, 120, kFastIn); // from where it got to, without a leap
            cancelHold();
            return;
        }
        const bool inTheWay = !clear(pillX, pillW);
        if (!pending || std::abs(pending->x - t.x) > step || std::abs(pending->w - t.w) > step
            || (inTheWay && !pendingInTheWay)) {
            ::SetTimer(helper, kHoldTimer, inTheWay ? kBriefHoldMs : kHoldMs, nullptr);
            pendingInTheWay = inTheWay;
        }
        pending = t;
    }

    // Nothing on the taskbar in the way of the box there (the gaps halved).
    bool clear(double x, double w) const
    {
        RECT bar {};
        if (!::GetWindowRect(tray, &bar) || x < bar.left || x + w > bar.right)
            return false;
        const double gap = kGap * scale() / 2;
        const auto hits = [&](const RECT& r) { return x < r.right + gap && x + w > r.left - gap; };
        if (taskbarLayout.icons && hits(*taskbarLayout.icons))
            return false;
        return std::none_of(taskbarLayout.obstacles.begin(), taskbarLayout.obstacles.end(), hits);
    }

    void cancelHold()
    {
        if (pending) {
            pending.reset();
            ::KillTimer(helper, kHoldTimer);
        }
    }

    void jump(const Target& t)
    {
        cancelHold();
        move = Move::Still;
        pillX = t.x;
        pillW = t.w;
        if (!animating()) {
            place();
            render();
        }
    }

    void glideTo(const Target& t, double ms, Curve curve)
    {
        cancelHold();
        fromX = pillX;
        fromW = pillW;
        toX = t.x;
        toW = t.w;
        glide.snap(0);
        glide.go(1, nowMs(), ms, curve);
        move = Move::Glide;
    }

    void springTo(const Target& t)
    {
        cancelHold();
        if (!animations) {
            jump(t);
            return;
        }
        springX.value = pillX;
        springW.value = pillW;
        springX.rest = springW.rest = 0.3;
        springX.to(t.x, 18, 0.82);
        springW.to(t.w, 18, 0.82);
        move = Move::Spring;
    }

    void appear()
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
        setShown(true);
    }

    // No room for it: fading out.
    void hide()
    {
        if (!placed || hiding)
            return;
        setShown(false);
        if (!animations || move == Move::Drag) {
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
        move = Move::Still;
        iconDown = lifted = selecting = false;
        liftT.snap(0);
    }

    void setShown(bool on)
    {
        if (!on) {
            std::lock_guard lock(mutex);
            shownRect.reset();
            caretRect.reset();
        }
        if (shown == on)
            return;
        shown = on;
        call(callbacks.shownChanged, on);
    }

    void publishRect()
    {
        if (!target || !shown)
            return;
        const RECT r {std::lround(target->x), target->y, std::lround(target->x + target->w), target->y + target->h};
        std::lock_guard lock(mutex);
        shownRect = r;
    }

    // The window round the pill where it is now.
    void place()
    {
        const HWND window = box.load();
        if (!window || !tray || !placed)
            return;
        const double left = std::floor(pillX);
        frac = static_cast<float>(pillX - left);
        POINT pos {static_cast<LONG>(left) - marginX, pillY - marginY};
        ::ScreenToClient(tray, &pos);
        const SIZE size {static_cast<LONG>(std::ceil(frac + pillW)) + 2 * marginX, pillH + 2 * marginY};
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
        {
            std::lock_guard lock(mutex);
            if (look.name != drawnLook.name && box.load())
                ::SetWindowTextW(box.load(), look.name.c_str());
            if (look.placeholder != drawnLook.placeholder && placed && animations) {
                oldPlaceholder = drawnLook.placeholder; // fades out as the new one fades in
                placeholderT.snap(0);
                placeholderT.go(1, now, 200, kFastIn);
            }
            drawnLook = look;
            next = text;
        }
        if (next.active != drawnText.active) {
            scroll = 0;
            if (animations) {
                activeT.go(next.active ? 1 : 0, now, next.active ? 250 : 167, kFastIn);
                if (next.active) {
                    glowT.snap(0);
                    glowT.go(1, now, 600, kLinear);
                    iconS.velocity += 2.4; // a little hop
                    iconS.to(1, 16, 0.42);
                }
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
            pressS.to(0, 22, 0.5); // back with a little bounce
    }

    // ---- the pointer ----

    // The window's own pixels to the pill's.
    float localX(int x) const { return static_cast<float>(x) - static_cast<float>(marginX) - frac; }
    float localY(int y) const { return static_cast<float>(y - marginY); }

    bool onIcon(int x) const
    {
        const double s = scale();
        return localX(x) < static_cast<float>((kPadding + kIconSize + kIconGap / 2) * s);
    }

    // Where in the field's text the box is pressed at (x, y) (its window's pixels).
    std::optional<int> positionAt(int x, int y) const
    {
        if (!drawnText.active)
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
        press.at = at;
        press.word = word;
        call(callbacks.pressed, press);
    }

    // The icon held down long enough, or moved with: dragged from now on.
    void lift()
    {
        if (lifted || !placed)
            return;
        ::KillTimer(helper, kLiftTimer);
        lifted = true;
        move = Move::Drag;
        cancelHold();
        grab = downAt.x - pillX;
        if (animations)
            liftT.go(1, nowMs(), 167, kFastIn);
        else
            liftT.snap(1);
        setPressed(false);
        ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
        render();
    }

    void drag(POINT pointer)
    {
        Room room;
        if (measureRoom(room) != Fit::Fits)
            return;
        const double from = std::min(room.beforeFrom, room.afterFrom);
        const double to = std::max(room.beforeTo, room.afterTo);
        pillX = std::clamp(pointer.x - grab, from, std::max(from, to - pillW));
        place();
        render();
    }

    void drop()
    {
        if (!lifted)
            return;
        lifted = false;
        if (animations)
            liftT.go(0, nowMs(), 167, kFastIn);
        else
            liftT.snap(0);
        move = Move::Still;
        Room room;
        if (measureRoom(room) != Fit::Fits)
            return;
        spot = spotAt(room, pillX, pillW);
        call(callbacks.moved, spot);
        const std::optional<Target> t = targetIn(room, spot);
        if (!t) {
            setTarget(std::nullopt);
            return;
        }
        target = t;
        pillY = t->y;
        pillH = t->h;
        springTo(*t);
        publishRect();
    }

    // ---- drawing ----

    struct Palette {
        // edge: the line round it while typing, the grey of our windows' edge
        // (Theme.windowEdge): it stands for the launcher's field then.
        D2D1_COLOR_F fill, hoverFill, pressedFill, activeFill, border, edge, text, placeholder, selection, caret;
    };

    Palette palette() const
    {
        const D2D1_COLOR_F accent = colorOf(drawnLook.accent);
        if (highContrast) {
            const D2D1_COLOR_F window = colorOf(::GetSysColor(COLOR_WINDOW));
            const D2D1_COLOR_F ink = colorOf(::GetSysColor(COLOR_WINDOWTEXT));
            const D2D1_COLOR_F highlight = colorOf(::GetSysColor(COLOR_HIGHLIGHT));
            return {window, window, window, window, ink, highlight, ink, colorOf(::GetSysColor(COLOR_GRAYTEXT)),
                highlight, ink};
        }
        D2D1_COLOR_F selection = accent;
        if (dark) {
            selection.a = 0.45f;
            return {gray(1, 0.06f), gray(1, 0.11f), gray(1, 0.03f), gray(0.11f, 0.94f), gray(1, 0.09f),
                gray(0x6A / 255.0f, 1), gray(1, 1), gray(1, 0.66f), selection, gray(1, 1)};
        }
        selection.a = 0.32f;
        return {gray(1, 0.6f), gray(1, 0.82f), gray(1, 0.42f), gray(1, 0.98f), gray(0, 0.08f), gray(0xB4 / 255.0f, 1),
            gray(0, 0.9f), gray(0, 0.6f), selection, gray(0, 0.9f)};
    }

    void releaseSurface()
    {
        renderTarget.Reset();
        icon.Reset();
        if (memory) {
            ::SelectObject(memory, oldBitmap);
            ::DeleteDC(std::exchange(memory, nullptr));
        }
        if (bitmap)
            ::DeleteObject(std::exchange(bitmap, nullptr));
        bitmapSize = {};
    }

    // A bitmap as big as the window gets (it changes size while gliding).
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

    // Win顺's icon at its exact size (the .ico has 16 to 48 in steps).
    void ensureIcon()
    {
        if (icon && iconDpi == dpi)
            return;
        icon.Reset();
        iconDpi = dpi;
        const int px = static_cast<int>(std::lround(kIconSize * dpi / 96.0));
        const auto handle = static_cast<HICON>(
            ::LoadImageW(::GetModuleHandleW(nullptr), L"IDI_ICON1", IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
        if (!handle)
            return;
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
                    &icon);
            }
            if (info.hbmColor)
                ::DeleteObject(info.hbmColor);
            if (info.hbmMask)
                ::DeleteObject(info.hbmMask);
        }
        ::DestroyIcon(handle);
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
        const bool active = drawnText.active;
        const float activeAmount = activeT.value;
        const float idle = 1 - activeAmount;
        const float lift = liftT.value;
        const float press = static_cast<float>(std::clamp(pressS.value, -0.5, 1.0));
        const float pill = iconPressed ? 0 : press * idle; // the box itself gives a little when pressed

        // Pressed, lifted, appearing: about its middle.
        const float grow = (1 - 0.035f * pill) * (1 + 0.04f * lift) * (0.9f + 0.1f * shownT.value);
        const D2D1::Matrix3x2F base = D2D1::Matrix3x2F::Scale(grow, grow, D2D1::Point2F(w / 2, h / 2))
            * D2D1::Matrix3x2F::Translation(static_cast<float>(marginX) + frac, static_cast<float>(marginY));
        const bool still = grow == 1 && frac == 0;
        const D2D1_DRAW_TEXT_OPTIONS textOptions = still ? D2D1_DRAW_TEXT_OPTIONS_NONE : D2D1_DRAW_TEXT_OPTIONS_NO_SNAP;

        renderTarget->BeginDraw();
        renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
        renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
        renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // on alpha: no ClearType
        renderTarget->SetTransform(base);
        ComPtr<ID2D1SolidColorBrush> brush;
        renderTarget->CreateSolidColorBrush(p.fill, &brush);
        const float radius = h / 2;
        const auto rounded = [&](float out, float dy = 0) {
            return D2D1_ROUNDED_RECT {
                D2D1::RectF(-out, -out + dy, w + out, h + out + dy), std::max(0.0f, radius + out), std::max(0.0f, radius + out)};
        };

        if (lift > 0) { // off the taskbar: a soft shadow under it
            for (int i = 4; i >= 1; --i) {
                brush->SetColor(D2D1::ColorF(0, 0, 0, (dark ? 0.12f : 0.06f) * lift));
                renderTarget->FillRoundedRectangle(rounded(i * 0.8f * s, 1.5f * s * lift), brush.Get());
            }
        }
        if (glowT.running && !highContrast) { // a thin ring of the edge's grey, spreading as it fades
            const float t = glowT.value;
            const float strength = t < 0.15f ? t / 0.15f : 1 - kFastIn((t - 0.15f) / 0.85f);
            brush->SetColor(faded(p.edge, 0.6f * strength * activeAmount));
            renderTarget->DrawRoundedRectangle(rounded(s * (0.5f + 3.5f * kFastIn(t))), brush.Get(), s);
        }

        // The pill, with a line round it, 1 logical px as round our windows;
        // with high contrast the highlight's, thicker, while typing in it.
        const float stroke = highContrast ? s * (1 + activeAmount) : s;
        D2D1_COLOR_F fill = mix(p.fill, p.hoverFill, std::max(hoverT.value, lift));
        fill = mix(fill, p.pressedFill, std::clamp(pill, 0.0f, 1.0f));
        brush->SetColor(mix(fill, p.activeFill, activeAmount));
        renderTarget->FillRoundedRectangle(rounded(-stroke / 2), brush.Get());
        brush->SetColor(mix(p.border, p.edge, activeAmount));
        renderTarget->DrawRoundedRectangle(rounded(-stroke / 2), brush.Get(), stroke);

        const float iconSize = std::round(static_cast<float>(kIconSize) * s);
        const float iconX = std::round(static_cast<float>(kPadding) * s);
        if (icon) {
            const float y = std::round((h - iconSize) / 2);
            const float k = static_cast<float>(iconS.value) * (1 - 0.14f * std::max(0.0f, press));
            renderTarget->SetTransform(
                D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(iconX + iconSize / 2, y + iconSize / 2)) * base);
            renderTarget->DrawBitmap(icon.Get(), D2D1::RectF(iconX, y, iconX + iconSize, y + iconSize), 1.0f,
                still && k == 1 ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            renderTarget->SetTransform(base);
        }
        textLeft = iconX + iconSize + std::round(static_cast<float>(kIconGap) * s);
        const float textRight = w - std::round(static_cast<float>(kPadding) * s);
        const float room = std::max(1.0f, textRight - textLeft);
        renderTarget->PushAxisAlignedClip(D2D1::RectF(textLeft, 0, textRight, h), D2D1_ANTIALIAS_MODE_ALIASED);

        std::optional<D2D1_RECT_F> caret; // in the pill
        const float caretWidth = std::max(1.0f, std::round(s));
        const std::wstring& typed = drawnText.text;
        const std::wstring& composition = drawnText.composition;
        if (format && (!active || (typed.empty() && composition.empty()))) {
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
            const ComPtr<IDWriteTextLayout> layout = placeholder(drawnLook.placeholder, in, shift * (1 - in));
            if (active && layout) { // typing starts here, over the placeholder
                float x = 0;
                float y = 0;
                DWRITE_HIT_TEST_METRICS line {};
                layout->HitTestTextPosition(0, FALSE, &x, &y, &line);
                caret = D2D1::RectF(textLeft, std::round(line.top), textLeft + caretWidth, std::round(line.top + line.height));
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
        if (caret && active && caretOn && typed.empty() && composition.empty()) {
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
                self->requestMeasure();
                break;
            case kAttachTimer:
                if (self->attach() || --self->attachTriesLeft <= 0)
                    ::KillTimer(window, kAttachTimer);
                break;
            case kCaretTimer:
                self->caretOn = !self->caretOn;
                self->render();
                break;
            case kHoldTimer:
                ::KillTimer(window, kHoldTimer);
                if (const std::optional<Target> pending = std::exchange(self->pending, std::nullopt);
                    pending && self->move != Move::Drag) {
                    if (self->pendingInTheWay)
                        self->glideTo(*pending, 167, kFastIn);
                    else
                        self->glideTo(*pending, 333, kPointToPoint);
                }
                break;
            case kLiftTimer:
                ::KillTimer(window, kLiftTimer);
                if (self->iconDown)
                    self->lift();
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
            const wchar_t* shape = self->lifted ? IDC_SIZEALL
                : self->drawnText.active && !self->onIcon(pointer.x) ? IDC_IBEAM
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
            if (self->iconDown && (wParam & MK_LBUTTON)) {
                POINT pointer {x, y};
                ::ClientToScreen(window, &pointer);
                const int threshold = ::GetSystemMetricsForDpi(SM_CXDRAG, self->dpi);
                if (!self->lifted
                    && (std::abs(pointer.x - self->downAt.x) >= threshold || std::abs(pointer.y - self->downAt.y) >= threshold))
                    self->lift();
                if (self->lifted)
                    self->drag(pointer);
            } else if (self->selecting && (wParam & MK_LBUTTON)) {
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
            if (self->onIcon(x)) { // a click on release, or a drag
                self->iconDown = true;
                self->iconPressed = self->drawnText.active;
                self->downAt = {x, y};
                ::ClientToScreen(window, &self->downAt);
                ::SetTimer(self->helper, kLiftTimer, kLiftMs, nullptr);
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
            const bool click = self->iconDown && !self->lifted;
            if (::GetCapture() == window)
                ::ReleaseCapture(); // ends the drag (WM_CAPTURECHANGED)
            if (click)
                self->reportPress(std::nullopt, false);
            return 0;
        }
        case WM_CAPTURECHANGED:
            ::KillTimer(self->helper, kLiftTimer);
            self->drop();
            self->iconDown = false;
            self->selecting = false;
            self->setPressed(false);
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            self->requestMeasure();
            return 0;
        case WM_DESTROY:
            self->box = nullptr;
            self->textLayout.Reset();
            if (!self->destroying) { // with its taskbar: Explorer ended
                if (self->hook)
                    ::UnhookWinEvent(std::exchange(self->hook, nullptr));
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

    // Asked (the taskbar moved something): all of it, then the frame's
    // children on every frame until they have stopped moving.
    void measureLoop()
    {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            Measurer measurer;
            bool following = false;
            double since = 0;
            double until = 0;
            double changed = 0;
            double lastFull = -kFullEveryMs;
            Layout last;
            for (;;) {
                const HANDLE handles[] = {quit, measureRequest, measureFrame};
                const DWORD woke = ::WaitForMultipleObjects(following ? 3 : 2, handles, FALSE, following ? 200 : INFINITE);
                if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED || stopping.load())
                    break;
                HWND bar = nullptr;
                int forGeneration = 0;
                {
                    std::lock_guard lock(measureMutex);
                    bar = measureTray;
                    forGeneration = measureGeneration;
                }
                const double now = nowMs();
                const bool asked = woke == WAIT_OBJECT_0 + 1;
                if (asked) {
                    if (!following)
                        since = now;
                    until = now + kTrackMs;
                    following = true;
                    wantFrames(kMeasureFrames, true);
                }
                const bool full = asked && now - lastFull >= kFullEveryMs;
                if (full)
                    lastFull = now;
                Layout layout = full ? measurer.measure(bar, forGeneration) : measurer.track(bar, forGeneration);
                if (!sameLayout(layout, last) || layout.tray != last.tray)
                    changed = now;
                last = layout;
                publish(std::move(layout));
                if (following && ((now > until && now - changed > kQuietMs) || now - since > kTrackMaxMs)) {
                    following = false;
                    wantFrames(kMeasureFrames, false);
                }
            }
            wantFrames(kMeasureFrames, false);
        }
        if (SUCCEEDED(com))
            ::CoUninitialize();
    }
};

thread_local SearchBox::Impl* SearchBox::Impl::t_current = nullptr;

SearchBox::SearchBox(Callbacks callbacks, Look look, Spot spot) : m_impl(std::make_shared<Impl>())
{
    m_impl->callbacks = std::move(callbacks);
    m_impl->look = std::move(look);
    m_impl->initialSpot = spot;
    m_impl->ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_impl->quit = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_impl->measureRequest = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_impl->boxFrame = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_impl->measureFrame = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
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
