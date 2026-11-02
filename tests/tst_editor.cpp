// The editing window's controller (src/capture/CaptureEditor; docs/design-
// capture-roadmap.md, 2.2): the zoom and where the picture lies, the mouse
// as image pixels, the ways out (a new entry, the original again, a pin
// replaced), Esc, the crop, and the window going away empty. No native
// window: the owner's window hooks are stood in for, and the window is an
// offscreen QQuickWindow. The pin's PinWindow::setPicture is checked with a
// real (briefly shown) pin.
#include "CaptureEditor.h"
#include "CaptureSession.h"

#include "Annotation.h"
#include "AnnotationFormat.h"
#include "AnnotationModel.h"
#include "AnnotationRender.h"
#include "AnnotationStyles.h"
#include "EditView.h"
#include "Geometry.h"
#include "PinWindow.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QTest>

#include <cmath>
#include <optional>

using namespace ws::capture;
using namespace ws::capture::annot;
using namespace Qt::StringLiterals;

namespace {

QImage noise(QSize size, quint64 seed)
{
    quint64 state = seed;
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            quint64 z = (state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            line[x] = 0xff000000u | QRgb((z ^ (z >> 31)) & 0xffffffu);
        }
    }
    return image;
}

int differing(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.format() != b.format())
        return -1;
    int count = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto* x = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* w = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int i = 0; i < a.width(); ++i)
            count += x[i] != w[i];
    }
    return count;
}

// A project of `base` with one red arrow.
QByteArray arrowProject(const QImage& base, QRect crop = {})
{
    Document doc;
    doc.canvas = base.size();
    doc.crop = crop.isNull() ? QRect(QPoint(0, 0), base.size()) : crop;
    doc.dpr = 1.5;
    Item arrow;
    arrow.id = 1;
    arrow.seq = 1;
    arrow.kind = Kind::Arrow;
    arrow.points = {QPointF(30.5, 30.5), QPointF(160.5, 90.5)};
    arrow.style.width = 6;
    doc.items.append(arrow);
    doc.nextId = 2;
    doc.nextSeq = 2;
    return writeProject(doc, base);
}

struct Calls {
    QList<QImage> copied;
    QList<QByteArray> copiedProjects;
    QList<qint64> touched;
    QList<QImage> recorded;
    QList<QByteArray> recordedProjects;
    QList<QImage> saved;
    QList<QString> savedPaths;
    int savePickers = 0;
    QList<QImage> pinned;
    QList<QPoint> pinnedAt;
    QList<QString> styles;
    int closed = 0;
    QList<bool> cloaks;
    int shows = 0;
};

// An editor with its hooks recording, over a history of one entry.
struct Rig {
    explicit Rig(QSize picture = QSize(400, 300), bool project = true, bool exists = true)
        : base(noise(picture, 5))
    {
        original = project ? arrowProject(base) : QByteArray();
        CaptureEditor::Hooks hooks;
        hooks.project = [this](qint64) { return original; };
        hooks.picture = [this](qint64) { return base; };
        hooks.exists = [exists](qint64) { return exists; };
        hooks.copy = [this](const QImage& image, const QByteArray& p) {
            calls.copied.append(image);
            calls.copiedProjects.append(p);
        };
        hooks.copyOriginal = [this](qint64 id) { calls.touched.append(id); };
        hooks.record = [this](const QImage& image, const QByteArray& p) {
            calls.recorded.append(image);
            calls.recordedProjects.append(p);
        };
        hooks.save = [this](const QImage& image, const QString& path) {
            calls.saved.append(image);
            calls.savedPaths.append(path);
        };
        hooks.chooseSavePath = [this](QQuickWindow*) { ++calls.savePickers; return savePath; };
        hooks.pin = [this](const QImage& image, const QByteArray&, QPoint at) {
            calls.pinned.append(image);
            calls.pinnedAt.append(at);
        };
        hooks.styles = [] { return QString(); };
        hooks.stylesChanged = [this](const QString& json) { calls.styles.append(json); };
        hooks.closed = [this] { ++calls.closed; };
        editor.setHooks(std::move(hooks));
        // A window of 1000 x 700 at 150 %: Qt's real buffer scale is exactly 1.5 there.
        editor.setMetrics(QSize(1000, 700), 1.5);
    }

    QString savePath = u"C:/chosen/capture.png"_s; // intercepted by the save hook; no file is written

