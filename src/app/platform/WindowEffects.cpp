#include "WindowEffects.h"

#include "Foreground.h"
#include "OwnInput.h"

#include <QDebug>
#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QPointer>
#include <QScopeGuard>
#include <QStyleHints>
#include <QVariantMap>
#include <QWindow>

#include <dwmapi.h>
#include <commctrl.h>
#include <windows.h>
#include <msctf.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::win {

namespace {

// Values from the Windows 11 SDK; spelled out so older SDKs still build.
constexpr DWORD kUseImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kBorderColor = 34; // DWMWA_BORDER_COLOR
constexpr DWORD kSystemBackdropType = 38; // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int kCornerRound = 2; // DWMWCP_ROUND
constexpr int kCornerSquare = 1; // DWMWCP_DONOTROUND
constexpr int kCornerSmall = 3; // DWMWCP_ROUNDSMALL
constexpr wchar_t kCornerStyle[] = L"WinShun.CornerStyle";
bool g_compositedPreview = false;
std::atomic<bool> g_round {true};
std::mutex g_cornerMutex; // preview HWNDs are created on another thread
constexpr COLORREF kColorDefault = 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT
constexpr COLORREF kColorNone = 0xFFFFFFFE; // DWMWA_COLOR_NONE
constexpr int kBackdropMica = 2; // DWMSBT_MAINWINDOW

// Own frames (setOwnFrame).
constexpr DWORD kNcRenderingPolicy = 2; // DWMWA_NCRENDERING_POLICY
constexpr int kNcUseWindowStyle = 0; // DWMNCRP_USEWINDOWSTYLE
constexpr int kNcDisabled = 1; // DWMNCRP_DISABLED: no DWM frame, shadow included
constexpr int kCornerDefault = 0; // DWMWCP_DEFAULT
constexpr int kBackdropNone = 1; // DWMSBT_NONE
// Undocumented: the visual styles' own caption and frame, drawn without
// DWM's (Chromium and Firefox stop them too for frames of their own).
constexpr UINT kNcUahDrawCaption = 0x00AE; // WM_NCUAHDRAWCAPTION
constexpr UINT kNcUahDrawFrame = 0x00AF; // WM_NCUAHDRAWFRAME
constexpr wchar_t kOwnFrameProp[] = L"WinShun.OwnFrame";
constexpr UINT_PTR kOwnFrameSubclass = 0x4F46;
constexpr UINT_PTR kShadowTimer = 1;
constexpr UINT_PTR kPictureTimer = 2; // the shadow's picture let go
// setCornersClickThrough's wish: 0 none, else 1 + its side edge in 64ths of a logical px.
constexpr wchar_t kCornersProp[] = L"WinShun.CornersClickThrough";
// The most a style's shadow gets (logical px, layers): a typo must not make
// a picture of hundreds of MB, drawn again at every resize.
constexpr double kMaxShadowReach = 64;
constexpr std::size_t kMaxShadowLayers = 8;

// One layer of an own frame's shadow, as setOwnFrame gets it.
struct ShadowLayer {
    double alpha = 0; // the colour's own alpha included
    double blur = 0; // logical px
    double dy = 0; // logical px, down
    double red = 0, green = 0, blue = 0; // 0..1
    bool operator==(const ShadowLayer&) const = default;
};

// drawShadow's picture in pieces that are the same at any size of the window
// (one look, one scale): its corners, the one value of each of the top's and
// the bottom's rows, and the row copied down the sides. Laid out again at
// each size (composeShadow), with no pixel worked out anew.
struct ShadowParts {
    std::vector<ShadowLayer> layers; // as drawn: physical px
    double radius = -1; // physical px
    int margin = 0;
    // drawShadow's bands at any size: x0, width - x1, y0, height - y1.
    int left = 0, right = 0, top = 0, bottom = 0;
    std::vector<std::uint32_t> topLeft, topRight, bottomLeft, bottomRight; // rows of left or right px
    std::vector<std::uint32_t> topEdge, bottomEdge; // a value per row
    std::vector<std::uint32_t> leftEdge, rightEdge; // the sides' row
    bool valid() const { return radius >= 0; }
};

// A window with an own frame (setOwnFrame), and the layered window below it
// that draws its shadow. Made the first time it is turned on; kept, on or
// off, until the window is destroyed (its WM_NCDESTROY), and found from its
// window property (other threads too: setRoundedCorners) or the subclass.
struct OwnFrameState {
    HWND window = nullptr;
    QPointer<QWindow> qwindow; // its devicePixelRatio: the layers' logical px
    HWND shadow = nullptr;
    bool on = false;
    double radius = 0; // logical px
    std::vector<ShadowLayer> layers;
    int backdrop = 0; // DWMWA_SYSTEMBACKDROP_TYPE to bring back when off
    // The shadow's picture (physical px) and where the shadow is.
    int drawnWidth = 0;
    int drawnHeight = 0;
    double drawnDpr = 0;
    int drawnMargin = 0; // how far it reaches past the window
    bool stale = true;
    // drawShadow's picture in parts, and the picture they are laid out in:
    // kept while the window is resized (each step only copies), let go a
    // while after the last.
    ShadowParts parts;
    HDC pictureDc = nullptr;
    HBITMAP picture = nullptr;
    HGDIOBJ pictureOld = nullptr;
    std::uint32_t* pictureBits = nullptr;
    int pictureStride = 0; // px
    int pictureRows = 0;
    int cleanRight = 0, cleanBottom = 0; // composeShadow's: the inside that is 0
    // The window region it has from us (setCornersClickThrough), and its size.
    bool hasRegion = false;
    bool settingRegion = false;
    int regionWidth = 0, regionHeight = 0, regionSide = 0;
    double regionRadius = 0; // physical px
    POINT placed {LONG_MIN, LONG_MIN};
    bool shown = false;
    // The window's opacity (QWindow::opacity, the launcher's fade): the
    // shadow is as see-through, or it would stay whole around a window fading
    // out. As the shadow has it now (UpdateLayeredWindow's constant alpha).
    BYTE alpha = 255;
    QPointer<QWindow> watched; // the window whose opacityChanged syncs this
    // Shown again, the shadow waits for the window's first frame (WM_PAINT,
    // or the timer) or DWM's animation back from the taskbar or from
    // maximised (the timer).
    bool holding = false;
    bool holdForPaint = false;
    bool restoring = false;
    int depth = 0; // ownFrameProc calls under way
    bool gone = false; // the window destroyed: deleted by the last of them

