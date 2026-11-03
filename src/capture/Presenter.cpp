#include "Presenter.h"

#include "Geometry.h"

#include <QPainter>
#include <QPixmap>
#include <QScopedValueRollback>

#include <windows.h>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws::capture {

using presenter::Board;
using presenter::Mode;
using annot::Kind;

namespace {

// The models' picture: the compositor draws on nothing (transparent), so
// nothing ever reads it; it only gives the canvas its size. Zero pages from
// VirtualAlloc are in no working set until written, so even at 4K it costs
// commit (33 MB), not memory (docs/design-presenter.md 4.3).
QImage zeroPicture(QSize size)
{
    if (size.isEmpty())
        return {};
    const qsizetype stride = qsizetype(size.width()) * 4;
    void* bits = ::VirtualAlloc(nullptr, SIZE_T(stride) * SIZE_T(size.height()), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!bits) {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        return image;
    }
    return QImage(static_cast<uchar*>(bits), size.width(), size.height(), stride, QImage::Format_ARGB32_Premultiplied,
        [](void* p) { ::VirtualFree(p, 0, MEM_RELEASE); }, bits);
}

// A tool's remembered style as a change for the panel (AnnotationModel::setPanel).
QVariantMap panelOf(const annot::StyleMemory::Tool& t)
{
    return {
        {u"color"_s, colorHex(t.color)},
        {u"width"_s, t.width},
        {u"fill"_s, annot::fillName(t.fill)},
        {u"size"_s, t.size},
        {u"bold"_s, t.bold},
        {u"boxed"_s, t.boxed},
        {u"stroke"_s, t.stroke},
        {u"strokeColor"_s, t.strokeColor != 0 ? colorHex(t.strokeColor) : u"auto"_s},
        {u"lineMode"_s, annot::lineModeName(t.lineMode)},
        {u"markerMode"_s, annot::markerModeName(t.markerMode)},
    };
}

// Equal as far as the panel can make them (the number format is the document's: left alone).
bool sameTool(annot::StyleMemory::Tool a, const annot::StyleMemory::Tool& b)
{
    a.format = b.format;
    return a == b;
}

// A round cursor of `diameter` logical px in `color`, ringed so it shows on any background.
QCursor dotCursor(QColor color, qreal diameter, qreal dpr)
{
    const qreal d = std::clamp(diameter, 4.0, 64.0);
    const int side = int(std::ceil((d + 4) * dpr)) | 1; // odd: the hot spot on a pixel's middle
    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPointF centre(side / 2.0, side / 2.0);
        const qreal r = d * dpr / 2;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 110));
        painter.drawEllipse(centre, r + 2, r + 2);
        painter.setBrush(Qt::white);
        painter.drawEllipse(centre, r + 1, r + 1);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.setBrush(color);
        painter.drawEllipse(centre, r, r);
    }
    pixmap.setDevicePixelRatio(dpr);
    return QCursor(pixmap);
}

} // namespace

// ---- PresenterScreen ---------------------------------------------------------------------

PresenterScreen::PresenterScreen(Presenter* presenter, int index, QSize pixels, qreal dpr)
    : QObject(presenter)
    , m_presenter(presenter)
    , m_index(index)
    , m_pixels(pixels)
    , m_dpr(dpr > 0 ? dpr : 1.0)
{
    m_desk.compositor().setTransparent(true);
    presenter->watch(&m_desk);
}

PresenterScreen::~PresenterScreen() = default;

void PresenterScreen::setCursorItem(QQuickItem* item)
{
    if (item == m_cursorItem)
        return;
    m_cursorItem = item;
    m_cursorLook.reset();
    emit cursorItemChanged();
}

void PresenterScreen::begin(const QImage& base, const annot::StyleMemory& styles, int tool)
{
    m_base = base;
    m_desk.begin(base, m_dpr, styles);
    m_desk.setSelection(QRect(QPoint(0, 0), m_pixels)); // all of the monitor takes ink
    m_desk.setTool(tool);
}

void PresenterScreen::end()
{
    const bool hadBoard = m_boardKind != Board::None || m_board;
    m_boardKind = Board::None;
    std::unique_ptr<AnnotationModel> board = std::move(m_board);
    if (hadBoard)
        emit boardChanged(); // the windows let go of it before it goes
    board.reset();
    m_desk.end();
    m_base = QImage();
    m_pressed = false;
}

