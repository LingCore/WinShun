#include "CaptureSurfaceBridge.h"

#include "platform/QtKeys.h"

#include <QCoreApplication>
#include <QCursor>
#include <QDebug>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QImage>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUuid>
#include <QWheelEvent>

#include <sddl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

using namespace capsurface;

constexpr int kSurfaceStartTimeoutMs = 1500;
constexpr DWORD kWriteTimeoutMs = 300;

Qt::KeyboardModifiers toQtModifiers(std::uint8_t mask)
{
    Qt::KeyboardModifiers mods;
    if (mask & kModShift)
        mods |= Qt::ShiftModifier;
    if (mask & kModCtrl)
        mods |= Qt::ControlModifier;
    if (mask & kModAlt)
        mods |= Qt::AltModifier;
    return mods;
}

Qt::MouseButton toQtButton(MouseButton b)
{
    switch (b) {
    case MouseButton::Left: return Qt::LeftButton;
    case MouseButton::Right: return Qt::RightButton;
    case MouseButton::Middle: return Qt::MiddleButton;
    case MouseButton::X1: return Qt::BackButton;
    case MouseButton::X2: return Qt::ForwardButton;
    case MouseButton::None:
        break;
    }
    return Qt::NoButton;
}

Qt::MouseButtons toQtButtons(std::uint8_t mask)
{
    Qt::MouseButtons out;
    if (mask & kBtnLeft) out |= Qt::LeftButton;
    if (mask & kBtnRight) out |= Qt::RightButton;
    if (mask & kBtnMiddle) out |= Qt::MiddleButton;
    if (mask & kBtnX1) out |= Qt::BackButton;
    if (mask & kBtnX2) out |= Qt::ForwardButton;
    return out;
}

QQuickItem* scopedFocusItem(QQuickWindow* window)
{
    if (!window)
        return nullptr;
    QQuickItem* item = window->contentItem();
    while (item && item->isFocusScope() && item->scopedFocusItem() && item->scopedFocusItem()->isEnabled())
        item = item->scopedFocusItem();
    return item;
}

std::uint64_t hashRgbaCursor(std::uint16_t w, std::uint16_t h, std::int16_t hotX, std::int16_t hotY,
    const std::uint8_t* data, std::size_t size)
{
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](const void* p, std::size_t n) {
        const auto* bytes = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    mix(&w, sizeof w);
    mix(&h, sizeof h);
    mix(&hotX, sizeof hotX);
    mix(&hotY, sizeof hotY);
    mix(data, size);
    return hash ? hash : 1;
}

// Builds a security descriptor granting pipe access only to the current user,
// SYSTEM and Administrators so no other account or sandboxed process can open it.
PSECURITY_DESCRIPTOR currentUserPipeDescriptor()
{
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken))
        return nullptr;
    DWORD size = 0;
    ::GetTokenInformation(rawToken, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buf(size);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (size > 0 && ::GetTokenInformation(rawToken, TokenUser, buf.data(), size, &size)) {
        LPWSTR sidStr = nullptr;
        if (::ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(buf.data())->User.Sid, &sidStr) && sidStr) {
            const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;FRFW;;;" + std::wstring(sidStr) + L")";
            ::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
            ::LocalFree(sidStr);
        }
    }
    ::CloseHandle(rawToken);
    return sd;
}

} // namespace