    // The window position (logical) of a device pixel inside an image pixel: Qt's
    // logical positions are whole device pixels over the scale (1.5 here).
    QPointF at(QPoint pixel) const
    {
        const double zoom = editor.zoom();
        const int half = int(zoom / 2);
        const QPoint device = editor.viewportDevice().topLeft() + editor.originDevice()
            + QPoint(int(std::lround(pixel.x() * zoom)) + half, int(std::lround(pixel.y() * zoom)) + half);
        return QPointF(device.x() / 1.5, device.y() / 1.5);
    }
    void click(QPoint pixel, int modifiers = 0)
    {
        const QPointF p = at(pixel);
        editor.mousePressed(p.x(), p.y(), Qt::LeftButton, modifiers);
        editor.mouseReleased(p.x(), p.y(), Qt::LeftButton);
    }
    void drag(QPoint from, QPoint to, int modifiers = 0)
    {
        const QPointF a = at(from);
        editor.mousePressed(a.x(), a.y(), Qt::LeftButton, modifiers);
        for (int i = 1; i <= 6; ++i) {
            const QPointF p = at(from + (to - from) * i / 6);
            editor.mouseMoved(p.x(), p.y(), modifiers);
        }
        const QPointF b = at(to);
        editor.mouseReleased(b.x(), b.y(), Qt::LeftButton);
    }
    int toolIndex(Kind kind) const
    {
        const QList<ToolInfo>& list = tools();
        for (qsizetype i = 0; i < list.size(); ++i) {
            if (list[i].kind == kind)
                return int(i);
        }
        return -1;
    }

    QImage base;
    QByteArray original;
    Calls calls;
    CaptureEditor editor;
};

} // namespace

class TestEditor : public QObject {
    Q_OBJECT

private slots:
    void exitRules()
    {
        using namespace editview;
        // A history entry: changed, a new entry; unchanged, the original again.
        QCOMPARE(planDone(Origin::History, true, true).exit, Exit::NewEntry);
        QCOMPARE(planDone(Origin::History, false, true).exit, Exit::TouchOriginal);
        QCOMPARE(planDone(Origin::History, false, false).exit, Exit::NewEntry); // it was deleted meanwhile
        QVERIFY(!planDone(Origin::History, true, true).record);
        // A pin: 完成 hands over a changed picture, and the history gets it too.
        const ExitPlan changed = planDone(Origin::Pin, true, true);
        QCOMPARE(changed.exit, Exit::ReplacePin);
        QVERIFY(changed.record);
        QCOMPARE(planDone(Origin::Pin, false, true).exit, Exit::Close);
        QCOMPARE(planDone(Origin::Pin, true, false).exit, Exit::NewEntry); // closed meanwhile
        QCOMPARE(planDone(Origin::Fresh, false, false).exit, Exit::NewEntry);
    }

    void windowForAPin()
    {
        using namespace editview;
        const QRect work(0, 0, 2560, 1400);
        // With room: the canvas lands exactly where the picture was.
        WindowPlace place = placeForPin(QRect(800, 600, 400, 300), QPoint(300, 200), QSize(1000, 700), work);
        QCOMPARE(place.topLeft, QPoint(500, 400));
        QVERIFY(place.exact);
        // Too near the top left: moved in, and told it is no longer exact.
        place = placeForPin(QRect(100, 80, 400, 300), QPoint(300, 200), QSize(1000, 700), work);
        QCOMPARE(place.topLeft, QPoint(0, 0));
        QVERIFY(!place.exact);
        // Too near the bottom right.
        place = placeForPin(QRect(2400, 1300, 400, 300), QPoint(300, 200), QSize(1000, 700), work);
        QCOMPARE(place.topLeft, QPoint(1560, 700));
        QVERIFY(!place.exact);
        // A window larger than the work area: its top left stays at the area's.
        place = placeForPin(QRect(10, 10, 100, 100), QPoint(0, 0), QSize(3000, 2000), work);
        QCOMPARE(place.topLeft, QPoint(0, 0));
    }

    void viewIsOnDevicePixels()
    {
        Rig rig(QSize(3000, 2000), false);
        QVERIFY(rig.editor.openImage(rig.base));
        // Too large for the window: fitted, the whole picture shown.
        QVERIFY(rig.editor.fitted());
        QVERIFY(rig.editor.zoom() < 1.0);
        QVERIFY(rig.editor.linear());
        QCOMPARE(rig.editor.imageSize(), QSize(3000, 2000));
        const QRectF seen = rig.editor.visible();
        QVERIFY(seen.contains(QPointF(1, 1)));
        QVERIFY(seen.right() >= 2999 && seen.bottom() >= 1999);
        // Z: 100 %, one image pixel one device pixel, nearest sampling; back to the whole.
        QVERIFY(rig.editor.keyPressed(Qt::Key_Z, 0));
        QCOMPARE(rig.editor.zoom(), 1.0);
        QVERIFY(!rig.editor.linear());
        QVERIFY(!rig.editor.fitted());
        QCOMPARE(rig.editor.zoomText(), u"100%"_s);
        // The picture's top left, in the window, is on a device pixel; a pixel is 1/1.5 logical px.
        const QPointF origin = rig.editor.contentOrigin();
        QCOMPARE(std::fmod(origin.x() * 1.5, 1.0), 0.0);
        QCOMPARE(std::fmod(origin.y() * 1.5, 1.0), 0.0);
        QCOMPARE(rig.editor.scaleX(), 1.0 / 1.5);
        QCOMPARE(rig.editor.unitX(), 1.0 / 1.5);
        QVERIFY(rig.editor.keyPressed(Qt::Key_Z, 0));
        QVERIFY(rig.editor.fitted());
        QVERIFY(rig.editor.zoom() < 1.0);
    }