void PresenterScreen::setBoard(Board board, const annot::StyleMemory& styles, int tool)
{
    if (board == m_boardKind)
        return;
    if (board != Board::None && !m_board) {
        m_board = std::make_unique<AnnotationModel>();
        m_board->compositor().setTransparent(true);
        m_board->begin(m_base, m_dpr, styles);
        m_board->setSelection(QRect(QPoint(0, 0), m_pixels));
        m_board->setTool(tool);
        m_presenter->watch(m_board.get());
    }
    m_boardKind = board;
    m_pressed = false;
    emit boardChanged();
}

QPoint PresenterScreen::imageAt(qreal x, qreal y) const
{
    const int px = std::clamp(int(std::floor(x * m_dpr)), 0, std::max(0, m_pixels.width() - 1));
    const int py = std::clamp(int(std::floor(y * m_dpr)), 0, std::max(0, m_pixels.height() - 1));
    return {px, py};
}

void PresenterScreen::showCursor(QPoint image)
{
    if (!m_cursorItem)
        return;
    const CursorLook look = m_presenter->cursorLookAt(this, image);
    if (look == m_cursorLook)
        return;
    m_cursorLook = look;
    m_cursorItem->setCursor(Presenter::cursorOf(look, m_dpr));
}

void PresenterScreen::mousePressed(qreal x, qreal y, int button, int modifiers)
{
    if (m_presenter->mode() != Mode::Draw)
        return;
    m_presenter->activate(this);
    const auto which = static_cast<Qt::MouseButton>(button);
    if (which == Qt::RightButton) {
        m_presenter->back();
        return;
    }
    if (which != Qt::LeftButton || m_presenter->laser())
        return; // the laser is the window's own (LaserTrail), never the model's
    AnnotationModel* m = model();
    const QPoint p = imageAt(x, y);
    const Qt::KeyboardModifiers mods(modifiers);
    m_pressed = true;
    if (!m->pressFirst(p, mods))
        m->pressSecond(p, mods);
    showCursor(p);
}

void PresenterScreen::mouseMoved(qreal x, qreal y, int modifiers)
{
    if (m_presenter->mode() != Mode::Draw)
        return;
    AnnotationModel* m = model();
    const QPoint p = imageAt(x, y);
    if (m_pressed || m->gestureActive())
        m->move(p, Qt::KeyboardModifiers(modifiers));
    showCursor(p);
}

void PresenterScreen::mouseReleased(qreal x, qreal y, int button)
{
    if (static_cast<Qt::MouseButton>(button) != Qt::LeftButton || !m_pressed)
        return;
    m_pressed = false;
    const QPoint p = imageAt(x, y);
    AnnotationModel* m = model();
    m->release(p);
    // Ink is ink: a freehand stroke is not left selected with handles (a
    // shape is, to be adjusted at once, as in a screenshot).
    if (m->tool() == int(Kind::Pen) || m->tool() == int(Kind::Marker))
        m_presenter->deselect(m);
    showCursor(p);
}

void PresenterScreen::mouseDoubleClicked(qreal x, qreal y)
{
    if (m_presenter->mode() != Mode::Draw || m_presenter->laser())
        return;
    AnnotationModel* m = model();
    // The second press began a gesture that the double click replaces.
    if (m->gestureActive() && !m->pathActive())
        m->cancelGesture();
    m_pressed = false;
    m->doubleClick(imageAt(x, y));
}

void PresenterScreen::wheeled(int angleDelta)
{
    if (m_presenter->mode() != Mode::Draw)
        return;
    m_presenter->activate(this);
    model()->wheel(angleDelta, m_presenter->m_clock.elapsed());
}

bool PresenterScreen::keyPressed(int key, int modifiers, bool autoRepeat)
{
    if (m_presenter->mode() != Mode::Draw)
        return false;
    return m_presenter->commandKey(this, key, Qt::KeyboardModifiers(modifiers), autoRepeat);
}

// ---- Presenter ---------------------------------------------------------------------------

Presenter::Presenter(QObject* parent)
    : QObject(parent)
{
    m_clock.start();
}

Presenter::~Presenter()
{
    // The screens' models first: their signals reach this while it is still whole.
    m_screens.clear();
}