    void turnOn();
    void turnOff();
    void sync(const WINDOWPOS* pos = nullptr);
    void hold(UINT ms, bool forPaint);
    void release();
    void hide();
    void place(const RECT& rect);
    bool draw(int width, int height, double dpr, POINT at);
    bool makePicture(int width, int height);
    void freePicture();
    void syncRegion();
    void destroyShadow();
    void forgetShadow(); // none now: made again when it is to show
    BYTE windowAlpha() const
    {
        return qwindow ? BYTE(std::lround(std::clamp(qwindow->opacity(), 0.0, 1.0) * 255)) : BYTE(255);
    }
};

OwnFrameState* ownFrameOf(HWND window)
{
    return window ? static_cast<OwnFrameState*>(::GetPropW(window, kOwnFrameProp)) : nullptr;
}

bool ownFrameOn(HWND window)
{
    const OwnFrameState* state = ownFrameOf(window);
    return state && state->on;
}

LRESULT CALLBACK nativeFrameProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
{
    switch (message) {
    case WM_NCCALCSIZE:
        if (!::IsZoomed(window))
            return 0; // Keep the native frame entirely in the client area.
        break;
    case WM_STYLECHANGING:
        if (static_cast<int>(wParam) == GWL_STYLE) {
            auto* styles = reinterpret_cast<STYLESTRUCT*>(lParam);
            if (!(styles->styleNew & WS_CHILD))
                styles->styleNew |= WS_CAPTION;
        }
        break;
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(window, nativeFrameProc, id);
        break;
    }
    return ::DefSubclassProc(window, message, wParam, lParam);
}

// Windows 10 has no DWM corner preference, and drawn corners conflict with
// DWM's frame there: a window region turns its shadow off, and its border
// shows through the corners' transparent pixels (GitHub #165; QWindowKit and
// Telegram Desktop came to the same). Square there, with DWM's frame and
// shadow, as VS Code and Windows Terminal; the setting still rounds the
// controls inside (Theme.qml).
void applyWindowCorners(HWND window, int style)
{
    if (ownFrameOn(window))
        return; // square for DWM: the window rounds itself (setOwnFrame)
    const int value = g_round.load() && nativeWindowCorners() ? style : kCornerSquare;
    ::DwmSetWindowAttribute(window, kCornerPreference, &value, sizeof value);
}

HWND handleOf(QWindow* window)
{
    return window ? reinterpret_cast<HWND>(window->winId()) : nullptr;
}

using foreground::programOf;

// "Notepad (notepad.exe)", for the log.
QString describe(HWND hwnd)
{
    if (!hwnd)
        return u"no window"_s;
    wchar_t className[256] {};
    ::GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    return QString::fromWCharArray(className) + u" ("_s + programOf(hwnd) + u')';
}

bool activate(HWND hwnd)
{
    ::SetForegroundWindow(hwnd);
    ::BringWindowToTop(hwnd);
    ::SetFocus(hwnd);
    return ::GetForegroundWindow() == hwnd;
}

// Windows' Start menu, search, notification center and quick settings keep
// the foreground while they are open: no SetForegroundWindow gets past them,
// not with the input shared, after an input sent, or after Alt (measured
// 2026-10-09). Esc closes them as it would for the user, and the window that
// was in front before gets the foreground back. Returns the window in front then.
HWND closeShellFlyout(HWND foreground)
{
    if (!isShellFlyout(foreground))
        return foreground;
    INPUT esc[2] {};
    esc[0].type = esc[1].type = INPUT_KEYBOARD;
    esc[0].ki.wVk = esc[1].ki.wVk = VK_ESCAPE;
    esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
    esc[0].ki.dwExtraInfo = esc[1].ki.dwExtraInfo = kOwnInput; // TypeToSearch lets it through to the Start menu
    ::SendInput(2, esc, sizeof esc[0]);
    for (int waited = 0; waited < 500 && ::GetForegroundWindow() == foreground; waited += 10)
        ::Sleep(10); // closing takes some 150 ms
    return ::GetForegroundWindow();
}

} // namespace

bool isShellFlyout(HWND window)
{
    return window && foreground::isShellFlyout(programOf(window));
}

QRect shellFlyoutBounds(HWND window)
{
    if (!isShellFlyout(window))
        return {};
    const auto visibleBounds = [](HWND candidate) -> QRect {
        RECT rect {}, region {};
        if (!::GetWindowRect(candidate, &rect))
            return {};
        // Immersive hosts can cover a monitor while their window region
        // contains just Start and its sidebar. Region coordinates are local
        // to the window; DWM frame/client bounds still describe the full host.
        const int kind = ::GetWindowRgnBox(candidate, &region);
        if (kind == NULLREGION)
            return {};
        if (kind == SIMPLEREGION || kind == COMPLEXREGION)
            return {rect.left + region.left, rect.top + region.top,
                region.right - region.left, region.bottom - region.top};
        MONITORINFO monitor {sizeof monitor};
        if (::GetMonitorInfoW(::MonitorFromWindow(candidate, MONITOR_DEFAULTTONEAREST), &monitor)
            && ::EqualRect(&rect, &monitor.rcWork))
            return {}; // an unbounded transparent companion is not its content
        return {rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
    };
    DWORD host = 0;
    QRect bounds = visibleBounds(window);
    RECT primary {};
    if (!::GetWindowRect(window, &primary))
        return {};
    ::GetWindowThreadProcessId(window, &host);
    // Start from the flyout's own Z-order list: desktop enumeration can omit
    // immersive Shell windows in other display bands.
    for (HWND candidate = ::GetWindow(window, GW_HWNDFIRST); candidate; candidate = ::GetWindow(candidate, GW_HWNDNEXT)) {
        DWORD pid = 0, cloaked = 0;
        ::GetWindowThreadProcessId(candidate, &pid);
        if (pid != host || !::IsWindowVisible(candidate) || ::IsIconic(candidate))
            continue;
        if (SUCCEEDED(::DwmGetWindowAttribute(candidate, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
            continue;
        bounds = bounds.united(visibleBounds(candidate));
    }
    // Search can own the foreground while a Start-hosted sidebar is visible
    // next to it. Its native root may span the whole screen, so using that
    // root's full rectangle would incorrectly leave nowhere for our panel.
    // Hit-test adjacent strips, then use the companion's actual window region.
    const int gap = std::max(1, int(::GetDpiForWindow(window)) / 8);
    for (const bool right : {false, true}) {
        for (int row : {1, 2, 3}) {
            const POINT point {right ? primary.right + gap : primary.left - gap,
                primary.top + (primary.bottom - primary.top) * row / 4};
            const HWND hit = ::GetAncestor(::WindowFromPoint(point), GA_ROOT);
            if (!isShellFlyout(hit))
                continue;
            bounds = bounds.united(visibleBounds(hit));
            break;
        }
    }
    return bounds;
}

bool nativeWindowCorners()
{
    return !g_compositedPreview && QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11;
}

void useCompositedFramesForPreview() { g_compositedPreview = true; }

void styleFramelessWindow(QWindow* window, bool backdrop)
{
    const HWND hwnd = handleOf(window);
    if (!hwnd)
        return;
    // Retain a fixed native frame for DWM's border and shadow, while
    // Qt/WindowFrame still own hit testing and resizing.
    if (::SetWindowSubclass(hwnd, nativeFrameProc, 0, 0)) {
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, ::GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_CAPTION);
        ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }
    setWindowCorners(hwnd); // kept for later, not applied, with an own frame
    if (OwnFrameState* own = ownFrameOf(hwnd); own && own->on) {
        // Its frame stays its own (setOwnFrame); Mica comes when it goes.
        if (backdrop && backdropSupported())
            own->backdrop = kBackdropMica;
        return;
    }
    // Windows 10 draws no border here; the preview of it on Windows 11 neither.
    ::DwmSetWindowAttribute(hwnd, kBorderColor, g_compositedPreview ? &kColorNone : &kColorDefault, sizeof kColorDefault);
    // Extend a single pixel for DWM's shadow when the native frame/corner
    // style supports it, without exposing the system's caption buttons.
    const MARGINS margins {0, 0, 1, 0};
    ::DwmExtendFrameIntoClientArea(hwnd, &margins);
    // The backdrop fills the whole window, frame extended or not; extended,
    // DWM would also draw its caption buttons on it, next to ours.
    if (backdrop && backdropSupported())
        ::DwmSetWindowAttribute(hwnd, kSystemBackdropType, &kBackdropMica, sizeof kBackdropMica);
}

void setDarkFrame(QWindow* window, bool dark)
{
    if (const HWND hwnd = handleOf(window)) {
        const BOOL value = dark;
        ::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &value, sizeof value);
    }
}

bool isDarkFrame(QWindow* window)
{
    BOOL value = FALSE;
    if (const HWND hwnd = handleOf(window))
        ::DwmGetWindowAttribute(hwnd, kUseImmersiveDarkMode, &value, sizeof value);
    return value;
}

void keepDarkFrameOnShow(QWindow* window)
{
    // visibleChanged comes before the native window is shown. Read first: set
    // only when it is wrong. Not created yet, there is nothing to keep (ours
    // are created, and set, before their first show).
    QObject::connect(window, &QWindow::visibleChanged, window, [window](bool visible) {
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        if (visible && window->handle() && isDarkFrame(window) != dark)
            setDarkFrame(window, dark);
    });
}

bool roundedCorners()
{
    return g_round.load();
}

void setWindowCorners(HWND window, bool smallCorners)
{
    if (!window)
        return;
    std::lock_guard lock(g_cornerMutex);
    const int style = smallCorners ? kCornerSmall : kCornerRound;
    ::SetPropW(window, kCornerStyle, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(style)));
    applyWindowCorners(window, style);
}

bool setRoundedCorners(bool on)
{
    std::lock_guard lock(g_cornerMutex);
    if (g_round.exchange(on) == on)
        return false;
    // Includes hidden menus and the preview worker's non-Qt popup. Window
    // properties die with the HWND, so destroyed windows cannot stay registered.
    ::EnumWindows(
        [](HWND window, LPARAM) -> BOOL {
            DWORD process = 0;
            ::GetWindowThreadProcessId(window, &process);
            if (process == ::GetCurrentProcessId()) {
                const auto style = reinterpret_cast<INT_PTR>(::GetPropW(window, kCornerStyle));
                if (style == kCornerRound || style == kCornerSmall) {
                    applyWindowCorners(window, static_cast<int>(style));
                }
            }
            return TRUE;
        },
        0);
    return true;
}

bool backdropSupported()
{
    static const bool supported = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11_22H2;
    return nativeWindowCorners() && supported;
}

bool materialsEnabled()
{
    HIGHCONTRASTW contrast {sizeof contrast};
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON))
        return false;
    DWORD transparency = 1;
    DWORD size = sizeof transparency;
    ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &size);
    return transparency != 0;
}

