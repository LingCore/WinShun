#include "CaptureSession.h"

#include <QDateTime>
#include <QDebug>
#include <QGuiApplication>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws::capture {

// ---------------------------------------------------------------------------
// CaptureScreen

CaptureScreen::CaptureScreen(int index, QObject* parent)
    : QObject(parent)
    , m_index(index)
{
}

void CaptureScreen::setGeometry(const QRect& rect, qreal dpr, QSize logicalWindow)
{
    const qreal scaleX = 1.0 / bufferScale(logicalWindow.width(), dpr);
    const qreal scaleY = 1.0 / bufferScale(logicalWindow.height(), dpr);
    if (rect == m_rect && dpr == m_dpr && scaleX == m_scaleX && scaleY == m_scaleY)
        return;
    m_rect = rect;
    m_dpr = dpr;
    m_scaleX = scaleX;
    m_scaleY = scaleY;
    emit geometryChanged();
}

void CaptureScreen::setWorkArea(const QRect& rect)
{
    if (rect == m_work)
        return;
    m_work = rect;
    emit geometryChanged();
}

QRect CaptureScreen::workArea() const
{
    const QRect work = m_work.intersected(m_rect).translated(-m_rect.topLeft());
    return work.isEmpty() ? bounds() : work;
}

void CaptureScreen::setImage(const QImage& image)
{
    m_image = image;
    if (image.isNull()) {
        m_loupe = {};
        m_loupeImageKey = 0;
        m_color.clear();
        m_colorText.clear();
    }
    emit imageChanged();
}

void CaptureScreen::setState(const QRect& hover, const QRect& selection, bool dimmed, bool adjusting, bool magnifierShown)
{
    if (hover == m_hover && selection == m_selection && dimmed == m_dimmed && adjusting == m_adjusting
        && magnifierShown == m_magnifierShown)
        return;
    m_hover = hover;
    m_selection = selection;
    m_dimmed = dimmed;
    m_adjusting = adjusting;
    m_magnifierShown = magnifierShown;
    emit changed();
}

void CaptureScreen::setCursor(bool here, QPoint at, int shape)
{
    if (here == m_cursorHere && at == m_cursor && shape == m_cursorShape)
        return;
    const bool moved = here != m_cursorHere || at != m_cursor;
    m_cursorHere = here;
    m_cursor = at;
    m_cursorShape = shape;
    if (moved && here && !m_image.isNull()) {
        const bool inside = at.x() >= 0 && at.y() >= 0 && at.x() < m_image.width() && at.y() < m_image.height();
        m_rgb = inside ? m_image.pixel(at) : 0;
        m_color = inside ? colorHex(m_rgb) : QString();
        m_colorText = inside ? capture::colorText(m_rgb, m_colorFormat) : QString();
    }
    emit cursorChanged();
}

QImage CaptureScreen::loupe() const
{
    if (!m_cursorHere || m_image.isNull())
        return {};
    if (m_loupeAt == m_cursor && m_loupeImageKey == m_image.cacheKey())
        return m_loupe;
    constexpr int half = kLoupeCells / 2;
    QImage image(kLoupeCells, kLoupeCells, QImage::Format_RGB32);
    image.fill(Qt::black);
    for (int y = 0; y < kLoupeCells; ++y) {
        const int sy = m_cursor.y() - half + y;
        if (sy < 0 || sy >= m_image.height())
            continue;
        const auto* in = reinterpret_cast<const QRgb*>(m_image.constScanLine(sy));
        auto* out = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < kLoupeCells; ++x) {
            const int sx = m_cursor.x() - half + x;
            if (sx >= 0 && sx < m_image.width())
                out[x] = in[sx] | 0xFF000000u;
        }
    }
    m_loupe = image;
    m_loupeAt = m_cursor;
    m_loupeImageKey = m_image.cacheKey();
    return m_loupe;
}

void CaptureScreen::setColorFormat(ColorFormat format)
{
    if (m_colorFormat == format)
        return;
    m_colorFormat = format;
    if (!m_color.isEmpty()) {
        m_colorText = capture::colorText(m_rgb, format);
        emit cursorChanged();
    }
}

void CaptureScreen::setFlash(const QString& text)
{
    if (text == m_flash)
        return;
    m_flash = text;
    emit flashChanged();
}

void CaptureScreen::setNotice(const QString& text)
{
    if (text == m_notice)
        return;
    m_notice = text;
    emit noticeChanged();
}

QSize CaptureScreen::toPhysical(qreal width, qreal height) const
{
    return QSize(static_cast<int>(std::ceil(width / m_scaleX - 1e-6)), static_cast<int>(std::ceil(height / m_scaleY - 1e-6)));
}

QPointF CaptureScreen::toLogical(QPoint p) const
{
    return QPointF(p.x() * m_scaleX, p.y() * m_scaleY);
}

QPointF CaptureScreen::placeToolbar(const QRect& selection, qreal width, qreal height) const
{
    const int gap = static_cast<int>(std::lround(8 * m_dpr));
    return toLogical(capture::placeToolbar(selection, toPhysical(width, height), bounds(), gap).topLeft);
}

QPointF CaptureScreen::placeSizeLabel(const QRect& selection, qreal width, qreal height) const
{
    const int gap = static_cast<int>(std::lround(6 * m_dpr));
    return toLogical(capture::placeSizeLabel(selection, toPhysical(width, height), bounds(), gap));
}

QPointF CaptureScreen::placeMagnifier(const QPoint& cursor, qreal width, qreal height) const
{
    const int offset = static_cast<int>(std::lround(20 * m_dpr));
    return toLogical(capture::placeMagnifier(cursor, toPhysical(width, height), bounds(), offset));
}

// ---------------------------------------------------------------------------
// CaptureSession

CaptureSession::CaptureSession(QObject* parent)
    : QObject(parent)
    , m_annot(this)
{
    m_noticeTimer.setSingleShot(true);
    m_noticeTimer.setInterval(2500);
    connect(&m_noticeTimer, &QTimer::timeout, this, [this] {
        for (CaptureScreen* s : m_screens)
            s->setNotice({});
    });
    connect(&m_annot, &AnnotationModel::notice, this, &CaptureSession::showNotice);
    // The toolbar sets the tool on the model itself, not through publish():
    // a tool chosen counts as input (KeyHintPolicy rule 3).
    connect(&m_annot, &AnnotationModel::toolChanged, this, &CaptureSession::hintInput);
    for (const auto change : {&AnnotationModel::gestureChanged, &AnnotationModel::editingChanged,
             &AnnotationModel::numberEditChanged, &AnnotationModel::pickingChanged})
        connect(&m_annot, change, this, &CaptureSession::refreshKeyHints);
    m_hintClock.start();
    m_hintTimer.setSingleShot(true);
    connect(&m_hintTimer, &QTimer::timeout, this, &CaptureSession::refreshKeyHints);
}

