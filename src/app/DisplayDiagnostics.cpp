#include "DisplayDiagnostics.h"

#include "DiagnosticLog.h"
#include "TextCheck.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFont>
#include <QFontInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <windows.h>
#include <dxgi.h>

using namespace Qt::StringLiterals;

namespace ws::diagnostics {
namespace {

constexpr qint64 kTextCheckEveryMs = 10 * 60 * 1000;
constexpr qsizetype kFontsWatched = 6; // a row's name and folder lines, the panel's, the dialog bar's...
constexpr qint64 kFontChangesApartMs = 2000; // an installer broadcasts once for each font it adds

// The GUI thread's, as all below but the window counters.
QJsonObject g_uiFont;
QJsonArray g_screens; // as last recorded

struct TextWatch {
    QElapsedTimer since; // the last check
    QList<QFont> fonts; // the latest first
    int state = -1; // not checked yet; 0: some text drawn wrong; 1: all right
    QJsonObject last; // {time, ok, fonts}
};

TextWatch& textWatch()
{
    static TextWatch watch;
    return watch;
}

struct SystemEvents {
    int suspends = 0;
    int resumes = 0;
    QDateTime lastResume;
    int fontChanges = 0; // WM_FONTCHANGE: Qt 6.12 does nothing on it
    QDateTime lastFontChange;
    QElapsedTimer fontChangeRecorded;
    int fontDatabaseChanges = 0; // Qt's own: its font caches emptied (an application font added or removed)
    QDateTime lastFontDatabaseChange;
};

SystemEvents& systemEvents()
{
    static SystemEvents events;
    return events;
}

// Counted on each window's render thread, read for the summary.
struct WindowStarts {
    QString name;
    std::shared_ptr<std::atomic<int>> starts;
};
std::mutex g_windowsMutex;
std::vector<WindowStarts> g_windows;

QString utcNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QJsonObject describe(const QFont& font)
{
    const QFontInfo info(font);
    return {{u"family"_s, info.family()}, {u"style"_s, info.styleName()}, {u"weight"_s, info.weight()},
        {u"pixel_size"_s, info.pixelSize()}, {u"exact"_s, info.exactMatch()}};
}

QJsonArray screens()
{
    QJsonArray list;
    const QScreen* primary = QGuiApplication::primaryScreen();
    for (const QScreen* screen : QGuiApplication::screens()) {
        const QSize size = screen->size();
        list.append(QJsonObject {{u"size"_s, u"%1x%2"_s.arg(size.width()).arg(size.height())},
            {u"scale"_s, screen->devicePixelRatio()}, {u"dpi"_s, screen->logicalDotsPerInch()},
            {u"hz"_s, qRound(screen->refreshRate())}, {u"primary"_s, screen == primary}});
    }
    return list;
}

void recordScreens()
{
    QJsonArray now = screens();
    if (now == g_screens)
        return; // one change often comes as several signals
    g_screens = now;
    record(u"screens"_s, {{u"screens"_s, now}});
}

// The display adapters showing the desktop, by name (several on a laptop with two GPUs).
QJsonArray adapters()
{
    QJsonArray names;
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW device {sizeof device};
        if (!::EnumDisplayDevicesW(nullptr, i, &device, 0))
            break;
        const QString name = QString::fromWCharArray(device.DeviceString);
        if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && !names.contains(name))
            names.append(name);
    }
    return names;
}

// The same adapters through DXGI, with the driver's version ("a.b.c.d"), for a
// driver update that broke the drawing; software adapters left out. [{name, driver}]
QJsonArray adapterDrivers()
{
    QJsonArray list;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
        return list;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            LARGE_INTEGER version {};
            QString driver;
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
                driver = u"%1.%2.%3.%4"_s.arg(HIWORD(version.HighPart)).arg(LOWORD(version.HighPart))
                             .arg(HIWORD(version.LowPart)).arg(LOWORD(version.LowPart));
            // One GPU can be listed several times (seen 6× on a laptop): once is enough.
            const QJsonObject entry {{u"name"_s, QString::fromWCharArray(desc.Description)}, {u"driver"_s, driver}};
            if (!list.contains(entry))
                list.append(entry);
        }
        adapter->Release();
    }
    factory->Release();
    return list;
}

QString apiName(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Software:
        return u"software"_s;
    case QSGRendererInterface::Direct3D11:
        return u"d3d11"_s;
    case QSGRendererInterface::Direct3D12:
        return u"d3d12"_s;
    case QSGRendererInterface::OpenGL:
        return u"opengl"_s;
    case QSGRendererInterface::Vulkan:
        return u"vulkan"_s;
    default:
        return QString::number(static_cast<int>(api));
    }
}

// A glyph that is not its character's, in one line: "色 U+8272: glyph 14498,
// Alibaba PuHuiTi 3.0 Bold 14px has 21312".
QString glyphLine(const QJsonObject& m)
{
    if (m.contains(u"note"_s))
        return m[u"note"_s].toString(); // glyphs and characters not one to one
    const qint64 expected = m[u"expected"_s].toInteger();
    return u"%1 %2: glyph %3, %4 %5 %6px has %7"_s.arg(m[u"char"_s].toString(), m[u"code"_s].toString())
        .arg(m[u"glyph"_s].toInteger())
        .arg(m[u"font"_s].toString(), m[u"style"_s].toString())
        .arg(m[u"pixel_size"_s].toInt())
        .arg(expected ? QString::number(expected) : u"none"_s);
}