void setCloaked(QWindow* window, bool cloaked)
{
    if (const HWND hwnd = handleOf(window)) {
        const BOOL value = cloaked;
        ::DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof value);
        // In the same composition as the window (the cloak event comes later).
        updateOwnFrame(hwnd);
    }
}

void disableTransitions(QWindow* window)
{
    if (const HWND hwnd = handleOf(window)) {
        const BOOL value = TRUE;
        ::DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &value, sizeof value);
    }
}

namespace {

// ---- own frames ---------------------------------------------------------------

// Signed distance from a point to a rounded rectangle (centre, half sizes,
// corner radius): negative inside.
double roundedRectDistance(double x, double y, double cx, double cy, double halfWidth, double halfHeight, double radius)
{
    const double qx = std::abs(x - cx) - (halfWidth - radius);
    const double qy = std::abs(y - cy) - (halfHeight - radius);
    const double ox = std::max(qx, 0.0);
    const double oy = std::max(qy, 0.0);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - radius;
}

// The window in the shadow's picture, and the layers in its pixels.
struct ShadowShape {
    double cx = 0, cy = 0, halfWidth = 0, halfHeight = 0, radius = 0;
    std::vector<ShadowLayer> layers; // blur and dy in physical px
};

// One premultiplied BGRA pixel at the pixel centre (x, y). Each layer is the
// window's shape moved down by dy, fading out over blur (smoothstep); they
// are laid nearest first, over one another. Nothing under the window's
// opaque body: the shadow fades in across its antialiased edge.
std::uint32_t shadowPixel(const ShadowShape& shape, double x, double y)
{
    const double inside = roundedRectDistance(x, y, shape.cx, shape.cy, shape.halfWidth, shape.halfHeight, shape.radius);
    if (inside <= -1.0)
        return 0;
    double keep = 1; // what shows through all of them
    double red = 0, green = 0, blue = 0;
    for (const ShadowLayer& layer : shape.layers) {
        const double d = roundedRectDistance(x, y, shape.cx, shape.cy + layer.dy, shape.halfWidth, shape.halfHeight, shape.radius);
        double t = d <= 0 ? 1.0 : std::max(0.0, 1.0 - d / layer.blur);
        t = t * t * (3 - 2 * t);
        const double a = layer.alpha * t;
        red += keep * a * layer.red;
        green += keep * a * layer.green;
        blue += keep * a * layer.blue;
        keep *= 1 - a;
    }
    const double fade = std::min(1.0, inside + 1.0);
    const auto byte = [fade](double v) { return std::uint32_t(std::lround(std::clamp(v * fade, 0.0, 1.0) * 255)); };
    return byte(1 - keep) << 24 | byte(red) << 16 | byte(green) << 8 | byte(blue);
}

// Where a picture's pixels depend on both their row and their column (the
// corners): every layer's sides are straight in the columns x0..x1 and the
// rows y0..y1, 2 px further in than the corners for the antialiased edge.
struct ShadowBands {
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    double up = 0, down = 0; // the layers' offsets, and the window's own (0)
};

ShadowBands shadowBands(int width, int height, const ShadowShape& shape)
{
    ShadowBands bands;
    for (const ShadowLayer& layer : shape.layers) {
        bands.up = std::min(bands.up, layer.dy);
        bands.down = std::max(bands.down, layer.dy);
    }
    const double straight = shape.radius + 2;
    const double left = shape.cx - shape.halfWidth, right = shape.cx + shape.halfWidth;
    const double top = shape.cy - shape.halfHeight, bottom = shape.cy + shape.halfHeight;
    bands.x0 = std::clamp(int(std::ceil(left + straight - 0.5)), 0, width);
    bands.x1 = std::clamp(int(std::floor(right - straight - 0.5)) + 1, bands.x0, width);
    bands.y0 = std::clamp(int(std::ceil(top + bands.down + straight - 0.5)), 0, height);
    bands.y1 = std::clamp(int(std::floor(bottom + bands.up - straight - 0.5)) + 1, bands.y0, height);
    return bands;
}

// The whole picture (width × height, every pixel written; rows `stride`
// pixels apart). Pixel by pixel only where the corners are: in the rows where
// every layer's sides are straight a pixel depends on its column alone (one
// row, copied), and in the columns where the top and the bottom are straight
// on its row alone (one value). The window's inside is 0.
void drawShadow(std::uint32_t* bits, int stride, int width, int height, const ShadowShape& shape)
{
    const ShadowBands bands = shadowBands(width, height, shape);
    const int x0 = bands.x0, x1 = bands.x1, y0 = bands.y0, y1 = bands.y1;
    const auto drawRow = [&](int y, bool sidesOnly) {
        std::uint32_t* row = bits + std::size_t(y) * stride;
        const double cy = y + 0.5;
        for (int x = 0; x < x0; ++x)
            row[x] = shadowPixel(shape, x + 0.5, cy);
        for (int x = x1; x < width; ++x)
            row[x] = shadowPixel(shape, x + 0.5, cy);
        if (x0 < x1)
            std::fill(row + x0, row + x1, sidesOnly ? 0u : shadowPixel(shape, x0 + 0.5, cy));
    };
    for (int y = 0; y < y0; ++y)
        drawRow(y, false);
    for (int y = y1; y < height; ++y)
        drawRow(y, false);
    if (y0 < y1) {
        drawRow(y0, true); // its middle is inside the window
        const std::uint32_t* first = bits + std::size_t(y0) * stride;
        for (int y = y0 + 1; y < y1; ++y)
            std::copy(first, first + width, bits + std::size_t(y) * stride);
    }
}

// Whether drawShadow's picture at this size is made of the parts: each band
// on its own side of every layer's middle (a pixel's distance to the shape
// then depends only on how far it is from that side), and the corners not
// cut down to the window's size.
bool partsFit(const ShadowShape& shape, const ShadowBands& bands, double radius)
{
    return bands.x0 < bands.x1 && bands.y0 < bands.y1 && shape.radius == radius
        && bands.x0 + 0.5 <= shape.cx && bands.x1 + 0.5 >= shape.cx
        && bands.y0 + 0.5 <= shape.cy + bands.up && bands.y1 + 0.5 >= shape.cy + bands.down;
}

ShadowParts shadowParts(int width, int height, const ShadowShape& shape, const ShadowBands& bands, int margin)
{
    ShadowParts parts;
    parts.layers = shape.layers;
    parts.radius = shape.radius;
    parts.margin = margin;
    parts.left = bands.x0;
    parts.right = width - bands.x1;
    parts.top = bands.y0;
    parts.bottom = height - bands.y1;
    const auto pixel = [&shape](int x, int y) { return shadowPixel(shape, x + 0.5, y + 0.5); };
    const auto corner = [&](std::vector<std::uint32_t>& to, int x, int columns, int y, int rows) {
        to.resize(std::size_t(columns) * rows);
        for (int row = 0; row < rows; ++row)
            for (int column = 0; column < columns; ++column)
                to[std::size_t(row) * columns + column] = pixel(x + column, y + row);
    };
    corner(parts.topLeft, 0, parts.left, 0, parts.top);
    corner(parts.topRight, bands.x1, parts.right, 0, parts.top);
    corner(parts.bottomLeft, 0, parts.left, bands.y1, parts.bottom);
    corner(parts.bottomRight, bands.x1, parts.right, bands.y1, parts.bottom);
    corner(parts.topEdge, bands.x0, 1, 0, parts.top);
    corner(parts.bottomEdge, bands.x0, 1, bands.y1, parts.bottom);
    corner(parts.leftEdge, 0, parts.left, bands.y0, 1);
    corner(parts.rightEdge, bands.x1, parts.right, bands.y0, 1);
    return parts;
}

// drawShadow's picture from its parts (bands as the parts have them). The
// window's inside is written only where it may not be 0 yet: all of it
// below row cleanBottom, right of column cleanRight above it (the inside
// the picture had before, left as it was).
void composeShadow(std::uint32_t* bits, int stride, int width, int height, const ShadowParts& parts,
    int cleanRight, int cleanBottom)
{
    const int left = parts.left, right = parts.right;
    const int middleEnd = width - right;
    const auto band = [&](int y, const std::uint32_t* leftPart, std::uint32_t edge, const std::uint32_t* rightPart) {
        std::uint32_t* row = bits + std::size_t(y) * stride;
        std::copy(leftPart, leftPart + left, row);
        std::fill(row + left, row + middleEnd, edge);
        std::copy(rightPart, rightPart + right, row + middleEnd);
    };
    for (int y = 0; y < parts.top; ++y)
        band(y, &parts.topLeft[std::size_t(y) * left], parts.topEdge[y], &parts.topRight[std::size_t(y) * right]);
    const int bottomStart = height - parts.bottom;
    for (int k = 0; k < parts.bottom; ++k)
        band(bottomStart + k, &parts.bottomLeft[std::size_t(k) * left], parts.bottomEdge[k],
            &parts.bottomRight[std::size_t(k) * right]);
    for (int y = parts.top; y < bottomStart; ++y) {
        std::uint32_t* row = bits + std::size_t(y) * stride;
        std::copy(parts.leftEdge.begin(), parts.leftEdge.end(), row);
        const int from = y < cleanBottom ? std::max(left, cleanRight) : left;
        if (from < middleEnd)
            std::fill(row + from, row + middleEnd, 0u);
        std::copy(parts.rightEdge.begin(), parts.rightEdge.end(), row + middleEnd);
    }
}

// The layers from QML ({alpha, blur, dy, color?}); empty or invisible ones
// left out, and any with a number that is not one (NaN, infinite). Blur and
// dy go up to kMaxShadowReach, the layers to kMaxShadowLayers.
std::vector<ShadowLayer> shadowLayers(const QVariantList& list)
{
    std::vector<ShadowLayer> layers;
    bool cut = false;
    for (const QVariant& item : list) {
        const QVariantMap map = item.toMap();
        QColor color(Qt::black);
        if (const QVariant value = map.value(u"color"_s); value.isValid() && !value.isNull()) {
            color = value.metaType() == QMetaType::fromType<QColor>() ? value.value<QColor>() : QColor::fromString(value.toString());
            if (!color.isValid())
                color = Qt::black;
        }
        const double alpha = map.value(u"alpha"_s).toDouble();
        const double blur = map.value(u"blur"_s).toDouble();
        const double dy = map.value(u"dy"_s).toDouble();
        if (!std::isfinite(alpha) || !std::isfinite(blur) || !std::isfinite(dy)) {
            cut = true;
            continue;
        }
        ShadowLayer layer;
        layer.alpha = std::clamp(alpha, 0.0, 1.0) * color.alphaF();
        layer.blur = std::clamp(blur, 0.0, kMaxShadowReach);
        layer.dy = std::clamp(dy, -kMaxShadowReach, kMaxShadowReach);
        layer.red = color.redF();
        layer.green = color.greenF();
        layer.blue = color.blueF();
        cut = cut || layer.blur != blur || layer.dy != dy;
        if (layer.alpha <= 0)
            continue;
        if (layers.size() == kMaxShadowLayers) {
            cut = true;
            break;
        }
        layers.push_back(layer);
    }
    if (cut)
        qWarning("Own frame: shadow layers past %g px or %d layers, or not numbers, were cut",
            kMaxShadowReach, int(kMaxShadowLayers));
    return layers;
}

// The layers in physical px; a blur of less than half a pixel is an
// antialiased edge.
std::vector<ShadowLayer> physicalLayers(const std::vector<ShadowLayer>& layers, double dpr)
{
    std::vector<ShadowLayer> scaled = layers;
    for (ShadowLayer& layer : scaled) {
        layer.blur = std::max(0.5, layer.blur * dpr);
        layer.dy *= dpr;
    }
    return scaled;
}

// How far the shadow reaches past the window, physical px.
int shadowMargin(const std::vector<ShadowLayer>& physical)
{
    double reach = 0;
    for (const ShadowLayer& layer : physical)
        reach = std::max(reach, layer.blur + std::abs(layer.dy));
    return int(std::ceil(reach)) + 2;
}

// A window's rounded shape as a region (window px), and strips `side` px
// wide along its left and right edges. A pixel is in it when the shape grown
// by 1 px (its antialiased edge, whatever draws it) reaches into it: the
// region clips nothing that shows.
HRGN frameRegion(int width, int height, double radius, int side)
{
    const double grown = radius + 1;
    const int cornerRows = std::min(int(std::ceil(radius)), height / 2);
    // The first pixel in, in a corner's row: k rows from the top (or bottom).
    const auto inset = [&](int k) {
        const double dy = std::max(0.0, radius - (k + 1));
        const double reach = std::sqrt(std::max(0.0, grown * grown - dy * dy));
        return std::clamp(int(std::floor(radius - 1 - reach)) + 1, 0, width / 2);
    };
    std::vector<RECT> rects;
    const auto band = [&](int top, int bottom, int in) {
        if (in <= side) {
            rects.push_back({0, top, width, bottom});
            return;
        }
        if (side > 0)
            rects.push_back({0, top, side, bottom});
        if (in < width - in)
            rects.push_back({in, top, width - in, bottom});
        if (side > 0)
            rects.push_back({width - side, top, width, bottom});
    };
    // Rows with the same inset as one band, top to bottom.
    const auto rowInset = [&](int y) {
        return y < cornerRows ? inset(y) : y >= height - cornerRows ? inset(height - 1 - y) : 0;
    };
    for (int y = 0; y < height;) {
        const int in = rowInset(y);
        int end = y + 1;
        while (end < height && rowInset(end) == in)
            ++end;
        band(y, end, in);
        y = end;
    }
    std::vector<std::byte> data(sizeof(RGNDATAHEADER) + rects.size() * sizeof(RECT));
    auto* region = reinterpret_cast<RGNDATA*>(data.data());
    region->rdh = {sizeof(RGNDATAHEADER), RDH_RECTANGLES, DWORD(rects.size()), DWORD(rects.size() * sizeof(RECT)),
        {0, 0, width, height}};
    std::memcpy(region->Buffer, rects.data(), rects.size() * sizeof(RECT));
    return ::ExtCreateRegion(nullptr, DWORD(data.size()), region);
}

bool isCloaked(HWND window)
{
    DWORD cloaked = 0;
    return SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked;
}

// Full screen: all of its monitor.
bool fillsMonitor(HWND window, const RECT& rect)
{
    MONITORINFO monitor {sizeof monitor};
    return ::GetMonitorInfoW(::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)
        && ::EqualRect(&rect, &monitor.rcMonitor);
}

// Windows animates a window back from the taskbar (and to it).
bool restoreAnimates()
{
    ANIMATIONINFO info {sizeof info, 0};
    return ::SystemParametersInfoW(SPI_GETANIMATION, sizeof info, &info, 0) && info.iMinAnimate;
}

HINSTANCE thisModule()
{
    static const int anchor = 0;
    HMODULE module = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&anchor), &module);
    return module;
}