QStringList CaptureSession::keyHintRows() const
{
    if (!m_active || m_phase == Phase::Dragging || m_phase == Phase::Adjusting || m_phase == Phase::Annotating)
        return {};
    if (m_annot.active() && (m_annot.gestureActive() || m_annot.pathActive() || m_annot.editingText()
            || m_annot.editingNumber() || m_annot.picking()))
        return {};
    QStringList rows;
    if (m_ocrOnly) // 识别屏幕文字: picking a region is all there is
        return {u"wheel"_s, u"wasd"_s, u"quit"_s};
    if (m_selection.screen < 0) { // a press not yet a drag too: what it was over
        rows = {u"wheel"_s, u"wasd"_s};
        if (!m_selectionHistory.empty())
            rows.append(u"recall"_s);
        if (m_historyAvailable)
            rows.append(u"history"_s);
        if (m_ocrAvailable)
            rows.append(u"ocr"_s);
        rows.append(u"quit"_s);
    } else if (m_annot.active() && m_annot.tool() >= 0) {
        // Shift: squares and circles, 15° lines, straight strokes; nothing for text, numbers or redaction.
        const auto kind = annot::tools().value(m_annot.tool()).kind;
        if (kind != annot::Kind::Text && kind != annot::Kind::Number && kind != annot::Kind::Redact)
            rows.append(u"shift"_s);
        rows.append({u"width"_s, u"undo"_s, u"delete"_s, u"toolOff"_s, u"hide"_s});
    } else {
        rows = {u"copy"_s};
        if (m_annot.keys().has(KeyAction::Save)) // none when the user cleared it (#187)
            rows.append(u"save"_s);
        if (m_annot.keys().has(KeyAction::Pin))
            rows.append(u"pin"_s);
        rows.append(u"arrows"_s);
        if (m_ocrAvailable)
            rows.append(u"ocr"_s);
        rows.append(u"long"_s);
        if (m_recalledSelection >= 0)
            rows.append(u"recalled"_s);
    }
    return rows;
}

void CaptureSession::refreshKeyHints()
{
    QStringList rows = keyHintRows();
    const qint64 now = hintNow();
    using Stage = KeyHintPolicy::Stage;
    m_hintPolicy.setStage(!m_active || rows.isEmpty() ? Stage::None
            : m_selection.screen >= 0                ? Stage::Selected
                                                     : Stage::Choosing,
        now);
    m_hintPolicy.tick(now);
    if (const qint64 due = m_hintPolicy.nextDeadline(); due >= 0)
        m_hintTimer.start(static_cast<int>(std::max<qint64>(0, due - now)));
    else
        m_hintTimer.stop();
    const bool shown = m_hintPolicy.shown();
    if (rows == m_keyHints && shown == m_hintsShown)
        return;
    m_keyHints = std::move(rows);
    m_hintsShown = shown;
    emit keyHintsChanged();
}

void CaptureSession::hintInput()
{
    m_hintPolicy.input(hintNow());
    refreshKeyHints();
}

void CaptureSession::setKeys(const CaptureKeys& keys)
{
    m_annot.setKeys(keys);
    refreshKeyHints();
}

void CaptureSession::setKeyHints(bool on)
{
    m_hintPolicy.setEnabled(on);
    refreshKeyHints();
}

void CaptureSession::quietKeyHints()
{
    m_hintPolicy.setQuiet();
    refreshKeyHints();
}

void CaptureSession::setOcrOnly(bool on)
{
    if (std::exchange(m_ocrOnly, on) == on)
        return;
    emit ocrOnlyChanged();
    refreshKeyHints();
}

void CaptureSession::setHistoryAvailable(bool on)
{
    m_historyAvailable = on;
    refreshKeyHints();
}

void CaptureSession::keepKeyHints(bool on)
{
    // The setting now, for this capture too: 不再显示 hides them, 一直显示 leaves them up.
    m_hintPolicy.keep(on);
    refreshKeyHints();
    emit keyHintsKept(on);
    if (!on && m_active)
        showNotice(tr("已关闭快捷键提示 · 按 ? 临时显示，截图设置里可重新打开"));
}



void CaptureSession::setStyles(const annot::StyleMemory& styles)
{
    m_styles = styles;
    m_stylesChanged = false;
}

annot::StyleMemory CaptureSession::styles() const
{
    return m_annot.active() ? m_annot.styles() : m_styles;
}

bool CaptureSession::stylesChanged() const
{
    return m_stylesChanged || (m_annot.active() && m_annot.stylesChanged());
}

void CaptureSession::setToolHintSeen(bool seen)
{
    if (std::exchange(m_toolHintSeen, seen) != seen)
        emit toolHintSeenChanged();
}

// A word by the selection, for a moment: on the monitor the annotations are
// on, else the one the cursor is on.
void CaptureSession::showNotice(const QString& text)
{
    const int target = m_selection.screen >= 0 ? m_selection.screen : m_parked ? m_parkedSelection.screen : m_cursorScreen;
    for (CaptureScreen* s : m_screens)
        s->setNotice(s->index() == target ? text : QString());
    m_noticeTimer.start();
}

// The model follows the selection: it begins when a selection is settled,
// moves with it, and ends when the selection goes without annotations.
void CaptureSession::syncAnnotation()
{
    const bool settled = m_selection.screen >= 0
        && (m_phase == Phase::Selected || m_phase == Phase::Annotating || m_phase == Phase::Adjusting);
    if (settled) {
        CaptureScreen* s = screenAt(m_selection.screen);
        if (s && (!m_annot.active() || m_annotScreen != m_selection.screen)) {
            retireAnnotation();
            m_annot.begin(s->image(), s->dpr(), m_styles);
            m_annotScreen = m_selection.screen;
        }
        if (m_annot.active())
            m_annot.setSelection(m_selection.rect);
    } else if (m_annot.active() && !m_parked && !m_annot.hasItems()) {
        retireAnnotation();
    }
}

void CaptureSession::retireAnnotation()
{
    if (m_annot.active()) {
        if (m_annot.stylesChanged()) {
            m_styles = m_annot.styles();
            m_stylesChanged = true;
        }
        m_annot.end();
    }
    m_annotScreen = -1;
}

void CaptureSession::dropParked()
{
    if (!m_parked)
        return;
    m_parked = false;
    m_parkedSelection = {};
    retireAnnotation();
}

void CaptureSession::begin(std::vector<CaptureScreen*> screens, Windows windows)
{
    m_pendingButton = Qt::NoButton;
    m_screens = std::move(screens);
    m_windows = std::move(windows);
    m_flyout = {};
    m_phase = Phase::Hover;
    m_releaseClock.invalidate();
    m_recalledSelection = -1;
    m_selection = {};
    m_previous = {};
    m_handle = Handle::None;
    m_cursorScreen = -1;
    m_cursorCovered = false;
    m_shiftAlone = false;
    m_hoverScreen = -1;
    m_chain.clear();
    m_level = 0;
    m_parked = false;
    m_parkedSelection = {};
    m_annot.end();
    m_annotScreen = -1;
    if (std::exchange(m_toolbarHidden, false))
        emit toolbarHiddenChanged();
    m_hintPolicy.begin(hintNow()); // afresh (rule 8); publish() below sets its stage
    for (CaptureScreen* s : m_screens) {
        s->setFlash({});
        s->setNotice({});
        s->setColorFormat(m_colorFormat);
    }
    const bool was = m_active;
    m_active = true;
    publish();
    if (!was)
        emit activeChanged();
    emit selectionChanged();
}

