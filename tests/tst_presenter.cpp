// The presenter (src/capture/Presenter, PresenterState; GitHub #189,
// docs/design-presenter.md): its rules on their own; the session driven
// with no window, as the ink windows hand it the mouse and keys (two
// monitors at 150 % and 100 %): the hotkey's three steps, through and the
// foreground given back, Esc's step back, the boards, clear as one undo
// step, undo across the screens, the styles every screen shares, the hints
// shown once, what is remembered; the compositor drawing on nothing; and
// what a pen move costs on a 4K canvas while one long stroke grows.
#include "AnnotationCanvas.h"
#include "AnnotationModel.h"
#include "AnnotationRender.h"
#include "LiveStroke.h"
#include "Presenter.h"
#include "PresenterState.h"

#include <QElapsedTimer>
#include <QPainter>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace ws::capture;
using namespace ws::capture::presenter;
using annot::Kind;
using namespace Qt::StringLiterals;

namespace {

// The hooks' side: two monitors, a foreground window, a store.
struct Host {
    QList<Presenter::Monitor> monitors {{QSize(2560, 1600), 1.5}, {QSize(1920, 1080), 1.0}};
    int pointer = 0;
    quintptr foreground = 0x111;
    quintptr beneath = 0; // the window under the pointer
    QList<quintptr> gone; // windows closed since
    QList<quintptr> givenBack;
    QVariantMap store;

    Presenter::Hooks hooks()
    {
        Presenter::Hooks h;
        h.monitors = [this] { return monitors; };
        h.screenAtPointer = [this] { return pointer; };
        h.foreground = [this] { return foreground; };
        h.setForeground = [this](quintptr w) {
            if (gone.contains(w))
                return false;
            givenBack.append(w);
            return true;
        };
        h.beneathPointer = [this] { return beneath; };
        h.load = [this](const QString& key) { return store.value(key); };
        h.save = [this](const QString& key, const QVariant& value) { store.insert(key, value); };
        return h;
    }
};

// A stroke dragged with the left button, window logical px.
void drag(PresenterScreen* s, QPointF from, QPointF to, int steps = 8)
{
    s->mousePressed(from.x(), from.y(), Qt::LeftButton, 0);
    for (int i = 1; i <= steps; ++i) {
        const QPointF p = from + (to - from) * (qreal(i) / steps);
        s->mouseMoved(p.x(), p.y(), 0);
    }
    s->mouseReleased(to.x(), to.y(), Qt::LeftButton);
}

void click(PresenterScreen* s, QPointF at, Qt::MouseButton button = Qt::LeftButton)
{
    s->mousePressed(at.x(), at.y(), button, 0);
    s->mouseReleased(at.x(), at.y(), button);
}

// The canvas's polish (the tiles drawn on the GUI thread), called directly.
class Canvas : public AnnotationCanvas {
public:
    using AnnotationCanvas::updatePolish;
};

qint64 percentile(std::vector<qint64> v, double p)
{
    if (v.empty())
        return 0;
    std::ranges::sort(v);
    return v[std::min(v.size() - 1, std::size_t(std::ceil(p * double(v.size())) - 1))];
}

} // namespace

class TestPresenter : public QObject {
    Q_OBJECT

private slots:
    // ---- the rules ----

    void hotkeySteps()
    {
        QCOMPARE(hotkeyStep(Mode::Closed), HotkeyStep::Open);
        QCOMPARE(hotkeyStep(Mode::Draw), HotkeyStep::Close);
        QCOMPARE(hotkeyStep(Mode::Through), HotkeyStep::Draw);
    }

    void backSteps()
    {
        QCOMPARE(backStep(true, true, Board::White), BackStep::Done);
        QCOMPARE(backStep(false, true, Board::White), BackStep::Deselect);
        QCOMPARE(backStep(false, false, Board::Black), BackStep::CloseBoard);
        QCOMPARE(backStep(false, false, Board::None), BackStep::Through);
    }

