#include "CaptureEditor.h"

#include "AnnotationFormat.h"
#include "CaptureSession.h"
#include "Geometry.h"

#include <QCursor>
#include <QDebug>
#include <QEvent>
#include <QGuiApplication>
#include <QPainter>
#include <QQuickWindow>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws::capture {

namespace {

using Action = CaptureSession::Action;

// Logical px one wheel notch scrolls.
constexpr int kScrollNotch = 64;
// Logical px an arrow key scrolls.
constexpr int kScrollKey = 48;
constexpr int kNotch = 120;

// A picture on white: the base is opaque, and what shows through a
// transparent picture is the page it is pasted on, not black.
QImage flatten(const QImage& picture)
{
    if (!picture.hasAlphaChannel())
        return picture.format() == QImage::Format_RGB32 ? picture : picture.convertToFormat(QImage::Format_RGB32);
    QImage out(picture.size(), QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter painter(&out);
    painter.drawImage(0, 0, picture);
    return out;
}

// The native (physical) rectangle of a screen's area, whose size Qt keeps in logical px.
QRect physical(const QRect& logicalArea, qreal dpr)
{
    return QRect(logicalArea.topLeft(), QSize(qRound(logicalArea.width() * dpr), qRound(logicalArea.height() * dpr)));
}

} // namespace

CaptureEditor::CaptureEditor(QObject* parent)
    : QObject(parent)
    , m_model(this)
{
    m_clock.start();
    m_noticeTimer.setSingleShot(true);
    m_noticeTimer.setInterval(kNoticeMs);
    connect(&m_noticeTimer, &QTimer::timeout, this, [this] {
        m_notice.clear();
        emit noticeChanged();
    });
    m_hideTimeout.setSingleShot(true);
    m_hideTimeout.setInterval(200ms); // should no frame come
    connect(&m_hideTimeout, &QTimer::timeout, this, &CaptureEditor::finishHide);
    m_settle.setSingleShot(true);
    m_settle.setInterval(80ms);
    connect(&m_settle, &QTimer::timeout, this, &CaptureEditor::settle);
    m_motionEnd.setSingleShot(true);
    m_motionEnd.setInterval(400ms);
    connect(&m_motionEnd, &QTimer::timeout, this, &CaptureEditor::flushMotion);
    connect(&m_model, &AnnotationModel::notice, this, &CaptureEditor::showNotice);
    connect(&m_model, &AnnotationModel::cropChanged, this, &CaptureEditor::sizeTextChanged);
    connect(&m_model, &AnnotationModel::gestureChanged, this, [this] {
        // A gesture ended some other way (undo, Esc): no press is held.
        if (!m_model.gestureActive())
            m_pressed = false;
    });
    setMetrics(m_windowLogical, m_dpr);
}

CaptureEditor::~CaptureEditor() = default;

void CaptureEditor::setHooks(Hooks hooks)
{
    m_hooks = std::move(hooks);
    m_model.setSmartHooks(m_hooks.smart);
}

void CaptureEditor::attach(QQuickWindow* window)
{
    m_window = window;
    if (!window)
        return;
    window->installEventFilter(this);
    connect(window, &QQuickWindow::frameSwapped, this, &CaptureEditor::frameDrawn);
    // Where a slow frame spends its time (logged by frameDrawn): the stages' times, from whichever thread.
    m_stageClock.start();
    connect(window, &QQuickWindow::beforeSynchronizing, this, [this] { m_stageBeforeSync = m_stageClock.nsecsElapsed(); }, Qt::DirectConnection);
    connect(window, &QQuickWindow::afterSynchronizing, this, [this] { m_stageAfterSync = m_stageClock.nsecsElapsed(); }, Qt::DirectConnection);
    connect(window, &QQuickWindow::beforeRendering, this, [this] { m_stageBeforeRender = m_stageClock.nsecsElapsed(); }, Qt::DirectConnection);
    connect(window, &QQuickWindow::afterRendering, this, [this] { m_stageAfterRender = m_stageClock.nsecsElapsed(); }, Qt::DirectConnection);
    connect(window, &QQuickWindow::frameSwapped, this, [this] {
        m_stagePrevSwapped = m_stageSwapped.load();
        m_stageSwapped = m_stageClock.nsecsElapsed();
    }, Qt::DirectConnection);
    connect(window, &QWindow::widthChanged, this, &CaptureEditor::updateScale);
    connect(window, &QWindow::heightChanged, this, &CaptureEditor::updateScale);
    connect(window, &QWindow::screenChanged, this, &CaptureEditor::updateScale);
    connect(window, &QQuickWindow::devicePixelRatioChanged, this, &CaptureEditor::updateScale);
    // Hidden outside the controller: the picture and the document go as for Esc.
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible && m_open)
            close();
    });
    // Not the window's own focus going elsewhere with the space bar down.
    connect(window, &QWindow::activeChanged, this, [this, window] {
        if (!window->isActive())
            spaceHeld(false);
    });
    updateScale();
}