    void pixelsUnderTheMouse()
    {
        Rig rig(QSize(1200, 800), false);
        QVERIFY(rig.editor.openImage(rig.base));
        rig.editor.zoomTo(1.0);
        QCOMPARE(rig.editor.zoom(), 1.0);
        // A rectangle dragged from pixel to pixel is those pixels, at 100 %.
        rig.editor.model()->setTool(rig.toolIndex(Kind::Rect));
        rig.drag(QPoint(100, 120), QPoint(220, 260));
        const Item& rect = rig.editor.model()->document().items.last();
        QCOMPARE(itemBox(rect), QRect(QPoint(100, 120), QPoint(220, 260)));
        // Scrolled by the wheel and zoomed in 200 %: the same mapping.
        rig.editor.zoomTo(2.0);
        QCOMPARE(rig.editor.zoom(), 2.0);
        rig.drag(QPoint(500, 400), QPoint(560, 460));
        const Item& second = rig.editor.model()->document().items.last();
        QCOMPARE(itemBox(second), QRect(QPoint(500, 400), QPoint(560, 460)));
        // Zoomed about the pointer: the pixel under it stays under it.
        const QPointF before = rig.at(QPoint(600, 500));
        rig.editor.zoomTo(4.0, QPoint(int(std::lround(before.x() * 1.5)), int(std::lround(before.y() * 1.5))) - rig.editor.viewportDevice().topLeft());
        const QPointF after = rig.at(QPoint(600, 500));
        QVERIFY(std::abs(after.x() - before.x()) <= 3.0 / 1.5);
        QVERIFY(std::abs(after.y() - before.y()) <= 3.0 / 1.5);
        // Hit tolerances are screen sizes: the model hears of the zoom.
        rig.editor.model()->setTool(-1);
    }

    void keysAndWheelZoom()
    {
        Rig rig(QSize(1200, 800), false);
        QVERIFY(rig.editor.openImage(rig.base));
        QCOMPARE(rig.editor.zoom(), 1.0);
        QVERIFY(rig.editor.keyPressed(Qt::Key_Plus, Qt::ControlModifier));
        QVERIFY(rig.editor.zoom() > 1.0);
        const double up = rig.editor.zoom();
        QVERIFY(rig.editor.keyPressed(Qt::Key_Minus, Qt::ControlModifier));
        QCOMPARE(rig.editor.zoom(), 1.0);
        QVERIFY(rig.editor.keyPressed(Qt::Key_Equal, Qt::ControlModifier | Qt::KeypadModifier)); // + on a US keyboard
        QCOMPARE(rig.editor.zoom(), up);
        QVERIFY(rig.editor.keyPressed(Qt::Key_0, Qt::ControlModifier));
        QVERIFY(rig.editor.fitted());
        // Ctrl+wheel: a notch is a step, a touchpad's pieces add up.
        const QPointF p = rig.at(QPoint(300, 300));
        rig.editor.wheeled(p.x(), p.y(), 120, 0, Qt::ControlModifier);
        QVERIFY(rig.editor.zoom() > 1.0);
        const double one = rig.editor.zoom();
        rig.editor.wheeled(p.x(), p.y(), 40, 0, Qt::ControlModifier);
        rig.editor.wheeled(p.x(), p.y(), 40, 0, Qt::ControlModifier);
        QCOMPARE(rig.editor.zoom(), one);
        rig.editor.wheeled(p.x(), p.y(), 40, 0, Qt::ControlModifier);
        QVERIFY(rig.editor.zoom() > one);
        // The plain wheel scrolls (a long picture); the model's size is Alt+wheel.
        rig.editor.zoomTo(2.0);
        const QPointF before = rig.editor.contentOrigin();
        rig.editor.wheeled(p.x(), p.y(), -120, 0, 0);
        QVERIFY(rig.editor.contentOrigin().y() < before.y());
        rig.editor.wheeled(p.x(), p.y(), -120, 0, Qt::ShiftModifier);
        QVERIFY(rig.editor.contentOrigin().x() < before.x());
        // Never past the picture's edge.
        rig.editor.scrollBy(QPoint(100000, 100000));
        QCOMPARE(rig.editor.contentOrigin(), QPointF(0, rig.editor.topHeight()));
        rig.editor.scrollBy(QPoint(-100000, -100000));
        QVERIFY(rig.editor.visible().right() >= 1199.5 && rig.editor.visible().bottom() >= 799.5);
        // Space and drag moves the picture, and nothing is drawn.
        rig.editor.model()->setTool(rig.toolIndex(Kind::Rect));
        rig.editor.spaceHeld(true);
        QCOMPARE(rig.editor.cursorShape(), int(Qt::OpenHandCursor));
        const QPointF origin = rig.editor.contentOrigin();
        rig.editor.mousePressed(500, 400, Qt::LeftButton, 0);
        rig.editor.mouseMoved(540, 430, 0);
        rig.editor.mouseReleased(540, 430, Qt::LeftButton);
        rig.editor.spaceHeld(false);
        QVERIFY(rig.editor.contentOrigin() != origin);
        QCOMPARE(rig.editor.model()->document().items.size(), qsizetype(0));
    }

