// The drop pie's geometry (PieGeometry): the chips round the middle, which
// sector a direction chooses, moving or copying, where the pie goes near a
// screen's edges and the window around it.
#include "PieGeometry.h"

#include <QtTest>

#include <cmath>
#include <utility>
#include <vector>

using namespace ws::pie;

namespace {

// No two chips within the gap of each other or of the middle.
bool apart(const Layout& layout, const Metrics& m)
{
    const double half = m.chipGap / 2 - 1e-6;
    for (std::size_t i = 0; i < layout.chips.size(); ++i) {
        const QRectF& a = layout.chips[i];
        const double dx = std::max({a.left(), -a.right(), 0.0});
        const double dy = std::max({a.top(), -a.bottom(), 0.0});
        if (std::hypot(dx, dy) < m.hub + m.chipGap - 1e-6)
            return false;
        for (std::size_t j = i + 1; j < layout.chips.size(); ++j) {
            if (a.adjusted(-half, -half, half, half).intersects(layout.chips[j].adjusted(-half, -half, half, half)))
                return false;
        }
    }
    return true;
}

} // namespace

class TestPieGeometry : public QObject {
    Q_OBJECT

private slots:
    void angles()
    {
        QCOMPARE(angleOf({0, -10}), 0.0);
        QCOMPARE(angleOf({10, 0}), 90.0);
        QCOMPARE(angleOf({0, 10}), 180.0);
        QCOMPARE(angleOf({-10, 0}), 270.0);
        const QPointF p = polar(100, 90);
        QVERIFY(std::abs(p.x() - 100) < 1e-9 && std::abs(p.y()) < 1e-9);
    }

    void sectors()
    {
        // Eight: sector 0 centred at the top, then clockwise.
        QCOMPARE(sectorAt(0, 8), 0);
        QCOMPARE(sectorAt(22, 8), 0);
        QCOMPARE(sectorAt(23, 8), 1);
        QCOMPARE(sectorAt(359, 8), 0);
        QCOMPARE(sectorAt(337, 8), 7);
        QCOMPARE(sectorAt(180, 8), 4);
        QCOMPARE(sectorCentre(2, 8), 90.0);
        // One takes every direction; three split them in thirds.
        QCOMPARE(sectorAt(200, 1), 0);
        QCOMPARE(sectorAt(59, 3), 0);
        QCOMPARE(sectorAt(61, 3), 1);
        QCOMPARE(sectorAt(181, 3), 2);
    }

    void chipsRoundTheMiddle()
    {
        const Metrics m;
        const std::vector<double> widths(8, 96);
        const Layout l = layOut(widths, m);
        QCOMPARE(l.chips.size(), std::size_t(8));
        QVERIFY(apart(l, m));
        // Above: centred on the direction, its bottom on the ring or further
        // out (clear of the chips up to the right and left).
        QCOMPARE(l.chips[0].center().x(), 0.0);
        QVERIFY(l.chips[0].bottom() <= -l.radius);
        QVERIFY(l.chips[0].bottom() <= l.chips[1].top() - m.chipGap + 1e-9);
        // To the right: starting on the ring, centred on the direction.
        QCOMPARE(l.chips[2].left(), l.radius);
        QVERIFY(std::abs(l.chips[2].center().y()) < 1e-9);
        // Up to the right: starting on the ring at its direction.
        QVERIFY(std::abs(l.chips[1].left() - polar(l.radius, 45).x()) < 1e-9);
        QVERIFY(std::abs(l.chips[1].center().y() - polar(l.radius, 45).y()) < 1e-9);
        // To the left: ending there.
        QCOMPARE(l.chips[6].right(), -l.radius);
        // Small: far less than the old ring's 452 across.
        QVERIFY2(l.bounds.width() < 330 && l.bounds.height() < 210,
            qPrintable(QStringLiteral("%1 x %2").arg(l.bounds.width()).arg(l.bounds.height())));
        QCOMPARE(l.reach, l.bounds.adjusted(-m.reach, -m.reach, m.reach, m.reach));
    }