    void commands()
    {
        const auto none = Qt::NoModifier;
        QCOMPARE(command(Qt::Key_X, none, false), Command::Laser);
        QCOMPARE(command(Qt::Key_W, none, false), Command::Whiteboard);
        QCOMPARE(command(Qt::Key_K, none, false), Command::Blackboard);
        QCOMPARE(command(Qt::Key_H, none, false), Command::Toolbar);
        QCOMPARE(command(Qt::Key_Delete, none, false), Command::Clear);
        QCOMPARE(command(Qt::Key_Delete, none, true), Command::None); // the selected objects
        QCOMPARE(command(Qt::Key_B, none, false), Command::Blocked);
        QCOMPARE(command(Qt::Key_B, Qt::ShiftModifier, false), Command::Blocked); // smart redaction
        QCOMPARE(command(Qt::Key_9, Qt::KeypadModifier, false), Command::Blocked);
        QCOMPARE(command(Qt::Key_I, none, false), Command::Blocked);
        QCOMPARE(command(Qt::Key_W, Qt::ControlModifier, false), Command::None);
        QCOMPARE(command(Qt::Key_X, Qt::AltModifier, false), Command::None);
        QCOMPARE(command(Qt::Key_R, none, false), Command::None); // the model's tool
        QCOMPARE(toggledBoard(Board::None, Board::White), Board::White);
        QCOMPARE(toggledBoard(Board::White, Board::White), Board::None);
        QCOMPARE(toggledBoard(Board::White, Board::Black), Board::Black);
        QVERIFY(toolOffered(-1));
        QVERIFY(toolOffered(int(Kind::Number)));
        QVERIFY(!toolOffered(int(Kind::Redact)));
    }

    // ---- the session ----

    void hotkeyOpensAndCloses()
    {
        Host host;
        Presenter p;
        p.setHooks(host.hooks());
        p.hotkey();
        QCOMPARE(p.mode(), Mode::Draw);
        QCOMPARE(int(p.screens().size()), 2);
        QCOMPARE(p.previousForeground(), quintptr(0x111));
        for (const auto& s : p.screens()) {
            AnnotationModel* m = s->desk();
            QVERIFY(m->active());
            QVERIFY(m->compositor().transparent());
            QCOMPARE(m->document().canvas, s->pixels());
            QCOMPARE(m->cropRect(), QRect(QPoint(0, 0), s->pixels())); // all of the monitor takes ink
            QCOMPARE(m->tool(), int(Kind::Pen)); // the first time: the pen
        }
        QCOMPARE(p.screens()[0]->dpr(), 1.5);
        drag(p.screen(0), {100, 100}, {300, 200});
        QCOMPARE(p.screen(0)->desk()->itemCount(), 1);
        // The ink is where the mouse was, in the monitor's physical pixels.
        const QRectF reach = p.screen(0)->desk()->document().items.first().points.isEmpty()
            ? QRectF() : QRectF(p.screen(0)->desk()->document().items.first().points.first(), QSizeF(1, 1));
        QVERIFY(qAbs(reach.x() - 150) <= 2 && qAbs(reach.y() - 150) <= 2);
        p.hotkey();
        QCOMPARE(p.mode(), Mode::Closed);
        QCOMPARE(host.givenBack, QList<quintptr> {0x111}); // the foreground back where it was
        QVERIFY(!p.screen(0)->desk()->active()); // the ink and its memory let go of
        p.hotkey();
        QCOMPARE(p.mode(), Mode::Draw);
        QCOMPARE(p.screen(0)->desk()->itemCount(), 0);
    }

    void throughGivesTheForegroundBack()
    {
        Host host;
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        drag(p.screen(0), {50, 50}, {150, 80});
        host.foreground = 0; // ours, while drawing
        p.screen(0)->mousePressed(400, 400, Qt::RightButton, 0); // the right button: one step back
        QCOMPARE(p.mode(), Mode::Through);
        QCOMPARE(host.givenBack, QList<quintptr> {0x111});
        QCOMPARE(p.screen(0)->desk()->itemCount(), 1); // the ink stays
        // In Through the windows take nothing; the hotkey draws again.
        p.screen(0)->mousePressed(10, 10, Qt::LeftButton, 0);
        QCOMPARE(p.screen(0)->desk()->itemCount(), 1);
        host.foreground = 0x222; // the user went on in another program
        p.hotkey();
        QCOMPARE(p.mode(), Mode::Draw);
        QCOMPARE(p.previousForeground(), quintptr(0x222));
        p.toggleThrough();
        QCOMPARE(p.mode(), Mode::Through);
        QCOMPARE(host.givenBack.last(), quintptr(0x222));
        p.toggleThrough();
        QCOMPARE(p.mode(), Mode::Draw);
    }

    // Opened from one of ours (no foreground remembered) or the program
    // closed since: Through gives the foreground to the window beneath the
    // pointer, or to none.
    void throughFallsBackToTheWindowBeneath()
    {
        Host host;
        host.foreground = 0; // opened from the capture settings
        host.beneath = 0x333;
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        p.through();
        QCOMPARE(host.givenBack, QList<quintptr> {0x333});
        host.foreground = 0x444;
        p.draw();
        host.gone = {0x444}; // closed meanwhile
        p.through();
        QCOMPARE(host.givenBack, (QList<quintptr> {0x333, 0x333}));
        p.draw();
        host.beneath = 0; // nothing of anyone's there
        p.through();
        QCOMPARE(host.givenBack.size(), 2);
        QCOMPARE(p.mode(), Mode::Through);
    }

