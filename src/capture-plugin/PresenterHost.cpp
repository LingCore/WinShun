#include "PresenterHost.h"

#include "IdleGraphics.h"
#include "WindowEffects.h"

#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QRegion>
#include <QScreen>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>

#include <optional>

#include <windows.h>
#include <dwmapi.h>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

PresenterHost* g_host = nullptr; // the WinEvent hook's way back

void CALLBACK foregroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
{
    if (g_host)
        g_host->raiseAll();
}

// The top-level windows' order changed: the desktop's children reordered.
void CALLBACK orderChanged(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD)
{
    if (g_host && object == OBJID_CLIENT && window == ::GetDesktopWindow())
        g_host->reordered();
}

HWND handleOf(QWindow* window)
{
    return window ? reinterpret_cast<HWND>(window->winId()) : nullptr;
}

// A monitor in physical pixels (the screen's logical size times its scale
// can be a pixel off: 1707 x 1.5 is 2560.5).
std::optional<QRect> monitorRect(QScreen* screen)
{
    if (const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>()) {
        MONITORINFO info {sizeof info};
        if (::GetMonitorInfoW(native->handle(), &info)) {
            const RECT& r = info.rcMonitor;
            return QRect(int(r.left), int(r.top), int(r.right - r.left), int(r.bottom - r.top));
        }
    }
    return std::nullopt;
}

QSize pixelsOf(QScreen* screen)
{
    if (const std::optional<QRect> r = monitorRect(screen))
        return r->size();
    return (QSizeF(screen->geometry().size()) * screen->devicePixelRatio()).toSize();
}

// The top window under the pointer that is not ours: the windows from the
// top down, as the click would find them (WindowFromPoint would stop at
// our toolbar, which is where the pointer is when its button is clicked).
HWND windowBeneathPointer()
{
    POINT point {};
    if (!::GetCursorPos(&point))
        return nullptr;
    const DWORD self = ::GetCurrentProcessId();
    for (HWND window = ::GetTopWindow(nullptr); window; window = ::GetWindow(window, GW_HWNDNEXT)) {
        if (!::IsWindowVisible(window) || ::IsIconic(window))
            continue;
        if (::GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TRANSPARENT)
            continue; // lets clicks through: not what is clicked
        RECT r {};
        if (!::GetWindowRect(window, &r) || !::PtInRect(&r, point))
            continue;
        BOOL cloaked = FALSE;
        ::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
        if (cloaked)
            continue;
        DWORD process = 0;
        ::GetWindowThreadProcessId(window, &process);
        if (process == self)
            continue;
        return ::GetAncestor(window, GA_ROOT);
    }
    return nullptr;
}

constexpr int kToolbarTop = 12; // the toolbar's first place: this far below the monitor's top, logical px
// Raising again when another program's topmost window went above the ink:
// a little later (its own moves come in bursts), and given up for a while
// when a program fights back (it raises itself again each time).
constexpr int kReorderDelayMs = 40;
constexpr int kFightRaises = 8; // this many in kFightWindowMs: a fight
constexpr qint64 kFightWindowMs = 2000;
constexpr qint64 kFightPauseMs = 5000;

} // namespace

