// Stage 7 of the screen capture (docs/design-capture-roadmap.md, 2.5), the parts
// that need no window: turning and flipping a pin's picture, the small forms
// it collapses to and where they go, dragging a corner, the text picked from
// the lines the reader found, the colour formats, the delayed capture's
// countdown, Shift+R's monitor memory, the mouse cursor drawn into a picture,
// the rules for asking UI Automation, and the QR reader (with codes made by
// zxing-cpp's own writer).
#include "CaptureTools.h"
#include "Geometry.h"
#include "PinTools.h"
#include "QrCode.h"
#include "WindowSnapshot.h"

#include <BitMatrix.h>
#include <qrcode/QRWriter.h>

#include <QGuiApplication>
#include <QPainter>
#include <QElapsedTimer>
#include <QDirIterator>
#include <QScopeGuard>
#include <QTest>

#include <windows.h>

#include <cmath>
#include <array>
#include <cstring>

using namespace ws::capture;
using namespace Qt::StringLiterals;

namespace {

// Own the cursor fixture: IDC_ARROW can be a transparent or custom system
// cursor. The drawing test must not depend on the user's current theme.
HCURSOR testCursor()
{
    std::array<BYTE, 32 * 4> andMask;
    andMask.fill(0xFF);
    std::array<BYTE, 32 * 4> xorMask {};
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x <= y / 2; ++x) {
            const std::size_t at = static_cast<std::size_t>(y * 4 + x / 8);
            const BYTE bit = BYTE(0x80u >> (x % 8));
            andMask[at] &= BYTE(~bit);
            if (x == 0 || x == y / 2 || y == 23)
                xorMask[at] |= bit;
        }
    }
    return ::CreateCursor(nullptr, 2, 3, 32, 32, andMask.data(), xorMask.data());
}

// A picture whose every pixel differs, so a wrong turn shows.
QImage patterned(int w, int h)
{
    QImage image(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            image.setPixel(x, y, qRgb((x * 7 + y * 3) & 0xFF, (x * 13) & 0xFF, (y * 29 + x) & 0xFF));
    }
    return image;
}

int differing(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return -1;
    int count = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x)
            count += a.pixel(x, y) != b.pixel(x, y);
    }
    return count;
}

// A line of text on a picture: corners of its box, characters spread evenly.
RedactLine line(const QString& text, QRectF box)
{
    RedactLine l;
    l.text = text;
    l.quad = QPolygonF(box);
    l.quad.removeLast(); // QPolygonF(QRectF) closes the polygon: four corners are all that is wanted
    for (int i = 0; i < text.size(); ++i)
        l.chars.append({i, static_cast<float>((i + 0.5) / text.size())});
    return l;
}

// A QR code for `text` as a picture: modules of `scale` pixels, a quiet zone of 4.
QImage qrPicture(const QString& text, int scale = 6, bool inverted = false)
{
    ZXing::QRCode::Writer writer;
    writer.setEncoding(ZXing::CharacterSet::UTF8);
    writer.setMargin(4);
    const QByteArray utf8 = text.toUtf8();
    ZXing::BitMatrix matrix;
    try {
        matrix = writer.encode(std::string(utf8.constData(), size_t(utf8.size())), 0, 0);
    } catch (const std::exception& e) {
        qWarning() << "QR writer:" << e.what();
        throw;
    }
    QImage image(matrix.width() * scale, matrix.height() * scale, QImage::Format_RGB32);
    image.fill(inverted ? Qt::black : Qt::white);
    QPainter painter(&image);
    for (int y = 0; y < matrix.height(); ++y) {
        for (int x = 0; x < matrix.width(); ++x) {
            if (matrix.get(x, y))
                painter.fillRect(x * scale, y * scale, scale, scale, inverted ? Qt::white : Qt::black);
        }
    }
    return image;
}

} // namespace

class TestStage7 : public QObject {
    Q_OBJECT

private slots:
    // ---- rotating and flipping -----------------------------------------------------

    void fourTurnsGiveTheSamePixels()
    {
        const QImage image = patterned(37, 23);
        for (const bool clockwise : {true, false}) {
            QImage turned = image;
            for (int i = 0; i < 4; ++i)
                turned = turnedImage(turned, clockwise);
            QCOMPARE(differing(image, turned), 0);
        }
        // Two clockwise are two counter-clockwise; one each way is none.
        QCOMPARE(differing(turnedImage(turnedImage(image, true), true), turnedImage(turnedImage(image, false), false)), 0);
        QCOMPARE(differing(turnedImage(turnedImage(image, true), false), image), 0);
    }

    void aTurnSwapsTheSidesAndMovesThePixelsTheRightWay()
    {
        const QImage image = patterned(37, 23);
        const QImage right = turnedImage(image, true);
        QCOMPARE(right.size(), QSize(23, 37));
        // Clockwise: the top left goes to the top right, the bottom left to the top left.
        QCOMPARE(right.pixel(22, 0), image.pixel(0, 0));
        QCOMPARE(right.pixel(0, 0), image.pixel(0, 22));
        QCOMPARE(right.pixel(22, 36), image.pixel(36, 0));
        const QImage left = turnedImage(image, false);
        QCOMPARE(left.pixel(0, 36), image.pixel(0, 0));
        QCOMPARE(left.pixel(0, 0), image.pixel(36, 0));
    }