void CaptureSession::replaceShot(Windows windows, int screen, const QRect& rect, const std::optional<annot::Document>& doc)
{
    retireAnnotation();
    m_recalledSelection = -1;
    m_pendingButton = Qt::NoButton;
    m_windows = std::move(windows);
    m_flyout = {};
    m_selection = {};
    m_previous = {};
    m_handle = Handle::None;
    m_hoverScreen = -1;
    m_chain.clear();
    m_level = 0;
    m_parked = false;
    m_parkedSelection = {};
    m_phase = Phase::Hover;
    if (CaptureScreen* s = screenAt(screen); s && !rect.isEmpty() && s->bounds().contains(rect)) {
        m_selection = {screen, rect};
        m_phase = Phase::Selected;
        if (doc && !doc->items.isEmpty() && doc->canvas == s->image().size()) {
            m_annot.begin(s->image(), *doc, m_styles);
            m_annotScreen = screen;
        }
        syncAnnotation();
    }
    emit selectionChanged();
    if (m_cursorScreen >= 0) {
        if (m_phase == Phase::Hover)
            updateHover(m_cursorScreen, m_cursorAt);
        updateCursor(m_cursorScreen, m_cursorAt);
    }
    publish();
}

CaptureSession::Kept CaptureSession::kept() const
{
    const Pick& pick = m_parked ? m_parkedSelection : m_selection;
    Kept k {pick.screen, pick.rect, std::nullopt};
    if (m_annot.active() && m_annot.hasItems() && m_annotScreen == pick.screen)
        k.doc = m_annot.committedDocument();
    return k;
}

void CaptureSession::setFlyout(const QRect& rect)
{
    m_flyout = rect;
    if (m_cursorScreen >= 0 && m_phase == Phase::Hover) {
        m_hoverScreen = -1; // picked again with it
        updateHover(m_cursorScreen, m_cursorAt);
        publish();
    }
}

void CaptureSession::refreshHover()
{
    if (m_active && m_phase == Phase::Hover && m_cursorScreen >= 0) {
        updateHover(m_cursorScreen, m_cursorAt);
        publish();
    }
}

void CaptureSession::setColorFormat(ColorFormat format)
{
    if (std::exchange(m_colorFormat, format) == format)
        return;
    for (CaptureScreen* s : m_screens)
        s->setColorFormat(format);
    emit colorFormatChanged(format);
}

void CaptureSession::setOcrAvailable(bool on)
{
    if (std::exchange(m_ocrAvailable, on) != on)
        emit ocrAvailableChanged();
    refreshKeyHints();
}

void CaptureSession::setHiddenButtons(const QStringList& ids)
{
    if (m_hiddenButtons == ids)
        return;
    m_hiddenButtons = ids;
    qInfo() << "Screen capture: toolbar buttons left off:" << ids;
    emit hiddenButtonsChanged();
}

void CaptureSession::end()
{
    m_noticeTimer.stop();
    for (CaptureScreen* s : m_screens) {
        s->setState({}, {}, false, false, false);
        s->setCursor(false, {-1, -1}, Qt::CrossCursor);
        s->setFlash({});
        s->setNotice({});
    }
    retireAnnotation();
    m_parked = false;
    m_parkedSelection = {};
    m_screens.clear();
    m_windows = {};
    m_flyout = {};
    m_chain.clear();
    m_selection = {};
    m_previous = {};
    m_phase = Phase::Hover;
    if (std::exchange(m_ocrOnly, false))
        emit ocrOnlyChanged();
    if (std::exchange(m_active, false))
        emit activeChanged();
    emit selectionChanged();
    refreshKeyHints();
}

CaptureScreen* CaptureSession::screenAt(int index) const
{
    const auto found = std::ranges::find(m_screens, index, &CaptureScreen::index);
    return found != m_screens.end() ? *found : nullptr;
}

QRect CaptureSession::hoverRect() const
{
    return m_level < m_chain.size() ? m_chain[m_level] : QRect();
}

QPoint CaptureSession::clampTo(int screen, QPoint p) const
{
    const CaptureScreen* s = screenAt(screen);
    if (!s)
        return p;
    return QPoint(std::clamp(p.x(), 0, std::max(0, s->pixels().width() - 1)),
        std::clamp(p.y(), 0, std::max(0, s->pixels().height() - 1)));
}

int CaptureSession::reach(int screen) const
{
    const CaptureScreen* s = screenAt(screen);
    return static_cast<int>(std::lround(6 * (s ? s->dpr() : 1.0)));
}

int CaptureSession::dragStart(int screen) const
{
    const CaptureScreen* s = screenAt(screen);
    return std::max(2, static_cast<int>(std::lround(3 * (s ? s->dpr() : 1.0))));
}

void CaptureSession::updateHover(int screen, QPoint p)
{
    const CaptureScreen* s = screenAt(screen);
    if (!s)
        return;
    const QPoint origin = s->rect().topLeft();
    // The flyout, when there is one, first: it is over everything listed.
    const std::size_t above = m_flyout.isEmpty() ? 0 : 1;
    std::vector<QRect> tops;
    tops.reserve(m_windows.tops.size() + above);
    if (above)
        tops.push_back(m_flyout.translated(-origin));
    for (const QRect& r : m_windows.tops)
        tops.push_back(r.translated(-origin));
    const auto children = [this, origin, above](std::size_t i) {
        std::vector<QRect> list;
        if (m_windows.children && i >= above) {
            for (const QRect& r : m_windows.children(i - above))
                list.push_back(r.translated(-origin));
        }
        return list;
    };
    std::vector<QRect> chain = hitChain(tops, children, p, s->bounds());
    // The window the point is in: UI Automation may know more of it than its child windows.
    if (m_windows.probe && m_phase == Phase::Hover) {
        for (std::size_t i = above; i < tops.size(); ++i) {
            if (tops[i].intersected(s->bounds()).contains(p)) {
                m_windows.probe(i - above, origin + p);
                break;
            }
        }
    }
    // Stepped out with the wheel: stays out while that pick is still there.
    std::size_t level = 0;
    if (screen == m_hoverScreen && m_level > 0 && m_level < m_chain.size()) {
        const auto same = std::ranges::find(chain, m_chain[m_level]);
        if (same != chain.end())
            level = static_cast<std::size_t>(same - chain.begin());
    }
    m_hoverScreen = screen;
    m_chain = std::move(chain);
    m_level = level;
}