    void escapeStepsBack()
    {
        Host host;
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        PresenterScreen* s = p.screen(0);
        drag(s, {100, 100}, {300, 100});
        // No tool: a click on the stroke selects it.
        p.setTool(-1);
        click(s, {200, 100});
        QCOMPARE(s->desk()->selectedCount(), 1);
        QVERIFY(s->keyPressed(Qt::Key_Escape, 0));
        QCOMPARE(s->desk()->selectedCount(), 0);
        QCOMPARE(p.mode(), Mode::Draw);
        QCOMPARE(s->desk()->itemCount(), 1); // never the ink
        p.toggleBoard(int(Board::White));
        QCOMPARE(s->board(), Board::White);
        QVERIFY(s->keyPressed(Qt::Key_Escape, 0));
        QCOMPARE(s->board(), Board::None);
        QCOMPARE(p.mode(), Mode::Draw);
        QVERIFY(s->keyPressed(Qt::Key_Escape, 0));
        QCOMPARE(p.mode(), Mode::Through);
        QCOMPARE(s->desk()->itemCount(), 1);
    }

    void boardsKeepTheirOwnInk()
    {
        Host host;
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        PresenterScreen* s = p.screen(0);
        drag(s, {100, 100}, {300, 100});
        QVERIFY(s->keyPressed(Qt::Key_W, 0));
        QCOMPARE(p.board(), int(Board::White));
        QVERIFY(s->boardModel());
        QCOMPARE(s->model(), s->boardModel());
        QVERIFY(s->boardModel()->compositor().transparent());
        QVERIFY(!p.canThrough()); // one cannot see what a click would reach
        drag(s, {100, 300}, {300, 300});
        drag(s, {100, 400}, {300, 400});
        QCOMPARE(s->boardModel()->itemCount(), 2);
        QCOMPARE(s->desk()->itemCount(), 1);
        p.through();
        QCOMPARE(p.mode(), Mode::Draw);
        QVERIFY(s->keyPressed(Qt::Key_K, 0)); // the black board: the same ink
        QCOMPARE(s->board(), Board::Black);
        QCOMPARE(s->model()->itemCount(), 2);
        QVERIFY(s->keyPressed(Qt::Key_K, 0));
        QCOMPARE(s->board(), Board::None);
        QCOMPARE(s->model(), s->desk());
        QVERIFY(p.canThrough());
        // The other monitor stays on the desktop.
        QCOMPARE(p.screen(1)->board(), Board::None);
    }

    // W or K from Through: the board opens on the screen under the pointer
    // (draw() makes it the current one), not on the one drawn on before.
    void boardOpensUnderThePointer()
    {
        Host host;
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        QCOMPARE(p.current(), p.screen(0));
        p.through();
        host.pointer = 1;
        p.toggleBoard(int(Board::White));
        QCOMPARE(p.mode(), Mode::Draw);
        QCOMPARE(p.current(), p.screen(1));
        QCOMPARE(p.screen(1)->board(), Board::White);
        QCOMPARE(p.screen(0)->board(), Board::None);
        QCOMPARE(p.board(), int(Board::White));
    }

    void clearIsOneUndoStep()
    {
        Host host;
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        PresenterScreen* s = p.screen(1);
        for (int i = 0; i < 3; ++i)
            drag(s, {100.0, 100.0 + 50 * i}, {400.0, 100.0 + 50 * i});
        QCOMPARE(s->desk()->itemCount(), 3);
        QVERIFY(p.canClear());
        QVERIFY(s->keyPressed(Qt::Key_Delete, 0));
        QCOMPARE(s->desk()->itemCount(), 0);
        QVERIFY(s->keyPressed(Qt::Key_Z, Qt::ControlModifier));
        QCOMPARE(s->desk()->itemCount(), 3);
        QVERIFY(s->keyPressed(Qt::Key_Y, Qt::ControlModifier));
        QCOMPARE(s->desk()->itemCount(), 0);
    }

    void undoGoesBackAcrossScreens()
    {
        Host host;
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        drag(p.screen(0), {100, 100}, {300, 100});
        drag(p.screen(1), {100, 100}, {300, 100});
        QCOMPARE(p.current(), p.screen(1));
        p.undo(); // the newest change first: the second monitor's
        QCOMPARE(p.screen(1)->desk()->itemCount(), 0);
        QCOMPARE(p.screen(0)->desk()->itemCount(), 1);
        p.undo();
        QCOMPARE(p.screen(0)->desk()->itemCount(), 0);
        QVERIFY(!p.canUndo());
        p.redo();
        QCOMPARE(p.screen(0)->desk()->itemCount(), 1);
        QVERIFY(p.canRedo());
        p.redo();
        QCOMPARE(p.screen(1)->desk()->itemCount(), 1);
    }