    void flipsAreTheirOwnInverse()
    {
        const QImage image = patterned(31, 17);
        for (const bool horizontal : {true, false}) {
            const QImage once = flippedImage(image, horizontal);
            QCOMPARE(once.size(), image.size());
            QVERIFY(differing(once, image) > 0);
            QCOMPARE(differing(flippedImage(once, horizontal), image), 0);
        }
        QCOMPARE(flippedImage(image, true).pixel(0, 5), image.pixel(30, 5));
        QCOMPARE(flippedImage(image, false).pixel(5, 0), image.pixel(5, 16));
    }

    void turningAboutTheMiddleComesBackToThePixel()
    {
        // A picture of odd and even sizes at zoom 1: four turns end where they began.
        for (const QSize size : {QSize(101, 50), QSize(100, 51), QSize(7, 300), QSize(64, 64)}) {
            QPoint origin(1234, -77);
            QSize shown = size;
            for (int i = 0; i < 4; ++i) {
                const QRect before(origin, shown);
                const QSize after(shown.height(), shown.width());
                origin = recenteredOrigin(before, after);
                shown = after;
            }
            QCOMPARE(origin, QPoint(1234, -77));
            QCOMPARE(shown, size);
        }
        // And turning back undoes one turn.
        const QRect before(QPoint(500, 400), QSize(101, 50));
        const QPoint turned = recenteredOrigin(before, QSize(50, 101));
        QCOMPARE(recenteredOrigin(QRect(turned, QSize(50, 101)), QSize(101, 50)), before.topLeft());
        // An even difference keeps the middle exactly.
        QCOMPARE(recenteredOrigin(QRect(0, 0, 100, 40), QSize(40, 100)), QPoint(30, -30));
    }

    // ---- collapsing ----------------------------------------------------------------------

    void theDoubleClickSettingIsReadByName()
    {
        QCOMPARE(pinDoubleClickFromName(u"reset"_s), PinDoubleClick::Reset);
        QCOMPARE(pinDoubleClickFromName(u"close"_s), PinDoubleClick::Close);
        QCOMPARE(pinDoubleClickFromName(u"collapse"_s), PinDoubleClick::Collapse);
        QCOMPARE(pinDoubleClickFromName(u"none"_s), PinDoubleClick::None);
        QCOMPARE(pinDoubleClickFromName(u"what"_s), PinDoubleClick::Reset);
    }

    void theThumbnailFitsTheBoxAndNeverGrows()
    {
        QCOMPARE(tileThumbSize({1000, 500}, 1.0), QSize(96, 48));
        QCOMPARE(tileThumbSize({500, 1000}, 1.0), QSize(48, 96));
        QCOMPARE(tileThumbSize({1000, 500}, 1.5), QSize(144, 72)); // 150%: the box is 144 px
        QCOMPARE(tileThumbSize({40, 30}, 1.0), QSize(40, 30)); // smaller than the box: as it is
        QCOMPARE(tileThumbSize({4000, 1}, 1.0), QSize(96, 1)); // never under 1
        QCOMPARE(tileThumbSize({}, 1.0), QSize(1, 1));
        QCOMPARE(edgeTabSize(1.0), QSize(112, 28));
        QCOMPARE(edgeTabSize(1.5), QSize(168, 42));
    }

    void aTileStaysInTheWorkArea()
    {
        const QRect work(0, 0, 2560, 1560); // the taskbar took the bottom 40
        QCOMPARE(tileRect({100, 200}, {144, 72}, work), QRect(100, 200, 144, 72));
        QCOMPARE(tileRect({2500, 1540}, {144, 72}, work), QRect(2416, 1488, 144, 72));
        QCOMPARE(tileRect({-50, -50}, {144, 72}, work), QRect(0, 0, 144, 72));
        // A second monitor to the right: its own work area.
        const QRect second(2560, 0, 1920, 1040);
        QCOMPARE(tileRect({2570, 1030}, {100, 100}, second), QRect(2570, 940, 100, 100));
    }

    void edgeTabsGoOnTheTopEdgeBesideEachOther()
    {
        const QRect work(0, 40, 2560, 1520); // a taskbar on top: the tabs sit under it
        const QSize tab(168, 42);
        // Free: centred on the wish, on the top edge of the work area.
        QCOMPARE(edgeTabRect(1000, tab, work, {}), QRect(916, 40, 168, 42));
        // Held in the work area at both ends.
        QCOMPARE(edgeTabRect(-500, tab, work, {}).left(), 0);
        QCOMPARE(edgeTabRect(5000, tab, work, {}).right(), 2559);
        // Taken: moved right past it, with a gap.
        const QRect first = edgeTabRect(1000, tab, work, {});
        const QRect second = edgeTabRect(1000, tab, work, {first});
        QVERIFY(second.left() >= first.right() + 1 + 4);
        QVERIFY(!second.intersects(first));
        QCOMPARE(second.top(), 40);
        const QRect third = edgeTabRect(1000, tab, work, {first, second});
        QVERIFY(!third.intersects(first) && !third.intersects(second));
        // No room to the right: from the left end.
        const QRect last = edgeTabRect(2559, tab, work, {QRect(2392, 40, 168, 42)});
        QVERIFY(!last.intersects(QRect(2392, 40, 168, 42)));
        QVERIFY(last.left() >= 0 && last.right() <= 2559);
        // Every place taken: it overlaps rather than vanishes.
        std::vector<QRect> all;
        for (int x = 0; x < 2560; x += 100)
            all.push_back(QRect(x, 40, 100, 42));
        QCOMPARE(edgeTabRect(1000, tab, work, all).top(), 40);
    }