// The cursor over `p` of the selection's monitor, once it is settled: the
// selected annotation's handles first, then the selection's own, then what
// the annotations say (the tool's cross, a movable object), else the
// selection's (Move inside it).
int CaptureSession::cursorShapeAt(int screen, QPoint p) const
{
    const bool annotated = m_annot.active() && m_annotScreen == screen;
    if (annotated && m_annot.onHandle(p))
        return m_annot.cursorAt(p);
    const Handle handle = handleAt(m_selection.rect, p, reach(screen));
    if (annotated && (handle == Handle::None || handle == Handle::Move)) {
        if (const int shape = m_annot.cursorAt(p); shape >= 0)
            return shape;
    }
    return cursorFor(handle);
}

void CaptureSession::updateCursor(int screen, QPoint p)
{
    m_cursorScreen = screen;
    m_cursorAt = clampTo(screen, p);
    for (CaptureScreen* s : m_screens) {
        const bool here = s->index() == screen;
        int shape = Qt::CrossCursor;
        if (here && (m_phase == Phase::Selected || m_phase == Phase::Annotating) && m_selection.screen == screen)
            shape = cursorShapeAt(screen, m_cursorAt);
        else if (here && m_phase == Phase::Adjusting)
            shape = cursorFor(m_handle);
        s->setCursor(here, here ? m_cursorAt : QPoint(-1, -1), shape);
    }
}

void CaptureSession::publish()
{
    syncAnnotation();
    const bool pressedOverPrevious = m_phase == Phase::Pressed && m_previous.screen >= 0;
    const Pick shown = pressedOverPrevious ? m_previous : m_selection;
    const bool hovering = (m_phase == Phase::Hover || m_phase == Phase::Pressed) && !pressedOverPrevious;
    const bool adjusting = m_phase == Phase::Dragging || m_phase == Phase::Adjusting;
    // The magnifier while picking, drawing and adjusting; with a selection
    // made, only over it and its frame (beside it, on the handles outside,
    // on the toolbar or on another monitor it was only in the way).
    const CaptureScreen* cursorScreen = screenAt(m_cursorScreen);
    const int line = std::max(1, static_cast<int>(std::lround(cursorScreen ? cursorScreen->dpr() : 1.0)));
    const bool overShown = shown.screen == m_cursorScreen && shown.rect.adjusted(-line, -line, line, line).contains(m_cursorAt);
    const bool magnifier = m_magnifier && !m_cursorCovered && (hovering || adjusting || overShown);
    for (CaptureScreen* s : m_screens) {
        const QRect hover = hovering && s->index() == m_hoverScreen ? hoverRect() : QRect();
        const QRect selection = s->index() == shown.screen ? shown.rect : QRect();
        s->setState(hover, selection, m_active, adjusting, magnifier && s->index() == m_cursorScreen);
    }
    refreshKeyHints();
}

CaptureSession::Pick CaptureSession::hovered() const
{
    if (m_hoverScreen >= 0 && m_level < m_chain.size())
        return {m_hoverScreen, m_chain[m_level]};
    if (const CaptureScreen* s = m_cursorScreen >= 0 ? screenAt(m_cursorScreen) : nullptr)
        return {s->index(), s->bounds()};
    if (!m_screens.empty())
        return {m_screens.front()->index(), m_screens.front()->bounds()};
    return {};
}

void CaptureSession::pointerMove(int screen, QPoint p, Qt::KeyboardModifiers modifiers)
{
    if (!m_active || !screenAt(screen))
        return;
    if (m_pendingButton != Qt::NoButton
        && (screen != m_pendingScreen || (p - m_pendingAt).manhattanLength() > dragStart(screen)))
        m_pendingButton = Qt::NoButton; // moved before the release: not a click
    m_cursorCovered = false;
    // Windows sends moves without a move too (a window shown, a timer):
    // only a real one counts as the user's input for the hints.
    const bool moved = screen != m_cursorScreen || clampTo(screen, p) != m_cursorAt;
    switch (m_phase) {
    case Phase::Hover:
        updateHover(screen, clampTo(screen, p));
        break;
    case Phase::Pressed: {
        // Positions while the button is held come from the window pressed
        // in (Qt grabs the mouse there), beyond its edges too.
        const QPoint here = p + screenAt(screen)->rect().topLeft() - screenAt(m_pressScreen)->rect().topLeft();
        if ((here - m_pressAt).manhattanLength() < dragStart(m_pressScreen)) {
            if (m_previous.screen < 0)
                updateHover(screen, clampTo(screen, p));
            break;
        }
        m_phase = Phase::Dragging;
        m_previous = {};
        dropParked();
        m_selection = {m_pressScreen, spanRect(m_pressAt, clampTo(m_pressScreen, here))};
        emit selectionChanged();
        break;
    }
    case Phase::Dragging: {
        const QPoint here = p + screenAt(screen)->rect().topLeft() - screenAt(m_pressScreen)->rect().topLeft();
        m_selection.rect = spanRect(m_pressAt, clampTo(m_pressScreen, here));
        break;
    }
    case Phase::Adjusting: {
        const QPoint here = p + screenAt(screen)->rect().topLeft() - screenAt(m_pressScreen)->rect().topLeft();
        m_selection.rect = dragHandle(m_startRect, m_handle, here - m_pressAt, screenAt(m_pressScreen)->bounds());
        break;
    }
    case Phase::Annotating: {
        const QPoint here = p + screenAt(screen)->rect().topLeft() - screenAt(m_pressScreen)->rect().topLeft();
        m_annot.move(here, modifiers);
        break;
    }
    case Phase::Selected:
        if (m_annot.pathActive() && m_annotScreen == screen)
            m_annot.move(p, modifiers);
        break;
    }
    updateCursor(screen, p);
    if (moved)
        m_hintPolicy.input(hintNow()); // publish() below shows that
    publish();
}

void CaptureSession::pointerPress(int screen, QPoint p, Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    if (!m_active || !screenAt(screen))
        return;
    m_hintPolicy.press(); // any button: the hints go at once (KeyHintPolicy rule 2)
    refreshKeyHints();
    m_pendingButton = Qt::NoButton; // another press voids one waiting for its release
    m_shiftAlone = false; // Shift held for the press (a square, a straight line): not a Shift alone
    if (button == Qt::RightButton && m_annot.finishPath()) {
        m_phase = Phase::Selected;
        publish();
        return;
    }
    if (button == Qt::RightButton || button == Qt::MiddleButton || button == Qt::BackButton
        || button == Qt::ForwardButton) {
        mouseButton(button, screen, p);
        return;
    }
    if (button != Qt::LeftButton)
        return;
    p = clampTo(screen, p);
    m_pressScreen = screen;
    m_pressAt = p;
    if (m_phase == Phase::Selected) {
        // Who takes a press, in turn (3.6): a text being typed or a handle of
        // the selected annotation; the selection's own handles; the tools and
        // the annotations under the press; the selection itself (Move); a new
        // selection. Whoever asks first and says yes has it.
        const bool annotated = m_annot.active() && m_annotScreen == screen && m_selection.screen == screen;
        const auto annotating = [this] {
            m_phase = m_annot.gestureActive() ? Phase::Annotating : Phase::Selected;
        };
        if (annotated && m_annot.pressFirst(p, modifiers)) {
            annotating();
            updateCursor(screen, p);
            publish();
            return;
        }
        const Handle handle = m_selection.screen == screen ? handleAt(m_selection.rect, p, reach(screen)) : Handle::None;
        if (handle != Handle::None && handle != Handle::Move) {
            m_phase = Phase::Adjusting;
            m_handle = handle;
            m_startRect = m_selection.rect;
        } else if (annotated && m_annot.pressSecond(p, modifiers)) {
            annotating();
            updateCursor(screen, p);
            publish();
            return;
        } else if (handle == Handle::Move) {
            m_phase = Phase::Adjusting;
            m_handle = handle;
            m_startRect = m_selection.rect;
        } else if (m_annot.active() && m_annot.hasItems()) {
            // Annotations stay on the content they were drawn on: no new
            // selection over them; the edge moves the selection.
            showNotice(tr("已有标注：拖边框调整选区，Esc 放弃标注"));
            return;
        } else {
            // Outside: a drag draws a new selection, a click keeps this one.
            m_phase = Phase::Pressed;
            m_previous = m_selection;
        }
    } else {
        m_phase = Phase::Pressed;
        m_previous = {};
        updateHover(screen, p);
    }
    updateCursor(screen, p);
    publish();
}