    void stylesAreEveryones()
    {
        Host host;
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        drag(p.screen(1), {100, 100}, {300, 100}); // something to undo on the second monitor
        drag(p.screen(0), {100, 100}, {300, 100});
        p.model()->setPanel({{u"color"_s, u"#1E88E5"_s}, {u"width"_s, 12}});
        const QRgb blue = qRgb(0x1E, 0x88, 0xE5);
        QCOMPARE(p.screen(0)->desk()->styles().tool(Kind::Pen).color, blue);
        // The other monitor's first press takes them, its undo kept.
        drag(p.screen(1), {100, 300}, {300, 300});
        QCOMPARE(p.screen(1)->desk()->styles().tool(Kind::Pen).color, blue);
        QCOMPARE(p.screen(1)->desk()->styles().tool(Kind::Pen).width, 12);
        const annot::Item& last = p.screen(1)->desk()->document().items.last();
        QCOMPARE(last.style.color, blue);
        QCOMPARE(p.screen(1)->desk()->itemCount(), 2);
        // A board made later starts with them too.
        p.toggleBoard(int(Board::Black));
        QCOMPARE(p.screen(1)->boardModel()->styles().tool(Kind::Pen).color, blue);
        // Remembered when it closes.
        p.close();
        QCOMPARE(annot::StyleMemory::fromJson(host.store.value(u"Styles"_s).toString()).tool(Kind::Pen).color, blue);
    }

    // The toolbar's panel changes the current model's styles: the next
    // stroke has them (a press put the styles before back: found live), and
    // the other screen takes them when it is drawn on.
    void panelStylesAreKept()
    {
        Host host;
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        p.setTool(int(Kind::Pen));
        PresenterScreen* s = p.screen(0);
        s->desk()->setPanel({{u"color"_s, u"#1E88E5"_s}, {u"width"_s, 7}});
        QCOMPARE(s->desk()->styles().tool(Kind::Pen).color, qRgb(0x1E, 0x88, 0xE5));
        drag(s, {100, 100}, {300, 120});
        const annot::Item& item = s->desk()->document().items.last();
        QCOMPARE(item.kind, Kind::Pen);
        QCOMPARE(item.style.color, qRgb(0x1E, 0x88, 0xE5));
        QCOMPARE(s->desk()->styles().tool(Kind::Pen).width, 7);
        drag(p.screen(1), {100, 100}, {300, 120});
        QCOMPARE(p.screen(1)->desk()->document().items.last().style.color, qRgb(0x1E, 0x88, 0xE5));
        QCOMPARE(p.screen(1)->desk()->styles().tool(Kind::Pen).width, 7);
    }

    void keys()
    {
        Host host;
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        PresenterScreen* s = p.screen(0);
        QVERIFY(s->keyPressed(Qt::Key_R, 0)); // the rectangle's letter: the model's
        QCOMPARE(p.tool(), int(Kind::Rect));
        QVERIFY(s->keyPressed(Qt::Key_B, 0)); // redaction: swallowed, nothing changes
        QCOMPARE(p.tool(), int(Kind::Rect));
        QVERIFY(s->keyPressed(Qt::Key_I, 0)); // the eyedropper: the same
        QVERIFY(!s->desk()->picking());
        QVERIFY(s->keyPressed(Qt::Key_X, 0));
        QVERIFY(p.laser());
        QCOMPARE(s->desk()->tool(), -1);
        click(s, {500, 500}); // the laser draws nothing in the document
        QCOMPARE(s->desk()->itemCount(), 0);
        QVERIFY(s->keyPressed(Qt::Key_P, 0)); // a tool's letter ends the laser
        QVERIFY(!p.laser());
        QCOMPARE(p.tool(), int(Kind::Pen));
        QVERIFY(s->keyPressed(Qt::Key_H, 0));
        QVERIFY(!p.toolbarShown());
        QVERIFY(s->keyPressed(Qt::Key_H, 0));
        QVERIFY(p.toolbarShown());
    }