    void aTileOpensWhereItWasOrWhereItWasDraggedTo()
    {
        QCOMPARE(expandedOrigin({300, 200}, {500, 100}, {500, 100}), QPoint(300, 200)); // not moved: to the pixel
        QCOMPARE(expandedOrigin({300, 200}, {500, 100}, {620, 80}), QPoint(420, 180));
    }

    // ---- dragging a corner ---------------------------------------------------------------

    void cornersAreSquareGripsAroundTheCornerPoints()
    {
        const QRect picture(100, 100, 400, 300); // right() = 499, bottom() = 399: the corner points are 500, 400
        QCOMPARE(cornerAt(picture, {100, 100}, 10), Corner::TopLeft);
        QCOMPARE(cornerAt(picture, {108, 94}, 10), Corner::TopLeft);
        QCOMPARE(cornerAt(picture, {500, 100}, 10), Corner::TopRight);
        QCOMPARE(cornerAt(picture, {100, 400}, 10), Corner::BottomLeft);
        QCOMPARE(cornerAt(picture, {495, 395}, 10), Corner::BottomRight);
        QCOMPARE(cornerAt(picture, {300, 250}, 10), Corner::None);
        QCOMPARE(cornerAt(picture, {300, 100}, 10), Corner::None); // an edge's middle is not a corner
        // A small pin: the grip is no more than a third of it.
        QCOMPARE(cornerAt(QRect(0, 0, 24, 24), {12, 12}, 10), Corner::None);
        QCOMPARE(cornerAt(QRect(0, 0, 24, 24), {0, 0}, 10), Corner::TopLeft);
        QCOMPARE(cornerAt({}, {0, 0}, 10), Corner::None);
    }

    void draggingACornerKeepsTheOppositeOneAndTheAspect()
    {
        const QSize image(400, 300);
        const QRect start(100, 100, 400, 300); // shown at 100%
        // Bottom right out to double: the top left stays.
        Resized r = resizeFromCorner(Corner::BottomRight, start, image, {900, 700}, 0.1, 8.0);
        QCOMPARE(r.origin, QPoint(100, 100));
        QCOMPARE(r.zoom, 2.0);
        // Top left to half: the bottom right (500, 400) stays.
        r = resizeFromCorner(Corner::TopLeft, start, image, {300, 250}, 0.1, 8.0);
        QCOMPARE(r.zoom, 0.5);
        QCOMPARE(r.origin, QPoint(300, 250));
        QCOMPARE(QRect(r.origin, zoomedSize(image, r.zoom)).bottomRight() + QPoint(1, 1), QPoint(500, 400));
        // Top right and bottom left keep their own opposite corners.
        r = resizeFromCorner(Corner::TopRight, start, image, {300, 250}, 0.1, 8.0);
        QCOMPARE(r.origin.x(), 100);
        QCOMPARE(r.origin.y() + zoomedSize(image, r.zoom).height(), 400);
        r = resizeFromCorner(Corner::BottomLeft, start, image, {300, 250}, 0.1, 8.0);
        QCOMPARE(r.origin.y(), 100);
        QCOMPARE(r.origin.x() + zoomedSize(image, r.zoom).width(), 500);
        // The larger of the two ratios wins (the aspect stays), and the zoom is kept in range.
        r = resizeFromCorner(Corner::BottomRight, start, image, {300, 900}, 0.1, 8.0);
        QCOMPARE(r.zoom, 800.0 / 300.0);
        r = resizeFromCorner(Corner::BottomRight, start, image, {20000, 20000}, 0.1, 3.5);
        QCOMPARE(r.zoom, 3.5);
        r = resizeFromCorner(Corner::BottomRight, start, image, {50, 50}, 0.1, 8.0); // across the corner: the smallest
        QCOMPARE(r.zoom, 0.1);
        QCOMPARE(resizeFromCorner(Corner::None, start, image, {1, 1}, 0.1, 8.0).origin, start.topLeft());
    }

    // ---- picking text --------------------------------------------------------------------