    void unchangedEnterCopiesTheOriginalAgain()
    {
        Rig rig;
        const QByteArray original = rig.original;
        QVERIFY(rig.editor.openHistory(7));
        QVERIFY(rig.editor.isOpen());
        QVERIFY(!rig.editor.fromPin());
        QCOMPARE(rig.editor.copyText(), u"复制"_s);
        QCOMPARE(rig.editor.model()->document().items.size(), qsizetype(1));
        QVERIFY(!rig.editor.model()->modified());
        // 100 % shows the entry's own picture: the compositor's, pixel for pixel.
        QVERIFY(rig.editor.keyPressed(Qt::Key_Return, 0));
        QCOMPARE(rig.calls.touched, (QList<qint64>{7}));
        QVERIFY(rig.calls.copied.isEmpty());
        QVERIFY(rig.calls.recorded.isEmpty());
        QVERIFY(!rig.editor.isOpen());
        QCOMPARE(rig.calls.closed, 1);
        QCOMPARE(rig.original, original); // nothing was written
    }

    void changedEnterIsANewEntry()
    {
        Rig rig;
        QVERIFY(rig.editor.openHistory(7));
        // The arrow made blue.
        rig.click(QPoint(95, 60)); // on the arrow's middle
        QCOMPARE(rig.editor.model()->selectedCount(), 1);
        rig.editor.model()->setPanel({{u"color"_s, u"#1E88E5"_s}});
        QVERIFY(rig.editor.model()->modified());
        rig.editor.act(0); // 复制
        QCOMPARE(rig.calls.copied.size(), qsizetype(1));
        QVERIFY(rig.calls.touched.isEmpty());
        QCOMPARE(rig.calls.copied[0].size(), rig.base.size());
        // The new entry's project opens with the arrow blue and the base as it was; the original is untouched.
        QString error;
        const std::optional<Project> project = readProject(rig.calls.copiedProjects[0], &error);
        QVERIFY2(project.has_value(), qPrintable(error));
        QCOMPARE(project->doc.items.size(), qsizetype(1));
        QCOMPARE(project->doc.items[0].style.color, qRgb(0x1E, 0x88, 0xE5));
        QCOMPARE(differing(project->base, rig.base), 0);
        QVERIFY(rig.calls.copiedProjects[0] != rig.original);
        const std::optional<Project> before = readProject(rig.original, &error);
        QCOMPARE(before->doc.items[0].style.color, Style().color);
        // The picture sent is the composition.
        Compositor compositor;
        QCOMPARE(differing(rig.calls.copied[0], compositor.exportImage(project->doc, project->base, QRect(QPoint(0, 0), project->base.size()))), 0);
        QVERIFY(!rig.editor.isOpen());
    }