void Presenter::setHooks(Hooks hooks)
{
    m_hooks = std::move(hooks);
}

PresenterScreen* Presenter::screen(int index) const
{
    return index >= 0 && index < int(m_screens.size()) ? m_screens[std::size_t(index)].get() : nullptr;
}

PresenterScreen* Presenter::current() const
{
    return screen(m_current);
}

AnnotationModel* Presenter::model() const
{
    PresenterScreen* s = current();
    return s ? s->model() : nullptr;
}

int Presenter::board() const
{
    PresenterScreen* s = current();
    return s ? int(s->board()) : 0;
}

bool Presenter::canThrough() const
{
    return std::ranges::none_of(m_screens, [](const auto& s) { return s->board() != Board::None; });
}

bool Presenter::canUndo() const
{
    if (!isOpen())
        return false;
    for (const auto& s : m_screens) {
        if (s->desk()->canUndo() || (s->boardModel() && s->boardModel()->canUndo()))
            return true;
    }
    return false;
}

bool Presenter::canRedo() const
{
    if (!isOpen())
        return false;
    return std::ranges::any_of(m_undone, [](const QPointer<AnnotationModel>& m) { return m && m->canRedo(); });
}

bool Presenter::canClear() const
{
    const AnnotationModel* m = model();
    return isOpen() && m && m->hasItems();
}

void Presenter::ensureScreens()
{
    if (!m_screens.empty())
        return;
    const QList<Monitor> monitors = m_hooks.monitors ? m_hooks.monitors() : QList<Monitor>();
    for (int i = 0; i < monitors.size(); ++i) {
        if (monitors[i].pixels.isEmpty())
            continue;
        m_screens.push_back(std::make_unique<PresenterScreen>(this, int(m_screens.size()), monitors[i].pixels, monitors[i].dpr));
    }
    m_current = 0;
    if (!m_screens.empty())
        emit screensChanged();
}

void Presenter::resetScreens()
{
    close();
    m_screens.clear();
    m_current = 0;
    emit currentChanged();
}

void Presenter::watch(AnnotationModel* model)
{
    // A change the user made (not the presenter's own calls): undo goes back along these.
    connect(model, &AnnotationModel::changed, this, [this, model] {
        if (!m_syncing)
            touched(model);
    });
    connect(model, &AnnotationModel::historyChanged, this, &Presenter::historyChanged);
    connect(model, &AnnotationModel::itemsChanged, this, &Presenter::historyChanged);
    connect(model, &AnnotationModel::toolChanged, this, [this, model] {
        if (m_syncing || model != this->model())
            return;
        // A tool's letter, or the same letter again (no tool).
        const int tool = model->tool();
        if (tool == m_tool && !m_laser)
            return;
        if (tool < 0 && m_laser)
            return;
        m_tool = tool;
        m_laser = false;
        emit toolChanged();
    });
}

void Presenter::touched(AnnotationModel* model)
{
    if (!isOpen())
        return;
    m_undone.clear();
    if (!m_changed.empty() && m_changed.back() == model)
        return;
    std::erase_if(m_changed, [model](const QPointer<AnnotationModel>& m) { return !m || m == model; });
    m_changed.emplace_back(model);
}

void Presenter::setMode(Mode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    emit modeChanged();
}

void Presenter::hotkey()
{
    switch (presenter::hotkeyStep(m_mode)) {
    case presenter::HotkeyStep::Open: open(); break;
    case presenter::HotkeyStep::Close: close(); break;
    case presenter::HotkeyStep::Draw: draw(); break;
    }
}