LRESULT CALLBACK shadowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CLOSE:
        // Destroyed with its window, not at a request to close the program's
        // windows (taskkill without /f): DefWindowProc would destroy it.
        return 0;
    case WM_NCDESTROY:
        // By someone else all the same (our own destroyShadow forgets it first).
        if (auto* state = reinterpret_cast<OwnFrameState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            state->forgetShadow();
        }
        break;
    case WM_TIMER:
        if (wParam == kShadowTimer) {
            ::KillTimer(hwnd, kShadowTimer);
            if (auto* state = reinterpret_cast<OwnFrameState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
                state->release();
                state->sync();
            }
            return 0;
        }
        if (wParam == kPictureTimer) {
            if (auto* state = reinterpret_cast<OwnFrameState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
                state->freePicture();
            else
                ::KillTimer(hwnd, kPictureTimer);
            return 0;
        }
        break;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

// Click-through, never active, in no taskbar or Alt+Tab, shown and hidden
// at once (no DWM fade of its own).
HWND createShadow(OwnFrameState* state)
{
    static bool registered = false; // tried again next time if it failed
    if (!registered) {
        WNDCLASSEXW windowClass {sizeof windowClass};
        windowClass.lpfnWndProc = shadowProc;
        windowClass.hInstance = thisModule();
        windowClass.lpszClassName = kFrameShadowClass;
        registered = ::RegisterClassExW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        if (!registered)
            return nullptr;
    }
    // Topmost from the start as its window is: a UIAccess process (the
    // clipboard's display) puts it in its window's band then.
    const DWORD topmost = ::GetWindowLongPtrW(state->window, GWL_EXSTYLE) & WS_EX_TOPMOST;
    const HWND shadow = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | topmost,
        kFrameShadowClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, thisModule(), nullptr);
    if (!shadow)
        return nullptr;
    ::SetWindowLongPtrW(shadow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    const BOOL off = TRUE;
    ::DwmSetWindowAttribute(shadow, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof off);
    return shadow;
}

// Cloaked by Windows (another virtual desktop) or by us: the shadow goes too.
// Told through this thread's message loop.
void CALLBACK cloakChanged(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD)
{
    if (object != OBJID_WINDOW || child != CHILDID_SELF)
        return;
    if (OwnFrameState* state = ownFrameOf(window))
        state->sync();
}

void watchCloaking()
{
    static HWINEVENTHOOK hook = nullptr;
    if (!hook)
        hook = ::SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, cloakChanged,
            ::GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
}

LRESULT CALLBACK ownFrameProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    auto* state = reinterpret_cast<OwnFrameState*>(data);
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(window, ownFrameProc, id);
        ::RemovePropW(window, kOwnFrameProp);
        ::RemovePropW(window, kCornersProp);
        state->destroyShadow();
        state->on = false;
        state->gone = true;
        const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
        if (state->depth == 0)
            delete state;
        return result;
    }
    if (!state->on || state->gone)
        return ::DefSubclassProc(window, message, wParam, lParam);
    ++state->depth;
    const auto leave = qScopeGuard([state] {
        if (--state->depth == 0 && state->gone)
            delete state;
    });
    switch (message) {
    case WM_NCPAINT:
    case kNcUahDrawCaption:
    case kNcUahDrawFrame:
        return 0; // no frame of the system's, not its classic one either (in the see-through corners)
    case WM_NCACTIVATE:
        return ::DefSubclassProc(window, message, wParam, -1); // likewise: no caption repainted
    case WM_SETICON:
    case WM_SETTEXT: {
        // The default handling repaints the caption for these, themed and
        // whatever WM_NCPAINT does: not while the window is marked hidden
        // (no change on the screen; Chromium's redraw lock).
        const LONG_PTR style = ::GetWindowLongPtrW(window, GWL_STYLE);
        if (!(style & WS_VISIBLE))
            break;
        ::SetWindowLongPtrW(window, GWL_STYLE, style & ~WS_VISIBLE);
        const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
        ::SetWindowLongPtrW(window, GWL_STYLE, ::GetWindowLongPtrW(window, GWL_STYLE) | WS_VISIBLE);
        return result;
    }
    case WM_SHOWWINDOW:
        if (!wParam)
            state->hide();
        break;
    case WM_WINDOWPOSCHANGED:
        // Before Qt's handling (a resize renders): with the window as DWM moves it.
        if (!(reinterpret_cast<const WINDOWPOS*>(lParam)->flags & SWP_NOSIZE))
            state->syncRegion();
        state->sync(reinterpret_cast<const WINDOWPOS*>(lParam));
        break;
    case WM_DPICHANGED: {
        const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam); // Qt's new scale, and size
        state->stale = true;
        state->syncRegion();
        state->sync();
        return result;
    }
    case WM_PAINT: {
        // Qt renders the frame a window shows with at its first paint, and
        // returns once it is presented: its shadow with it.
        const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
        if (state->holding && state->holdForPaint && !state->gone) {
            state->release();
            state->sync();
        }
        return result;
    }
    default:
        break;
    }
    return ::DefSubclassProc(window, message, wParam, lParam);
}