    void ringGrowsOnlyAsNeeded()
    {
        const Metrics m;
        // Four short ones fit at the smallest ring.
        const std::vector<double> four(4, 64);
        QCOMPARE(layOut(four, m).radius, m.minRadius);
        // Eight: three high on each side push it out; long names do not.
        const Layout narrow = layOut(std::vector<double>(8, 64), m);
        const Layout wide = layOut(std::vector<double>(8, 214), m);
        QVERIFY(narrow.radius > m.minRadius);
        QCOMPARE(wide.radius, narrow.radius);
        QVERIFY(wide.bounds.height() < 210); // only the ones above and below moved out, a little
        QVERIFY(apart(wide, m));
        // One alone: above the middle.
        const std::vector<double> one {80};
        const Layout single = layOut(one, m);
        QCOMPARE(single.chips[0].bottom(), -m.minRadius);
        QVERIFY(apart(single, m));
    }

    // Any number of folders, short names or long: apart, and each chip
    // chosen on it and all the way along its direction out to it.
    void everyChipReachable()
    {
        const Metrics m;
        for (int count = 1; count <= 8; ++count) {
            const std::vector<std::vector<double>> sets {std::vector<double>(count, 64.0),
                std::vector<double>(count, 214.0), [count] {
                    std::vector<double> mixed;
                    for (int i = 0; i < count; ++i)
                        mixed.push_back(i % 2 ? 214 : 64);
                    return mixed;
                }()};
            for (const std::vector<double>& widths : sets) {
                const Layout l = layOut(widths, m);
                const QString what = QStringLiteral("%1 chips, first %2 wide").arg(count).arg(widths[0]);
                QVERIFY2(apart(l, m), qPrintable(what));
                for (int i = 0; i < count; ++i) {
                    const QRectF& chip = l.chips[static_cast<std::size_t>(i)];
                    QVERIFY2(hitTest(chip.center(), l, m).sector == i, qPrintable(what));
                    const double angle = sectorCentre(i, count);
                    for (double r = m.dead + 0.5; !chip.contains(polar(r, angle)) && r < 400; r += 1) {
                        const Hit hit = hitTest(polar(r, angle), l, m);
                        QVERIFY2(hit.zone == Zone::Sector && hit.sector == i,
                            qPrintable(QStringLiteral("%1: chip %2 at %3").arg(what).arg(i).arg(r)));
                    }
                }
            }
        }
    }

    void directionChooses()
    {
        const Metrics m;
        const Layout l = layOut(std::vector<double>(8, 96), m);
        QCOMPARE(hitTest({0, 0}, l, m).zone, Zone::Dead);
        QCOMPARE(hitTest({0, -25}, l, m).zone, Zone::Dead);
        // Just past the middle: the direction's chip, long before reaching it.
        const Hit up = hitTest({0, -30}, l, m);
        QCOMPARE(up.zone, Zone::Sector);
        QCOMPARE(up.sector, 0);
        QCOMPARE(hitTest({30, 0}, l, m).sector, 2);
        QCOMPARE(hitTest({-22, 22}, l, m).sector, 5);
        // On a long chip's far end, past its direction's sector: still it.
        const Layout wide = layOut(std::vector<double>(8, 200), m);
        const QPointF farEnd(wide.chips[1].right() - 4, wide.chips[1].center().y());
        QCOMPARE(sectorAt(angleOf(farEnd), 8), 2);
        QCOMPARE(hitTest(farEnd, wide, m).sector, 1);
        // Beyond the chip but within reach: still it; past the reach: off the pie.
        QCOMPARE(hitTest({0, l.bounds.top() - m.reach + 2}, l, m).sector, 0);
        QCOMPARE(hitTest({0, l.bounds.top() - m.reach - 2}, l, m).zone, Zone::Outside);
        QCOMPARE(hitTest({l.bounds.right() + m.reach + 2, 0}, l, m).zone, Zone::Outside);
        // No chips: nothing to choose.
        QCOMPARE(hitTest({0, -60}, Layout {}, m).zone, Zone::Outside);
        // Scaled for 宽松 (14 px labels): the middle grows too.
        const Metrics big = m.scaled(scaleForStep(2));
        QCOMPARE(hitTest({0, -27}, layOut(std::vector<double>(8, 96), big), big).zone, Zone::Dead);
    }