PresenterHost::PresenterHost(QQmlEngine& engine, QString stateFile, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_stateFile(std::move(stateFile))
{
    capture::Presenter::Hooks hooks;
    hooks.monitors = [] {
        QList<capture::Presenter::Monitor> list;
        for (QScreen* screen : QGuiApplication::screens())
            list.append({pixelsOf(screen), screen->devicePixelRatio()});
        return list;
    };
    hooks.screenAtPointer = [] {
        const QList<QScreen*> screens = QGuiApplication::screens();
        return int(screens.indexOf(QGuiApplication::screenAt(QCursor::pos())));
    };
    hooks.foreground = [] {
        const HWND window = ::GetForegroundWindow();
        DWORD process = 0;
        ::GetWindowThreadProcessId(window, &process);
        return process == ::GetCurrentProcessId() ? quintptr(0) : quintptr(window);
    };
    hooks.setForeground = [](quintptr window) {
        const HWND hwnd = reinterpret_cast<HWND>(window);
        // Gone, or hidden since (the launcher the presenter was opened from).
        if (!::IsWindow(hwnd) || !::IsWindowVisible(hwnd))
            return false;
        ::SetForegroundWindow(hwnd);
        return true;
    };
    hooks.beneathPointer = [] { return quintptr(windowBeneathPointer()); };
    hooks.load = [this](const QString& key) {
        QSettings s(m_stateFile, QSettings::IniFormat);
        return s.value(u"Presenter/"_s + key);
    };
    hooks.save = [this](const QString& key, const QVariant& value) {
        QSettings s(m_stateFile, QSettings::IniFormat);
        s.setValue(u"Presenter/"_s + key, value);
    };
    m_presenter.setHooks(std::move(hooks));
    m_reorderTimer = new QTimer(this);
    m_reorderTimer->setSingleShot(true);
    m_reorderTimer->setInterval(kReorderDelayMs);
    connect(m_reorderTimer, &QTimer::timeout, this, &PresenterHost::raiseIfCovered);
    connect(&m_presenter, &capture::Presenter::modeChanged, this, &PresenterHost::modeChanged);

    // A monitor added, gone, resized or rescaled: the windows are made again (the ink goes).
    auto* rebuild = new QTimer(this);
    rebuild->setSingleShot(true);
    rebuild->setInterval(200);
    connect(rebuild, &QTimer::timeout, this, &PresenterHost::monitorsChanged);
    const auto follow = [this, rebuild](QScreen* screen) {
        connect(screen, &QScreen::geometryChanged, rebuild, qOverload<>(&QTimer::start));
        connect(screen, &QScreen::logicalDotsPerInchChanged, rebuild, qOverload<>(&QTimer::start));
    };
    for (QScreen* screen : QGuiApplication::screens())
        follow(screen);
    connect(qApp, &QGuiApplication::screenAdded, this, [follow, rebuild](QScreen* screen) {
        follow(screen);
        rebuild->start();
    });
    connect(qApp, &QGuiApplication::screenRemoved, rebuild, qOverload<>(&QTimer::start));
}

PresenterHost::~PresenterHost()
{
    watchForeground(false);
    dropWindows();
}

QString PresenterHost::screenKey(int index) const
{
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (index < 0 || index >= screens.size())
        return {};
    const QRect g = screens[index]->geometry();
    return u"Toolbar/%1_%2_%3_%4x%5"_s.arg(screens[index]->name().remove(u'\\').remove(u'.'))
        .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height());
}

void PresenterHost::makeWindows()
{
    const QList<QScreen*> screens = QGuiApplication::screens();
    const auto& models = m_presenter.screens();
    for (std::size_t i = 0; i < models.size(); ++i) {
        QScreen* screen = int(i) < screens.size() ? screens[int(i)] : QGuiApplication::primaryScreen();
        QQmlComponent component(&m_engine, u"WinShun"_s, u"PresenterOverlay"_s);
        QObject* object = component.createWithInitialProperties({
            {u"presenter"_s, QVariant::fromValue(&m_presenter)},
            {u"screenModel"_s, QVariant::fromValue(models[i].get())},
        });
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << "Presenter: no ink window:" << component.errorString();
            delete object;
            m_inks.emplace_back();
            continue;
        }
        QSurfaceFormat format = window->format();
        format.setAlphaBufferSize(8); // drawn on nothing: what is not ink shows the desktop
        window->setFormat(format);
        window->setScreen(screen);
        window->setGeometry(screen->geometry());
        window->create();
        if (const std::optional<QRect> r = monitorRect(screen)) // exactly the monitor, not a pixel past it
            ::SetWindowPos(handleOf(window), nullptr, r->x(), r->y(), r->width(), r->height(),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        win::disableTransitions(window);
        capture::releaseIdleGraphics(window);
        m_inks.emplace_back(window);
    }
    QQmlComponent component(&m_engine, u"WinShun"_s, u"PresenterToolbar"_s);
    QObject* object = component.createWithInitialProperties({{u"presenter"_s, QVariant::fromValue(&m_presenter)}});
    m_toolbar = qobject_cast<QQuickWindow*>(object);
    if (!m_toolbar) {
        qWarning().noquote() << "Presenter: no toolbar:" << component.errorString();
        delete object;
        return;
    }
    QSurfaceFormat format = m_toolbar->format();
    format.setAlphaBufferSize(8); // the rounded card and its shadow
    m_toolbar->setFormat(format);
    m_toolbar->create();
    win::disableTransitions(m_toolbar);
    capture::releaseIdleGraphics(m_toolbar);
    // Clicks only where the toolbar shows (QML inputRects): its mask follows it.
    const QMetaObject* meta = m_toolbar->metaObject();
    if (const int index = meta->indexOfProperty("inputRects"); index >= 0) {
        connect(m_toolbar, meta->property(index).notifySignal(), this,
            metaObject()->method(metaObject()->indexOfSlot("updateToolbarMask()")));
    }
    updateToolbarMask();
}