int qtKeyFromSurface(std::uint32_t vk, bool extended, bool shift, const QString& text)
{
    if (const int key = ws::qtKeyOf(vk, extended, shift); key != 0)
        return key;
    switch (vk) {
    case VK_SNAPSHOT: return Qt::Key_Print;
    case VK_OEM_COMMA: return shift ? Qt::Key_Less : Qt::Key_Comma;
    case VK_OEM_PERIOD: return shift ? Qt::Key_Greater : Qt::Key_Period;
    case VK_OEM_2: return shift ? Qt::Key_Question : Qt::Key_Slash;
    case VK_OEM_4: return shift ? Qt::Key_BraceLeft : Qt::Key_BracketLeft;
    case VK_OEM_6: return shift ? Qt::Key_BraceRight : Qt::Key_BracketRight;
    case VK_OEM_PLUS: return shift ? Qt::Key_Plus : Qt::Key_Equal;
    case VK_OEM_MINUS: return shift ? Qt::Key_Underscore : Qt::Key_Minus;
    case VK_OEM_1: return shift ? Qt::Key_Colon : Qt::Key_Semicolon;
    case VK_OEM_3: return shift ? Qt::Key_AsciiTilde : Qt::Key_QuoteLeft;
    case VK_OEM_5: return shift ? Qt::Key_Bar : Qt::Key_Backslash;
    case VK_OEM_7: return shift ? Qt::Key_QuoteDbl : Qt::Key_Apostrophe;
    case VK_MULTIPLY: return Qt::Key_Asterisk;
    case VK_DIVIDE: return Qt::Key_Slash;
    case VK_DECIMAL: return Qt::Key_Period;
    default:
        break;
    }
    if (!text.isEmpty() && text.at(0).isPrint())
        return text.at(0).toUpper().unicode();
    return 0;
}

CaptureSurfaceBridge::CaptureSurfaceBridge(QObject* parent)
    : QObject(parent)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(kSurfaceStartTimeoutMs);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (!m_running || m_ready)
            return;
        stop();
        emit failed(u"timeout"_s, ERROR_TIMEOUT);
    });
}

CaptureSurfaceBridge::~CaptureSurfaceBridge()
{
    stop();
}

bool CaptureSurfaceBridge::isSurfaceHwnd(HWND hwnd) const noexcept
{
    return hwnd && std::ranges::find(m_surfaceHwnds, hwnd) != m_surfaceHwnds.end();
}

bool CaptureSurfaceBridge::start(HWND fullScreenTarget, std::vector<Target> targets, int activeIndex,
    OpenFn openSurface, CloseFn closeSurface)
{
    stop();
    if (!fullScreenTarget || targets.empty() || !openSurface)
        return false;
    m_targets = std::move(targets);
    m_activeIndex = std::clamp(activeIndex, 0, static_cast<int>(m_targets.size()) - 1);
    m_focusedIndex = -1;
    m_hoverIndex = -1;
    m_captureIndex = -1;
    m_closeSurface = std::move(closeSurface);
    m_overlaysReady = false;
    m_initSent = false;
    m_ready = false;
    m_hostReplied = false;
    m_pipeConnected = false;
    m_expectedPeerPid = 0;
    m_connectedPeerPid = 0;
    m_surfaceHwnds.clear();
    m_hasLastCursor = false;
    m_sentRgbaCursors.clear();
    m_lastImeEnabled = false;
    m_lastImeMonitor = -1;
    m_lastImeCaret = {};
    m_stopping.store(false);

    const QString token = QUuid::createUuid().toString(QUuid::Id128).toLower();
    const QString pipeName = u"WinShun.CaptureSurface.%1.%2"_s.arg(::GetCurrentProcessId()).arg(token);
    const std::wstring fullPath = L"\\\\.\\pipe\\" + pipeName.toStdWString();

    PSECURITY_DESCRIPTOR sd = currentUserPipeDescriptor();
    SECURITY_ATTRIBUTES sa {sizeof sa, sd, FALSE};
    m_pipe = ::CreateNamedPipeW(fullPath.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64 * 1024, 64 * 1024, 0, sd ? &sa : nullptr);
    if (sd)
        ::LocalFree(sd);
    if (m_pipe == INVALID_HANDLE_VALUE)
        return false;

    m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_writeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent || !m_writeEvent) {
        stop();
        return false;
    }

    m_running = true;
    m_timeout.start();
    HANDLE pipeHandle = m_pipe;
    HANDLE stopHandle = m_stopEvent;
    m_reader = std::thread([this, pipeHandle, stopHandle] { pipeLoop(pipeHandle, stopHandle); });

    QPointer<CaptureSurfaceBridge> self(this);
    openSurface(fullScreenTarget, pipeName, [self](OpenReply reply) {
        if (self)
            self->onHostOpenReply(std::move(reply));
    });
    return true;
}

void CaptureSurfaceBridge::setOverlaysReady()
{
    if (!m_running || m_overlaysReady)
        return;
    m_overlaysReady = true;
    trySendInit();
}