void CaptureSession::pointerRelease(int screen, QPoint p, Qt::MouseButton button)
{
    if (m_active && button != Qt::LeftButton && button == m_pendingButton) {
        // The button that was pressed on a selection at rest is let go of on
        // the capture: now it acts (the release must not reach the window
        // below, which a closed capture would let it).
        m_pendingButton = Qt::NoButton;
        if (m_phase == Phase::Selected && m_selection.screen >= 0 && !annotationBusy())
            perform(m_pendingAct);
        return;
    }
    if (!m_active || button != Qt::LeftButton)
        return;
    m_releaseClock.start();
    switch (m_phase) {
    case Phase::Pressed:
        if (m_previous.screen >= 0) {
            m_selection = std::exchange(m_previous, {});
            m_phase = Phase::Selected;
            break;
        }
        dropParked();
        m_selection = hovered(); // a click picks the window or control under it
        m_phase = Phase::Selected;
        emit selectionChanged();
        if (m_ocrOnly || m_copyOnSelect) {
            finish(m_ocrOnly ? Action::OcrCopy : Action::Copy);
            return;
        }
        break;
    case Phase::Dragging:
        m_phase = Phase::Selected;
        if (m_ocrOnly) {
            finish(Action::OcrCopy);
            return;
        }
        if (m_copyOnSelect) {
            finish(Action::Copy);
            return;
        }
        break;
    case Phase::Adjusting:
        m_phase = Phase::Selected;
        m_handle = Handle::None;
        break;
    case Phase::Annotating: {
        // The press screen's own pixels, beyond its edges too (Qt grabs the mouse there).
        const CaptureScreen* from = screenAt(screen);
        const CaptureScreen* pressed = screenAt(m_pressScreen);
        const QPoint here = from && pressed ? p + from->rect().topLeft() - pressed->rect().topLeft() : p;
        m_annot.release(here); // a text opens for typing here, when it was only a click
        m_phase = Phase::Selected;
        break;
    }
    case Phase::Hover:
    case Phase::Selected:
        break;
    }
    if (screenAt(screen))
        updateCursor(screen, p);
    publish();
}

void CaptureSession::doubleClick(int screen, QPoint p)
{
    if (!m_active)
        return;
    if (m_annotScreen == screen && m_annot.finishPath()) {
        m_phase = Phase::Selected;
        publish();
        return;
    }
    // A text or a number under it opens for editing; with a tool on, and on
    // any annotation, it never copies (3.6).
    // Qt sends the second press of a double click as a press and then this,
    // so a gesture the press began (a move, a stroke) is still open: it was
    // only a click, drop it.
    if (m_phase == Phase::Annotating && m_annot.active() && m_annotScreen == screen) {
        m_annot.cancelGesture();
        m_phase = Phase::Selected;
        publish();
    }
    if (m_annot.active() && m_annotScreen == screen && m_selection.screen == screen
        && (m_phase == Phase::Selected || m_phase == Phase::Adjusting)
        && m_annot.doubleClick(clampTo(screen, p))) {
        if (m_phase == Phase::Adjusting) {
            m_selection.rect = m_startRect; // the press began a move that did not happen
            m_phase = Phase::Selected;
            publish();
        }
        return;
    }
    // The first click picked (or kept) the selection: the second copies it.
    if (m_selection.screen == screen && m_selection.rect.contains(clampTo(screen, p))) {
        if (m_phase == Phase::Adjusting) {
            m_selection.rect = m_startRect; // the press began a move that did not happen
            m_phase = Phase::Selected;
        }
        perform(m_actDoubleLeft);
    }
}

void CaptureSession::setMouseActions(const MouseActions& actions)
{
    m_actDoubleLeft = parseMouseAct(actions.doubleLeft, MouseAct::Copy);
    m_actRight = parseMouseAct(actions.right, MouseAct::Back);
    m_actMiddle = parseMouseAct(actions.middle, MouseAct::None);
    m_actBack = parseMouseAct(actions.back, MouseAct::None);
    m_actForward = parseMouseAct(actions.forward, MouseAct::None);
    // "back" is the right button's alone.
    if (m_actDoubleLeft == MouseAct::Back)
        m_actDoubleLeft = MouseAct::Copy;
    for (MouseAct* act : {&m_actMiddle, &m_actBack, &m_actForward}) {
        if (*act == MouseAct::Back)
            *act = MouseAct::None;
    }
}

CaptureSession::MouseAct CaptureSession::parseMouseAct(const QString& name, MouseAct fallback)
{
    static const std::pair<const char16_t*, MouseAct> names[] {{u"none", MouseAct::None}, {u"copy", MouseAct::Copy},
        {u"save", MouseAct::Save}, {u"pin", MouseAct::Pin}, {u"ocr", MouseAct::Ocr}, {u"cancel", MouseAct::Cancel},
        {u"back", MouseAct::Back}, {u"ocrcopy", MouseAct::OcrCopy}, {u"ocrsearch", MouseAct::OcrSearch},
        {u"ocrweb", MouseAct::OcrWeb}, {u"copypin", MouseAct::CopyPin}};
    for (const auto& [text, act] : names) {
        if (name == QStringView(text))
            return act;
    }
    return fallback;
}

bool CaptureSession::annotationBusy() const
{
    return m_annot.active()
        && (m_annot.tool() >= 0 || m_annot.gestureActive() || m_annot.editingText() || m_annot.editingNumber());
}