void Presenter::open()
{
    if (isOpen())
        return;
    ensureScreens();
    if (m_screens.empty())
        return;
    if (!m_stylesLoaded) {
        m_stylesLoaded = true;
        const QString json = m_hooks.load ? m_hooks.load(QString::fromLatin1(kStylesKey)).toString() : QString();
        if (!json.isEmpty()) {
            m_styles = annot::StyleMemory::fromJson(json);
        } else {
            // Seen from across a room: a thicker pen than a screenshot's.
            annot::Style pen = m_styles.forTool(Kind::Pen, 1.0);
            pen.width = kFirstPenWidth;
            m_styles.remember(Kind::Pen, pen, 1.0);
        }
        const QVariant tool = m_hooks.load ? m_hooks.load(QString::fromLatin1(kToolKey)) : QVariant();
        if (tool.toString() == u"laser") {
            m_laser = true;
        } else if (bool ok = false; tool.isValid()) {
            const int value = tool.toInt(&ok);
            if (ok && presenter::toolOffered(value))
                m_tool = value;
        }
    }
    const int start = m_hooks.screenAtPointer ? m_hooks.screenAtPointer() : 0;
    m_current = screen(start) ? start : 0;
    {
        QScopedValueRollback guard(m_syncing, true);
        for (const auto& s : m_screens) {
            QImage base = zeroPicture(s->pixels());
            s->begin(base, m_styles, m_laser ? -1 : m_tool);
        }
    }
    m_changed.clear();
    m_undone.clear();
    m_previous = m_hooks.foreground ? m_hooks.foreground() : 0;
    m_toolbarShown = true;
    setMode(Mode::Draw);
    emit currentChanged();
    emit boardsChanged();
    emit toolChanged();
    emit historyChanged();
    emit toolbarShownChanged();
    showHint(kRecordHintKey,
        tr("录屏或共享屏幕时，请选「显示器采集」或「共享整个屏幕」：只选一个窗口录不到笔迹"));
}

void Presenter::close()
{
    if (!isOpen())
        return;
    if (AnnotationModel* m = model())
        collectStyles(m);
    saveState();
    {
        QScopedValueRollback guard(m_syncing, true);
        for (const auto& s : m_screens)
            s->end();
    }
    m_changed.clear();
    m_undone.clear();
    m_hint.clear();
    const bool drawing = m_mode == Mode::Draw;
    setMode(Mode::Closed);
    emit hintChanged();
    emit boardsChanged();
    emit historyChanged();
    // The windows are hidden now: the foreground back where it was (from Through it is there already).
    if (drawing && m_previous && m_hooks.setForeground)
        m_hooks.setForeground(m_previous);
}

void Presenter::draw()
{
    if (!isOpen()) {
        open();
        return;
    }
    if (m_mode == Mode::Draw)
        return;
    if (const quintptr foreground = m_hooks.foreground ? m_hooks.foreground() : 0)
        m_previous = foreground;
    if (m_hooks.screenAtPointer) {
        if (PresenterScreen* s = screen(m_hooks.screenAtPointer()))
            activate(s);
    }
    setMode(Mode::Draw);
}

void Presenter::through()
{
    if (m_mode != Mode::Draw || !canThrough())
        return;
    // Whatever is under way is finished first: a text typed, a path, a drag.
    for (const auto& s : m_screens) {
        settle(s->model());
        s->m_pressed = false;
    }
    setMode(Mode::Through);
    // The program that had the foreground, else (opened from one of ours)
    // the one beneath the pointer, else none: the keys go where the user looks.
    if (m_hooks.setForeground && !(m_previous && m_hooks.setForeground(m_previous))) {
        if (const quintptr beneath = m_hooks.beneathPointer ? m_hooks.beneathPointer() : 0)
            m_hooks.setForeground(beneath);
    }
    showHint(kThroughHintKey,
        m_hotkeyText.isEmpty() ? tr("现在可以操作下面的程序，笔迹还在；点工具条上的“回到画笔”接着画")
                               : tr("现在可以操作下面的程序，笔迹还在；按 %1 或点工具条上的“回到画笔”接着画").arg(m_hotkeyText));
}

void Presenter::toggleThrough()
{
    if (m_mode == Mode::Through)
        draw();
    else
        through();
}

void Presenter::back()
{
    PresenterScreen* s = current();
    if (!s || m_mode != Mode::Draw)
        return;
    AnnotationModel* m = s->model();
    const bool took = m->finishPath() || m->escape();
    switch (presenter::backStep(took, m->selectedCount() > 0, s->board())) {
    case presenter::BackStep::Done:
        break;
    case presenter::BackStep::Deselect:
        deselect(m);
        break;
    case presenter::BackStep::CloseBoard:
        toggleBoard(int(s->board()));
        break;
    case presenter::BackStep::Through:
        through();
        break;
    }
}