    // A key held down: the switches act once (W does not flicker the board,
    // Delete does not go on to clear the screen); Ctrl+Z goes on undoing.
    void heldKeysActOnce()
    {
        QVERIFY(!repeats(Qt::Key_W, Qt::NoModifier));
        QVERIFY(!repeats(Qt::Key_Delete, Qt::NoModifier));
        QVERIFY(!repeats(Qt::Key_Escape, Qt::NoModifier));
        QVERIFY(!repeats(Qt::Key_P, Qt::NoModifier));
        QVERIFY(repeats(Qt::Key_Z, Qt::ControlModifier));
        QVERIFY(repeats(Qt::Key_Left, Qt::NoModifier));
        QVERIFY(repeats(Qt::Key_BracketRight, Qt::NoModifier));

        Host host;
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        PresenterScreen* s = p.screen(0);
        QVERIFY(s->keyPressed(Qt::Key_W, 0));
        for (int i = 0; i < 5; ++i)
            QVERIFY(s->keyPressed(Qt::Key_W, 0, true));
        QCOMPARE(s->board(), Board::White);
        QVERIFY(s->keyPressed(Qt::Key_W, 0));
        QCOMPARE(s->board(), Board::None);
        QVERIFY(s->keyPressed(Qt::Key_X, 0));
        QVERIFY(s->keyPressed(Qt::Key_X, 0, true));
        QVERIFY(p.laser());
        QVERIFY(s->keyPressed(Qt::Key_P, 0));
        QVERIFY(s->keyPressed(Qt::Key_P, 0, true)); // not the same letter again (no tool)
        QCOMPARE(p.tool(), int(Kind::Pen));
        for (int i = 0; i < 3; ++i)
            drag(s, {100, 100.0 + 50 * i}, {300, 100.0 + 50 * i});
        QCOMPARE(s->desk()->itemCount(), 3);
        p.setTool(-1);
        click(s, {200, 100});
        QCOMPARE(s->desk()->selectedCount(), 1);
        QVERIFY(s->keyPressed(Qt::Key_Delete, 0)); // the selected stroke
        QVERIFY(s->keyPressed(Qt::Key_Delete, 0, true)); // held: not the rest of the screen
        QCOMPARE(s->desk()->itemCount(), 2);
        QVERIFY(s->keyPressed(Qt::Key_Escape, 0, true));
        QCOMPARE(p.mode(), Mode::Draw);
        QVERIFY(s->keyPressed(Qt::Key_Z, int(Qt::ControlModifier)));
        QVERIFY(s->keyPressed(Qt::Key_Z, int(Qt::ControlModifier), true));
        QCOMPARE(s->desk()->itemCount(), 2); // two steps back: the delete, the third stroke
        QVERIFY(s->desk()->canRedo());
    }

    void hintsOnce()
    {
        Host host;
        {
            Presenter p;
            p.setHooks(host.hooks());
            p.open();
            QVERIFY(p.hint().contains(u"整个屏幕"_s));
            p.dismissHint();
            QVERIFY(p.hint().isEmpty());
            p.setHotkeyText(u"Ctrl+Alt+P"_s);
            p.through();
            QVERIFY(p.hint().contains(u"Ctrl+Alt+P"_s));
            p.close();
        }
        Presenter again;
        again.setHooks(host.hooks());
        again.open();
        QVERIFY(again.hint().isEmpty());
        again.through();
        QVERIFY(again.hint().isEmpty());
    }

    void remembersTheTool()
    {
        Host host;
        {
            Presenter p;
            p.setHooks(host.hooks());
            p.open();
            p.setTool(int(Kind::Arrow));
            p.close();
        }
        {
            Presenter p;
            p.setHooks(host.hooks());
            p.open();
            QCOMPARE(p.tool(), int(Kind::Arrow));
            QCOMPARE(p.screen(1)->desk()->tool(), int(Kind::Arrow));
            p.setLaser(true);
            p.close();
        }
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        QVERIFY(p.laser());
    }

    // ---- the compositor on nothing ----