    void aPictureWithoutAProject()
    {
        Rig rig(QSize(400, 300), false);
        QVERIFY(rig.editor.openHistory(3));
        // The picture is the base of an empty document.
        QCOMPARE(rig.editor.model()->document().items.size(), qsizetype(0));
        QCOMPARE(rig.editor.model()->document().canvas, rig.base.size());
        QCOMPARE(rig.editor.model()->document().crop, QRect(QPoint(0, 0), rig.base.size()));
        QVERIFY(!rig.editor.model()->modified());
        QCOMPARE(differing(rig.editor.model()->composed(), rig.base), 0);
        // Unchanged: the original again, as for any entry.
        rig.editor.done();
        QCOMPARE(rig.calls.touched, (QList<qint64>{3}));
        // Changed: a new entry that can be edited again.
        QVERIFY(rig.editor.openHistory(3));
        rig.editor.model()->setTool(rig.toolIndex(Kind::Rect));
        rig.drag(QPoint(40, 50), QPoint(140, 120));
        rig.editor.done();
        QCOMPARE(rig.calls.copied.size(), qsizetype(1));
        const std::optional<Project> project = readProject(rig.calls.copiedProjects[0], nullptr);
        QVERIFY(project.has_value());
        QCOMPARE(project->doc.items.size(), qsizetype(1));
        QCOMPARE(differing(project->base, rig.base), 0);
        QVERIFY(differing(rig.calls.copied[0], rig.base) > 0);
        // A transparent picture is put on white, not black.
        QImage clear(40, 30, QImage::Format_ARGB32);
        clear.fill(Qt::transparent);
        Rig other(QSize(40, 30), false);
        other.base = clear;
        QVERIFY(other.editor.openHistory(1));
        QCOMPARE(other.editor.model()->base().pixel(5, 5), qRgb(255, 255, 255));
    }

    void escapeLadder()
    {
        Rig rig;
        QVERIFY(rig.editor.openHistory(7));
        rig.editor.model()->setTool(rig.toolIndex(Kind::Rect));
        rig.drag(QPoint(200, 100), QPoint(260, 160));
        rig.editor.model()->setTool(rig.toolIndex(Kind::Ellipse));
        rig.drag(QPoint(210, 170), QPoint(300, 250));
        rig.editor.model()->setTool(-1);
        QCOMPARE(rig.editor.model()->document().items.size(), qsizetype(3));
        // Esc with changes gives them all up, in one step, and says so.
        QSignalSpy notice(&rig.editor, &CaptureEditor::noticeChanged);
        QVERIFY(rig.editor.keyPressed(Qt::Key_Escape, 0));
        QVERIFY(rig.editor.isOpen());
        QCOMPARE(rig.editor.model()->document().items.size(), qsizetype(1));
        QVERIFY(!rig.editor.model()->modified());
        QCOMPARE(rig.editor.notice(), u"已放弃修改 · Ctrl+Z 恢复"_s);
        QVERIFY(notice.count() >= 1);
        // Ctrl+Z gives them back.
        QVERIFY(rig.editor.keyPressed(Qt::Key_Z, Qt::ControlModifier));
        QCOMPARE(rig.editor.model()->document().items.size(), qsizetype(3));
        QVERIFY(rig.editor.model()->modified());
        // Esc again gives up again; Esc once more finds nothing to give up and closes.
        QVERIFY(rig.editor.keyPressed(Qt::Key_Escape, 0));
        QVERIFY(rig.editor.isOpen());
        QVERIFY(rig.editor.keyPressed(Qt::Key_Escape, 0));
        QVERIFY(!rig.editor.isOpen());
        QCOMPARE(rig.calls.closed, 1);
        QVERIFY(rig.calls.copied.isEmpty() && rig.calls.touched.isEmpty()); // closing hands nothing over
        // A step of the model's comes first: Esc in the middle of a stroke drops the stroke only.
        QVERIFY(rig.editor.openHistory(7));
        rig.editor.model()->setTool(rig.toolIndex(Kind::Pen));
        const QPointF a = rig.at(QPoint(50, 200));
        rig.editor.mousePressed(a.x(), a.y(), Qt::LeftButton, 0);
        const QPointF b = rig.at(QPoint(90, 230));
        rig.editor.mouseMoved(b.x(), b.y(), 0);
        QVERIFY(rig.editor.model()->gestureActive());
        QVERIFY(rig.editor.keyPressed(Qt::Key_Escape, 0));
        QVERIFY(rig.editor.isOpen());
        QVERIFY(!rig.editor.model()->gestureActive());
        QCOMPARE(rig.editor.model()->document().items.size(), qsizetype(1));
    }