// Right, middle and the side buttons. The right button always has a say
// (without the settings' action it is Esc's step back); the others act only
// on a selection at rest, and never in the middle of drawing.
void CaptureSession::mouseButton(Qt::MouseButton button, int screen, QPoint p)
{
    const bool atRest = m_phase == Phase::Selected && m_selection.screen >= 0 && !annotationBusy();
    if (button == Qt::RightButton && (!atRest || m_actRight == MouseAct::Back)) {
        back();
        return;
    }
    if (!atRest)
        return;
    // The action waits for the same button's release on the capture (a side
    // button let go of over the window below would go Back in a browser).
    m_pendingAct = button == Qt::RightButton ? m_actRight
        : button == Qt::MiddleButton         ? m_actMiddle
        : button == Qt::BackButton           ? m_actBack
                                             : m_actForward;
    if (m_pendingAct == MouseAct::None)
        return;
    m_pendingButton = button;
    m_pendingScreen = screen;
    m_pendingAt = p;
}

void CaptureSession::perform(MouseAct act)
{
    switch (act) {
    case MouseAct::Copy:
        finish(Action::Copy);
        break;
    case MouseAct::CopyPin:
        finish(Action::CopyPin);
        break;
    case MouseAct::Save:
        finish(Action::Save);
        break;
    case MouseAct::Pin:
        finish(Action::Pin);
        break;
    case MouseAct::Ocr:
        finish(Action::Ocr);
        break;
    case MouseAct::OcrCopy:
        finish(Action::OcrCopy);
        break;
    case MouseAct::OcrSearch:
        finish(Action::OcrSearch);
        break;
    case MouseAct::OcrWeb:
        finish(Action::OcrWeb);
        break;
    case MouseAct::Cancel:
        finish(Action::Cancel);
        break;
    case MouseAct::Back:
        back();
        break;
    case MouseAct::None:
        break;
    }
}

void CaptureSession::wheel(int screen, QPoint p, int steps)
{
    if (!m_active || m_phase != Phase::Hover || steps == 0)
        return;
    if (screen != m_hoverScreen)
        updateHover(screen, clampTo(screen, p));
    if (m_chain.empty())
        return;
    const auto last = static_cast<long long>(m_chain.size()) - 1;
    m_level = static_cast<std::size_t>(std::clamp(static_cast<long long>(m_level) + steps, 0LL, last));
    publish();
}

void CaptureSession::wheelAngle(int screen, QPoint p, int angleDelta, qint64 nowMs)
{
    if (!m_active || angleDelta == 0)
        return;
    m_shiftAlone = false;
    hintInput();
    if (m_annot.active() && m_annotScreen == screen && m_selection.screen == screen
        && (m_phase == Phase::Selected || m_phase == Phase::Annotating) && m_annot.wheel(angleDelta, nowMs)) {
        publish();
        return;
    }
    wheel(screen, p, angleDelta > 0 ? 1 : -1);
}

bool CaptureSession::key(int key, Qt::KeyboardModifiers modifiers)
{
    if (!m_active)
        return false;
    const bool ctrl = modifiers & Qt::ControlModifier;
    const bool shift = modifiers & Qt::ShiftModifier;
    const bool busy = m_phase == Phase::Pressed || m_phase == Phase::Dragging || m_phase == Phase::Adjusting
        || m_phase == Phase::Annotating;
    // Shift with no other modifier may be a Shift alone (keyReleased); any other key ends that.
    m_shiftAlone = key == Qt::Key_Shift && !(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    const bool plain = !(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    // ? (the / key, with Shift or not) and F1: the key hints, at any time.
    // F1 is often another screenshot tool's hotkey, so ? is the one shown.
    if (plain && (key == Qt::Key_Slash || key == Qt::Key_Question || key == Qt::Key_F1)) {
        if (m_annot.editingText() || m_annot.editingNumber())
            return false; // typed
        m_hintPolicy.toggle();
        refreshKeyHints();
        return true;
    }
    // Any other key (not a modifier alone, which comes before one) is input for the hints.
    if (key != Qt::Key_Shift && key != Qt::Key_Control && key != Qt::Key_Alt && key != Qt::Key_Meta)
        hintInput();
    const bool wasd = key == Qt::Key_W || key == Qt::Key_A || key == Qt::Key_S || key == Qt::Key_D;
    if (m_ocrOnly) {
        // 识别屏幕文字 (#198): the pointer (WASD, the arrows), Enter for what is
        // under it, Shift+C as Enter, C for the colour the magnifier shows, Esc.
        // Nothing else: no picture is taken, saved, pinned or brought back
        // here, and R would leave a selection nothing finishes.
        const int step = shift ? 10 : 1;
        switch (key) {
        case Qt::Key_Escape:
            back();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (!busy && plain)
                finish(Action::OcrCopy);
            return true;
        case Qt::Key_W:
        case Qt::Key_A:
        case Qt::Key_S:
        case Qt::Key_D:
            if (plain)
                movePointer((key == Qt::Key_A ? -1 : key == Qt::Key_D ? 1 : 0) * step,
                    (key == Qt::Key_W ? -1 : key == Qt::Key_S ? 1 : 0) * step);
            return true;
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
            // The pointer as WASD, with a button held too (the drag goes along).
            if (plain && (busy || m_selection.screen < 0))
                movePointer((key == Qt::Key_Left ? -1 : key == Qt::Key_Right ? 1 : 0) * step,
                    (key == Qt::Key_Up ? -1 : key == Qt::Key_Down ? 1 : 0) * step);
            return true;
        case Qt::Key_C:
            if (busy || !plain)
                return true;
            if (shift) {
                finish(Action::OcrCopy);
            } else if (CaptureScreen* s = screenAt(m_cursorScreen); s && s->magnifierShown() && !s->colorText().isEmpty()) {
                emit colorCopied(s->colorText());
                finish(Action::Cancel);
            }
            return true;
        case Qt::Key_Shift:
        case Qt::Key_Control:
        case Qt::Key_Alt:
        case Qt::Key_Meta:
            return false;
        default:
            return true;
        }
    }
    // Crop keys take precedence over the tools' letters in the overlay.
    // The editor keeps its own tool keys; 1 and 3 still select rectangle/arrow here.
    if (plain && (wasd || key == Qt::Key_R)) {
        if (m_annot.editingText() || m_annot.editingNumber())
            return false;
        if (key == Qt::Key_R) {
            if (!busy)
                recallSelection(shift);
            return true;
        }
        // WASD is the mouse pointer in every phase (a pixel, Shift ten); with a
        // button held the system's mouse move it causes carries the drag,
        // the adjustment or the stroke along.
        const int step = shift ? 10 : 1;
        movePointer((key == Qt::Key_A ? -1 : key == Qt::Key_D ? 1 : 0) * step,
            (key == Qt::Key_W ? -1 : key == Qt::Key_S ? 1 : 0) * step);
        return true;
    }
    // The tools, undo, delete and the arrows on a selected annotation come
    // first; what the model leaves is the selection's and the capture's own.
    if (m_annot.active() && (m_phase == Phase::Selected || m_phase == Phase::Annotating) && key != Qt::Key_Escape
        && m_annot.key(key, modifiers)) {
        publish();
        return true;
    }
    // The pin and save keys, as the user set them (#187; Ctrl+S, F3 by
    // default). CaptureKeys::problem keeps them off every key below and off
    // the plain letters above, so plain S still moves the pointer.
    if (const auto action = m_annot.keys().triggered(KeyContext::Capture, key, modifiers)) {
        if (*action == KeyAction::Save && busy)
            return false;
        if (!busy)
            finish(*action == KeyAction::Pin ? Action::Pin : Action::Save);
        return true;
    }
    switch (key) {
    case Qt::Key_Escape:
        back();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!busy)
            finish(Action::Copy);
        return true;
    case Qt::Key_L:
        if (!ctrl)
            return false;
        if (!busy)
            act(static_cast<int>(Action::Long));
        return true;
    case Qt::Key_H:
        if (ctrl || busy || m_selection.screen < 0)
            return false;
        m_toolbarHidden = !m_toolbarHidden;
        emit toolbarHiddenChanged();
        return true;
    case Qt::Key_Z:
        // Ctrl+Z after Esc put the annotations down: pick them up again.
        if (ctrl && !shift && m_parked && m_phase == Phase::Hover) {
            m_selection = std::exchange(m_parkedSelection, {});
            m_parked = false;
            m_phase = Phase::Selected;
            for (CaptureScreen* s : m_screens)
                s->setNotice({});
            emit selectionChanged();
            if (m_cursorScreen >= 0)
                updateCursor(m_cursorScreen, m_cursorAt);
            publish();
            return true;
        }
        return false;
    case Qt::Key_C: {
        if (busy)
            return true;
        if (ctrl) {
            finish(Action::Copy);
            return true;
        }
        if (shift) {
            // Shift+C: its words to the clipboard, as in PixPin (识别文字并复制).
            finish(Action::OcrCopy);
            return true;
        }
        // C goes with the magnifier, which shows the colour: copied, and the
        // capture ends, as Ctrl+C ends it with the picture (as in Snipaste and
        // PixPin). Annotations are kept, as when the hotkey ends it.
        CaptureScreen* s = screenAt(m_cursorScreen);
        if (s && s->magnifierShown() && !s->colorText().isEmpty()) {
            emit colorCopied(s->colorText());
            finish(Action::Cancel);
        }
        return true;
    }
    case Qt::Key_R:
        if (ctrl && !shift && !busy) {
            act(static_cast<int>(Action::Record));
            return true;
        }
        return false;
    case Qt::Key_Q:
        if (ctrl || shift)
            return false;
        if (!busy)
            act(static_cast<int>(Action::Qr));
        return true;
    case Qt::Key_Comma:
    case Qt::Key_Less:
    case Qt::Key_Period:
    case Qt::Key_Greater:
        // The history (PixPin's and Snipaste's keys): not while a text or a stroke is under way.
        if (ctrl || modifiers & Qt::AltModifier)
            return false;
        if (!busy && !(m_annot.active() && (m_annot.gestureActive() || m_annot.editingText() || m_annot.editingNumber())))
            emit historyStep(key == Qt::Key_Comma || key == Qt::Key_Less ? 1 : -1);
        return true;
    case Qt::Key_D:
        if (!ctrl || !shift || modifiers & (Qt::AltModifier | Qt::MetaModifier))
            return false;
        if (!busy)
            act(static_cast<int>(Action::Delay));
        return true;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const int dx = key == Qt::Key_Left ? -1 : key == Qt::Key_Right ? 1 : 0;
        const int dy = key == Qt::Key_Up ? -1 : key == Qt::Key_Down ? 1 : 0;
        if (busy) {
            // A button is held (dragging the selection out, an edge, a stroke):
            // nothing to nudge, so the arrows are the pointer as WASD is, a
            // pixel or ten with Shift, and the drag goes along. They did
            // nothing here, while moving the pointer before the press.
            if (plain && !m_annot.editingText() && !m_annot.editingNumber())
                movePointer(dx * (shift ? 10 : 1), dy * (shift ? 10 : 1));
            return true;
        }
        nudgeSelection(dx, dy, shift);
        return true;
    }
    default:
        return false;
    }
}