bool CaptureEditor::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window && event->type() == QEvent::Close) {
        // Reused with its native frame: Alt+F4 must empty and hide it like
        // the title bar, keeping the HWND that WindowFrame is attached to.
        event->ignore();
        close();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

// ---- measures ------------------------------------------------------------------------

void CaptureEditor::updateScale()
{
    if (!m_window)
        return;
    setMetrics(m_window->size(), m_window->devicePixelRatio());
    if (m_open && (m_window->size() != m_loggedWindow || m_dpr != m_loggedDpr))
        logView("window changed");
}

void CaptureEditor::logView(const char* reason) const
{
    m_loggedWindow = m_windowLogical;
    m_loggedDpr = m_dpr;
    const QRect client = m_window && m_hooks.window.nativeRect ? m_hooks.window.nativeRect(m_window) : QRect();
    const QRect canvas = canvasOnScreen();
    const QRect crop = m_model.cropRect();
    qInfo().noquote() << u"Editor: %1; zoom %2, dpr %3, window %4x%5 logical, client at %6,%7 %8x%9, viewport %10,%11 %12x%13 device px, "
                         u"top left of the picture in the window %14,%15, picture %16x%17, crop %18,%19 %20x%21, export on the screen at %22,%23 %24x%25"_s
                             .arg(QString::fromLatin1(reason))
                             .arg(m_zoom)
                             .arg(m_dpr)
                             .arg(m_windowLogical.width())
                             .arg(m_windowLogical.height())
                             .arg(client.x())
                             .arg(client.y())
                             .arg(client.width())
                             .arg(client.height())
                             .arg(m_viewDev.x())
                             .arg(m_viewDev.y())
                             .arg(m_viewDev.width())
                             .arg(m_viewDev.height())
                             .arg(imageInWindow().x())
                             .arg(imageInWindow().y())
                             .arg(m_model.document().canvas.width())
                             .arg(m_model.document().canvas.height())
                             .arg(crop.x())
                             .arg(crop.y())
                             .arg(crop.width())
                             .arg(crop.height())
                             .arg(canvas.x())
                             .arg(canvas.y())
                             .arg(canvas.width())
                             .arg(canvas.height());
}

void CaptureEditor::viewMoved()
{
    m_motionUntil = m_clock.elapsed() + 250;
    m_motionEnd.start();
}

void CaptureEditor::frameDrawn()
{
    if (m_wheelAt != 0) {
        m_latency.push_back(m_stageClock.nsecsElapsed() - m_wheelAt);
        m_wheelAt = 0;
    }
    if (m_clock.elapsed() < m_motionUntil) {
        if (m_frameOpen) {
            m_frames.push_back(m_frameClock.nsecsElapsed());
            if (m_frames.back() > 25'000'000) {
                const double ms = 1e-6;
                qInfo().noquote() << u"Editor: slow frame %1 ms: %2 ms from the last swap to the sync, sync %3 ms, to the render %4 ms, render %5 ms, to the swap %6 ms"_s
                                         .arg(m_frames.back() * ms, 0, 'f', 1)
                                         .arg((m_stageBeforeSync - m_stagePrevSwapped) * ms, 0, 'f', 1)
                                         .arg((m_stageAfterSync - m_stageBeforeSync) * ms, 0, 'f', 1)
                                         .arg((m_stageBeforeRender - m_stageAfterSync) * ms, 0, 'f', 1)
                                         .arg((m_stageAfterRender - m_stageBeforeRender) * ms, 0, 'f', 1)
                                         .arg((m_stageSwapped - m_stageAfterRender) * ms, 0, 'f', 1);
            }
        }
        m_frameClock.restart();
        m_frameOpen = true;
    } else {
        m_frameOpen = false;
    }
}

void CaptureEditor::flushMotion()
{
    if (m_frames.size() >= 8) {
        std::vector<qint64> frames = std::move(m_frames);
        std::ranges::sort(frames);
        const auto at = [&frames](double q) { return frames[static_cast<std::size_t>(q * double(frames.size() - 1))] / 1e6; };
        qInfo().noquote() << u"Editor: view moved over %1 frames, between them p50 %2 ms, p95 %3 ms, max %4 ms (zoom %5)"_s
                                 .arg(frames.size())
                                 .arg(at(0.5), 0, 'f', 1)
                                 .arg(at(0.95), 0, 'f', 1)
                                 .arg(at(1.0), 0, 'f', 1)
                                 .arg(m_zoom);
    }
    if (m_handled.size() >= 4) {
        std::vector<qint64> handled = std::move(m_handled);
        std::ranges::sort(handled);
        const auto at = [&handled](double q) { return handled[static_cast<std::size_t>(q * double(handled.size() - 1))] / 1e6; };
        qInfo().noquote() << u"Editor: %1 wheel notches handled in p50 %2 ms, p95 %3 ms, max %4 ms (the view's bindings included)"_s
                                 .arg(handled.size())
                                 .arg(at(0.5), 0, 'f', 1)
                                 .arg(at(0.95), 0, 'f', 1)
                                 .arg(at(1.0), 0, 'f', 1);
    }
    if (m_latency.size() >= 4) {
        std::vector<qint64> latency = std::move(m_latency);
        std::ranges::sort(latency);
        const auto at = [&latency](double q) { return latency[static_cast<std::size_t>(q * double(latency.size() - 1))] / 1e6; };
        qInfo().noquote() << u"Editor: from a wheel notch to the frame that shows it: p50 %1 ms, p95 %2 ms, max %3 ms (%4 notches)"_s
                                 .arg(at(0.5), 0, 'f', 1)
                                 .arg(at(0.95), 0, 'f', 1)
                                 .arg(at(1.0), 0, 'f', 1)
                                 .arg(latency.size());
    }
    m_latency.clear();
    m_handled.clear();
    m_frames.clear();
    m_frameOpen = false;
}

void CaptureEditor::setMetrics(QSize windowLogical, qreal dpr)
{
    m_windowLogical = windowLogical;
    m_dpr = dpr > 0 ? dpr : 1.0;
    // The scene is stretched onto a buffer of the rounded size: the real
    // scale of each axis is not the monitor's (docs/pitfalls.md, 全屏遮罩差 1 个像素).
    m_bufferX = bufferScale(windowLogical.width(), m_dpr);
    m_bufferY = bufferScale(windowLogical.height(), m_dpr);
    m_viewport = QRectF(0, topHeight(), windowLogical.width(),
        std::max(0, windowLogical.height() - topHeight() - bottomHeight()));
    relayout();
}

void CaptureEditor::relayout()
{
    const int x0 = int(std::lround(m_viewport.left() * m_bufferX));
    const int y0 = int(std::lround(m_viewport.top() * m_bufferY));
    const int x1 = int(std::lround(m_viewport.right() * m_bufferX));
    const int y1 = int(std::lround(m_viewport.bottom() * m_bufferY));
    m_viewDev = QRect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
    if (m_open) {
        const QSize canvas = m_model.document().canvas;
        if (m_fit)
            m_zoom = editview::fitZoom(canvas, m_viewDev.size());
        m_zoom = editview::clampZoom(m_zoom);
        m_originDev = editview::clampOrigin(m_originDev, editview::shownSize(canvas, m_zoom), m_viewDev.size());
        m_model.setView(m_bufferX, m_zoom);
    }
    emit viewChanged();
}

QPointF CaptureEditor::contentOrigin() const
{
    return QPointF((m_viewDev.x() + m_originDev.x()) / m_bufferX, (m_viewDev.y() + m_originDev.y()) / m_bufferY);
}

QSizeF CaptureEditor::contentSize() const
{
    const QSize canvas = m_model.document().canvas;
    return QSizeF(canvas.width() * scaleX(), canvas.height() * scaleY());
}

QRectF CaptureEditor::visible() const
{
    return editview::visibleImage(m_originDev, m_zoom, m_viewDev.size());
}

QString CaptureEditor::zoomText() const
{
    return tr("%1%").arg(int(std::lround(m_zoom * 100)));
}

QString CaptureEditor::sizeText() const
{
    const QSize size = m_model.cropRect().size();
    return size.isEmpty() ? QString() : tr("%1 × %2").arg(size.width()).arg(size.height());
}

QString CaptureEditor::copyText() const
{
    return fromPin() ? tr("完成") : tr("复制");
}

void CaptureEditor::showNotice(const QString& text)
{
    m_notice = text;
    m_noticeTimer.start();
    emit noticeChanged();
}

void CaptureEditor::setCursor(int shape)
{
    if (shape == m_cursor)
        return;
    m_cursor = shape;
    emit cursorChanged();
}

// ---- opening --------------------------------------------------------------------------

annot::Project CaptureEditor::projectOfPicture(const QImage& picture) const
{
    annot::Project project;
    if (picture.isNull())
        return project;
    project.base = flatten(picture);
    project.doc.canvas = project.base.size();
    project.doc.crop = QRect(QPoint(0, 0), project.base.size());
    project.doc.dpr = 0; // the window's, when the model begins
    return project;
}

bool CaptureEditor::openHistory(qint64 clipId)
{
    annot::Project project;
    bool have = false;
    if (m_hooks.project) {
        const QByteArray bytes = m_hooks.project(clipId);
        if (!bytes.isEmpty()) {
            QString error;
            if (std::optional<annot::Project> read = annot::readProject(bytes, &error)) {
                project = std::move(*read);
                have = true;
            } else {
                qWarning().noquote() << "Screen capture: the project of entry" << clipId << "cannot be opened:" << error;
            }
        }
    }
    if (!have && m_hooks.picture)
        project = projectOfPicture(m_hooks.picture(clipId)); // any picture: it is the base of an empty document
    if (project.base.isNull())
        return false;
    abortHide();
    if (m_open)
        end();
    m_clipId = clipId;
    return begin(std::move(project), editview::Origin::History);
}

bool CaptureEditor::openPin(PinTarget pin)
{
    if (pin.alive && !pin.alive())
        return false;
    annot::Project project;
    if (!pin.project.isEmpty()) {
        if (std::optional<annot::Project> read = annot::readProject(pin.project, nullptr))
            project = std::move(*read);
    }
    if (project.base.isNull())
        project = projectOfPicture(pin.image);
    if (project.base.isNull())
        return false;
    abortHide();
    if (m_open)
        end();
    m_pin = std::move(pin);
    m_clipId = 0;
    return begin(std::move(project), editview::Origin::Pin);
}

bool CaptureEditor::openImage(const QImage& picture)
{
    annot::Project project = projectOfPicture(picture);
    if (project.base.isNull())
        return false;
    abortHide();
    if (m_open)
        end();
    m_clipId = 0;
    return begin(std::move(project), editview::Origin::Fresh);
}

// The screen the window opens on: a pin's own, else the one under the mouse
// (where the launcher or the clipboard window was).
QScreen* CaptureEditor::targetScreen() const
{
    if (m_origin == editview::Origin::Pin && !m_pin.picture.isEmpty()) {
        for (QScreen* screen : QGuiApplication::screens()) {
            if (physical(screen->geometry(), screen->devicePixelRatio()).contains(m_pin.picture.center()))
                return screen;
        }
    }
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    return screen ? screen : QGuiApplication::primaryScreen();
}

bool CaptureEditor::begin(annot::Project project, Origin origin)
{
    m_origin = origin;
    const QScreen* screen = m_window ? targetScreen() : nullptr;
    const qreal deviceDpr = screen ? screen->devicePixelRatio() : m_dpr;
    const annot::StyleMemory styles = annot::StyleMemory::fromJson(m_hooks.styles ? m_hooks.styles() : QString());
    m_model.begin(project, deviceDpr, styles);
    m_open = true;
    m_zoom = origin == Origin::Pin && m_pin.zoom > 0 ? m_pin.zoom : 1.0;
    m_fit = origin != Origin::Pin; // a pin keeps its zoom
    m_originDev = QPoint(0, 0);
    m_pressed = m_panning = m_space = false;
    m_wheelZoom = 0;
    m_notice.clear();
    m_noticeTimer.stop();
    setCursor(Qt::ArrowCursor);
    emit openChanged();
    emit sizeTextChanged();
    present();
    return true;
}

// The window's size and place for this picture, then shown: a picture that
// fits is shown whole at 100 % with a little room round it; a larger one
// takes most of the work area and is fitted; a pin's picture keeps its zoom
// and, with room, lies where the pin was.
void CaptureEditor::present()
{
    if (!m_window) {
        relayout();
        return;
    }
    QScreen* screen = targetScreen();
    const qreal dpr = screen->devicePixelRatio();
    const QRect avail = screen->availableGeometry(); // logical size, native origin
    const bool pin = m_origin == Origin::Pin;
    const int minWidth = std::max(kMinWindowWidth, m_window->minimumWidth());
    const int minHeight = std::max(kMinWindowHeight, m_window->minimumHeight());
    const int maxWidth = std::max(minWidth, int(avail.width() * kWorkShare));
    const int maxHeight = std::max(minHeight, int(avail.height() * kWorkShare));
    const QSize canvas = m_model.document().canvas;
    const double zoom = pin ? m_zoom : 1.0;
    const int chrome = topHeight() + bottomHeight();
    const int padding = pin ? 0 : kPadding;
    const int wantWidth = int(std::ceil(canvas.width() * zoom / dpr)) + 2 * padding;
    const int wantHeight = int(std::ceil(canvas.height() * zoom / dpr)) + 2 * padding + chrome;
    const QSize size(std::clamp(wantWidth, minWidth, maxWidth), std::clamp(wantHeight, minHeight, maxHeight));

    m_window->setScreen(screen);
    m_window->resize(size);
    m_wantedSize = size;
    m_window->setPosition(avail.topLeft() + QPoint((avail.width() - size.width()) / 2, (avail.height() - size.height()) / 2));
    m_wantedPosition = m_window->position();
    setMetrics(size, dpr);
    if (pin && m_pin.setShown)
        m_pin.setShown(false); // the canvas takes its place
    if (m_hooks.window.show)
        m_hooks.window.show(m_window);
    alignToPin();
    m_settle.start(); // the window's real scale and size, once it is up
}

void CaptureEditor::settle()
{
    if (!m_open || !m_window)
        return;
    // A window that was snapped to an edge when it was closed comes back with the size it had before
    // (Windows keeps that with the window): once it is up, the size it should have again.
    if (m_window->visibility() == QWindow::Windowed && m_window->size() != m_wantedSize && !m_wantedSize.isEmpty()) {
        qInfo().noquote() << u"Editor: the window came up %1x%2, not %3x%4: sized again"_s.arg(m_window->width())
                                 .arg(m_window->height())
                                 .arg(m_wantedSize.width())
                                 .arg(m_wantedSize.height());
        m_window->setGeometry(QRect(m_wantedPosition, m_wantedSize));
    }
    updateScale();
    alignToPin();
    logView(m_origin == Origin::Pin ? "settled (pin)" : "settled");
}

// The canvas where the pin's picture was, when the work area has room.
void CaptureEditor::alignToPin()
{
    if (m_origin != Origin::Pin || !m_open || !m_window || !m_hooks.window.nativeRect || !m_hooks.window.moveTo)
        return;
    const QScreen* screen = m_window->screen();
    const QRect native = m_hooks.window.nativeRect(m_window);
    if (native.isEmpty() || !screen)
        return;
    // The crop of the base is what the pin showed.
    const QPoint cropOffset(int(std::lround(m_model.cropRect().x() * m_zoom)), int(std::lround(m_model.cropRect().y() * m_zoom)));
    const QRect picture(m_pin.picture.topLeft() - cropOffset, m_pin.picture.size());
    const editview::WindowPlace place = editview::placeForPin(picture, imageInWindow(), native.size(),
        physical(screen->availableGeometry(), screen->devicePixelRatio()));
    if (place.topLeft != native.topLeft())
        m_hooks.window.moveTo(m_window, place.topLeft);
}

QRect CaptureEditor::canvasOnScreen() const
{
    if (!m_window || !m_hooks.window.nativeRect)
        return {};
    const QRect native = m_hooks.window.nativeRect(m_window);
    const QRect crop = m_model.cropRect();
    const QPoint at = native.topLeft() + imageInWindow()
        + QPoint(int(std::lround(crop.x() * m_zoom)), int(std::lround(crop.y() * m_zoom)));
    return QRect(at, editview::shownSize(crop.size(), m_zoom));
}

// ---- the mouse ---------------------------------------------------------------------------

QPoint CaptureEditor::deviceInViewport(qreal x, qreal y) const
{
    // Qt's logical position is the native one over the monitor's scale.
    return QPoint(toPhysical(x, m_dpr) - m_viewDev.x(), toPhysical(y, m_dpr) - m_viewDev.y());
}

QPoint CaptureEditor::imageAt(QPoint viewportDevice) const
{
    return editview::imagePixelAt(viewportDevice, m_originDev, m_zoom);
}

void CaptureEditor::hoverAt(QPoint image)
{
    if (m_panning) {
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (m_space) {
        setCursor(Qt::OpenHandCursor);
        return;
    }
    const int shape = m_model.cursorAt(image);
    setCursor(shape >= 0 ? shape : Qt::ArrowCursor);
}

void CaptureEditor::mousePressed(qreal x, qreal y, int button, int modifiers)
{
    if (!m_open)
        return;
    const QPoint device = deviceInViewport(x, y);
    const auto which = static_cast<Qt::MouseButton>(button);
    if (which == Qt::MiddleButton || (which == Qt::LeftButton && m_space)) {
        m_panning = true;
        m_panFrom = device;
        m_panOrigin = m_originDev;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (which == Qt::RightButton) {
        if (!m_model.finishPath())
            m_model.escape();
        return;
    }
    if (which != Qt::LeftButton)
        return;
    const QPoint image = imageAt(device);
    const Qt::KeyboardModifiers mods(modifiers);
    m_pressed = true;
    if (!m_model.pressFirst(image, mods))
        m_model.pressSecond(image, mods);
    hoverAt(image);
}

void CaptureEditor::mouseMoved(qreal x, qreal y, int modifiers)
{
    if (!m_open)
        return;
    const QPoint device = deviceInViewport(x, y);
    if (m_panning) {
        m_originDev = editview::clampOrigin(m_panOrigin + (device - m_panFrom),
            editview::shownSize(m_model.document().canvas, m_zoom), m_viewDev.size());
        viewMoved();
        emit viewChanged();
        return;
    }
    const QPoint image = imageAt(device);
    if (m_pressed || m_model.gestureActive())
        m_model.move(image, Qt::KeyboardModifiers(modifiers));
    hoverAt(image);
}

void CaptureEditor::mouseReleased(qreal x, qreal y, int button)
{
    if (!m_open)
        return;
    const auto which = static_cast<Qt::MouseButton>(button);
    if (m_panning && (which == Qt::MiddleButton || which == Qt::LeftButton)) {
        m_panning = false;
        hoverAt(imageAt(deviceInViewport(x, y)));
        return;
    }
    if (which != Qt::LeftButton || !m_pressed)
        return;
    m_pressed = false;
    const QPoint image = imageAt(deviceInViewport(x, y));
    m_model.release(image);
    hoverAt(image);
}

void CaptureEditor::mouseDoubleClicked(qreal x, qreal y)
{
    if (!m_open || m_space)
        return;
    // The second press began a gesture that the double click replaces.
    if (m_model.gestureActive() && !m_model.pathActive())
        m_model.cancelGesture();
    m_pressed = false;
    m_model.doubleClick(imageAt(deviceInViewport(x, y)));
}

void CaptureEditor::spaceHeld(bool held)
{
    if (m_space == held)
        return;
    m_space = held;
    if (!held && m_panning)
        m_panning = false;
    setCursor(held ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

// The plain wheel scrolls (a picture of 30000 px has to be scrolled; the
// overlay's wheel-sizes-the-tool is Alt+wheel here), Shift scrolls sideways,
// Ctrl zooms about the pointer.
void CaptureEditor::wheeled(qreal x, qreal y, int angleDeltaY, int angleDeltaX, int modifiers)
{
    if (!m_open)
        return;
    QElapsedTimer handling;
    handling.start();
    struct Handled {
        std::vector<qint64>& all;
        QElapsedTimer& clock;
        ~Handled() { all.push_back(clock.nsecsElapsed()); }
    } handled {m_handled, handling};
    m_wheelAt = m_stageClock.nsecsElapsed();
    const Qt::KeyboardModifiers mods(modifiers);
    if (mods & Qt::ControlModifier) {
        if (angleDeltaY == 0)
            return;
        if (m_wheelZoom != 0 && (angleDeltaY > 0) != (m_wheelZoom > 0))
            m_wheelZoom = 0; // turned the other way: start again
        m_wheelZoom += angleDeltaY;
        const int steps = m_wheelZoom / kNotch; // whole notches, toward zero
        if (steps != 0) {
            m_wheelZoom -= steps * kNotch;
            zoomBySteps(steps, deviceInViewport(x, y));
        }
        return;
    }
    if (mods & Qt::AltModifier) {
        // Qt turns Alt+wheel sideways (the vertical delta comes as the horizontal one).
        m_model.wheel(angleDeltaY != 0 ? angleDeltaY : angleDeltaX, m_clock.elapsed());
        return;
    }
    int dx = angleDeltaX;
    int dy = angleDeltaY;
    if (mods & Qt::ShiftModifier) {
        dx = dy != 0 ? dy : dx;
        dy = 0;
    }
    scrollBy(QPoint(int(std::lround(dx * kScrollNotch * m_bufferX / kNotch)), int(std::lround(dy * kScrollNotch * m_bufferY / kNotch))));
}

void CaptureEditor::scrollBy(QPoint device)
{
    if (!m_open || device.isNull())
        return;
    const QPoint next = editview::clampOrigin(m_originDev + device, editview::shownSize(m_model.document().canvas, m_zoom),
        m_viewDev.size());
    if (next == m_originDev)
        return;
    m_originDev = next;
    viewMoved();
    emit viewChanged();
}

// ---- the zoom -----------------------------------------------------------------------------

void CaptureEditor::zoomTo(double zoom, std::optional<QPoint> anchor)
{
    if (!m_open)
        return;
    zoom = editview::clampZoom(zoom);
    if (std::abs(zoom - m_zoom) < 1e-9 && !m_fit)
        return;
    const QPoint about = anchor.value_or(QPoint(m_viewDev.width() / 2, m_viewDev.height() / 2));
    m_originDev = zoomAbout(m_originDev, about, m_zoom, zoom);
    m_zoom = zoom;
    m_fit = false;
    viewMoved();
    relayout();
}

void CaptureEditor::zoomBySteps(int steps, std::optional<QPoint> anchor)
{
    zoomTo(nextZoom(m_zoom, steps, editview::kMaxZoom), anchor);
}

void CaptureEditor::fit()
{
    if (!m_open)
        return;
    m_fit = true;
    relayout();
}

void CaptureEditor::toggleFit()
{
    if (!m_open)
        return;
    const double whole = editview::fitZoom(m_model.document().canvas, m_viewDev.size());
    if (whole < 1.0 - 1e-9 && (m_fit || std::abs(m_zoom - whole) < 1e-9))
        zoomTo(1.0);
    else
        fit();
}

// ---- keys ---------------------------------------------------------------------------------

bool CaptureEditor::keyPressed(int key, int modifiers)
{
    if (!m_open)
        return false;
    if (m_choosingSave)
        return true;
    const Qt::KeyboardModifiers mods = Qt::KeyboardModifiers(modifiers) & ~Qt::KeypadModifier;
    if (m_model.key(key, mods))
        return true;
    const bool ctrl = mods & Qt::ControlModifier;
    const bool alt = mods & (Qt::AltModifier | Qt::MetaModifier);
    if (key == Qt::Key_F4 && (mods & Qt::AltModifier)) { // as the close button: emptied, then hidden
        close();
        return true;
    }
    // The user's keys for saving and pinning; the fixed keys below stay as they are.
    // The validator keeps these clear of them, so the order does not matter.
    if (m_keys.matches(KeyAction::Save, key, mods)) {
        act(static_cast<int>(Action::Save));
        return true;
    }
    if (m_keys.matches(KeyAction::Pin, key, mods)) {
        act(static_cast<int>(Action::Pin));
        return true;
    }
    if (alt)
        return false;
    switch (key) {
    case Qt::Key_Escape:
        return escape();
    case Qt::Key_Return:
    case Qt::Key_Enter:
        done();
        return true;
    case Qt::Key_C:
        if (!ctrl)
            return false;
        done();
        return true;
    case Qt::Key_Z:
        if (ctrl)
            return false;
        toggleFit();
        return true;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        if (!ctrl)
            return false;
        zoomBySteps(1);
        return true;
    case Qt::Key_Minus:
    case Qt::Key_Underscore:
        if (!ctrl)
            return false;
        zoomBySteps(-1);
        return true;
    case Qt::Key_0:
        if (!ctrl)
            return false;
        fit();
        return true;
    case Qt::Key_Left:
        scrollBy(QPoint(int(std::lround(kScrollKey * m_bufferX)), 0));
        return true;
    case Qt::Key_Right:
        scrollBy(QPoint(-int(std::lround(kScrollKey * m_bufferX)), 0));
        return true;
    case Qt::Key_Up:
        scrollBy(QPoint(0, int(std::lround(kScrollKey * m_bufferY))));
        return true;
    case Qt::Key_Down:
        scrollBy(QPoint(0, -int(std::lround(kScrollKey * m_bufferY))));
        return true;
    case Qt::Key_PageUp:
        scrollBy(QPoint(0, m_viewDev.height() * 9 / 10));
        return true;
    case Qt::Key_PageDown:
        scrollBy(QPoint(0, -m_viewDev.height() * 9 / 10));
        return true;
    case Qt::Key_Home:
        scrollBy(QPoint(m_viewDev.width() * 100, m_viewDev.height() * 100));
        return true;
    case Qt::Key_End:
        scrollBy(QPoint(-m_viewDev.width() * 100, -m_viewDev.height() * 100));
        return true;
    default:
        return false;
    }
}

bool CaptureEditor::escape()
{
    if (!m_open)
        return false;
    if (m_model.escape())
        return true; // a text, a stroke, the number box, the eyedropper
    if (m_model.modified()) {
        m_model.discardChanges();
        showNotice(tr("已放弃修改 · Ctrl+Z 恢复"));
        qInfo().noquote() << u"Editor: Esc gave up the changes (%1 objects now)"_s.arg(m_model.itemCount());
        return true;
    }
    qInfo() << "Editor: Esc closes it";
    close();
    return true;
}

// ---- the ways out --------------------------------------------------------------------------

QByteArray CaptureEditor::packProject()
{
    const annot::Project project = m_model.project();
    if (project.base.isNull())
        return {};
    // Nothing to edit again when nothing is drawn and nothing cropped.
    if (project.doc.items.isEmpty() && project.doc.crop == QRect(QPoint(0, 0), project.doc.canvas))
        return {};
    return annot::writeProject(project.doc, project.base, project.basePng);
}

void CaptureEditor::act(int action)
{
    if (!m_open || m_choosingSave)
        return;
    switch (static_cast<Action>(action)) {
    case Action::Copy:
        done();
        break;
    case Action::Save:
    case Action::SavePrevious:
    case Action::SaveAs: {
        // A save folder chosen: Save goes straight into it, 另存为… asks (#194).
        const bool pick = static_cast<Action>(action) == Action::SaveAs
            || (static_cast<Action>(action) == Action::Save && !m_model.saveDirect());
        QString path;
        if (pick) {
            if (!m_hooks.chooseSavePath)
                break;
            m_choosingSave = true;
            path = m_hooks.chooseSavePath(m_window);
            m_choosingSave = false;
            if (path.isEmpty() || !m_open)
                break;
        }
        const QImage image = m_model.composed();
        if (image.isNull())
            break;
        const QByteArray project = packProject();
        if (m_hooks.save)
            m_hooks.save(image, path);
        if (m_hooks.record)
            m_hooks.record(image, project);
        close();
        break;
    }
    case Action::Pin: {
        if (fromPin()) { // it is a pin already: 完成
            done();
            break;
        }
        const QImage image = m_model.composed();
        if (image.isNull())
            break;
        const QByteArray project = packProject();
        // Where the canvas is when it lies as it was taken; else in the
        // middle of the window's screen.
        QRect where = canvasOnScreen();
        if (where.isEmpty() || std::abs(m_zoom - 1.0) > 1e-9) {
            const QRect native = m_window && m_hooks.window.nativeRect ? m_hooks.window.nativeRect(m_window) : QRect();
            where = QRect(native.center() - QPoint(image.width() / 2, image.height() / 2), image.size());
        }
        if (m_hooks.pin)
            m_hooks.pin(image, project, where.topLeft());
        if (m_hooks.record)
            m_hooks.record(image, project);
        close();
        break;
    }
    case Action::Ocr: {
        const QImage image = m_model.composed();
        if (!image.isNull() && m_hooks.ocr)
            m_hooks.ocr(image, m_dpr, canvasOnScreen());
        break;
    }
    case Action::Cancel:
        close();
        break;
    default:
        break;
    }
}

// Enter: copy, or 完成 on a pin (editview::planDone).
void CaptureEditor::done()
{
    if (!m_open)
        return;
    m_model.commitPending();
    const bool modified = m_model.modified();
    bool there = false;
    if (m_origin == Origin::History)
        there = m_hooks.exists && m_hooks.exists(m_clipId);
    else if (m_origin == Origin::Pin)
        there = m_pin.alive && m_pin.alive();
    const editview::ExitPlan plan = editview::planDone(m_origin, modified, there);
    qInfo().noquote() << u"Editor: Enter, origin %1, modified %2, original there %3 -> exit %4"_s.arg(int(m_origin)).arg(modified).arg(there).arg(int(plan.exit));
    switch (plan.exit) {
    case editview::Exit::TouchOriginal:
        if (m_hooks.copyOriginal)
            m_hooks.copyOriginal(m_clipId);
        break;
    case editview::Exit::NewEntry: {
        const QImage image = m_model.composed();
        if (!image.isNull() && m_hooks.copy)
            m_hooks.copy(image, packProject());
        break;
    }
    case editview::Exit::ReplacePin: {
        const QImage image = m_model.composed();
        if (image.isNull())
            break;
        const QByteArray project = packProject();
        if (m_pin.replace)
            m_pin.replace(image, project);
        if (plan.record && m_hooks.record)
            m_hooks.record(image, project);
        break;
    }
    case editview::Exit::Close:
        break;
    }
    close();
}

// ---- closing --------------------------------------------------------------------------------

void CaptureEditor::close()
{
    end();
    hideWindow();
}

// The picture, the document and the tiles let go of, the window left as it is.
void CaptureEditor::end()
{
    if (m_open) {
        qInfo() << "Editor: closed";
        m_model.commitPending();
        if (m_model.stylesChanged() && m_hooks.stylesChanged)
            m_hooks.stylesChanged(m_model.styles().toJson());
        // The pin comes back where it was, with the new picture if 完成 gave one.
        if (m_origin == Origin::Pin && m_pin.alive && m_pin.alive() && m_pin.setShown)
            m_pin.setShown(true);
        m_pin = PinTarget {};
        m_model.end();
        m_open = false;
        m_pressed = m_panning = m_space = false;
        m_settle.stop();
        m_notice.clear();
        m_noticeTimer.stop();
        emit openChanged();
        emit noticeChanged();
        emit sizeTextChanged();
        emit viewChanged();
    }
}

// The window is cloaked at once, drawn empty (the canvas lets go of its
// tiles and textures in its polish and sync), then hidden: a hidden window
// is not drawn, so it would keep them.
void CaptureEditor::hideWindow()
{
    if (m_hiding)
        return;
    if (!m_window || !m_window->isVisible()) {
        finishHide();
        return;
    }
    m_hiding = true;
    if (m_hooks.window.cloak)
        m_hooks.window.cloak(m_window, true);
    m_closingFrames = 2;
    m_frameConnection = connect(m_window, &QQuickWindow::frameSwapped, this, [this] {
        if (--m_closingFrames <= 0)
            finishHide();
        else if (m_window)
            m_window->update();
    }, Qt::QueuedConnection);
    m_hideTimeout.start();
    m_window->update();
}

void CaptureEditor::finishHide()
{
    disconnect(m_frameConnection);
    m_hideTimeout.stop();
    const bool hiding = std::exchange(m_hiding, false);
    if (m_window && !m_open) {
        if (m_window->isVisible())
            m_window->hide();
        if (hiding && m_hooks.window.cloak)
            m_hooks.window.cloak(m_window, false);
        m_window->releaseResources();
    }
    if (m_hooks.closed)
        m_hooks.closed();
}

// Opened again while the last one was still emptying: it stays up.
void CaptureEditor::abortHide()
{
    if (!m_hiding)
        return;
    disconnect(m_frameConnection);
    m_hideTimeout.stop();
    m_hiding = false;
    if (m_window && m_hooks.window.cloak)
        m_hooks.window.cloak(m_window, false);
}

} // namespace ws::capture