    void cropAndBringItBack()
    {
        Rig rig(QSize(400, 300), false);
        QVERIFY(rig.editor.openHistory(5));
        rig.editor.zoomTo(1.0);
        // The bottom right corner dragged in, with no tool on.
        rig.drag(QPoint(399, 299), QPoint(300, 200));
        const QRect crop = rig.editor.model()->cropRect();
        QVERIFY(crop != QRect(0, 0, 400, 300));
        QCOMPARE(rig.editor.sizeText(), u"%1 × %2"_s.arg(crop.width()).arg(crop.height()));
        rig.editor.done();
        // The exported size is the crop; the project keeps all the picture and the crop.
        QCOMPARE(rig.calls.copied.size(), qsizetype(1));
        QCOMPARE(rig.calls.copied[0].size(), crop.size());
        QCOMPARE(differing(rig.calls.copied[0], rig.base.copy(crop)), 0);
        const std::optional<Project> project = readProject(rig.calls.copiedProjects[0], nullptr);
        QVERIFY(project.has_value());
        QCOMPARE(project->doc.crop, crop);
        QCOMPARE(project->base.size(), rig.base.size());
        QCOMPARE(differing(project->base, rig.base), 0);
        // Opened again, the cropped parts are still there, and dragging the edge out brings them back.
        rig.original = rig.calls.copiedProjects[0];
        QVERIFY(rig.editor.openHistory(5));
        rig.editor.zoomTo(1.0);
        QCOMPARE(rig.editor.model()->cropRect(), crop);
        QVERIFY(!rig.editor.model()->modified());
        rig.drag(QPoint(crop.right(), crop.bottom()), QPoint(450, 350));
        QCOMPARE(rig.editor.model()->cropRect(), QRect(0, 0, 400, 300));
        rig.editor.act(0);
        QCOMPARE(rig.calls.copied.size(), qsizetype(2));
        QCOMPARE(differing(rig.calls.copied[1], rig.base), 0);
        // Nothing drawn and nothing cropped: no project to keep.
        QVERIFY(rig.calls.copiedProjects[1].isEmpty());
        // Ctrl+Z takes a crop back.
        rig.original = rig.calls.copiedProjects[1];
        QVERIFY(rig.editor.openHistory(5));
        rig.drag(QPoint(399, 299), QPoint(250, 150));
        QVERIFY(rig.editor.model()->cropRect() != QRect(0, 0, 400, 300));
        QVERIFY(rig.editor.keyPressed(Qt::Key_Z, Qt::ControlModifier));
        QCOMPARE(rig.editor.model()->cropRect(), QRect(0, 0, 400, 300));
    }

    void saveAndPin()
    {
        Rig rig;
        QVERIFY(rig.editor.openHistory(7));
        rig.editor.act(1); // 保存
        QCOMPARE(rig.calls.saved.size(), qsizetype(1));
        QCOMPARE(rig.calls.savePickers, 1);
        QCOMPARE(rig.calls.savedPaths[0], rig.savePath);
        QCOMPARE(rig.calls.recorded.size(), qsizetype(1)); // the history gets it too
        QCOMPARE(rig.calls.recordedProjects[0].isEmpty(), false);
        QVERIFY(!rig.editor.isOpen());
        QVERIFY(rig.editor.openHistory(7));
        rig.editor.act(3); // 贴图
        QCOMPARE(rig.calls.pinned.size(), qsizetype(1));
        QCOMPARE(rig.calls.recorded.size(), qsizetype(2));
        QVERIFY(!rig.editor.isOpen());
        // OCR does not close the window.
        QVERIFY(rig.editor.openHistory(7));
        QImage got;
        CaptureEditor::Hooks hooks;
        hooks.ocr = [&got](const QImage& image, double, const QRect&) { got = image; };
        rig.editor.setHooks(hooks);
        rig.editor.act(4);
        QVERIFY(!got.isNull());
        QVERIFY(rig.editor.isOpen());
        rig.editor.close();
    }

    void cancellingSaveKeepsTheEditorAndOldPathSkipsThePicker()
    {
        Rig rig;
        QVERIFY(rig.editor.openHistory(7));
        rig.savePath.clear();
        rig.editor.act(1); // Cancel Save As.
        QCOMPARE(rig.calls.savePickers, 1);
        QVERIFY(rig.editor.isOpen());
        QVERIFY(rig.calls.saved.isEmpty());
        QVERIFY(rig.calls.recorded.isEmpty());
        QCOMPARE(rig.calls.closed, 0);
        QVERIFY(rig.editor.model()->hasItems());
        // Try again, still cancelling: nothing is recorded or closed.
        rig.editor.act(1);
        QCOMPARE(rig.calls.savePickers, 2);
        QVERIFY(rig.editor.isOpen());
        QVERIFY(rig.calls.saved.isEmpty());
        // The menu's action deliberately keeps the old automatic save contract.
        rig.editor.act(static_cast<int>(ws::capture::CaptureSession::Action::SavePrevious));
        QCOMPARE(rig.calls.savePickers, 2);
        QCOMPARE(rig.calls.saved.size(), qsizetype(1));
        QVERIFY(rig.calls.savedPaths[0].isEmpty());
        QCOMPARE(rig.calls.recorded.size(), qsizetype(1));
        QVERIFY(!rig.editor.isOpen());
    }