    void moveOrCopy()
    {
        const Allowed both;
        // As File Explorer: within a drive it moves, to another it copies.
        QCOMPARE(modeFor(true, {}, both), Mode::Move);
        QCOMPARE(modeFor(false, {}, both), Mode::Copy);
        // Ctrl copies and Shift moves while held; both held is neither key.
        QCOMPARE(modeFor(true, {.ctrl = true}, both), Mode::Copy);
        QCOMPARE(modeFor(false, {.shift = true}, both), Mode::Move);
        QCOMPARE(modeFor(false, {.ctrl = true, .shift = true}, both), Mode::Copy);
        // What the source does not allow gives way.
        const Allowed copyOnly {.move = false, .copy = true};
        QCOMPARE(modeFor(true, {}, copyOnly), Mode::Copy);
        QCOMPARE(modeFor(true, {.shift = true}, copyOnly), Mode::Copy);
        const Allowed moveOnly {.move = true, .copy = false};
        QCOMPARE(modeFor(false, {.ctrl = true}, moveOnly), Mode::Move);
        QCOMPARE(modeFor(false, {}, Allowed {.move = false, .copy = false}), Mode::None);
    }

    void placement()
    {
        const QRectF area(0, 0, 1920, 1040);
        const QRectF extent(-200, -130, 400, 320); // the chips, and the line under them
        // Room all round: on the pointer.
        Placement p = placeCentre({900, 500}, area, extent, 22);
        QCOMPARE(p.centre, QPointF(900, 500));
        QVERIFY(!p.warp);
        // Near the right edge: moved in just enough; a little, the pointer
        // still in the middle.
        p = placeCentre({1730, 500}, area, extent, 22);
        QCOMPARE(p.centre, QPointF(1720, 500));
        QVERIFY(!p.warp);
        // Further: the pointer goes to the middle.
        p = placeCentre({1860, 500}, area, extent, 22);
        QCOMPARE(p.centre, QPointF(1720, 500));
        QVERIFY(p.warp);
        // In a corner, by the line under the chips too.
        p = placeCentre({10, 1030}, area, extent, 22);
        QCOMPARE(p.centre, QPointF(200, 1040 - 190));
        QVERIFY(p.warp);
        // A work area on a monitor to the left (negative coordinates).
        p = placeCentre({-1910, 20}, QRectF(-1920, 0, 1920, 1040), extent, 22);
        QCOMPARE(p.centre, QPointF(-1720, 130));
        // A work area that does not hold it: centred in it.
        p = placeCentre({150, 200}, QRectF(0, 0, 300, 300), extent, 22);
        QCOMPARE(p.centre, QPointF(150, 120));
    }

    void window()
    {
        const QRect monitor(0, 0, 1920, 1080);
        const QRectF extent(-200.5, -130.25, 401, 320.5);
        const QRect rect = windowRect({900.5, 500.5}, extent, monitor, 1);
        QCOMPARE(rect.left(), 700);
        QCOMPARE(rect.top(), 370);
        QVERIFY(QRectF(rect).contains(extent.translated(900.5, 500.5)));
        // On a second monitor to the right, near its left edge: never onto the first.
        const QRect second(1920, 0, 1280, 1024);
        const QRect onSecond = windowRect({1920 + 150, 400}, extent, second, 1);
        QVERIFY(second.contains(onSecond));
        QCOMPARE(onSecond.left(), 1920);
    }

    // At 125, 150 and 175 %: every edge of the window on a whole device
    // pixel, counted from the monitor's corner, and the extent inside.
    void windowOnWholePixels()
    {
        QCOMPARE(wholePixel(301, 1.5, 1), 302);
        QCOMPARE(wholePixel(301, 1.5, -1), 300);
        QCOMPARE(wholePixel(2560 / 1.5, 1.5, -1), 1706);
        QCOMPARE(wholePixel(2560 / 1.75, 1.75, -1), 1460);
        QCOMPARE(wholePixel(377.3, 1.25, -1), 376);
        QCOMPARE(wholePixel(10, 1.33, 1), 10); // no whole one near: the logical pixel
        const QRectF extent(-200.5, -130.25, 401, 320.5);
        for (const double dpr : {1.25, 1.5, 1.75}) {
            const QRect monitor(-1280, 0, 1280, 800); // logical, left of the primary
            for (const QPointF centre : {QPointF(-640.3, 400.7), QPointF(-1101, 151), QPointF(-1250, 790)}) {
                const QRect rect = windowRect(centre, extent, monitor, dpr);
                const auto whole = [dpr](int v) { return std::abs(v * dpr - std::round(v * dpr)) < 1e-6; };
                const QString what = QStringLiteral("%1 at %2,%3").arg(dpr).arg(centre.x()).arg(centre.y());
                QVERIFY2(whole(rect.left() - monitor.left()) && whole(rect.top() - monitor.top()), qPrintable(what));
                QVERIFY2(whole(rect.width()) && whole(rect.height()), qPrintable(what));
                QVERIFY2(monitor.contains(rect), qPrintable(what));
                QVERIFY2(QRectF(rect).contains(extent.translated(centre).intersected(QRectF(monitor))), qPrintable(what));
            }
        }
    }