void PresenterHost::dropWindows()
{
    for (const QPointer<QQuickWindow>& window : m_inks)
        delete window.data();
    m_inks.clear();
    delete m_toolbar.data();
    m_toolbar = nullptr;
}

void PresenterHost::monitorsChanged()
{
    m_presenter.close();
    dropWindows();
    m_presenter.resetScreens();
}

void PresenterHost::modeChanged()
{
    using capture::presenter::Mode;
    switch (m_presenter.mode()) {
    case Mode::Draw: {
        if (m_inks.size() != m_presenter.screens().size()) {
            dropWindows();
            makeWindows();
        }
        const bool showing = m_toolbar && !m_toolbar->isVisible();
        for (const QPointer<QQuickWindow>& window : m_inks) {
            if (!window)
                continue;
            window->setFlag(Qt::WindowTransparentForInput, false);
            if (!window->isVisible()) {
                // Out of sight until it has drawn its first (empty) frame: no flash of an old one.
                win::setCloaked(window, true);
                connect(window, &QQuickWindow::frameSwapped, this, [window] {
                    if (window)
                        win::setCloaked(window, false);
                }, Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection));
                window->show();
            }
        }
        if (showing) {
            placeToolbar();
            m_toolbar->show();
        }
        // The keys to the ink of the monitor the pointer is on.
        if (capture::PresenterScreen* current = m_presenter.current()) {
            if (std::size_t(current->index()) < m_inks.size()) {
                if (QQuickWindow* window = m_inks[std::size_t(current->index())]) {
                    win::bringToFront(window, win::ShellFlyout::Preserve);
                    window->requestActivate();
                }
            }
        }
        raiseAll();
        watchForeground(true);
        break;
    }
    case Mode::Through:
        for (const QPointer<QQuickWindow>& window : m_inks) {
            if (window)
                window->setFlag(Qt::WindowTransparentForInput, true);
        }
        raiseAll();
        break;
    case Mode::Closed:
        watchForeground(false);
        rememberToolbar();
        for (const QPointer<QQuickWindow>& window : m_inks) {
            if (window)
                window->hide();
        }
        if (m_toolbar)
            m_toolbar->hide();
        break;
    }
}

void PresenterHost::raiseAll()
{
    if (!m_presenter.isOpen())
        return;
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    for (const QPointer<QQuickWindow>& window : m_inks) {
        if (window && window->isVisible())
            ::SetWindowPos(handleOf(window), HWND_TOPMOST, 0, 0, 0, 0, flags);
    }
    if (m_toolbar && m_toolbar->isVisible())
        ::SetWindowPos(handleOf(m_toolbar), HWND_TOPMOST, 0, 0, 0, 0, flags); // above the ink
}

void PresenterHost::reordered()
{
    if (m_presenter.isOpen() && !m_reorderTimer->isActive())
        m_reorderTimer->start();
}