    void pinDone()
    {
        const QImage picture = noise(QSize(300, 200), 9);
        bool shown = true;
        bool alive = true;
        QImage replacedWith;
        QByteArray replacedProject;
        int replaced = 0;
        Rig rig;
        auto makeTarget = [&] {
            CaptureEditor::PinTarget target;
            target.image = picture;
            target.picture = QRect(500, 400, 300, 200);
            target.zoom = 1.0;
            target.alive = [&alive] { return alive; };
            target.setShown = [&shown](bool on) { shown = on; };
            target.replace = [&](const QImage& image, const QByteArray& project) {
                replacedWith = image;
                replacedProject = project;
                ++replaced;
            };
            return target;
        };
        // Unchanged: 完成 hands nothing over, the pin comes back as it was.
        QVERIFY(rig.editor.openPin(makeTarget()));
        QVERIFY(rig.editor.fromPin());
        QCOMPARE(rig.editor.copyText(), u"完成"_s);
        QCOMPARE(rig.editor.model()->document().canvas, QSize(300, 200));
        QVERIFY(!rig.editor.model()->modified());
        QCOMPARE(rig.editor.zoom(), 1.0); // the pin's zoom, not a fit
        rig.editor.done();
        QCOMPARE(replaced, 0);
        QVERIFY(rig.calls.copied.isEmpty());
        QVERIFY(shown);
        // Changed: the pin shows the new picture, and the history gets it.
        QVERIFY(rig.editor.openPin(makeTarget()));
        rig.editor.model()->setTool(rig.toolIndex(Kind::Rect));
        rig.drag(QPoint(30, 30), QPoint(120, 90));
        rig.editor.keyPressed(Qt::Key_Return, 0);
        QCOMPARE(replaced, 1);
        QCOMPARE(replacedWith.size(), picture.size());
        QVERIFY(differing(replacedWith, picture) > 0);
        QVERIFY(!replacedProject.isEmpty());
        QVERIFY(readProject(replacedProject, nullptr).has_value());
        QCOMPARE(rig.calls.recorded.size(), qsizetype(1));
        QVERIFY(rig.calls.copied.isEmpty()); // the system clipboard is left alone
        QVERIFY(shown);
        QVERIFY(!rig.editor.isOpen());
        // F3 on a pin is 完成 too, not a second pin.
        QVERIFY(rig.editor.openPin(makeTarget()));
        rig.editor.model()->setTool(rig.toolIndex(Kind::Rect));
        rig.drag(QPoint(30, 30), QPoint(120, 90));
        QVERIFY(rig.editor.keyPressed(Qt::Key_F3, 0));
        QCOMPARE(replaced, 2);
        QVERIFY(rig.calls.pinned.isEmpty());
        // Esc leaves the pin as it was (and shows it).
        QVERIFY(rig.editor.openPin(makeTarget()));
        QVERIFY(rig.editor.keyPressed(Qt::Key_Escape, 0));
        QVERIFY(!rig.editor.isOpen());
        QVERIFY(shown);
        QCOMPARE(replaced, 2);
        // A pin closed meanwhile: a changed picture is copied instead.
        QVERIFY(rig.editor.openPin(makeTarget()));
        rig.editor.model()->setTool(rig.toolIndex(Kind::Rect));
        rig.drag(QPoint(30, 30), QPoint(120, 90));
        alive = false;
        rig.editor.done();
        QCOMPARE(replaced, 2);
        QCOMPARE(rig.calls.copied.size(), qsizetype(1));
        // And one that is gone cannot be opened.
        QVERIFY(!rig.editor.openPin(makeTarget()));
    }

    void stylesAreRememberedWhenClosing()
    {
        Rig rig;
        QVERIFY(rig.editor.openHistory(7));
        rig.editor.close();
        QVERIFY(rig.calls.styles.isEmpty()); // nothing changed
        QVERIFY(rig.editor.openHistory(7));
        rig.editor.model()->setTool(rig.toolIndex(Kind::Arrow));
        rig.editor.model()->setPanel({{u"color"_s, u"#1E88E5"_s}, {u"width"_s, 7}});
        rig.editor.close();
        QCOMPARE(rig.calls.styles.size(), qsizetype(1));
        QVERIFY(rig.calls.styles[0].contains(u"#1E88E5"_s, Qt::CaseInsensitive));
        // And everything is let go of.
        QVERIFY(!rig.editor.model()->active());
        QVERIFY(rig.editor.model()->base().isNull());
        QVERIFY(!rig.editor.isOpen());
    }