// `fonts` checked now: the whole result kept (for the summary); recorded the
// first time and when it turns, short enough for one record (8 KB), a
// failure also written to WinShun.log.
QJsonObject runTextCheck(TextWatch& watch, const QList<QFont>& fonts)
{
    constexpr qsizetype kGlyphsRecorded = 4; // per case
    bool ok = true;
    QJsonArray results;
    QJsonArray names;
    QJsonArray wrong; // [{font, case, runs, glyphs}]
    for (const QFont& font : fonts) {
        const QJsonObject result = textcheck::check(font);
        ok = ok && result.value(u"ok"_s).toBool();
        names.append(result.value(u"font"_s));
        results.append(result);
        for (const QJsonValue& c : result[u"cases"_s].toArray()) {
            const QJsonArray mismatches = c[u"mismatches"_s].toArray();
            if (mismatches.isEmpty())
                continue;
            QJsonArray glyphs;
            for (qsizetype i = 0; i < std::min(kGlyphsRecorded, mismatches.size()); ++i)
                glyphs.append(glyphLine(mismatches[i].toObject()));
            wrong.append(QJsonObject {{u"font"_s, result[u"font"_s]}, {u"case"_s, c[u"case"_s]},
                {u"runs"_s, c[u"runs"_s]}, {u"glyphs"_s, glyphs}});
        }
    }
    watch.last = {{u"time"_s, utcNow()}, {u"ok"_s, ok}, {u"fonts"_s, results}};
    const int state = ok ? 1 : 0;
    if (state != watch.state) {
        QJsonObject details {{u"ok"_s, ok}, {u"fonts"_s, names}};
        if (!ok) {
            details[u"wrong"_s] = wrong;
            const QJsonObject first = wrong.first().toObject();
            qWarning("Text check: characters drawn with another font's glyphs (GitHub #70), in %s (%s): %s",
                qUtf8Printable(first[u"font"_s].toString()), qUtf8Printable(first[u"case"_s].toString()),
                qUtf8Printable(first[u"glyphs"_s].toArray().first().toString()));
        }
        record(u"text-check"_s, details);
        watch.state = state;
    }
    return watch.last;
}

void remember(TextWatch& watch, const QFont& font)
{
    const qsizetype at = watch.fonts.indexOf(font);
    if (at == 0)
        return;
    if (at > 0) {
        watch.fonts.move(at, 0);
        return;
    }
    watch.fonts.prepend(font);
    if (watch.fonts.size() > kFontsWatched)
        watch.fonts.removeLast();
}

} // namespace

void setUiFont(const QJsonObject& loaded)
{
    g_uiFont = loaded;
    record(u"ui-font"_s, loaded);
}

void watchDisplay()
{
    auto* app = QGuiApplication::instance();
    QObject::connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, app, [] {
        SystemEvents& events = systemEvents();
        ++events.fontDatabaseChanges;
        events.lastFontDatabaseChange = QDateTime::currentDateTimeUtc();
        record(u"font-database-changed"_s, {{u"times_since_start"_s, events.fontDatabaseChanges}});
    });
    // Before Qt's font caches go: no font kept past them.
    QObject::connect(qGuiApp, &QGuiApplication::aboutToQuit, app, [] { textWatch().fonts.clear(); });

    const auto watch = [app](QScreen* screen) {
        QObject::connect(screen, &QScreen::geometryChanged, app, [] { recordScreens(); });
        QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, app, [] { recordScreens(); });
    };
    for (QScreen* screen : QGuiApplication::screens())
        watch(screen);
    QObject::connect(qGuiApp, &QGuiApplication::screenAdded, app, [watch](QScreen* screen) {
        watch(screen);
        recordScreens();
    });
    // Still listed while the signal is out.
    QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, app,
        [app] { QTimer::singleShot(0, app, [] { recordScreens(); }); });
    recordScreens();
}

void watchHighlightFont(const QFont& font)
{
    TextWatch& watch = textWatch();
    remember(watch, font);
    if (watch.since.isValid() && watch.since.elapsed() < kTextCheckEveryMs)
        return;
    watch.since.start();
    runTextCheck(watch, watch.fonts);
}

void noteSystemMessage(unsigned message, quintptr wParam)
{
    SystemEvents& events = systemEvents();
    if (message == WM_POWERBROADCAST && (wParam == PBT_APMSUSPEND || wParam == PBT_APMRESUMEAUTOMATIC)) {
        const bool resume = wParam == PBT_APMRESUMEAUTOMATIC;
        ++(resume ? events.resumes : events.suspends);
        if (resume)
            events.lastResume = QDateTime::currentDateTimeUtc();
        record(u"power"_s, {{u"state"_s, resume ? u"resume"_s : u"suspend"_s}});
    } else if (message == WM_FONTCHANGE) {
        ++events.fontChanges;
        events.lastFontChange = QDateTime::currentDateTimeUtc();
        if (!events.fontChangeRecorded.isValid() || events.fontChangeRecorded.elapsed() > kFontChangesApartMs) {
            events.fontChangeRecorded.start();
            record(u"fonts-changed"_s, {{u"times_since_start"_s, events.fontChanges}});
        }
    }
}