void CaptureSurfaceBridge::stop()
{
    m_timeout.stop();
    clearQuickFocus();
    removeWatchers();
    const bool wasRunning = std::exchange(m_running, false);
    m_ready = false;
    m_initSent = false;
    m_overlaysReady = false;
    m_surfaceHwnds.clear();
    m_hoverIndex = -1;
    m_captureIndex = -1;

    if (wasRunning && m_pipe != INVALID_HANDLE_VALUE && m_pipeConnected)
        sendFrame(encodeClose());

    m_stopping.store(true);
    if (m_stopEvent)
        ::SetEvent(m_stopEvent);
    if (m_pipe != INVALID_HANDLE_VALUE)
        ::CancelIoEx(m_pipe, nullptr);
    if (m_reader.joinable())
        m_reader.join();
    {
        const std::lock_guard lock(m_writeMutex);
        if (m_pipe != INVALID_HANDLE_VALUE) {
            ::DisconnectNamedPipe(m_pipe);
            ::CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
    }
    if (m_stopEvent) {
        ::CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
    if (m_writeEvent) {
        ::CloseHandle(m_writeEvent);
        m_writeEvent = nullptr;
    }
    if (auto closeFn = std::exchange(m_closeSurface, {}))
        closeFn();
    m_targets.clear();
}

void CaptureSurfaceBridge::onHostOpenReply(OpenReply reply)
{
    if (!m_running)
        return;
    m_hostReplied = true;
    if (!reply.ok || !reply.childPid) {
        stop();
        emit failed(reply.reason.isEmpty() ? u"failed"_s : reply.reason, reply.error);
        return;
    }
    m_expectedPeerPid = reply.childPid;
    if (m_pipeConnected && m_connectedPeerPid != m_expectedPeerPid) {
        stop();
        emit failed(u"peer-mismatch"_s, ERROR_ACCESS_DENIED);
        return;
    }
    trySendInit();
}

void CaptureSurfaceBridge::onPipeConnected(DWORD clientPid)
{
    if (!m_running)
        return;
    m_pipeConnected = true;
    m_connectedPeerPid = clientPid;
    if (m_hostReplied && m_connectedPeerPid != m_expectedPeerPid) {
        stop();
        emit failed(u"peer-mismatch"_s, ERROR_ACCESS_DENIED);
        return;
    }
    trySendInit();
}

void CaptureSurfaceBridge::trySendInit()
{
    if (!m_running || m_initSent || !m_overlaysReady || !m_hostReplied || !m_pipeConnected)
        return;
    if (!m_expectedPeerPid || m_connectedPeerPid != m_expectedPeerPid)
        return;
    InitMsg init;
    init.activeIndex = static_cast<std::uint32_t>(m_activeIndex);
    init.monitors.reserve(m_targets.size());
    for (const Target& t : m_targets) {
        if (!t.window) {
            stop();
            emit failed(u"overlay-gone"_s, ERROR_INVALID_WINDOW_HANDLE);
            return;
        }
        init.monitors.push_back(MonitorSpec {
            static_cast<std::uint64_t>(t.window->winId()),
            t.physicalRect.x(), t.physicalRect.y(), t.physicalRect.width(), t.physicalRect.height()});
    }
    m_initSent = true;
    if (!sendFrame(encode(init))) {
        stop();
        emit failed(u"init-write"_s, ERROR_BROKEN_PIPE);
        return;
    }
    if (m_activeIndex >= 0 && m_activeIndex < static_cast<int>(m_targets.size()) && m_targets[m_activeIndex].window)
        syncCursor(m_targets[m_activeIndex].window);
}

void CaptureSurfaceBridge::onFrameReceived(Type type, std::vector<std::byte> payload)
{
    if (!m_running)
        return;
    switch (type) {
    case Type::Ready: {
        ReadyMsg msg;
        if (!decode(payload, msg) || !msg.ok || msg.surfaceHwnds.size() != m_targets.size()) {
            const DWORD err = msg.error ? msg.error : ERROR_GEN_FAILURE;
            stop();
            emit failed(u"surface-init"_s, err);
            return;
        }
        m_timeout.stop();
        m_surfaceHwnds.clear();
        m_surfaceHwnds.reserve(msg.surfaceHwnds.size());
        for (std::uint64_t h : msg.surfaceHwnds)
            m_surfaceHwnds.push_back(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(h)));
        m_ready = true;
        installWatchers();
        setQuickFocus(m_activeIndex);
        if (m_activeIndex >= 0 && m_activeIndex < static_cast<int>(m_targets.size()) && m_targets[m_activeIndex].window)
            syncCursor(m_targets[m_activeIndex].window);
        syncIme();
        emit ready(m_surfaceHwnds);
        break;
    }
    case Type::Mouse: {
        MouseMsg msg;
        if (m_ready && decode(payload, msg))
            deliverMouse(msg);
        break;
    }
    case Type::Key: {
        KeyMsg msg;
        if (m_ready && decode(payload, msg))
            deliverKey(msg);
        break;
    }
    case Type::Ime: {
        ImeEventMsg msg;
        if (m_ready && decode(payload, msg))
            deliverIme(msg);
        break;
    }
    case Type::Closed: {
        ClosedMsg msg;
        const CloseReason reason = decode(payload, msg) ? msg.reason : CloseReason::Error;
        const bool wasReady = m_ready;
        stop();
        if (wasReady)
            emit closedExternally(reason);
        else
            emit failed(u"closed-early"_s, static_cast<unsigned long>(reason));
        break;
    }
    default:
        break;
    }
}

void CaptureSurfaceBridge::onPipeBroken()
{
    if (!m_running)
        return;
    const bool wasReady = m_ready;
    stop();
    if (wasReady)
        emit closedExternally(CloseReason::Error);
    else
        emit failed(u"pipe-broken"_s, ERROR_BROKEN_PIPE);
}

int CaptureSurfaceBridge::targetIndexAt(int globalX, int globalY) const
{
    for (int i = 0; i < static_cast<int>(m_targets.size()); ++i) {
        if (m_targets[i].physicalRect.contains(globalX, globalY))
            return i;
    }
    return m_activeIndex;
}

void CaptureSurfaceBridge::deliverMouse(const MouseMsg& msg)
{
    if (m_targets.empty())
        return;
    if (msg.kind == MouseKind::Leave) {
        if (m_hoverIndex >= 0 && m_hoverIndex < static_cast<int>(m_targets.size()) && m_targets[m_hoverIndex].window) {
            QEvent leave(QEvent::Leave);
            QCoreApplication::sendEvent(m_targets[m_hoverIndex].window, &leave);
        }
        m_hoverIndex = -1;
        return;
    }

    int idx = (m_captureIndex >= 0 && msg.buttons != 0) ? m_captureIndex : targetIndexAt(msg.globalX, msg.globalY);
    if (msg.kind == MouseKind::Release && m_captureIndex >= 0)
        idx = m_captureIndex;
    idx = std::clamp(idx, 0, static_cast<int>(m_targets.size()) - 1);
    const Target& target = m_targets[idx];
    QQuickWindow* win = target.window;
    if (!win)
        return;

    const double dpr = target.dpr > 0 ? target.dpr : win->devicePixelRatio();
    const QPointF local((msg.globalX - target.physicalRect.x()) / dpr, (msg.globalY - target.physicalRect.y()) / dpr);
    const QPointF global = win->mapToGlobal(local);
    const Qt::MouseButton btn = toQtButton(msg.button);
    const Qt::MouseButtons buttons = toQtButtons(msg.buttons);
    const Qt::KeyboardModifiers mods = toQtModifiers(msg.modifiers);

    if (m_hoverIndex != idx && m_captureIndex < 0) {
        if (m_hoverIndex >= 0 && m_hoverIndex < static_cast<int>(m_targets.size()) && m_targets[m_hoverIndex].window) {
            QEvent leave(QEvent::Leave);
            QCoreApplication::sendEvent(m_targets[m_hoverIndex].window, &leave);
        }
        QEnterEvent enter(local, local, global);
        QCoreApplication::sendEvent(win, &enter);
        m_hoverIndex = idx;
    }

    switch (msg.kind) {
    case MouseKind::Move: {
        QMouseEvent ev(QEvent::MouseMove, local, local, global, Qt::NoButton, buttons, mods);
        QCoreApplication::sendEvent(win, &ev);
        break;
    }
    case MouseKind::Press: {
        m_activeIndex = idx;
        setQuickFocus(idx);
        if (m_captureIndex < 0)
            m_captureIndex = idx;
        QMouseEvent ev(QEvent::MouseButtonPress, local, local, global, btn, buttons, mods);
        QCoreApplication::sendEvent(win, &ev);
        break;
    }
    case MouseKind::Release: {
        QMouseEvent ev(QEvent::MouseButtonRelease, local, local, global, btn, buttons, mods);
        QCoreApplication::sendEvent(win, &ev);
        if (buttons == Qt::NoButton)
            m_captureIndex = -1;
        break;
    }
    case MouseKind::DblClick: {
        // Qt's Windows platform plugin delivers MouseButtonPress followed by
        // MouseButtonDblClick on WM_*BUTTONDBLCLK; matching that exact sequence
        // keeps AnnotateLayer / CaptureSession double-click handling identical.
        m_activeIndex = idx;
        setQuickFocus(idx);
        if (m_captureIndex < 0)
            m_captureIndex = idx;
        QMouseEvent press(QEvent::MouseButtonPress, local, local, global, btn, buttons, mods);
        QCoreApplication::sendEvent(win, &press);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, local, local, global, btn, buttons, mods);
        QCoreApplication::sendEvent(win, &dbl);
        break;
    }
    case MouseKind::Wheel:
    case MouseKind::HWheel: {
        const QPoint angle = msg.kind == MouseKind::Wheel ? QPoint(0, msg.wheelDelta) : QPoint(msg.wheelDelta, 0);
        QWheelEvent ev(local, global, QPoint(), angle, buttons, mods, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(win, &ev);
        break;
    }
    case MouseKind::Leave:
        break;
    }

    if (m_running && m_ready) {
        syncCursor(win);
        syncIme();
    }
}

void CaptureSurfaceBridge::deliverKey(const KeyMsg& msg)
{
    if (m_targets.empty())
        return;
    const int idx = std::clamp(m_activeIndex, 0, static_cast<int>(m_targets.size()) - 1);
    QQuickWindow* win = m_targets[idx].window;
    if (!win)
        return;
    if (m_focusedIndex != idx || !win->activeFocusItem())
        setQuickFocus(idx);

    const Qt::KeyboardModifiers mods = toQtModifiers(msg.modifiers);
    const QString text = QString::fromUtf16(msg.text.data(), static_cast<qsizetype>(msg.text.size()));
    const int qtKey = qtKeyFromSurface(msg.vk, msg.extended, mods.testFlag(Qt::ShiftModifier), text);
    QKeyEvent ev(msg.press ? QEvent::KeyPress : QEvent::KeyRelease, qtKey, mods, msg.scan, msg.vk, 0, text, msg.repeat);
    QCoreApplication::sendEvent(win, &ev);
    if (m_running && m_ready) {
        syncCursor(win);
        syncIme();
    }
}

void CaptureSurfaceBridge::deliverIme(const ImeEventMsg& msg)
{
    if (m_targets.empty())
        return;
    const int idx = std::clamp(m_activeIndex, 0, static_cast<int>(m_targets.size()) - 1);
    QQuickWindow* win = m_targets[idx].window;
    if (!win)
        return;
    QQuickItem* item = win->activeFocusItem();
    if (!item)
        item = scopedFocusItem(win);
    if (!item)
        return;

    const QString text = QString::fromUtf16(msg.text.data(), static_cast<qsizetype>(msg.text.size()));
    if (msg.kind == ImeKind::Preedit) {
        QList<QInputMethodEvent::Attribute> attrs;
        attrs.append(QInputMethodEvent::Attribute(QInputMethodEvent::Cursor, msg.cursorPos, 1, QVariant()));
        QInputMethodEvent ev(text, attrs);
        QCoreApplication::sendEvent(item, &ev);
    } else if (msg.kind == ImeKind::Commit) {
        QInputMethodEvent ev;
        ev.setCommitString(text);
        QCoreApplication::sendEvent(item, &ev);
    } else {
        QInputMethodEvent ev;
        QCoreApplication::sendEvent(item, &ev);
    }
    if (m_running && m_ready)
        syncIme();
}

void CaptureSurfaceBridge::syncCursor(QQuickWindow* window)
{
    if (!m_running || !m_initSent || !window)
        return;
    const QCursor cursor = window->cursor();
    if (cursor.shape() != Qt::BitmapCursor) {
        const auto shape = static_cast<std::uint32_t>(cursor.shape());
        if (m_hasLastCursor && m_lastCursorKind == CursorKind::Standard && m_lastStandardShape == shape)
            return;
        m_hasLastCursor = true;
        m_lastCursorKind = CursorKind::Standard;
        m_lastStandardShape = shape;
        CursorMsg msg;
        msg.kind = CursorKind::Standard;
        msg.qtShape = shape;
        sendFrame(encode(msg));
        return;
    }
    const QPixmap pm = cursor.pixmap();
    if (pm.isNull())
        return;
    const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (img.isNull() || img.width() <= 0 || img.height() <= 0 || img.width() > kMaxCursorDim
        || img.height() > kMaxCursorDim)
        return;
    const auto w = static_cast<std::uint16_t>(img.width());
    const auto h = static_cast<std::uint16_t>(img.height());
    const double pmDpr = pm.devicePixelRatio() > 0 ? pm.devicePixelRatio() : 1.0;
    const auto hotX = static_cast<std::int16_t>(std::clamp(qRound(cursor.hotSpot().x() * pmDpr), 0, w - 1));
    const auto hotY = static_cast<std::int16_t>(std::clamp(qRound(cursor.hotSpot().y() * pmDpr), 0, h - 1));
    std::vector<std::uint8_t> bgra(std::size_t {w} * std::size_t {h} * 4u);
    for (int y = 0; y < h; ++y)
        std::memcpy(bgra.data() + static_cast<std::size_t>(y) * w * 4u, img.constScanLine(y), std::size_t {w} * 4u);
    const std::uint64_t key = hashRgbaCursor(w, h, hotX, hotY, bgra.data(), bgra.size());
    if (m_hasLastCursor && m_lastCursorKind == CursorKind::Rgba && m_lastRgbaKey == key)
        return;
    m_hasLastCursor = true;
    m_lastCursorKind = CursorKind::Rgba;
    m_lastRgbaKey = key;
    CursorMsg msg;
    msg.kind = CursorKind::Rgba;
    msg.cacheKey = key;
    if (m_sentRgbaCursors.contains(key)) {
        msg.width = 0;
        msg.height = 0;
    } else {
        msg.width = w;
        msg.height = h;
        msg.hotX = hotX;
        msg.hotY = hotY;
        msg.bgra = std::move(bgra);
        m_sentRgbaCursors.insert(key);
    }
    sendFrame(encode(msg));
}

void CaptureSurfaceBridge::syncIme()
{
    if (!m_running || !m_ready || m_targets.empty())
        return;
    const int idx = std::clamp(m_activeIndex, 0, static_cast<int>(m_targets.size()) - 1);
    QQuickWindow* win = m_targets[idx].window;
    QQuickItem* item = win ? (win->activeFocusItem() ? win->activeFocusItem() : scopedFocusItem(win)) : nullptr;
    bool enabled = false;
    QRect caret;
    if (item && win) {
        QInputMethodQueryEvent query(Qt::ImEnabled | Qt::ImCursorRectangle);
        QCoreApplication::sendEvent(item, &query);
        enabled = query.value(Qt::ImEnabled).toBool();
        if (enabled) {
            const QRectF localRect = query.value(Qt::ImCursorRectangle).toRectF();
            const QPointF sceneTopLeft = item->mapToScene(localRect.topLeft());
            const double dpr = m_targets[idx].dpr > 0 ? m_targets[idx].dpr : win->devicePixelRatio();
            caret = QRect(qRound(sceneTopLeft.x() * dpr), qRound(sceneTopLeft.y() * dpr),
                std::max(1, qRound(localRect.width() * dpr)), std::max(1, qRound(localRect.height() * dpr)));
        }
    }
    if (enabled == m_lastImeEnabled && (!enabled || (idx == m_lastImeMonitor && caret == m_lastImeCaret)))
        return;
    m_lastImeEnabled = enabled;
    m_lastImeMonitor = idx;
    m_lastImeCaret = caret;
    ImeStateMsg msg;
    msg.enabled = enabled;
    msg.monitorIndex = static_cast<std::uint32_t>(idx);
    msg.caretX = caret.x();
    msg.caretY = caret.y();
    msg.caretW = caret.width();
    msg.caretH = caret.height();
    sendFrame(encode(msg));
}

bool CaptureSurfaceBridge::eventFilter(QObject* watched, QEvent* event)
{
    if (m_running && event->type() == QEvent::CursorChange) {
        if (auto* win = qobject_cast<QQuickWindow*>(watched))
            syncCursor(win);
    }
    return QObject::eventFilter(watched, event);
}

void CaptureSurfaceBridge::setQuickFocus(int index)
{
    if (index < 0 || index >= static_cast<int>(m_targets.size()))
        return;
    if (m_focusedIndex == index && m_targets[index].window && m_targets[index].window->activeFocusItem())
        return;
    if (m_focusedIndex >= 0 && m_focusedIndex != index && m_focusedIndex < static_cast<int>(m_targets.size())) {
        if (QQuickWindow* prev = m_targets[m_focusedIndex].window) {
            QFocusEvent out(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
            QCoreApplication::sendEvent(prev, &out);
        }
    }
    m_focusedIndex = index;
    if (QQuickWindow* win = m_targets[index].window) {
        // Pure Qt FocusIn (no Win32 activation): populates activeFocusItem() so
        // QQuickWindow::keyPressEvent delivers Keys.onPressed/TextEdit/TextInput
        // normally while Band-2 holds real Win32 foreground.
        QFocusEvent in(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
        QCoreApplication::sendEvent(win, &in);
    }
}

void CaptureSurfaceBridge::clearQuickFocus()
{
    if (m_focusedIndex >= 0 && m_focusedIndex < static_cast<int>(m_targets.size())) {
        if (QQuickWindow* prev = m_targets[m_focusedIndex].window) {
            QFocusEvent out(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
            QCoreApplication::sendEvent(prev, &out);
        }
    }
    m_focusedIndex = -1;
}

void CaptureSurfaceBridge::installWatchers()
{
    if (m_watchersInstalled)
        return;
    m_watchersInstalled = true;
    for (const Target& t : m_targets) {
        if (!t.window)
            continue;
        t.window->installEventFilter(this);
        connect(t.window, &QQuickWindow::activeFocusItemChanged, this, &CaptureSurfaceBridge::syncIme,
            Qt::UniqueConnection);
    }
}

void CaptureSurfaceBridge::removeWatchers()
{
    if (!m_watchersInstalled)
        return;
    m_watchersInstalled = false;
    for (const Target& t : m_targets) {
        if (!t.window)
            continue;
        t.window->removeEventFilter(this);
        disconnect(t.window, &QQuickWindow::activeFocusItemChanged, this, &CaptureSurfaceBridge::syncIme);
    }
}

bool CaptureSurfaceBridge::sendFrame(std::vector<std::byte> frame)
{
    const std::lock_guard lock(m_writeMutex);
    if (m_pipe == INVALID_HANDLE_VALUE || !m_writeEvent || m_stopping.load())
        return false;
    std::size_t sent = 0;
    while (sent < frame.size()) {
        OVERLAPPED ov {};
        ov.hEvent = m_writeEvent;
        ::ResetEvent(m_writeEvent);
        DWORD written = 0;
        const BOOL ok = ::WriteFile(m_pipe, frame.data() + sent,
            static_cast<DWORD>(frame.size() - sent), &written, &ov);
        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING)
                return false;
            HANDLE waits[2] {m_writeEvent, m_stopEvent};
            const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, kWriteTimeoutMs);
            if (w != WAIT_OBJECT_0) {
                ::CancelIoEx(m_pipe, &ov);
                ::GetOverlappedResult(m_pipe, &ov, &written, TRUE);
                return false;
            }
            if (!::GetOverlappedResult(m_pipe, &ov, &written, FALSE))
                return false;
        }
        if (written == 0)
            return false;
        sent += written;
    }
    return true;
}

void CaptureSurfaceBridge::pipeLoop(HANDLE pipe, HANDLE stopEvent)
{
    try {
        HANDLE ioEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ioEvent)
            return;
        auto cleanupEvent = [&] { ::CloseHandle(ioEvent); };

        // 1. Wait for the UIAccess helper to connect.
        OVERLAPPED connOv {};
        connOv.hEvent = ioEvent;
        bool connected = ::ConnectNamedPipe(pipe, &connOv) != FALSE;
        if (!connected) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] {ioEvent, stopEvent};
                if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
                    DWORD unused = 0;
                    connected = ::GetOverlappedResult(pipe, &connOv, &unused, FALSE) != FALSE;
                } else {
                    ::CancelIoEx(pipe, &connOv);
                    DWORD unused = 0;
                    ::GetOverlappedResult(pipe, &connOv, &unused, TRUE);
                }
            }
        }
        if (!connected || m_stopping.load()) {
            cleanupEvent();
            return;
        }
        ULONG clientPid = 0;
        if (!::GetNamedPipeClientProcessId(pipe, &clientPid) || !clientPid) {
            cleanupEvent();
            QPointer<CaptureSurfaceBridge> self(this);
            QMetaObject::invokeMethod(this, [self] { if (self) self->onPipeBroken(); }, Qt::QueuedConnection);
            return;
        }
        {
            QPointer<CaptureSurfaceBridge> self(this);
            QMetaObject::invokeMethod(this, [self, pid = static_cast<DWORD>(clientPid)] {
                if (self)
                    self->onPipeConnected(pid);
            }, Qt::QueuedConnection);
        }

        // 2. Read length-prefixed frames until closed or stopped.
        std::vector<std::byte> rx;
        std::byte chunk[4096] {};
        while (!m_stopping.load()) {
            OVERLAPPED ov {};
            ov.hEvent = ioEvent;
            ::ResetEvent(ioEvent);
            DWORD got = 0;
            BOOL ok = ::ReadFile(pipe, chunk, sizeof chunk, &got, &ov);
            if (!ok) {
                const DWORD err = ::GetLastError();
                if (err != ERROR_IO_PENDING)
                    break;
                HANDLE waits[2] {ioEvent, stopEvent};
                if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
                    ::CancelIoEx(pipe, &ov);
                    ::GetOverlappedResult(pipe, &ov, &got, TRUE);
                    break;
                }
                if (!::GetOverlappedResult(pipe, &ov, &got, FALSE))
                    break;
            }
            if (got == 0)
                break;
            rx.insert(rx.end(), chunk, chunk + got);
            bool badFrame = false;
            while (rx.size() >= kHeaderBytes) {
                Type type {};
                std::uint32_t len = 0;
                std::span<const std::byte, kHeaderBytes> hdr(rx.data(), kHeaderBytes);
                if (!parseHeader(hdr, type, len)) {
                    badFrame = true;
                    break;
                }
                if (rx.size() < kHeaderBytes + len)
                    break;
                std::vector<std::byte> payload(rx.begin() + kHeaderBytes,
                    rx.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + len));
                rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + len));
                QPointer<CaptureSurfaceBridge> self(this);
                QMetaObject::invokeMethod(this, [self, type, payload = std::move(payload)]() mutable {
                    if (self)
                        self->onFrameReceived(type, std::move(payload));
                }, Qt::QueuedConnection);
            }
            if (badFrame)
                break;
        }
        cleanupEvent();
        if (!m_stopping.load()) {
            QPointer<CaptureSurfaceBridge> self(this);
            QMetaObject::invokeMethod(this, [self] { if (self) self->onPipeBroken(); }, Qt::QueuedConnection);
        }
    } catch (...) {
        if (!m_stopping.load()) {
            QPointer<CaptureSurfaceBridge> self(this);
            QMetaObject::invokeMethod(this, [self] { if (self) self->onPipeBroken(); }, Qt::QueuedConnection);
        }
    }
}

} // namespace ws::capture