void OwnFrameState::turnOn()
{
    // What to bring back: Mica, if the window stood on it.
    int current = 0;
    if (FAILED(::DwmGetWindowAttribute(window, kSystemBackdropType, &current, sizeof current)) || current == kBackdropNone)
        current = 0;
    backdrop = current;
    on = true;
    ::DwmSetWindowAttribute(window, kCornerPreference, &kCornerSquare, sizeof kCornerSquare);
    ::DwmSetWindowAttribute(window, kBorderColor, &kColorNone, sizeof kColorNone);
    // Not extended: an extended frame gets its (square) shadow back.
    const MARGINS none {0, 0, 0, 0};
    ::DwmExtendFrameIntoClientArea(window, &none);
    // DWM's backdrop follows DWM's square shape.
    ::DwmSetWindowAttribute(window, kSystemBackdropType, &kBackdropNone, sizeof kBackdropNone);
    ::DwmSetWindowAttribute(window, kNcRenderingPolicy, &kNcDisabled, sizeof kNcDisabled);
    ::SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    if (!shadow)
        shadow = createShadow(this);
    stale = true;
}

void OwnFrameState::turnOff()
{
    on = false; // first: applyWindowCorners applies again
    syncRegion(); // none
    destroyShadow();
    ::DwmSetWindowAttribute(window, kNcRenderingPolicy, &kNcUseWindowStyle, sizeof kNcUseWindowStyle);
    // As styleFramelessWindow left it, or as Windows made it (OwnFrame in a
    // window with a title bar of the system's own).
    DWORD_PTR unused = 0;
    const bool styled = ::GetWindowSubclass(window, nativeFrameProc, 0, &unused);
    {
        std::lock_guard lock(g_cornerMutex);
        const auto style = reinterpret_cast<INT_PTR>(::GetPropW(window, kCornerStyle));
        if (style == kCornerRound || style == kCornerSmall)
            applyWindowCorners(window, static_cast<int>(style));
        else
            ::DwmSetWindowAttribute(window, kCornerPreference, &kCornerDefault, sizeof kCornerDefault);
    }
    ::DwmSetWindowAttribute(window, kBorderColor, styled && g_compositedPreview ? &kColorNone : &kColorDefault, sizeof kColorDefault);
    const MARGINS margins {0, 0, styled ? 1 : 0, 0};
    ::DwmExtendFrameIntoClientArea(window, &margins);
    ::DwmSetWindowAttribute(window, kSystemBackdropType, &backdrop, sizeof backdrop);
    ::SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    // DWM's frame active or not as the window is (WindowFrame's handling,
    // left out meanwhile, see hasOwnFrame): Mica's tint depends on it.
    ::SendMessageW(window, WM_NCACTIVATE, ::GetForegroundWindow() == window, -1);
}