    void aSelectionTakesTheLinesItTouches()
    {
        const QList<RedactLine> lines {
            line(u"Win顺 截图"_s, QRectF(10, 10, 200, 20)),
            line(u"第二行 second line"_s, QRectF(10, 40, 300, 20)),
            line(u"third"_s, QRectF(10, 70, 100, 20)),
        };
        // All three: whole lines, one per row.
        QCOMPARE(pickedText(pickedLines(lines, QRectF(0, 0, 400, 100))), u"Win顺 截图\n第二行 second line\nthird"_s);
        // Two rows only, dragged bottom-up (the corners swap).
        QCOMPARE(pickedText(pickedLines(lines, QRectF(QPointF(320, 65), QPointF(0, 5)))), u"Win顺 截图\n第二行 second line"_s);
        // Nothing under it.
        QVERIFY(pickedLines(lines, QRectF(0, 100, 400, 50)).isEmpty());
        QVERIFY(pickedLines(lines, QRectF(250, 0, 100, 30)).isEmpty()); // right of the first line
        QVERIFY(pickedLines({}, QRectF(0, 0, 400, 100)).isEmpty());
        // A sliver of a row's height does not take the line (under 40% of it).
        QVERIFY(pickedLines(lines, QRectF(0, 0, 400, 14)).isEmpty()); // 4 of the first line's 20 px
        QCOMPARE(pickedLines(lines, QRectF(0, 0, 400, 19)).size(), 1); // 9 of them
    }

    void aSelectionOfPartOfALineTakesItsCharacters()
    {
        const QList<RedactLine> lines {line(u"0123456789"_s, QRectF(100, 10, 100, 20))};
        // The middle: characters 3 to 6 (their middles at 0.35 .. 0.65 of the line).
        const QList<PickedLine> middle = pickedLines(lines, QRectF(130, 0, 40, 40));
        QCOMPARE(middle.size(), 1);
        QCOMPARE(middle[0].text, u"3456"_s);
        QVERIFY(middle[0].box.left() >= 125 && middle[0].box.right() <= 175);
        // From the left end past the middle: the first characters.
        QCOMPARE(pickedText(pickedLines(lines, QRectF(0, 0, 150, 40))), u"01234"_s);
        // Past both ends: the whole line, with its own box.
        const QList<PickedLine> all = pickedLines(lines, QRectF(0, 0, 400, 40));
        QCOMPARE(all[0].text, u"0123456789"_s);
        QCOMPARE(all[0].box, QRectF(100, 10, 100, 20));
        // Within a gap between two characters' middles: none.
        QVERIFY(pickedLines(lines, QRectF(131, 0, 2, 40)).isEmpty());
        // Without the characters' places: the whole line.
        RedactLine bare = lines[0];
        bare.chars.clear();
        QCOMPARE(pickedText(pickedLines({bare}, QRectF(130, 0, 10, 40))), u"0123456789"_s);
    }

    void piecesOnOneRowComeTogetherLeftToRight()
    {
        // The reader found "right" before "left", both on one row; a lower line apart.
        const QList<RedactLine> lines {
            line(u"right"_s, QRectF(200, 10, 80, 20)),
            line(u"lower"_s, QRectF(10, 60, 80, 20)),
            line(u"left"_s, QRectF(10, 11, 80, 20)),
        };
        QCOMPARE(pickedText(pickedLines(lines, QRectF(0, 0, 400, 100))), u"left right\nlower"_s);
    }

    // ---- the colour formats ---------------------------------------------------------------

    void colorsAreWrittenInTheThreeFormats()
    {
        const QRgb blueish = qRgb(0xAB, 0xCD, 0xEF);
        QCOMPARE(colorText(blueish, ColorFormat::Hex), u"#ABCDEF"_s);
        QCOMPARE(colorText(blueish, ColorFormat::Rgb), u"rgb(171, 205, 239)"_s);
        QCOMPARE(colorText(blueish, ColorFormat::Hsl), u"hsl(210, 68%, 80%)"_s);
        QCOMPARE(colorText(qRgb(255, 0, 0), ColorFormat::Hsl), u"hsl(0, 100%, 50%)"_s);
        QCOMPARE(colorText(qRgb(0, 255, 0), ColorFormat::Hsl), u"hsl(120, 100%, 50%)"_s);
        QCOMPARE(colorText(qRgb(0, 0, 255), ColorFormat::Hsl), u"hsl(240, 100%, 50%)"_s);
        QCOMPARE(colorText(qRgb(128, 128, 128), ColorFormat::Hsl), u"hsl(0, 0%, 50%)"_s);
        QCOMPARE(colorText(qRgb(0, 0, 0), ColorFormat::Hsl), u"hsl(0, 0%, 0%)"_s);
        QCOMPARE(colorText(qRgb(255, 255, 255), ColorFormat::Hsl), u"hsl(0, 0%, 100%)"_s);
        QCOMPARE(colorText(qRgb(255, 0, 128), ColorFormat::Hsl), u"hsl(330, 100%, 50%)"_s); // a negative hue wraps
        QCOMPARE(colorText(qRgb(0x1E, 0x90, 0xFF), ColorFormat::Rgb), u"rgb(30, 144, 255)"_s);
        // Alpha does not change the text.
        QCOMPARE(colorText(qRgba(0xAB, 0xCD, 0xEF, 10), ColorFormat::Hex), u"#ABCDEF"_s);
    }