    void transparentCompositor()
    {
        AnnotationModel m;
        m.compositor().setTransparent(true);
        QImage base(400, 300, QImage::Format_ARGB32_Premultiplied);
        base.fill(Qt::transparent);
        m.begin(base, 1.0, annot::StyleMemory());
        m.setSelection(QRect(0, 0, 400, 300));
        m.setTool(int(Kind::Pen));
        m.setPanel({{u"color"_s, u"#E53935"_s}, {u"width"_s, 8}});
        const auto stroke = [&](QPoint a, QPoint b) {
            if (!m.pressFirst(a, {}))
                m.pressSecond(a, {});
            for (int i = 1; i <= 10; ++i)
                m.move(a + (b - a) * i / 10, {});
            m.release(b);
        };
        stroke({20, 50}, {380, 50});
        m.setTool(int(Kind::Marker));
        m.setPanel({{u"color"_s, u"#1E88E5"_s}});
        stroke({20, 200}, {380, 200});
        QImage tile;
        m.compositor().render(m.document(), m.base(), QRect(0, 0, 400, 300), QRect(0, 0, 400, 300), &tile);
        QCOMPARE(tile.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(qAlpha(tile.pixel(200, 120)), 0); // nothing drawn: nothing there
        const QRgb pen = tile.pixel(200, 50);
        QCOMPARE(qAlpha(pen), 255);
        QVERIFY(qRed(pen) > 200 && qGreen(pen) < 80);
        // The marker (Multiply on a picture) is its colour at 40 % here.
        const QColor marker = QColor::fromRgba(tile.pixel(200, 200));
        QVERIFY2(marker.alpha() > 80 && marker.alpha() < 125, qPrintable(QString::number(marker.alpha())));
        QVERIFY(marker.blue() > marker.red());
        // Opaque as before when not transparent.
        AnnotationModel opaque;
        QImage white(100, 100, QImage::Format_RGB32);
        white.fill(Qt::white);
        opaque.begin(white, 1.0, annot::StyleMemory());
        QImage t2;
        opaque.compositor().render(opaque.document(), white, QRect(0, 0, 100, 100), QRect(0, 0, 100, 100), &t2);
        QCOMPARE(t2.format(), QImage::Format_RGB32);
        QCOMPARE(t2.pixel(50, 50), qRgb(255, 255, 255));
    }

    // ---- what a long stroke costs ----

    // A pen stroke of 3000 points (about 4.5 image px apart, as the sampler
    // keeps them) growing across a 4K monitor: the model's move and the
    // canvas's polish (the tiles drawn on the GUI thread) per move, with
    // the stroke in the tiles (as the capture overlay) and drawn by the GPU
    // (`live`, the presenter's). The budget (docs/design-presenter.md 4.5):
    // p95 4 ms. The live one's triangles are made on the render thread:
    // timed here too (its newest chunk at each move).
    void longStrokeCost_data()
    {
        QTest::addColumn<bool>("gpu");
        QTest::addColumn<int>("moves");
        // About 3000 points kept (the sampler leaves out some), and a stroke twice as long.
        QTest::newRow("tiles, 3000 points") << false << 3610;
        QTest::newRow("tiles, 6000 points") << false << 7220;
        QTest::newRow("live, 3000 points") << true << 3610;
    }

    void longStrokeCost()
    {
        QFETCH(bool, gpu);
        QFETCH(int, moves);
        AnnotationModel m;
        m.compositor().setTransparent(true);
        QImage base(3840, 2160, QImage::Format_ARGB32_Premultiplied);
        base.fill(Qt::transparent);
        m.begin(base, 1.5, annot::StyleMemory());
        m.setSelection(QRect(0, 0, 3840, 2160));
        m.setTool(int(Kind::Pen));
        m.setPanel({{u"width"_s, 6}});
        Canvas canvas;
        canvas.setModel(&m);
        canvas.setSparse(true);
        canvas.setLive(gpu);
        canvas.updatePolish();
        const int kPoints = moves;
        constexpr double kStep = 4.5; // image px between moves
        // A zigzag of 4 rows across the screen, waving a little.
        const auto at = [](int i) {
            const double along = i * kStep;
            const int row = int(along / 3400);
            const double into = std::fmod(along, 3400);
            const double x = 200 + (row % 2 == 0 ? into : 3400 - into);
            const double y = 200 + row * 180 + 60 * std::sin(i * 0.02);
            return QPoint(int(std::lround(x)), int(std::lround(y)));
        };
        QElapsedTimer clock;
        std::vector<qint64> all;
        std::vector<qint64> late;
        std::vector<qint64> mesh;
        if (!m.pressFirst(at(0), {}))
            m.pressSecond(at(0), {});
        canvas.updatePolish();
        for (int i = 1; i < kPoints; ++i) {
            clock.start();
            m.move(at(i), {});
            canvas.updatePolish();
            const qint64 ns = clock.nsecsElapsed();
            all.push_back(ns);
            if (i >= kPoints - 500)
                late.push_back(ns);
            if (gpu) {
                QCOMPARE(canvas.liveItem(), m.document().items.last().id);
                // What the render thread makes at this move: the newest chunk.
                const QList<QPointF>& points = m.document().items.last().points;
                clock.start();
                live::Mesh triangles;
                const qsizetype chunks = (live::pieceCount(points.size()) + live::kChunk - 1) / live::kChunk;
                live::appendChunk(triangles, points, std::max<qsizetype>(0, chunks - 1), 9, 1, qRgb(255, 0, 0));
                mesh.push_back(clock.nsecsElapsed());
            }
        }
        const int points = int(m.document().items.last().points.size());
        // Let go of: the tiles take the stroke back (live: a few each frame, the GPU showing it meanwhile).
        clock.start();
        m.release(at(kPoints - 1));
        canvas.updatePolish();
        qint64 handOver = clock.nsecsElapsed();
        int frames = 1;
        while (canvas.handoverItem() != 0 && frames < 1000) {
            clock.start();
            canvas.updatePolish();
            handOver = std::max(handOver, clock.nsecsElapsed());
            ++frames;
        }
        QCOMPARE(canvas.liveItem(), 0u);
        QCOMPARE(canvas.handoverItem(), 0u);
        const double ms = 1e-6;
        qInfo().noquote() << u"Long stroke on 4K (%1): %2 points; per move p50 %3 ms, p95 %4 ms, max %5 ms; the last 500 moves p50 %6 ms, p95 %7 ms; let go of: %10 frames, the longest %8 ms; triangles p95 %9 ms"_s
                                 .arg(gpu ? u"live, GPU"_s : u"in the tiles"_s)
                                 .arg(points)
                                 .arg(percentile(all, 0.5) * ms, 0, 'f', 2)
                                 .arg(percentile(all, 0.95) * ms, 0, 'f', 2)
                                 .arg(percentile(all, 1.0) * ms, 0, 'f', 2)
                                 .arg(percentile(late, 0.5) * ms, 0, 'f', 2)
                                 .arg(percentile(late, 0.95) * ms, 0, 'f', 2)
                                 .arg(handOver * ms, 0, 'f', 2)
                                 .arg(percentile(mesh, 0.95) * ms, 0, 'f', 3)
                                 .arg(frames);
        QVERIFY(points >= moves * 3 / 4);
        if (gpu) {
            QVERIFY2(percentile(all, 0.95) * ms <= 4.0, "a pen move over budget");
            QVERIFY2(handOver * ms <= 12.0, "letting go of a long stroke stalls a frame");
        }
    }

    // A second stroke drawn live (the GPU's) while the first is still the
    // GPU's: no frame came between letting go of the first and pressing for
    // the second (found in the live test: the first stroke went from the
    // screen when the next began). Both end in the tiles, as a canvas made
    // afresh draws them.
    void earlierInkStays()
    {
        Host host;
        host.monitors = {{QSize(2560, 1600), 1.5}};
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        PresenterScreen* s = p.screen(0);
        Canvas canvas;
        canvas.setModel(s->desk());
        canvas.setSparse(true);
        canvas.setLive(true);
        canvas.updatePolish();
        const auto stroke = [&](QPointF from, QPointF to) {
            s->mousePressed(from.x(), from.y(), Qt::LeftButton, 0);
            canvas.updatePolish();
            for (int i = 1; i <= 40; ++i) {
                const QPointF at = from + (to - from) * (i / 40.0);
                s->mouseMoved(at.x(), at.y(), 0);
                canvas.updatePolish();
            }
            s->mouseReleased(to.x(), to.y(), Qt::LeftButton); // no polish: the next press comes first
        };
        stroke({330, 400}, {870, 505});
        const quint32 first = s->desk()->document().items.last().id;
        QCOMPARE(canvas.liveItem(), first);
        stroke({300, 600}, {1130, 660});
        for (int frames = 0; frames < 100 && canvas.handoverItem() == 0 && canvas.liveItem() != 0; ++frames)
            canvas.updatePolish();
        for (int frames = 0; frames < 100; ++frames)
            canvas.updatePolish();
        QCOMPARE(canvas.liveItem(), 0u);
        QCOMPARE(canvas.handoverItem(), 0u);
        Canvas fresh;
        fresh.setModel(s->desk());
        fresh.setSparse(true);
        fresh.updatePolish();
        QVERIFY(fresh.tilesShown() > 0);
        QCOMPARE(canvas.tilesShown(), fresh.tilesShown());
    }

    // From the press to the first move nothing is live: the stroke before,
    // in the tiles already, is not taken out of them again.
    void pressAloneIsNotLive()
    {
        Host host;
        host.monitors = {{QSize(2560, 1600), 1.5}};
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        PresenterScreen* s = p.screen(0);
        Canvas canvas;
        canvas.setModel(s->desk());
        canvas.setSparse(true);
        canvas.setLive(true);
        canvas.updatePolish();
        drag(s, {330, 400}, {870, 505}, 40);
        for (int frames = 0; frames < 200; ++frames)
            canvas.updatePolish();
        QCOMPARE(canvas.handoverItem(), 0u);
        QVERIFY(canvas.tilesShown() > 0);
        const int shown = canvas.tilesShown();
        s->mousePressed(300, 700, Qt::LeftButton, 0);
        canvas.updatePolish();
        QCOMPARE(canvas.liveItem(), 0u);
        QCOMPARE(canvas.tilesShown(), shown);
        s->mouseMoved(320, 710, 0);
        canvas.updatePolish();
        QCOMPARE(canvas.liveItem(), s->desk()->document().items.last().id);
        s->mouseReleased(320, 710, Qt::LeftButton);
    }

    // Shift straightens a long stroke into fewer points: the chunks made
    // from the old points no longer hold, nor do those that a moved point reaches.
    void straightenedStrokeIsMadeAgain()
    {
        QList<QPointF> curve;
        for (int i = 0; i < 300; ++i)
            curve.append(QPointF(i * 3.0, 200 + 80 * std::sin(i * 0.05)));
        QCOMPARE(live::keptPieces(curve, curve), live::settledPieces(300));
        QList<QPointF> grown = curve;
        grown.append(QPointF(901, 200));
        QCOMPARE(live::keptPieces(curve, grown), live::settledPieces(300)); // a point more: all still hold
        const QList<QPointF> straight {curve.first(), curve.last()};
        QCOMPARE(live::keptPieces(curve, straight), 0);
        QList<QPointF> moved = curve;
        moved[200] += QPointF(0, 5);
        QVERIFY(live::keptPieces(curve, moved) <= 200 - 3); // piece j uses points up to j + 2
        QVERIFY(live::keptPieces(curve, moved) >= live::kChunk); // the first chunk holds
        const QList<QPointF> shorter = curve.mid(0, 150);
        QCOMPARE(live::keptPieces(curve, shorter), live::settledPieces(150));

        // The model: Shift on a long pen stroke straightens it into fewer points.
        Host host;
        host.monitors = {{QSize(2560, 1600), 1.5}};
        Presenter p;
        p.setHooks(host.hooks());
        p.open();
        PresenterScreen* s = p.screen(0);
        s->mousePressed(100, 300, Qt::LeftButton, 0);
        for (int i = 1; i <= 300; ++i)
            s->mouseMoved(100 + i * 3.0, 300 + 80 * std::sin(i * 0.05), 0);
        const QList<QPointF> before = s->desk()->document().items.last().points;
        QVERIFY(before.size() > live::kChunk + live::kSettledMargin);
        s->mouseMoved(1000, 300, int(Qt::ShiftModifier));
        const QList<QPointF> after = s->desk()->document().items.last().points;
        QVERIFY(after.size() < before.size());
        QVERIFY(live::keptPieces(before, after) < live::kChunk); // no chunk kept: the old curve would stay
        s->mouseReleased(1000, 300, Qt::LeftButton);
    }

    // The GPU's triangles cover what the compositor fills for the same
    // stroke (the centreline, the width, the round ends), but for the
    // antialiased edge.
    void liveStrokeMatchesTheTiles()
    {
        annot::Item item;
        item.id = 1;
        item.kind = Kind::Pen;
        item.style.width = 9;
        item.style.color = qRgb(229, 57, 53);
        for (int i = 0; i < 400; ++i)
            item.points.append(QPointF(40 + i * 4.3, 300 + 160 * std::sin(i * 0.045) + 40 * std::sin(i * 0.31)));
        annot::Document doc;
        doc.canvas = QSize(1800, 600);
        doc.crop = QRect(QPoint(0, 0), doc.canvas);
        doc.items.append(item);
        annot::Compositor compositor;
        compositor.setTransparent(true);
        QImage base(doc.canvas, QImage::Format_ARGB32_Premultiplied);
        base.fill(Qt::transparent);
        QImage tiles;
        compositor.render(doc, base, doc.crop, doc.crop, &tiles);

        live::Mesh mesh;
        const qsizetype chunks = (live::pieceCount(item.points.size()) + live::kChunk - 1) / live::kChunk;
        for (qsizetype c = 0; c < chunks; ++c)
            live::appendChunk(mesh, item.points, c, item.style.width, 0, item.style.color); // no fringe: the edge at the width
        QImage gpu(doc.canvas, QImage::Format_ARGB32_Premultiplied);
        gpu.fill(Qt::transparent);
        {
            QPainter painter(&gpu);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor::fromRgb(item.style.color));
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const live::Vertex& a = mesh.vertices[mesh.indices[i]];
                const live::Vertex& b = mesh.vertices[mesh.indices[i + 1]];
                const live::Vertex& c = mesh.vertices[mesh.indices[i + 2]];
                if (a.a < 128 || b.a < 128 || c.a < 128)
                    continue; // the fringe (empty here)
                painter.drawPolygon(QPolygonF {QPointF(a.x, a.y), QPointF(b.x, b.y), QPointF(c.x, c.y)});
            }
        }
        int both = 0;
        int either = 0;
        for (int y = 0; y < doc.canvas.height(); ++y) {
            for (int x = 0; x < doc.canvas.width(); ++x) {
                const bool t = qAlpha(tiles.pixel(x, y)) >= 128;
                const bool g = qAlpha(gpu.pixel(x, y)) >= 128;
                both += t && g;
                either += t || g;
            }
        }
        const double overlap = double(both) / std::max(1, either);
        qInfo().noquote() << u"Live stroke vs the tiles: %1 of the covered pixels in both"_s.arg(overlap, 0, 'f', 4);
        QVERIFY(overlap > 0.95);
    }
};

int main(int argc, char** argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    if (!qEnvironmentVariableIsSet("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", (qEnvironmentVariable("WINDIR", u"C:\\Windows"_s) + u"\\Fonts"_s).toLocal8Bit());
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    TestPresenter test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_presenter.moc"