    // The folders by pages of eight, in the favorites' order.
    void pageCounts()
    {
        QCOMPARE(kPageSize, 8);
        // None still has the one page that says so.
        for (const auto& [folders, pages] : std::vector<std::pair<int, int>> {{-3, 1}, {0, 1}, {1, 1}, {8, 1}, {9, 2},
                 {16, 2}, {17, 3}, {23, 3}, {24, 3}, {25, 4}, {400, 50}}) {
            QCOMPARE(pageCount(folders), pages);
        }
    }

    void pageSlices()
    {
        const auto same = [](PageSlice a, int first, int count) { return a.first == first && a.count == count; };
        QVERIFY(same(pageSlice(0, 0), 0, 0));
        QVERIFY(same(pageSlice(5, 0), 0, 5));
        QVERIFY(same(pageSlice(8, 0), 0, 8));
        QVERIFY(same(pageSlice(9, 0), 0, 8));
        QVERIFY(same(pageSlice(9, 1), 8, 1));
        QVERIFY(same(pageSlice(16, 1), 8, 8));
        // 23 favorites: 1-8, 9-16, 17-23.
        QVERIFY(same(pageSlice(23, 0), 0, 8));
        QVERIFY(same(pageSlice(23, 1), 8, 8));
        QVERIFY(same(pageSlice(23, 2), 16, 7));
        // A page that is not there is the nearest that is.
        QVERIFY(same(pageSlice(23, 3), 16, 7));
        QVERIFY(same(pageSlice(23, 99), 16, 7));
        QVERIFY(same(pageSlice(23, -1), 0, 8));
        QVERIFY(same(pageSlice(4, 2), 0, 4));
        // Every favorite is on exactly one page.
        for (int folders = 0; folders <= 40; ++folders) {
            int next = 0;
            for (int page = 0; page < pageCount(folders); ++page) {
                const PageSlice slice = pageSlice(folders, page);
                QCOMPARE(slice.first, next);
                QVERIFY(slice.count <= kPageSize);
                QVERIFY(slice.count > 0 || folders == 0);
                next += slice.count;
            }
            QCOMPARE(next, folders);
        }
    }

    void pagesTurnRound()
    {
        // One page: nowhere to go.
        QCOMPARE(turnedPage(0, 8, false), 0);
        QCOMPARE(turnedPage(0, 8, true), 0);
        QCOMPARE(turnedPage(0, 0, false), 0);
        // 23 favorites, 3 pages: forward round the end, back round the start.
        QCOMPARE(turnedPage(0, 23, false), 1);
        QCOMPARE(turnedPage(1, 23, false), 2);
        QCOMPARE(turnedPage(2, 23, false), 0);
        QCOMPARE(turnedPage(2, 23, true), 1);
        QCOMPARE(turnedPage(1, 23, true), 0);
        QCOMPARE(turnedPage(0, 23, true), 2);
        // Two pages: either way is the other one.
        QCOMPARE(turnedPage(0, 9, false), 1);
        QCOMPARE(turnedPage(1, 9, false), 0);
        QCOMPARE(turnedPage(0, 9, true), 1);
        QCOMPARE(turnedPage(1, 9, true), 0);
        // The favorites shrank under a page that was shown: it comes in range first.
        QCOMPARE(clampPage(2, 23), 2);
        QCOMPARE(clampPage(2, 9), 1);
        QCOMPARE(clampPage(5, 3), 0);
        QCOMPARE(clampPage(-1, 30), 0);
        QCOMPARE(turnedPage(2, 9, false), 0); // clamped to the second (last), then round
        QCOMPARE(turnedPage(7, 12, true), 0); // clamped to the second, back to the first
    }
};

QTEST_APPLESS_MAIN(TestPieGeometry)
#include "tst_piegeometry.moc"