void watchWindow(QQuickWindow* window, const QString& name)
{
    auto starts = std::make_shared<std::atomic<int>>(0);
    {
        // A window made again under the same name (the capture's overlays) replaces its entry.
        const std::lock_guard lock(g_windowsMutex);
        const auto same = std::ranges::find(g_windows, name, &WindowStarts::name);
        if (same != g_windows.end())
            same->starts = starts;
        else
            g_windows.push_back({name, starts});
    }
    // On the render thread; again after the window let its graphics go.
    QObject::connect(window, &QQuickWindow::sceneGraphInitialized, window, [window, name, starts] {
        if (++*starts > 1)
            return;
        const QSGRendererInterface* renderer = window->rendererInterface();
        record(u"graphics"_s, {{u"window"_s, name},
            {u"api"_s, renderer ? apiName(renderer->graphicsApi()) : u"?"_s}, {u"adapters"_s, adapters()},
            {u"adapter_drivers"_s, adapterDrivers()}});
    }, Qt::DirectConnection);
    QObject::connect(window, &QQuickWindow::sceneGraphError, window,
        [name](QQuickWindow::SceneGraphError error, const QString& message) {
            record(u"graphics-error"_s,
                {{u"window"_s, name}, {u"error"_s, static_cast<int>(error)}, {u"message"_s, message.left(512)}});
        });
}

QJsonObject display(const QString& renderer, bool backdrop)
{
    QJsonObject out {{u"renderer"_s, renderer}, {u"backdrop"_s, backdrop}, {u"ui_font"_s, g_uiFont}};
    const QFont font = QGuiApplication::font();
    QFont bold = font;
    bold.setBold(true);
    out[u"font"_s] = describe(font);
    out[u"font_bold"_s] = describe(bold);

    wchar_t name[LOCALE_NAME_MAX_LENGTH] {};
    if (::GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH))
        out[u"user_locale"_s] = QString::fromWCharArray(name);
    if (::GetSystemDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH))
        out[u"system_locale"_s] = QString::fromWCharArray(name);
    if (::LCIDToLocaleName(MAKELCID(::GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0))
        out[u"ui_language"_s] = QString::fromWCharArray(name);
    out[u"ansi_code_page"_s] = static_cast<int>(::GetACP());
    BOOL smoothing = FALSE;
    UINT smoothingType = 0;
    ::SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smoothing, 0);
    ::SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &smoothingType, 0);
    out[u"font_smoothing"_s] = !smoothing ? u"off"_s
        : smoothingType == FE_FONTSMOOTHINGCLEARTYPE ? u"cleartype"_s : u"standard"_s;
    out[u"screens"_s] = screens();
    out[u"adapters"_s] = adapters();
    out[u"adapter_drivers"_s] = adapterDrivers();

    FILETIME created {}, exited {}, kernel {}, user {};
    if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        FILETIME now {};
        ::GetSystemTimeAsFileTime(&now);
        const auto ticks = [](const FILETIME& t) { return (quint64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        out[u"uptime_minutes"_s] = static_cast<qint64>((ticks(now) - ticks(created)) / 600'000'000ull);
    }
    const SystemEvents& events = systemEvents();
    out[u"suspends"_s] = events.suspends;
    out[u"resumes"_s] = events.resumes;
    if (events.lastResume.isValid())
        out[u"minutes_since_resume"_s] = events.lastResume.secsTo(QDateTime::currentDateTimeUtc()) / 60;
    out[u"font_changes"_s] = events.fontChanges;
    if (events.lastFontChange.isValid())
        out[u"minutes_since_font_change"_s] = events.lastFontChange.secsTo(QDateTime::currentDateTimeUtc()) / 60;
    out[u"font_database_changes"_s] = events.fontDatabaseChanges; // since the UI font was loaded
    if (events.lastFontDatabaseChange.isValid())
        out[u"minutes_since_font_database_change"_s]
            = events.lastFontDatabaseChange.secsTo(QDateTime::currentDateTimeUtc()) / 60;
    {
        QJsonObject starts;
        const std::lock_guard lock(g_windowsMutex);
        for (const WindowStarts& window : g_windows)
            starts[window.name] = window.starts->load();
        out[u"scene_graph_starts"_s] = starts;
    }

    // The last check of the rows' fonts, and one now with the UI font as well.
    TextWatch& watch = textWatch();
    out[u"text_check_last"_s] = watch.last;
    QList<QFont> fonts = watch.fonts;
    if (!fonts.contains(font))
        fonts.append(font);
    out[u"text_check"_s] = runTextCheck(watch, fonts);
    return out;
}

} // namespace ws::diagnostics