void CaptureSession::movePointer(int dx, int dy)
{
    // m_cursorAt follows every pointer move, in the held-button phases too.
    if (const CaptureScreen* cursorScreen = screenAt(m_cursorScreen))
        emit cursorMoveRequested(cursorScreen->origin() + clampTo(m_cursorScreen, m_cursorAt + QPoint(dx, dy)));
}

void CaptureSession::nudgeSelection(int dx, int dy, bool resize)
{
    if (CaptureScreen* s = screenAt(m_selection.screen)) {
        const QRect moved = nudge(m_selection.rect, dx, dy, resize, s->bounds());
        if (moved != m_selection.rect) {
            m_selection.rect = moved;
            emit selectionChanged();
            publish();
        }
    } else if (const CaptureScreen* cursorScreen = screenAt(m_cursorScreen)) {
        // Nothing selected: the cursor itself, for the magnifier.
        emit cursorMoveRequested(cursorScreen->origin() + clampTo(m_cursorScreen, m_cursorAt + QPoint(dx, dy)));
    }
}

void CaptureSession::rememberSelection(const QRect& rect)
{
    if (rect.isEmpty())
        return;
    std::erase(m_selectionHistory, rect);
    m_selectionHistory.insert(m_selectionHistory.begin(), rect);
    if (m_selectionHistory.size() > kSelectionHistoryLimit)
        m_selectionHistory.resize(kSelectionHistoryLimit);
    m_recalledSelection = -1;
    emit selectionHistoryChanged();
    emit selectionChanged();
    refreshKeyHints();
}

void CaptureSession::recallSelection(bool previous)
{
    if (hasAnnotations() && !m_parked) {
        showNotice(tr("先完成或放弃标注，再恢复选区"));
        return;
    }
    std::vector<QRect> monitors;
    for (const CaptureScreen* s : m_screens)
        monitors.push_back(s->rect());
    const int count = selectionHistoryCount();
    for (int step = 1; step <= count; ++step) {
        const int at = m_recalledSelection < 0 ? step - 1
            : (m_recalledSelection + (previous ? -step : step) + count) % count;
        const auto last = lastSelectionOn(m_selectionHistory[static_cast<std::size_t>(at)], monitors);
        if (!last)
            continue; // Its monitor may have been disconnected; try the next region.
        dropParked();
        retireAnnotation();
        m_recalledSelection = at;
        m_selection = {m_screens[static_cast<std::size_t>(last->monitor)]->index(), last->rect};
        m_previous = {};
        m_phase = Phase::Selected;
        emit selectionChanged();
        if (m_cursorScreen >= 0)
            updateCursor(m_cursorScreen, m_cursorAt);
        publish();
        for (CaptureScreen* s : m_screens)
            s->setNotice({}); // The persistent keyboard strip shows the index, away from the size label.
        return;
    }
    showNotice(count == 0 ? tr("还没有最近选区，完成一次截图后即可恢复") : tr("最近选区所在的显示器已断开"));
}