    void theFormatsCycleAndHaveNames()
    {
        QCOMPARE(nextColorFormat(ColorFormat::Hex), ColorFormat::Rgb);
        QCOMPARE(nextColorFormat(ColorFormat::Rgb), ColorFormat::Hsl);
        QCOMPARE(nextColorFormat(ColorFormat::Hsl), ColorFormat::Hex);
        for (const ColorFormat f : {ColorFormat::Hex, ColorFormat::Rgb, ColorFormat::Hsl})
            QCOMPARE(colorFormatFromName(colorFormatName(f)), f);
        QCOMPARE(colorFormatFromName(u" RGB "_s), ColorFormat::Rgb);
        QCOMPARE(colorFormatFromName(u"cmyk"_s), ColorFormat::Hex);
        QCOMPARE(colorFormatFromName({}), ColorFormat::Hex);
    }

    // ---- the delayed capture --------------------------------------------------------------

    void theDelayIsThreeFiveOrTen()
    {
        QCOMPARE(normalizedDelay(3), 3);
        QCOMPARE(normalizedDelay(5), 5);
        QCOMPARE(normalizedDelay(10), 10);
        QCOMPARE(normalizedDelay(0), 5);
        QCOMPARE(normalizedDelay(-4), 5);
        QCOMPARE(normalizedDelay(4), 3);
        QCOMPARE(normalizedDelay(7), 5);
        QCOMPARE(normalizedDelay(8), 10);
        QCOMPARE(normalizedDelay(60), 10);
    }

    void theCountdownShowsWholeSecondsLeftAndEndsAtZero()
    {
        QCOMPARE(countdownSeconds(0, 3), 3);
        QCOMPARE(countdownSeconds(1, 3), 3); // 2999 ms left: still the third second
        QCOMPARE(countdownSeconds(999, 3), 3);
        QCOMPARE(countdownSeconds(1000, 3), 2);
        QCOMPARE(countdownSeconds(2500, 3), 1);
        QCOMPARE(countdownSeconds(2999, 3), 1);
        QCOMPARE(countdownSeconds(3000, 3), 0);
        QCOMPARE(countdownSeconds(9000, 3), 0);
        QCOMPARE(countdownRemaining(0, 5), 5000);
        QCOMPARE(countdownRemaining(4990, 5), 10);
        QCOMPARE(countdownRemaining(5000, 5), 0);
        QCOMPARE(countdownRemaining(99999, 5), 0);
        // Every second of ten shows its number once, in order.
        int last = 11;
        for (qint64 t = 0; t <= 10000; t += 50) {
            const int n = countdownSeconds(t, 10);
            QVERIFY(n <= last);
            last = n;
        }
        QCOMPARE(last, 0);
    }

    // ---- Shift+R -----------------------------------------------------------------------------

    void theLastSelectionIsFoundOnItsMonitor()
    {
        const std::vector<QRect> monitors {QRect(0, 0, 2560, 1600), QRect(2560, 0, 1920, 1080)};
        // On the first monitor.
        auto r = lastSelectionOn(QRect(100, 200, 300, 400), monitors);
        QVERIFY(r);
        QCOMPARE(r->monitor, 0);
        QCOMPARE(r->rect, QRect(100, 200, 300, 400));
        // On the second: in its own pixels.
        r = lastSelectionOn(QRect(2560 + 50, 60, 200, 100), monitors);
        QVERIFY(r);
        QCOMPARE(r->monitor, 1);
        QCOMPARE(r->rect, QRect(50, 60, 200, 100));
        // Across the seam: the monitor with most of it; kept inside.
        r = lastSelectionOn(QRect(2500, 100, 400, 100), monitors);
        QVERIFY(r);
        QCOMPARE(r->monitor, 1);
        QVERIFY(QRect(0, 0, 1920, 1080).contains(r->rect));
        // The monitor got smaller since: pulled in (moved, then shrunk if need be).
        r = lastSelectionOn(QRect(2560 + 1800, 1000, 300, 200), monitors);
        QVERIFY(r);
        QVERIFY(QRect(0, 0, 1920, 1080).contains(r->rect));
        QCOMPARE(r->rect.size(), QSize(300, 200));
        // Gone: nothing; nothing remembered: nothing.
        QVERIFY(!lastSelectionOn(QRect(9000, 9000, 10, 10), monitors));
        QVERIFY(!lastSelectionOn({}, monitors));
        QVERIFY(!lastSelectionOn(QRect(0, 0, 10, 10), {}));
    }

    // ---- the mouse cursor in a picture ------------------------------------------------------