    void systemCloseKeepsWindowForNextPicture()
    {
        QQuickWindow window;
        window.setFlags(Qt::Window | Qt::FramelessWindowHint);
        window.resize(1000, 700);
        Rig rig(QSize(200, 120), false);
        CaptureEditor::Hooks hooks;
        hooks.picture = [&rig](qint64) { return rig.base; };
        int closed = 0;
        hooks.closed = [&closed] { ++closed; };
        hooks.window.show = [](QQuickWindow* w) { w->show(); };
        rig.editor.setHooks(std::move(hooks));
        rig.editor.attach(&window);
        QVERIFY(rig.editor.openHistory(1));
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const WId before = window.winId();

        window.close(); // Alt+F4 / the system menu, outside the QML title bar
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 3000);
        QVERIFY(!rig.editor.isOpen());
        QVERIFY(rig.editor.model()->base().isNull());
        QCOMPARE(closed, 1);
        QCOMPARE(window.winId(), before); // WindowFrame's native binding must survive

        QVERIFY(rig.editor.openHistory(2));
        QVERIFY(window.isVisible());
        QCOMPARE(window.winId(), before);
        rig.editor.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 3000);
        QCOMPARE(closed, 2);
    }

    void aLongPictureAndTheWindow()
    {
        // The window: an offscreen QQuickWindow, shown and cloaked by stand-ins.
        QQuickWindow window;
        window.setTitle(u"edit"_s);
        window.resize(1000, 700);
        Rig rig(QSize(1080, 30000), false);
        CaptureEditor::Hooks hooks;
        hooks.picture = [&rig](qint64) { return rig.base; };
        QList<bool> cloaks;
        int closed = 0;
        hooks.closed = [&closed] { ++closed; };
        hooks.window.show = [](QQuickWindow* w) { w->show(); };
        hooks.window.cloak = [&cloaks](QQuickWindow*, bool on) { cloaks.append(on); };
        rig.editor.setHooks(hooks);
        rig.editor.attach(&window);
        QVERIFY(rig.editor.openHistory(1));
        QVERIFY(window.isVisible());
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // Opened to the width of the picture, not as a sliver of the whole.
        QVERIFY(rig.editor.zoom() >= editview::kLeastFit);
        QVERIFY(rig.editor.visible().height() < 30000);
        QVERIFY(window.width() <= int(QGuiApplication::primaryScreen()->availableGeometry().width() * CaptureEditor::kWorkShare) + 1);
        QVERIFY(window.height() <= int(QGuiApplication::primaryScreen()->availableGeometry().height() * CaptureEditor::kWorkShare) + 1);
        // Closed: cloaked, drawn empty, then hidden and uncloaked, and the owner is told.
        rig.editor.close();
        QVERIFY(!rig.editor.isOpen());
        QVERIFY(cloaks.contains(true));
        QTRY_VERIFY_WITH_TIMEOUT(closed == 1, 3000);
        QVERIFY(!window.isVisible());
        QCOMPARE(cloaks.last(), false);
    }

    void aPinGetsANewPicture()
    {
        PinWindow::Host host;
        host.closed = [](PinWindow*) {};
        host.menu = [](PinWindow*, POINT) {};
        host.copy = [](PinWindow*) {};
        host.save = [](PinWindow*) {};
        host.dark = [] { return false; };
        PinWindow pin(noise(QSize(200, 100), 1), QPoint(300, 300), host);
        if (!pin.hwnd())
            QSKIP("no window station to make a pin on");
        const QPoint at = pin.pictureRect().topLeft();
        const double zoom = pin.zoom();
        pin.setPicture(noise(QSize(260, 150), 2), QByteArray("project"));
        // The place and the zoom stay; the picture and its project are the new ones.
        QCOMPARE(pin.pictureRect().topLeft(), at);
        QCOMPARE(pin.zoom(), zoom);
        QCOMPARE(pin.pictureRect().size(), QSize(260, 150));
        QCOMPARE(pin.image().size(), QSize(260, 150));
        QCOMPARE(pin.project(), QByteArray("project"));
        pin.setPicture(QImage(), QByteArray("nothing")); // a null picture changes nothing
        QCOMPARE(pin.image().size(), QSize(260, 150));
        QCOMPARE(pin.project(), QByteArray("project"));
        // Out of sight while edited, and back.
        pin.setShown(false);
        QVERIFY(!::IsWindowVisible(pin.hwnd()));
        pin.setShown(true);
        QVERIFY(::IsWindowVisible(pin.hwnd()));
        pin.close();
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
    TestEditor test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_editor.moc"