void Presenter::activate(PresenterScreen* s)
{
    if (!s)
        return;
    PresenterScreen* before = current();
    if (before != s) {
        if (before) {
            AnnotationModel* m = before->model();
            collectStyles(m);
            QScopedValueRollback guard(m_syncing, true);
            m->commitPending();
            deselect(m);
        }
        m_current = s->index();
        emit currentChanged();
        emit boardsChanged();
        emit historyChanged();
    } else {
        // The same screen: its model has the newest styles (the toolbar's
        // panel changes them there), never put back to the ones before.
        collectStyles(s->model());
    }
    adoptStyles(s->model());
    applyTool(s->model());
}

void Presenter::collectStyles(AnnotationModel* m)
{
    if (m && m->active() && !(m->styles() == m_styles))
        m_styles = m->styles();
}

void Presenter::adoptStyles(AnnotationModel* m)
{
    if (!m || !m->active() || m->styles() == m_styles)
        return;
    QScopedValueRollback guard(m_syncing, true);
    const int was = m->tool();
    bool changed = false;
    for (int k = 0; k < presenter::kRedactTool; ++k) {
        const Kind kind = Kind(k);
        if (sameTool(m->styles().tool(kind), m_styles.tool(kind)))
            continue;
        if (!changed) {
            deselect(m); // a change on the panel goes to what is selected: nothing must be
            changed = true;
        }
        m->setTool(k);
        m->setPanel(panelOf(m_styles.tool(kind)));
    }
    if (changed)
        m->setTool(was);
}

void Presenter::applyTool(AnnotationModel* m)
{
    if (!m || !m->active())
        return;
    QScopedValueRollback guard(m_syncing, true);
    m->setTool(m_laser ? -1 : m_tool);
}

void Presenter::deselect(AnnotationModel* m)
{
    if (!m || m->selectedCount() == 0)
        return;
    QScopedValueRollback guard(m_syncing, true);
    // A tool lets go of every object of another kind; redaction is never offered here, so none is kept.
    const int was = m->tool();
    m->setTool(presenter::kRedactTool);
    m->setTool(was);
}

void Presenter::setTool(int tool)
{
    if (!presenter::toolOffered(tool))
        return;
    if (tool == m_tool && !m_laser)
        return;
    m_tool = tool;
    m_laser = false;
    applyTool(model());
    emit toolChanged();
}

void Presenter::setLaser(bool on)
{
    if (on == m_laser)
        return;
    m_laser = on;
    if (on) {
        if (AnnotationModel* m = model()) {
            QScopedValueRollback guard(m_syncing, true);
            settle(m);
            deselect(m);
        }
    }
    applyTool(model());
    emit toolChanged();
}

void Presenter::undo()
{
    if (!isOpen())
        return;
    AnnotationModel* target = nullptr;
    while (!m_changed.empty()) {
        AnnotationModel* m = m_changed.back();
        if (m && m->active() && m->canUndo()) {
            target = m;
            break;
        }
        m_changed.pop_back();
    }
    if (!target) {
        AnnotationModel* m = model();
        if (!m || !m->canUndo())
            return;
        target = m;
    }
    QScopedValueRollback guard(m_syncing, true);
    target->undo();
    if (m_undone.empty() || m_undone.back() != target)
        m_undone.emplace_back(target);
    emit historyChanged();
}

void Presenter::redo()
{
    if (!isOpen())
        return;
    while (!m_undone.empty()) {
        AnnotationModel* m = m_undone.back();
        if (m && m->active() && m->canRedo()) {
            QScopedValueRollback guard(m_syncing, true);
            m->redo();
            if (m_changed.empty() || m_changed.back() != m) {
                std::erase_if(m_changed, [m](const QPointer<AnnotationModel>& c) { return !c || c == m; });
                m_changed.emplace_back(m);
            }
            emit historyChanged();
            return;
        }
        m_undone.pop_back();
    }
}

void Presenter::clear()
{
    AnnotationModel* m = model();
    if (!isOpen() || !m || !m->hasItems())
        return;
    // All of this screen's layer in one step Ctrl+Z takes back.
    settle(m);
    m->selectAll();
    m->deleteSelection();
}