    void theCursorIsDrawnWhereTheHotSpotIs()
    {
        const HCURSOR cursor = testCursor();
        const auto cleanup = qScopeGuard([cursor] { if (cursor) ::DestroyCursor(cursor); });
        QVERIFY(cursor);
        QImage image(200, 160, QImage::Format_RGB32);
        image.fill(qRgb(0x20, 0x70, 0xB0));
        const QImage plain = image;
        QVERIFY(drawCursor(image, QPoint(100, 80), cursor));
        QVERIFY(differing(image, plain) > 20); // an arrow's worth
        ICONINFO info {};
        QVERIFY(::GetIconInfo(cursor, &info));
        const QPoint grip(int(info.xHotspot), int(info.yHotspot));
        BITMAP shape {};
        ::GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof shape, &shape);
        const QSize size(shape.bmWidth, info.hbmColor ? shape.bmHeight : shape.bmHeight / 2);
        if (info.hbmColor)
            ::DeleteObject(info.hbmColor);
        if (info.hbmMask)
            ::DeleteObject(info.hbmMask);
        // Nothing changed outside the cursor's own box.
        const QRect box(QPoint(100, 80) - grip, size);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (!box.contains(x, y))
                    QCOMPARE(image.pixel(x, y), plain.pixel(x, y));
            }
        }
        // The same pixels as Windows draws it by itself on a surface of the same background.
        BITMAPINFO header {};
        header.bmiHeader.biSize = sizeof header.bmiHeader;
        header.bmiHeader.biWidth = image.width();
        header.bmiHeader.biHeight = -image.height();
        header.bmiHeader.biPlanes = 1;
        header.bmiHeader.biBitCount = 32;
        header.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = ::CreateDIBSection(nullptr, &header, DIB_RGB_COLORS, &bits, nullptr, 0);
        QVERIFY(dib);
        HDC dc = ::CreateCompatibleDC(nullptr);
        HGDIOBJ old = ::SelectObject(dc, dib);
        std::memcpy(bits, plain.constBits(), size_t(plain.sizeInBytes()));
        ::DrawIconEx(dc, 100 - grip.x(), 80 - grip.y(), cursor, 0, 0, 0, nullptr, DI_NORMAL);
        ::GdiFlush();
        const QImage reference(static_cast<const uchar*>(bits), image.width(), image.height(), image.width() * 4,
            QImage::Format_RGB32);
        QCOMPARE(differing(image, reference.copy()), 0);
        ::SelectObject(dc, old);
        ::DeleteDC(dc);
        ::DeleteObject(dib);
    }

    void aCursorAtTheEdgeIsCutAndOneOutsideDrawsNothing()
    {
        const HCURSOR cursor = testCursor();
        const auto cleanup = qScopeGuard([cursor] { if (cursor) ::DestroyCursor(cursor); });
        QVERIFY(cursor);
        QImage image(60, 60, QImage::Format_RGB32);
        image.fill(Qt::white);
        QVERIFY(drawCursor(image, QPoint(55, 55), cursor)); // most of it is off the picture
        QVERIFY(drawCursor(image, QPoint(0, 0), cursor));
        QImage unchanged = image;
        QVERIFY(!drawCursor(image, QPoint(500, 500), cursor));
        QVERIFY(!drawCursor(image, QPoint(-500, -500), cursor));
        QCOMPARE(differing(image, unchanged), 0);
        QVERIFY(!drawCursor(image, QPoint(10, 10), nullptr));
        QImage none;
        QVERIFY(!drawCursor(none, QPoint(10, 10), cursor));
        // A picture with alpha 0 (a BitBlt grab) gets an opaque cursor over opaque pixels.
        QImage grab(60, 60, QImage::Format_ARGB32_Premultiplied);
        grab.fill(Qt::transparent);
        QVERIFY(drawCursor(grab, QPoint(30, 30), cursor));
        QCOMPARE(qAlpha(grab.pixel(30, 30)), 255);
    }

    // ---- UI Automation -------------------------------------------------------------------------

    void uiaIsAskedForProgramsThatDrawTheirOwnControls()
    {
        QVERIFY(wantsUia(u"WinUIDesktopWin32WindowClass"_s, 1));
        QVERIFY(wantsUia(u"ApplicationFrameWindow"_s, 5));
        QVERIFY(wantsUia(u"MozillaWindowClass"_s, 2));
        QVERIFY(wantsUia(u"Notepad"_s, 0)); // no child windows to pick: UI Automation may know more
        QVERIFY(!wantsUia(u"Notepad"_s, 2)); // its controls are child windows
        QVERIFY(!wantsUia(u"CabinetWClass"_s, 12));
    }

    // Asking a Chromium window makes it turn its accessibility mode on for good, and VS Code then shows
    // "optimized for a screen reader" until it restarts: never asked, with or without child windows.
    void uiaIsNeverAskedOfChromium()
    {
        for (const QString& chromium : {u"Chrome_WidgetWin_0"_s, u"Chrome_WidgetWin_1"_s, u"Chrome_WidgetWin_2"_s,
                 u"Chrome_RenderWidgetHostHWND"_s, u"Intermediate D3D Window"_s, u"CefBrowserWindow"_s}) {
            QVERIFY2(!wantsUia(chromium, 0), qPrintable(chromium));
            QVERIFY2(!wantsUia(chromium, 3), qPrintable(chromium)); // VS Code, Edge, WeChat
            QVERIFY2(WindowSnapshot::isChromiumClass(chromium.toStdWString()), qPrintable(chromium));
        }
        // Not Chromium's: Firefox, XAML/WinUI, ordinary windows, look-alikes.
        for (const QString& other : {u"MozillaWindowClass"_s, u"ApplicationFrameWindow"_s, u"Notepad"_s, u"CabinetWClass"_s,
                 u"Qt6120QWindowIcon"_s, u"Chrome"_s, u"Chrome_WidgetWin"_s, u"MyChrome_WidgetWin_1"_s, u""_s})
            QVERIFY2(!WindowSnapshot::isChromiumClass(other.toStdWString()), qPrintable(other));
        QVERIFY(wantsUia(u"Notepad"_s, 0)); // still asked where nothing is a child window
    }

    void smallToolbarButtonBeatsDeepWrapperPanes()
    {
        const QRect window(0, 0, 1200, 800);
        const std::vector<QRect> wrappers(20, window);
        const std::vector<QRect> button {window, QRect(30, 20, 32, 32)};
        QVERIFY(preferUiaChain(button, wrappers));
        QVERIFY(!preferUiaChain(wrappers, button));
        QVERIFY(!preferUiaChain({window, QRect(30, 20, 1, 1)}, button));
        QVERIFY(preferUiaChain({window, window, QRect(30, 20, 32, 32)}, button));
        QVERIFY(!preferUiaChain({}, button));
    }

    void uiaControlsAreCutAndFiltered()
    {
        const QRect window(100, 100, 800, 600);
        const std::vector<QRect> chain {
            QRect(100, 100, 800, 600), // the window itself: left to the window
            QRect(100, 100, 200, 600), // the sidebar
            QRect(120, 130, 3, 40), // too small
            QRect(110, 140, 100, 7), // too thin
            QRect(100, 100, 200, 600), // the same again
            QRect(-50, 300, 400, 200), // sticks out: cut
            QRect(2000, 2000, 100, 100), // outside
            QRect(),
        };
        const std::vector<QRect> controls = uiaControls(chain, window);
        QCOMPARE(controls.size(), std::size_t(2));
        QCOMPARE(controls[0], QRect(100, 100, 200, 600));
        QCOMPARE(controls[1], QRect(100, 300, 250, 200));
        QVERIFY(uiaControls({}, window).empty());
    }

    void aSlowOrSilentWindowIsNotAskedAgain()
    {
        UiaGate gate;
        const quintptr a = 0x1001;
        const quintptr b = 0x2002;
        QVERIFY(gate.allowed(a));
        gate.record(a, UiaGate::kBudgetMs, 4); // exactly on the budget: in time
        QVERIFY(gate.allowed(a));
        QVERIFY(!gate.slow(a));
        gate.record(a, UiaGate::kBudgetMs + 1, 4); // over it: Win32 child windows only from now on
        QVERIFY(!gate.allowed(a));
        QVERIFY(gate.slow(a));
        QVERIFY(gate.allowed(b)); // another window is its own case
        for (int i = 0; i < UiaGate::kEmptyLimit - 1; ++i) {
            gate.record(b, 10, 0);
            QVERIFY(gate.allowed(b));
        }
        gate.record(b, 10, 3); // an answer breaks the run of empty ones
        for (int i = 0; i < UiaGate::kEmptyLimit - 1; ++i)
            gate.record(b, 10, 0);
        QVERIFY(gate.allowed(b));
        gate.record(b, 10, 0);
        QVERIFY(!gate.allowed(b));
        QVERIFY(!gate.slow(b));
    }

    // ---- QR codes -------------------------------------------------------------------------------

    void aQrCodeGivesBackItsText_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("link") << u"https://example.com/path?a=1&b=2"_s;
        QTest::newRow("words") << u"Win顺 screenshot QR test 123"_s;
        QTest::newRow("chinese") << u"二维码测试：你好，世界"_s;
        QTest::newRow("long") << QString(u"0123456789abcdef"_s).repeated(12);
        QTest::newRow("wifi") << u"WIFI:T:WPA;S:home net;P:pass word;;"_s;
    }
    void aQrCodeGivesBackItsText()
    {
        QFETCH(QString, text);
        const QStringList found = readQrCodes(qrPicture(text));
        QCOMPARE(found.size(), 1);
        QCOMPARE(found[0], text);
    }

    void aQrCodeIsFoundInALargerPicture()
    {
        // On a screenshot-sized page of other pixels, at a place and a size.
        QImage page(1280, 800, QImage::Format_RGB32);
        page.fill(qRgb(0xF3, 0xF3, 0xF3));
        QPainter painter(&page);
        painter.fillRect(0, 0, 1280, 60, qRgb(0x20, 0x70, 0xB0));
        painter.fillRect(100, 300, 500, 200, qRgb(0xE0, 0xE0, 0xE0));
        const QImage code = qrPicture(u"https://winshun.example/q"_s, 5);
        painter.drawImage(700, 250, code);
        painter.end();
        const QStringList found = readQrCodes(page);
        QCOMPARE(found, QStringList {u"https://winshun.example/q"_s});
    }

    // The words read in a picture, and what its codes hold after them: what
    // the clipboard history searches and shows for it (GitHub #305).
    void aCodeJoinsThePicturesWords()
    {
        const QImage code = qrPicture(u"https://winshun.example/q"_s);
        QCOMPARE(withQrCodes({}, code), u"https://winshun.example/q"_s); // a code alone: no longer wordless
        QCOMPARE(withQrCodes(u"scan me"_s, code), u"scan me\nhttps://winshun.example/q"_s);
        QCOMPARE(withQrCodes(u"see https://winshun.example/q here"_s, code), u"see https://winshun.example/q here"_s); // read as text already
        QImage plain(200, 120, QImage::Format_RGB32);
        plain.fill(Qt::white);
        QCOMPARE(withQrCodes(u"just words"_s, plain), u"just words"_s);
        QCOMPARE(withQrCodes({}, plain), QString());
        QCOMPARE(withQrCodes(u"words"_s, QImage()), u"words"_s);
    }

    void aQrCodeSurvivesInversionAndATurn()
    {
        QCOMPARE(readQrCodes(qrPicture(u"inverted code"_s, 6, true)), QStringList {u"inverted code"_s}); // dark mode
        QCOMPARE(readQrCodes(turnedImage(qrPicture(u"turned code"_s), true)), QStringList {u"turned code"_s});
        QCOMPARE(readQrCodes(qrPicture(u"small modules"_s, 3)), QStringList {u"small modules"_s});
    }

    // Real pictures, which are not ours to keep in the repository (GitHub #305:
    // QQ's round, dotted codes): WS_QR_SAMPLES names a folder of them,
    // WS_QR_SCALE a size to draw them at first (0.25) and WS_QR_AT_LEAST how
    // many must be read. Skipped without the folder.
    void realPicturesAreRead()
    {
        const QString folder = qEnvironmentVariable("WS_QR_SAMPLES");
        if (folder.isEmpty())
            QSKIP("WS_QR_SAMPLES is not set");
        const double scale = qEnvironmentVariable("WS_QR_SCALE", u"1"_s).toDouble();
        int pictures = 0;
        int read = 0;
        QElapsedTimer timer;
        qint64 spent = 0;
        QDirIterator it(folder, QDir::Files);
        while (it.hasNext()) {
            QImage image(it.next());
            if (image.isNull())
                continue;
            if (scale > 0 && scale != 1.0)
                image = image.scaled(image.size() * scale, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            ++pictures;
            timer.start();
            const bool found = !readQrCodes(image).isEmpty();
            spent += timer.elapsed();
            if (found)
                ++read;
            else
                qInfo().noquote() << "not read:" << it.fileName();
        }
        qInfo() << "read" << read << "of" << pictures << "in" << spent << "ms";
        QVERIFY(read >= qEnvironmentVariableIntValue("WS_QR_AT_LEAST"));
    }

    void twoQrCodesGiveTwoTexts()
    {
        QImage both(900, 450, QImage::Format_RGB32);
        both.fill(Qt::white);
        QPainter painter(&both);
        painter.drawImage(0, 0, qrPicture(u"first code"_s, 6));
        painter.drawImage(450, 0, qrPicture(u"second code"_s, 6));
        painter.end();
        const QStringList found = readQrCodes(both);
        QCOMPARE(found.size(), 2);
        QVERIFY(found.contains(u"first code"_s) && found.contains(u"second code"_s));
    }

    void noQrCodeGivesNothing()
    {
        QVERIFY(readQrCodes({}).isEmpty());
        QImage blank(400, 300, QImage::Format_RGB32);
        blank.fill(Qt::white);
        QVERIFY(readQrCodes(blank).isEmpty());
        QVERIFY(readQrCodes(patterned(300, 200)).isEmpty());
        // Text drawn on a page is not a barcode.
        QImage text(400, 120, QImage::Format_RGB32);
        text.fill(Qt::white);
        QPainter painter(&text);
        painter.drawText(QRect(0, 0, 400, 120), Qt::AlignCenter, u"Hello 0123456789 | || ||| world"_s);
        painter.end();
        QVERIFY(readQrCodes(text).isEmpty());
    }

    void onlyWebAddressesAreOfferedAsLinks()
    {
        QCOMPARE(qrLink(u"https://example.com"_s), u"https://example.com"_s);
        QCOMPARE(qrLink(u"  http://a.b/c?d=e#f \n"_s), u"http://a.b/c?d=e#f"_s);
        QCOMPARE(qrLink(u"HTTPS://EXAMPLE.COM/x"_s), u"HTTPS://EXAMPLE.COM/x"_s);
        QVERIFY(qrLink(u"ftp://example.com/file"_s).isEmpty());
        QVERIFY(qrLink(u"file:///C:/Windows/System32/calc.exe"_s).isEmpty());
        QVERIFY(qrLink(u"javascript:alert(1)"_s).isEmpty());
        QVERIFY(qrLink(u"ms-settings:network"_s).isEmpty());
        QVERIFY(qrLink(u"https://"_s).isEmpty());
        QVERIFY(qrLink(u"see https://example.com"_s).isEmpty());
        QVERIFY(qrLink(u"https://exa mple.com"_s).isEmpty());
        QVERIFY(qrLink(u"https://example.com\nhttps://other.example"_s).isEmpty());
        QVERIFY(qrLink(u"example.com"_s).isEmpty());
        QVERIFY(qrLink({}).isEmpty());
        QVERIFY(qrLink(u"https://"_s + QString(3000, u'a')).isEmpty());
    }
};

int main(int argc, char** argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) // as the app: FreeType, so text is drawn as users see it
        qputenv("QT_QPA_PLATFORM", "windows:fontengine=freetype");
    QGuiApplication app(argc, argv);
    TestStage7 test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_stage7.moc"