void OwnFrameState::destroyShadow()
{
    if (shadow) {
        ::KillTimer(shadow, kShadowTimer);
        ::SetWindowLongPtrW(shadow, GWLP_USERDATA, 0);
        ::DestroyWindow(shadow);
    }
    forgetShadow();
}

void OwnFrameState::forgetShadow()
{
    freePicture();
    shadow = nullptr;
    shown = holding = holdForPaint = false;
    placed = {LONG_MIN, LONG_MIN};
    drawnWidth = drawnHeight = 0;
    stale = true;
    alpha = 255;
}

void OwnFrameState::hold(UINT ms, bool forPaint)
{
    // Without the timer nothing might end it: no wait then.
    if (!shadow || !::SetTimer(shadow, kShadowTimer, ms, nullptr))
        return;
    holding = true;
    holdForPaint = forPaint;
}

void OwnFrameState::release()
{
    holding = holdForPaint = false;
    if (shadow)
        ::KillTimer(shadow, kShadowTimer);
}

void OwnFrameState::hide()
{
    release();
    if (shown && shadow)
        ::SetWindowPos(shadow, nullptr, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    shown = false;
}

void OwnFrameState::sync(const WINDOWPOS* pos)
{
    if (!on || gone)
        return;
    const bool iconic = ::IsIconic(window);
    const bool zoomed = ::IsZoomed(window);
    RECT rect {};
    const bool wanted = !layers.empty() && ::IsWindowVisible(window) && !iconic && !zoomed
        && !isCloaked(window) && ::GetWindowRect(window, &rect) && !fillsMonitor(window, rect);
    if (!wanted) {
        hide();
        restoring = restoring || iconic || zoomed;
        return;
    }
    if (!shadow && !(shadow = createShadow(this)))
        return; // not made at turnOn, or destroyed since: tried again at the next change
    if (!shown && !holding) {
        if (pos && (pos->flags & SWP_SHOWWINDOW)) {
            // Shown: with its first frame (WM_PAINT), not around nothing.
            restoring = false;
            hold(100, true);
        } else if (std::exchange(restoring, false) && restoreAnimates()) {
            hold(250, false); // DWM animates it to its place meanwhile
        }
    }
    if (!holding)
        place(rect);
}

void OwnFrameState::place(const RECT& rect)
{
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const double dpr = qwindow ? qwindow->devicePixelRatio() : ::GetDpiForWindow(window) / 96.0;
    if (stale || width != drawnWidth || height != drawnHeight || dpr != drawnDpr) {
        const int margin = shadowMargin(physicalLayers(layers, dpr));
        if (!draw(width, height, dpr, {rect.left - margin, rect.top - margin})) {
            hide();
            return;
        }
        drawnMargin = margin;
        placed = {rect.left - margin, rect.top - margin}; // UpdateLayeredWindow moved it
    }
    const POINT at {rect.left - drawnMargin, rect.top - drawnMargin};
    if (const BYTE now = windowAlpha(); now != alpha) {
        BLENDFUNCTION blend {AC_SRC_OVER, 0, now, AC_SRC_ALPHA};
        if (::UpdateLayeredWindow(shadow, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA))
            alpha = now;
    }
    // Left out of screen pictures as the window is (capture::excludeFromCapture).
    DWORD affinity = WDA_NONE, own = WDA_NONE;
    if (::GetWindowDisplayAffinity(window, &affinity) && ::GetWindowDisplayAffinity(shadow, &own) && affinity != own)
        ::SetWindowDisplayAffinity(shadow, affinity);
    // Topmost as the window is, so that nothing comes between them; right
    // below it, and shown.
    const bool topmost = ::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST;
    if (topmost != bool(::GetWindowLongPtrW(shadow, GWL_EXSTYLE) & WS_EX_TOPMOST))
        ::SetWindowPos(shadow, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    UINT flags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (::GetWindow(window, GW_HWNDNEXT) == shadow)
        flags |= SWP_NOZORDER;
    if (placed.x == at.x && placed.y == at.y)
        flags |= SWP_NOMOVE;
    if (shown && (flags & SWP_NOZORDER) && (flags & SWP_NOMOVE))
        return;
    if (shown && (flags & SWP_NOZORDER)) {
        // Only moved: a tenth of SetWindowPos's time (0.2 ms against 2 ms
        // here), which every move of the window waits for.
        POINT to = at;
        if (::UpdateLayeredWindow(shadow, nullptr, &to, nullptr, nullptr, nullptr, 0, nullptr, 0)) {
            placed = at;
            return;
        }
    }
    if (!shown)
        flags |= SWP_SHOWWINDOW;
    if (::SetWindowPos(shadow, window, at.x, at.y, 0, 0, flags)) { // or again at the next change
        placed = at;
        shown = true;
    }
}

bool OwnFrameState::draw(int width, int height, double dpr, POINT at)
{
    std::vector<ShadowLayer> physical = physicalLayers(layers, dpr);
    const int margin = shadowMargin(physical);
    const int pictureWidth = width + 2 * margin;
    const int pictureHeight = height + 2 * margin;
    if (!makePicture(pictureWidth, pictureHeight))
        return false;
    ShadowShape shape;
    shape.cx = margin + width / 2.0;
    shape.cy = margin + height / 2.0;
    shape.halfWidth = width / 2.0;
    shape.halfHeight = height / 2.0;
    const double fullRadius = radius * dpr;
    shape.radius = std::clamp(fullRadius, 0.0, std::min(shape.halfWidth, shape.halfHeight));
    shape.layers = std::move(physical);
    const ShadowBands bands = shadowBands(pictureWidth, pictureHeight, shape);
    if (parts.valid() && !(parts.layers == shape.layers && parts.radius == fullRadius && parts.margin == margin)) {
        parts = {}; // another look or scale
        cleanRight = cleanBottom = 0;
    }
    if (!parts.valid() && partsFit(shape, bands, fullRadius))
        parts = shadowParts(pictureWidth, pictureHeight, shape, bands, margin);
    if (parts.valid() && partsFit(shape, bands, fullRadius) && bands.x0 == parts.left && bands.y0 == parts.top
        && pictureWidth - bands.x1 == parts.right && pictureHeight - bands.y1 == parts.bottom) {
        composeShadow(pictureBits, pictureStride, pictureWidth, pictureHeight, parts, cleanRight, cleanBottom);
        cleanRight = bands.x1;
        cleanBottom = bands.y1;
    } else {
        drawShadow(pictureBits, pictureStride, pictureWidth, pictureHeight, shape); // too small for its corners
        cleanRight = cleanBottom = 0;
    }
    // Let go once the window is no longer resized.
    ::SetTimer(shadow, kPictureTimer, 1500, nullptr);
    POINT origin {0, 0};
    SIZE size {pictureWidth, pictureHeight};
    const BYTE now = windowAlpha();
    BLENDFUNCTION blend {AC_SRC_OVER, 0, now, AC_SRC_ALPHA};
    POINT to = at;
    const bool done = ::UpdateLayeredWindow(shadow, nullptr, &to, &size, pictureDc, &origin, 0, &blend, ULW_ALPHA);
    if (done) {
        alpha = now;
        drawnWidth = width;
        drawnHeight = height;
        drawnDpr = dpr;
        stale = false;
    }
    return done;
}

// A picture of at least width × height; larger by some once it has to grow
// (a window made larger step by step).
bool OwnFrameState::makePicture(int width, int height)
{
    if (pictureBits && width <= pictureStride && height <= pictureRows)
        return true;
    const bool growing = pictureBits != nullptr;
    freePicture();
    const int slack = growing ? 256 : 0;
    BITMAPINFO info {};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width + slack, -(height + slack), 1, 32, BI_RGB};
    void* bits = nullptr;
    pictureDc = ::CreateCompatibleDC(nullptr);
    picture = pictureDc ? ::CreateDIBSection(pictureDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!picture || !bits) {
        freePicture();
        return false;
    }
    pictureOld = ::SelectObject(pictureDc, picture);
    pictureBits = static_cast<std::uint32_t*>(bits);
    pictureStride = width + slack;
    pictureRows = height + slack;
    cleanRight = cleanBottom = 0;
    return true;
}

void OwnFrameState::freePicture()
{
    if (shadow)
        ::KillTimer(shadow, kPictureTimer);
    if (pictureDc && pictureOld)
        ::SelectObject(pictureDc, pictureOld);
    if (picture)
        ::DeleteObject(picture);
    if (pictureDc)
        ::DeleteDC(pictureDc);
    pictureDc = nullptr;
    picture = nullptr;
    pictureOld = nullptr;
    pictureBits = nullptr;
    pictureStride = pictureRows = 0;
    cleanRight = cleanBottom = 0;
}

void OwnFrameState::syncRegion()
{
    // SetWindowRgn moves the window (WM_WINDOWPOSCHANGED, here again).
    if (settingRegion || ::IsIconic(window))
        return; // iconic: as it was, back with its size
    settingRegion = true;
    const auto done = qScopeGuard([this] { settingRegion = false; });
    const auto wish = reinterpret_cast<INT_PTR>(::GetPropW(window, kCornersProp));
    RECT rect {};
    const bool wanted = on && !gone && wish > 0 && radius > 0 && !::IsZoomed(window) && ::GetWindowRect(window, &rect)
        && !fillsMonitor(window, rect);
    if (!wanted) {
        if (hasRegion && ::SetWindowRgn(window, nullptr, FALSE))
            hasRegion = false;
        return;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const double dpr = qwindow ? qwindow->devicePixelRatio() : ::GetDpiForWindow(window) / 96.0;
    const double shape = std::clamp(radius * dpr, 0.0, std::min(width, height) / 2.0);
    const double sideEdge = (wish - 1) / 64.0;
    // As WindowFrame's hit testing has it (kSideEdge).
    const int side = sideEdge > 0 ? std::max(2, int(std::lround(sideEdge * dpr))) : 0;
    if (hasRegion && width == regionWidth && height == regionHeight && shape == regionRadius && side == regionSide)
        return;
    const HRGN region = frameRegion(width, height, shape, side);
    if (!region)
        return;
    if (!::SetWindowRgn(window, region, FALSE)) { // the system's from here on, once set
        ::DeleteObject(region);
        return;
    }
    hasRegion = true;
    regionWidth = width;
    regionHeight = height;
    regionRadius = shape;
    regionSide = side;
}

} // namespace

bool setOwnFrame(QWindow* window, bool on, qreal radius, const QVariantList& shadow)
{
    if (!window || !window->handle())
        return false; // not made yet: the frame is set once it is (OwnFrame)
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    OwnFrameState* state = ownFrameOf(hwnd);
    // See-through corners need an alpha channel from the GPU renderer: the
    // software one draws such a window as a layered window of its own
    // (UpdateLayeredWindow), with no DWM frame. Qt marks the GPU's layered
    // too (WS_EX_LAYERED, constant alpha 255), which changes nothing here.
    const bool possible = window->format().alphaBufferSize() > 0 && window->surfaceType() != QSurface::RasterSurface;
    if (!on || !possible || !(radius > 0) || !std::isfinite(radius)) {
        if (state && state->on)
            state->turnOff();
        return false;
    }
    std::vector<ShadowLayer> layers = shadowLayers(shadow);
    if (!state) {
        // Found by its property (ownFrameOf), or never: a second one would
        // take the subclass's place and leave this one's shadow behind.
        state = new OwnFrameState;
        state->window = hwnd;
        if (!::SetPropW(hwnd, kOwnFrameProp, state)) {
            delete state;
            return false;
        }
        if (!::SetWindowSubclass(hwnd, ownFrameProc, kOwnFrameSubclass, reinterpret_cast<DWORD_PTR>(state))) {
            ::RemovePropW(hwnd, kOwnFrameProp);
            delete state;
            return false;
        }
        watchCloaking();
    }
    state->qwindow = window;
    if (state->watched != window) {
        // Found again by its property: gone with the window, nothing to sync.
        state->watched = window;
        QObject::connect(window, &QWindow::opacityChanged, window, [hwnd] {
            if (OwnFrameState* own = ownFrameOf(hwnd))
                own->sync();
        });
    }
    if (state->on && state->radius == radius && state->layers == layers)
        return true;
    state->radius = radius;
    state->layers = std::move(layers);
    state->stale = true;
    if (!state->on)
        state->turnOn();
    state->syncRegion();
    state->sync();
    return true;
}

bool hasOwnFrame(HWND window)
{
    return ownFrameOn(window);
}

void updateOwnFrame(HWND window)
{
    if (OwnFrameState* state = ownFrameOf(window))
        state->sync();
}

void setCornersClickThrough(HWND window, bool on, qreal sideEdge)
{
    if (!window)
        return;
    if (on)
        ::SetPropW(window, kCornersProp,
            reinterpret_cast<HANDLE>(INT_PTR(1 + std::lround(std::clamp(sideEdge, qreal(0), qreal(1000)) * 64))));
    else
        ::RemovePropW(window, kCornersProp);
    if (OwnFrameState* state = ownFrameOf(window))
        state->syncRegion();
}

void setOwnFrameOpacity(HWND window, qreal opacity)
{
    OwnFrameState* state = ownFrameOf(window);
    if (!state || !state->on || !state->shadow || !state->shown)
        return;
    const BYTE now = BYTE(std::lround(std::clamp(opacity, qreal(0), qreal(1)) * 255));
    if (now == state->alpha)
        return;
    BLENDFUNCTION blend {AC_SRC_OVER, 0, now, AC_SRC_ALPHA};
    if (::UpdateLayeredWindow(state->shadow, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA))
        state->alpha = now;
}

bool outsideOwnFrame(HWND window, int x, int y)
{
    const OwnFrameState* state = ownFrameOf(window);
    RECT rect {};
    if (!state || !state->on || ::IsZoomed(window) || !::GetWindowRect(window, &rect))
        return false;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const double dpr = state->qwindow ? state->qwindow->devicePixelRatio() : ::GetDpiForWindow(window) / 96.0;
    const double radius = std::clamp(state->radius * dpr, 0.0, std::min(width, height) / 2.0);
    // The pixel's centre, past the corner's centre on both sides.
    const double qx = std::max(radius - (x + 0.5), (x + 0.5) - (width - radius));
    const double qy = std::max(radius - (y + 0.5), (y + 0.5) - (height - radius));
    return qx > 0 && qy > 0 && std::hypot(qx, qy) > radius;
}

Front bringToFront(QWindow* window, ShellFlyout flyout)
{
    const HWND hwnd = handleOf(window);
    if (!hwnd)
        return Front::Refused;
    // STARTF_USESHOWWINDOW/SW_HIDE can hide the first normal window even
    // after QWindow::show() marked it visible. A later show() then does
    // nothing. Honour this explicit request at the native boundary too.
    if (::IsIconic(hwnd))
        ::ShowWindow(hwnd, SW_RESTORE);
    else if (!::IsWindowVisible(hwnd))
        ::ShowWindow(hwnd, SW_SHOW);
    if (::GetForegroundWindow() == hwnd)
        return Front::Already;
    if (flyout == ShellFlyout::Dismiss)
        closeShellFlyout(::GetForegroundWindow());
    // Directly: Windows allows it to the process that got the latest input
    // (a hotkey, a click on its window or its tray icon) or that one let
    // (the UIAccess listener's AllowSetForegroundWindow, a command line's).
    // The window in front learns that it lost the focus on its own thread,
    // in its own time: busy, slow to deactivate or hanging, ours is in front
    // within 15 ms all the same (measured, GitHub #111). Never with the
    // input attached to its thread (AttachThreadInput), as before: that made
    // its deactivation ours to wait for, the 2 s of a busy program, 8.8 s of
    // a hanging one (#111), 6.9 s in #95, the GUI thread frozen meanwhile;
    // and it got the window to the front in no case this does not (#111).
    //
    // Otherwise (the latest input went elsewhere: a double Ctrl seen through
    // Raw Input, keys routed to the clipboard) the process that sent the
    // latest input may set the foreground: one that does nothing, a mouse
    // event without a move, a button or the wheel. PowerToys Run does the
    // same ("Send empty mouse event. This makes this thread the last to send
    // input, and hence allows it to pass foreground permission checks",
    // microsoft/PowerToys#4270). Not a key: a lone Shift, Alt or Ctrl
    // switches an input method or opens a menu bar, and a game sees any key.
    // It reaches Raw Input alone, as no move from no device, marked as ours
    // for Win顺's listeners; no low-level hook, no pointer move or shape
    // (measured 2026-10-27). Over an administrator's window, or the ghost of
    // a hung one, UIPI drops it and Windows refuses: only the UIAccess
    // listener's grant gets past those.
    //
    // Which of the two, asked before trying: AllowSetForegroundWindow for
    // this process succeeds only when it may set the foreground, and gives
    // it nothing it did not have. Trying is not free: a refused
    // SetForegroundWindow still makes the window the active one of its own
    // thread, in the background (WM_ACTIVATE and WM_SETFOCUS inside the
    // refused call, measured 2026-10-28), and Qt's work for that came before
    // the window got to the front: the first double Ctrl after the start,
    // without the UIAccess listener, took 10–26 ms instead of 5–10 (#111);
    // asked first, 7–8. A wrong answer costs nothing more: a wrong "no"
    // sends one empty input more (without the UIAccess listener it says no
    // every time, though Windows lets a direct try through after the first:
    // in front in 6–13 ms all the same, measured), a wrong "yes" ends up
    // sending it after all.
    bool sent = false;
    const auto sendNothing = [&sent] {
        INPUT nothing {};
        nothing.type = INPUT_MOUSE;
        nothing.mi.dwExtraInfo = kOwnInput;
        ::SendInput(1, &nothing, sizeof nothing); // a UIPI drop is not told (SendInput's documentation)
        sent = true;
    };
    if (!::AllowSetForegroundWindow(::GetCurrentProcessId()))
        sendNothing();
    if (activate(hwnd))
        return sent ? Front::Input : Front::Direct;
    if (!sent) {
        sendNothing();
        if (activate(hwnd))
            return Front::Input;
    }
    qWarning().noquote() << "Could not bring the window to the front over" << describe(::GetForegroundWindow());
    return Front::Refused;
}

void setMenuTheme(bool dark)
{
    // uxtheme exports these by ordinal only (stable since Windows 10 1903).
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    static const HMODULE uxtheme = ::LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme)
        return;
    static const auto setMode = reinterpret_cast<SetPreferredAppMode>(::GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    static const auto flush = reinterpret_cast<FlushMenuThemes>(::GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (setMode)
        setMode(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (flush)
        flush();
}

void prepareTextInput()
{
    // Kept active for the life of the thread, and never released: COM is
    // gone by the time statics are destroyed.
    static ITfThreadMgr* manager = nullptr;
    if (manager)
        return;
    ITfThreadMgr* created = nullptr;
    TfClientId client = TF_CLIENTID_NULL;
    if (FAILED(::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&created))))
        return;
    if (FAILED(created->Activate(&client))) {
        created->Release();
        return;
    }
    manager = created;
}

bool isChineseLayout(HKL layout)
{
    // Its low word is the input language.
    return PRIMARYLANGID(static_cast<LANGID>(reinterpret_cast<UINT_PTR>(layout) & 0xFFFF)) == LANG_CHINESE;
}

} // namespace ws::win