void Presenter::toggleBoard(int value)
{
    if (!isOpen() || value < int(Board::White) || value > int(Board::Black))
        return;
    if (m_mode == Mode::Through)
        draw(); // makes the screen under the pointer the current one
    PresenterScreen* s = current();
    if (!s)
        return;
    const Board next = presenter::toggledBoard(s->board(), Board(value));
    AnnotationModel* m = s->model();
    collectStyles(m);
    {
        QScopedValueRollback guard(m_syncing, true);
        settle(m);
        deselect(m);
        s->setBoard(next, m_styles, m_laser ? -1 : m_tool);
    }
    adoptStyles(s->model());
    applyTool(s->model());
    emit currentChanged();
    emit boardsChanged();
    emit historyChanged();
}

void Presenter::toggleToolbar()
{
    m_toolbarShown = !m_toolbarShown;
    emit toolbarShownChanged();
}

void Presenter::showHint(const char* key, const QString& text)
{
    const QString name = QString::fromLatin1(key);
    if (m_hooks.load && m_hooks.load(name).toBool())
        return;
    if (m_hooks.save)
        m_hooks.save(name, true);
    m_hint = text;
    emit hintChanged();
}

void Presenter::dismissHint()
{
    if (m_hint.isEmpty())
        return;
    m_hint.clear();
    emit hintChanged();
}

void Presenter::saveState()
{
    if (!m_hooks.save)
        return;
    m_hooks.save(QString::fromLatin1(kStylesKey), m_styles.toJson());
    m_hooks.save(QString::fromLatin1(kToolKey), m_laser ? QVariant(u"laser"_s) : QVariant(m_tool));
}

void Presenter::settle(AnnotationModel* m)
{
    if (!m)
        return;
    m->finishPath();
    if (m->gestureActive())
        m->cancelGesture();
    m->commitPending();
}

bool Presenter::commandKey(PresenterScreen* s, int key, Qt::KeyboardModifiers modifiers, bool autoRepeat)
{
    activate(s);
    const Qt::KeyboardModifiers mods = modifiers & ~Qt::KeypadModifier;
    // A key held down: a switch (W, K, X, H, a tool's letter) would flicker,
    // Delete would go on to clear the screen, Esc would step back to Through.
    if (autoRepeat && !presenter::repeats(key, mods))
        return true;
    AnnotationModel* m = s->model();
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool other = mods & (Qt::AltModifier | Qt::MetaModifier);
    if (key == Qt::Key_Escape && !ctrl && !other) {
        back();
        return true;
    }
    // Undo and redo go back along the screens' changes, newest first.
    if (ctrl && !other && ((key == Qt::Key_Z && !shift))) {
        undo();
        return true;
    }
    if (ctrl && !other && (key == Qt::Key_Y || (key == Qt::Key_Z && shift))) {
        redo();
        return true;
    }
    switch (presenter::command(key, mods, m->selectedCount() > 0)) {
    case presenter::Command::Laser:
        setLaser(!m_laser);
        return true;
    case presenter::Command::Whiteboard:
        toggleBoard(int(Board::White));
        return true;
    case presenter::Command::Blackboard:
        toggleBoard(int(Board::Black));
        return true;
    case presenter::Command::Toolbar:
        toggleToolbar();
        return true;
    case presenter::Command::Clear:
        clear();
        return true;
    case presenter::Command::Blocked:
        return true;
    case presenter::Command::None:
        break;
    }
    return m->key(key, mods);
}

CursorLook Presenter::cursorLookAt(PresenterScreen* s, QPoint image) const
{
    if (m_laser)
        return {CursorLook::Kind::Dot, 0, presenter::kLaserColor, presenter::kLaserWidth};
    AnnotationModel* m = s->model();
    const int shape = m->cursorAt(image);
    const int tool = m->tool();
    if ((tool == int(Kind::Pen) || tool == int(Kind::Marker)) && shape == Qt::CrossCursor) {
        const annot::StyleMemory::Tool& t = m->styles().tool(Kind(tool));
        QColor color = QColor::fromRgb(t.color);
        if (tool == int(Kind::Marker))
            color.setAlphaF(0.5);
        return {CursorLook::Kind::Dot, 0, color.rgba(), t.width};
    }
    return {CursorLook::Kind::Shape, shape >= 0 ? shape : int(Qt::ArrowCursor), 0, 0};
}

QCursor Presenter::cursorOf(const CursorLook& look, qreal dpr)
{
    if (look.kind == CursorLook::Kind::Dot)
        return dotCursor(QColor::fromRgba(look.color), look.width, dpr);
    return QCursor(Qt::CursorShape(look.shape));
}

} // namespace ws::capture