// Another program's topmost window above the ink, where it would hide it.
bool PresenterHost::covered() const
{
    const DWORD self = ::GetCurrentProcessId();
    for (const QPointer<QQuickWindow>& window : m_inks) {
        if (!window || !window->isVisible())
            continue;
        const HWND ink = handleOf(window);
        RECT inkRect {};
        ::GetWindowRect(ink, &inkRect);
        for (HWND above = ::GetWindow(ink, GW_HWNDPREV); above; above = ::GetWindow(above, GW_HWNDPREV)) {
            if (!::IsWindowVisible(above) || !(::GetWindowLongW(above, GWL_EXSTYLE) & WS_EX_TOPMOST))
                continue;
            DWORD process = 0;
            ::GetWindowThreadProcessId(above, &process);
            if (process == self)
                continue;
            BOOL cloaked = FALSE;
            ::DwmGetWindowAttribute(above, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
            RECT r {};
            RECT overlap {};
            if (!cloaked && ::GetWindowRect(above, &r) && ::IntersectRect(&overlap, &r, &inkRect))
                return true;
        }
    }
    return false;
}

void PresenterHost::raiseIfCovered()
{
    const qint64 now = qint64(::GetTickCount64());
    if (!m_presenter.isOpen() || now < m_quietUntil || !covered())
        return;
    std::erase_if(m_raises, [now](qint64 at) { return now - at > kFightWindowMs; });
    if (int(m_raises.size()) >= kFightRaises) {
        // It raises itself each time: let it be for a while (coming to the front still raises ours).
        qWarning() << "Presenter: another window keeps going above the ink; not raising for" << kFightPauseMs << "ms";
        m_quietUntil = now + kFightPauseMs;
        m_raises.clear();
        return;
    }
    m_raises.push_back(now);
    raiseAll();
}

void PresenterHost::watchForeground(bool on)
{
    if (on == (m_hook != nullptr))
        return;
    if (on) {
        g_host = this;
        m_hook = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundChanged, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!m_hook)
            qWarning() << "Presenter: no foreground hook:" << ::GetLastError();
        // Our own raises are another process's events to nobody but us: skipped here.
        m_reorderHook = ::SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr, orderChanged, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!m_reorderHook)
            qWarning() << "Presenter: no reorder hook:" << ::GetLastError();
    } else {
        ::UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_hook));
        if (m_reorderHook)
            ::UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_reorderHook));
        m_hook = nullptr;
        m_reorderHook = nullptr;
        m_reorderTimer->stop();
        m_raises.clear();
        m_quietUntil = 0;
        if (g_host == this)
            g_host = nullptr;
    }
}

void PresenterHost::placeToolbar()
{
    if (!m_toolbar)
        return;
    const QList<QScreen*> screens = QGuiApplication::screens();
    const capture::PresenterScreen* current = m_presenter.current();
    const int index = current ? current->index() : 0;
    QScreen* screen = index < screens.size() ? screens[index] : QGuiApplication::primaryScreen();
    const QRect area = screen->geometry();
    QSettings s(m_stateFile, QSettings::IniFormat);
    const QPoint saved = s.value(screenKey(index)).toPoint();
    QPoint at(area.x() + (area.width() - m_toolbar->width()) / 2, area.y() + kToolbarTop);
    if (!saved.isNull() && area.contains(saved) && area.contains(saved + QPoint(40, 20)))
        at = saved;
    m_toolbar->setScreen(screen);
    m_toolbar->setPosition(at);
}

void PresenterHost::updateToolbarMask()
{
    if (!m_toolbar)
        return;
    QRegion region;
    for (const QVariant& rect : m_toolbar->property("inputRects").toList())
        region += rect.toRectF().toAlignedRect();
    m_toolbar->setMask(region);
}

void PresenterHost::rememberToolbar()
{
    if (!m_toolbar || !m_toolbar->isVisible())
        return;
    const QList<QScreen*> screens = QGuiApplication::screens();
    const int index = int(screens.indexOf(QGuiApplication::screenAt(m_toolbar->position() + QPoint(8, 8))));
    if (index < 0)
        return;
    QSettings s(m_stateFile, QSettings::IniFormat);
    s.setValue(screenKey(index), m_toolbar->position());
}

} // namespace ws