void CaptureSession::back()
{
    if (m_ocrOnly) { // 识别屏幕文字: nothing to step back to
        finish(Action::Cancel);
        return;
    }
    // Esc, in steps (3.6): a text under way is kept and closed, a gesture
    // under way is dropped; only then the selection and its annotations.
    if (m_annot.active() && (m_phase == Phase::Selected || m_phase == Phase::Annotating) && m_annot.escape()) {
        if (m_phase == Phase::Annotating)
            m_phase = Phase::Selected;
        if (m_cursorScreen >= 0)
            updateCursor(m_cursorScreen, m_cursorAt);
        publish();
        return;
    }
    switch (m_phase) {
    case Phase::Annotating:
        m_phase = Phase::Selected;
        break;
    case Phase::Pressed:
    case Phase::Dragging:
    case Phase::Adjusting:
        // A drag let go of: back to how it was before the press.
        if (m_phase == Phase::Adjusting)
            m_selection.rect = m_startRect;
        else if (m_previous.screen >= 0)
            m_selection = std::exchange(m_previous, {});
        else
            m_selection = {};
        m_phase = m_selection.screen >= 0 ? Phase::Selected : Phase::Hover;
        break;
    case Phase::Selected:
        m_recalledSelection = -1;
        if (m_annot.active() && m_annot.hasItems()) {
            // The selection and its annotations are put down together; Ctrl+Z
            // brings them back as they were.
            m_parked = true;
            m_parkedSelection = m_selection;
            m_annot.setTool(-1);
            m_selection = {};
            m_phase = Phase::Hover;
            showNotice(tr("已放弃这次的标注 · Ctrl+Z 恢复"));
        } else {
            m_selection = {};
            m_phase = Phase::Hover;
        }
        if (m_cursorScreen >= 0)
            updateHover(m_cursorScreen, m_cursorAt);
        break;
    case Phase::Hover:
        finish(Action::Cancel);
        return;
    }
    emit selectionChanged();
    if (m_cursorScreen >= 0)
        updateCursor(m_cursorScreen, m_cursorAt);
    publish();
}

void CaptureSession::finish(Action action)
{
    if (action == Action::Copy && m_pinOnCopy)
        action = Action::CopyPin;
    // A save folder chosen: Save (button, key, mouse) goes straight into it,
    // as before 0.18; 另存为… asks. Past here Save means the picker (#194).
    if (action == Action::Save && saveDirect())
        action = Action::SavePrevious;
    else if (action == Action::SaveAs)
        action = Action::Save;
    if (m_annot.active())
        m_annot.commitPending(); // the words typed so far are part of the picture
    Pick pick;
    if (action != Action::Cancel && action != Action::Delay)
        pick = m_selection.screen >= 0 ? m_selection : hovered();
    if (action != Action::Cancel && action != Action::Delay && (pick.screen < 0 || pick.rect.isEmpty()))
        action = Action::Cancel;
    // Save first asks for a destination. The controller remembers its region
    // only once the picker is accepted; cancelling keeps this capture intact.
    if (const CaptureScreen* s = screenAt(pick.screen); s && action != Action::Cancel && action != Action::Delay && action != Action::Save)
        rememberSelection(pick.rect.translated(s->origin()));
    emit finished(action, pick.screen, pick.rect);
}

void CaptureSession::mouseMoved(int screen, qreal x, qreal y)
{
    if (const CaptureScreen* s = screenAt(screen))
        pointerMove(screen, QPoint(toPhysical(x, s->dpr()), toPhysical(y, s->dpr())), QGuiApplication::keyboardModifiers());
}

void CaptureSession::mousePressed(int screen, qreal x, qreal y, int button)
{
    if (const CaptureScreen* s = screenAt(screen)) {
        pointerPress(screen, QPoint(toPhysical(x, s->dpr()), toPhysical(y, s->dpr())), static_cast<Qt::MouseButton>(button),
            QGuiApplication::keyboardModifiers());
    }
}

void CaptureSession::mouseReleased(int screen, qreal x, qreal y, int button)
{
    if (const CaptureScreen* s = screenAt(screen))
        pointerRelease(screen, QPoint(toPhysical(x, s->dpr()), toPhysical(y, s->dpr())), static_cast<Qt::MouseButton>(button));
}

void CaptureSession::mouseDoubleClicked(int screen, qreal x, qreal y)
{
    if (const CaptureScreen* s = screenAt(screen))
        doubleClick(screen, QPoint(toPhysical(x, s->dpr()), toPhysical(y, s->dpr())));
}

void CaptureSession::mouseLeft(int screen)
{
    // With a button held the picture keeps the mouse (and its moves) beyond its edges.
    if (!m_active || screen != m_cursorScreen || (m_phase != Phase::Hover && m_phase != Phase::Selected))
        return;
    m_cursorCovered = true;
    publish();
}

void CaptureSession::wheeled(int screen, qreal x, qreal y, int angleDeltaY)
{
    const CaptureScreen* s = screenAt(screen);
    if (!s || angleDeltaY == 0)
        return;
    wheelAngle(screen, QPoint(toPhysical(x, s->dpr()), toPhysical(y, s->dpr())), angleDeltaY, QDateTime::currentMSecsSinceEpoch());
}

bool CaptureSession::keyPressed(int key, int modifiers, bool autoRepeat)
{
    if (m_active && key == Qt::Key_R && autoRepeat
        && !(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
        return true; // One press recalls one region; WASD still repeats while held.
    return this->key(key, Qt::KeyboardModifiers(modifiers));
}

void CaptureSession::keyReleased(int key)
{
    // Shift alone, as in PixPin and Snipaste: the colour's next format (HEX,
    // RGB, HSL), kept for the next capture; only where the magnifier shows it.
    if (!m_active || key != Qt::Key_Shift || !std::exchange(m_shiftAlone, false))
        return;
    CaptureScreen* s = screenAt(m_cursorScreen);
    if ((m_phase != Phase::Hover && m_phase != Phase::Selected) || !s || !s->magnifierShown())
        return;
    setColorFormat(nextColorFormat(m_colorFormat));
    s->setFlash(tr("颜色格式：%1").arg(colorFormatName(m_colorFormat).toUpper()));
}

void CaptureSession::act(int action)
{
    if (!m_active || action < static_cast<int>(Action::Copy) || action > static_cast<int>(Action::SaveAs))
        return;
    if (action == static_cast<int>(Action::Long) || action == static_cast<int>(Action::Record)) {
        // Its picture is the screen's, scrolled: annotations would be lost.
        if (hasAnnotations()) {
            showNotice(tr("先完成或放弃标注"));
            return;
        }
        const Pick pick = m_selection.screen >= 0 ? m_selection : hovered();
        if (pick.screen >= 0 && (pick.rect.width() < kLongMinimum || pick.rect.height() < kLongMinimum)) {
            showNotice(action == static_cast<int>(Action::Record) ? tr("选区太小，不能录屏") : tr("选区太小，不能长截图"));
            return;
        }
    }
    finish(static_cast<Action>(action));
}

} // namespace ws::capture
