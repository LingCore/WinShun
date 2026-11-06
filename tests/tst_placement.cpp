#include "BoxSide.h"
#include "DragBounds.h"
#include "WindowAvoidance.h"

#include <QtTest>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

// The clipboard window at each 界面大小 (Theme.qml; ClipboardPage.chromeHeight
// is the header, the tabs, a divider, 8 and the footer): all but its rows,
// and a row, in logical pixels. 3 to 8 rows (ClipboardWindow.qml).
struct Size {
    const char* name;
    int chrome;
    int row;
};
constexpr Size kSizes[] {{"compact", 56 + 40 + 1 + 8 + 36, 52}, {"standard", 64 + 44 + 1 + 8 + 42, 60},
    {"roomy", 72 + 48 + 1 + 8 + 48, 68}};
constexpr int kMinRows = 3;
constexpr int kMaxRows = 8;

constexpr int kChrome = 159; // standard
constexpr int kRow = 60;
constexpr int kNeeded = kChrome + kMinRows * kRow; // 339
constexpr int kWanted = kChrome + kMaxRows * kRow; // 639

// 1920 x 1080 at 100 %, the taskbar 48 high at the bottom: the work area,
// and a text caret 20 high whose bottom is at `b` (one past its last line of
// pixels, as a QRect's y + height). The rooms as Placement::settleBy finds
// them, the window 4 off the box (kBoxGap).
constexpr int kWorkTop = 0;
constexpr int kWorkBottom = 1032;
constexpr int kGap = 4;
constexpr int kCaret = 20;
constexpr int roomUnder(int b) { return kWorkBottom - (b + kGap); }
constexpr int roomOver(int b) { return b - kCaret - kGap - kWorkTop; }
constexpr bool aboveCaret(int b) { return boxside::above(roomUnder(b), roomOver(b), kNeeded, kWanted, kRow); }

// The rows ClipboardWindow.qml shows with `room` on its side.
int qmlRows(int room, int chrome, int row)
{
    return std::clamp(int(std::floor(double(room - chrome) / row)), kMinRows, kMaxRows);
}

} // namespace

class PlacementTest : public QObject {
    Q_OBJECT

private slots:
    void dragAcrossMonitorSeam()
    {
        const QList<QRect> areas {{-2560, 0, 2560, 1528}, {0, 0, 3840, 2088}};
        // #111, real dual-monitor trace: moving the pointer 8 physical
        // pixels across x=0 used to move the 1140-pixel window by 1140.
        for (const qreal scale : {1.0, 1.25, 1.5, 1.75, 2.0}) {
            const QSize size(qRound(760 * scale), qRound(106 * scale));
            const int grab = qRound(30 * scale);
            const QPoint before(5 - grab, 300);
            const QPoint after(-3 - grab, 300);
            QCOMPARE(dragbounds::constrain(before, size, areas), before);
            QCOMPARE(dragbounds::constrain(after, size, areas), after);
        }
        const QList<QRect> stacked {{0, -1200, 1920, 1160}, {0, 0, 1920, 1032}};
        QCOMPARE(dragbounds::constrain({300, -80}, {760, 400}, stacked), QPoint(300, -80));
    }

    void dragKeepsOuterDesktopLimits()
    {
        const QList<QRect> areas {{-2560, 0, 2560, 1528}, {0, 0, 3840, 2088}};
        QCOMPARE(dragbounds::constrain({-2600, -10}, {1140, 159}, areas), QPoint(-2560, 0));
        QCOMPARE(dragbounds::constrain({3000, 2000}, {1140, 159}, areas), QPoint(2700, 1929));
        QCOMPARE(dragbounds::constrain({100, 100}, {10000, 5000}, areas), QPoint(-2560, 0));
        const QList<QRect> single {{0, 0, 1920, 1032}};
        QCOMPARE(dragbounds::constrain({-20, 1000}, {760, 400}, single), QPoint(0, 632));
        QCOMPARE(dragbounds::constrain({1300, 200}, {760, 400}, single), QPoint(1160, 200));
        QCOMPARE(dragbounds::constrain({-30, 40}, {760, 400}, {}), QPoint(-30, 40));
    }

    void avoidFileManager_data()
    {
        QTest::addColumn<QRect>("work");
        QTest::addColumn<QRect>("source");
        QTest::addColumn<QPoint>("home");
        QTest::newRow("left") << QRect(0, 0, 1920, 1032) << QRect(0, 0, 1000, 900) << QPoint(580, 200);
        QTest::newRow("right") << QRect(0, 0, 1920, 1032) << QRect(920, 0, 1000, 900) << QPoint(580, 200);
        QTest::newRow("above") << QRect(0, 0, 1920, 1032) << QRect(0, 0, 1920, 300) << QPoint(580, 200);
        QTest::newRow("below") << QRect(0, 0, 1920, 1032) << QRect(0, 750, 1920, 282) << QPoint(580, 200);
        QTest::newRow("negative monitor origin") << QRect(-2560, -200, 2560, 1400)
            << QRect(-2560, -200, 1200, 1200) << QPoint(-1800, 100);
        QTest::newRow("partly off screen") << QRect(0, 0, 1920, 1032) << QRect(-600, -300, 1500, 1200) << QPoint(580, 200);
    }

    void avoidFileManager()
    {
        QFETCH(QRect, work);
        QFETCH(QRect, source);
        QFETCH(QPoint, home);
        const QSize capacity(760, 600);
        const QRect placed(windowavoidance::place(work, source, capacity, home), capacity);
        QVERIFY(work.contains(placed));
        QVERIFY(!placed.intersects(source));
        QCOMPARE(placed.size(), capacity);
    }

    void unavoidableOverlap()
    {
        const QRect work(0, 0, 1280, 960);
        const QSize capacity(760, 600);
        const QPoint home(260, 180);
        // A maximized source leaves nowhere outside it; keep the familiar spot.
        QCOMPARE(windowavoidance::place(work, work, capacity, home), home);
        const QRect source(120, 100, 1040, 800);
        const QRect moved(windowavoidance::place(work, source, capacity, home), capacity);
        const auto covered = [&](const QRect& rect) {
            const QRect overlap = rect.intersected(source);
            return overlap.width() * overlap.height();
        };
        QVERIFY(work.contains(moved));
        QVERIFY(covered(moved) < covered(QRect(home, capacity)));
    }

    // Typed in File Explorer (GitHub #203): beside its window only when all
    // of the box keeps clear of it there (GitHub #141), else into its file
    // list's bottom right corner (inList), as in Opus.
    void explorerTypedIn()
    {
        const QRect work(0, 0, 1920, 1032);
        const QSize box(760, 600), least(640, 250);
        const QPoint home(580, 206); // centred, a fifth of the way down
        // A window too large to keep clear of, centred: least overlap ties at
        // the four corners of the work area (500 x 484 each), two of them as
        // far from home, and the first tried, its top left, won. That is
        // where #203's box was.
        const QRect centred(260, 116, 1400, 800);
        QCOMPARE(windowavoidance::place(work, centred, box, home), work.topLeft());
        QVERIFY(!windowavoidance::clearOf(work, centred, box, home));
        // Into its file list instead (right of the navigation pane, above the
        // status bar), within its client area.
        const QRect client(centred.x() + 8, centred.y() + 31, centred.width() - 16, centred.height() - 39);
        const QRect list(client.x() + 240, client.y() + 90, client.width() - 240, client.height() - 120);
        const QRect into = windowavoidance::inList(work, list, {}, client, box, least);
        QCOMPARE(into, QRect(list.right() + 1 - 12 - 760, list.bottom() + 1 - 12 - 600, 760, 600));
        QVERIFY(client.contains(into));
        // The --folder-typing probe's window, the left third of the screen:
        // beside it, as before.
        const QRect leftThird(8, 8, 640, 1016);
        QVERIFY(windowavoidance::clearOf(work, leftThird, box, home));
        QVERIFY(!QRect(windowavoidance::place(work, leftThird, box, home), box).intersects(leftThird));
        // Maximized: into its list's corner, not at home over its middle.
        QVERIFY(!windowavoidance::clearOf(work, work, box, home));
        const QRect maxList(240, 120, 1680, 880);
        QCOMPARE(windowavoidance::inList(work, maxList, {}, work, box, least),
            QRect(maxList.right() + 1 - 12 - 760, maxList.bottom() + 1 - 12 - 600, 760, 600));
        // A list narrower than the box (a narrow window, its navigation pane
        // wide): as narrow as it gets, kept within the window, then the screen.
        const QRect narrowClient(1000, 100, 700, 800);
        const QRect narrowList(1200, 160, 500, 700);
        const QRect squeezed = windowavoidance::inList(work, narrowList, {}, narrowClient, box, least);
        QCOMPARE(squeezed.width(), least.width());
        QVERIFY(work.contains(squeezed));
        QCOMPARE(squeezed.right(), narrowList.right() - 12);
        // Nothing found but the window: its corner, still within it.
        QVERIFY(client.contains(windowavoidance::inList(work, client, {}, client, box, least)));
    }

    void cornerOfFileList()
    {
        // The launcher's width (its user's, at most) and least width; the room
        // for its rows wanted (8 of them) and needed (3).
        const QSize wanted(760, 560), least(640, 250);
        for (const auto& work : {QRect(0, 0, 1920, 1032), QRect(-2560, -200, 2560, 1400)}) {
            // A file list well inside the work area: the box in its bottom right, 12 px in, as wide and tall as wanted.
            const QRect list(work.topLeft() + QPoint(300, 100), QSize(1400, 800));
            QCOMPARE(windowavoidance::inList(work, list, list, {}, wanted, least),
                QRect(list.right() + 1 - 760 - 12, list.bottom() + 1 - 560 - 12, 760, 560));
            // A maximized one: the same corner, off the taskbar (the work area ends there).
            QCOMPARE(windowavoidance::inList(work, work, {}, {}, wanted, least),
                QRect(work.right() + 1 - 760 - 12, work.bottom() + 1 - 560 - 12, 760, 560));
            // Narrower and shorter than wanted: as wide and tall as the list leaves, inside it.
            const QRect mid(work.topLeft() + QPoint(200, 150), QSize(700, 420));
            QCOMPARE(windowavoidance::inList(work, mid, {}, {}, wanted, least),
                QRect(mid.x() + 12, mid.y() + 12, 700 - 24, 420 - 24));
            // A list partly off the work area: pulled back onto it.
            const QRect off(work.right() - 300, work.bottom() - 200, 800, 500);
            QVERIFY(work.contains(windowavoidance::inList(work, off, {}, {}, wanted, least)));
        }
    }

    void fileListOfOpusLayout()
    {
        // GitHub #82's screenshot, in Qt's pixels at 150 %: a lister with the
        // folder tree, two file lists side by side, typed in the right one,
        // and the viewer at its right; the box is wider than that list.
        const QRect work(0, 0, 1656, 1010);
        const QRect frame(0, 0, 1656, 1010); // maximized
        const QRect left(300, 120, 537, 830), right(840, 120, 537, 830);
        const QRect lists = left.united(right);
        const QSize wanted(760, 560), least(640, 250);
        const QRect box = windowavoidance::inList(work, right, lists, frame, wanted, least);
        // As narrow as it gets, its right edge 12 px in from the list's, over
        // the other list rather than the viewer; its rows end 12 px above the list's bottom.
        QCOMPARE(box, QRect(right.right() + 1 - 12 - 640, right.bottom() + 1 - 12 - 560, 640, 560));
        QVERIFY(lists.contains(box));
        // Typed in the left one: over the right list, not the folder tree.
        const QRect fromLeft = windowavoidance::inList(work, left, lists, frame, wanted, least);
        QCOMPARE(fromLeft, QRect(left.x(), left.bottom() + 1 - 12 - 560, 640, 560));
        QVERIFY(lists.contains(fromLeft));
        // One list only, narrower than the box: from its bottom right corner
        // still, over the tree beside it, and inside the lister.
        const QRect lister(100, 50, 1200, 950);
        const QRect single(600, 120, 500, 830);
        QCOMPARE(windowavoidance::inList(work, single, single, lister, wanted, least),
            QRect(single.right() + 1 - 12 - 640, single.bottom() + 1 - 12 - 560, 640, 560));
        // Near the lister's left edge: kept inside it.
        const QRect edge(120, 120, 500, 830);
        QCOMPARE(windowavoidance::inList(work, edge, edge, lister, wanted, least).x(), lister.x());
        // A list shorter than the room wanted: the rows end 12 px above its bottom.
        const QRect low(840, 600, 537, 300);
        QCOMPARE(windowavoidance::inList(work, low, {}, frame, wanted, least), QRect(725, 612, 640, 276));
        // Shorter than the room needed: over the toolbars above it, not past the lister's bottom.
        const QRect flat(840, 700, 537, 200);
        QCOMPARE(windowavoidance::inList(work, flat, {}, frame, wanted, least), QRect(725, 638, 640, 250));
    }

    void fileListOnSecondMonitor()
    {
        // A second monitor to the left and above, at 150 %: Qt's pixels there
        // start at its native origin, so the list is in those too.
        const QRect work(-1707, -300, 1707, 912);
        const QRect list(-1500, -200, 600, 700);
        const QRect box = windowavoidance::inList(work, list, list, work, {760, 560}, {640, 250});
        QCOMPARE(box, QRect(list.right() + 1 - 12 - 640, list.bottom() + 1 - 12 - 560, 640, 560));
        QVERIFY(work.contains(box));
    }

    void clipboardBesideShellPanel()
    {
        for (const auto& work : {QRect(0, 0, 1920, 1032), QRect(-1920, -200, 1920, 1032)}) {
            const QSize clipboard(360, 480);
            // Both centered Start/Search panels and left-anchored Windows 10
            // panels must leave a fully usable clipboard when space exists.
            for (const QRect panel : {QRect(work.topLeft() + QPoint(600, 200), QSize(720, 800)),
                     QRect(work.topLeft() + QPoint(0, 200), QSize(720, 800))}) {
                const QPoint anchor = panel.topLeft() + QPoint(20, 40);
                const QRect placed(windowavoidance::place(work, panel, clipboard, anchor), clipboard);
                QVERIFY(work.contains(placed));
                QVERIFY(!placed.intersects(panel));
            }
        }
    }

    // GitHub #38: low on the screen the clipboard stayed under the caret with
    // three rows while all eight fit over it. Under it while all of it fits
    // there (b up to 389); else on the side with more whole rows.
    void byCaret_data()
    {
        QTest::addColumn<int>("bottom");
        QTest::addColumn<bool>("above");

        QTest::newRow("top of the screen, nothing over it") << 30 << false;
        QTest::newRow("all fits under it") << 100 << false;
        QTest::newRow("all just fits under it") << 389 << false;
        QTest::newRow("7 rows under, 3 over") << 390 << false;
        QTest::newRow("5 rows under, 5 over") << 520 << false;
        QTest::newRow("5 rows under, 6 over") << 543 << true;
        QTest::newRow("3 rows under, 7 over (the report)") << 650 << true;
        QTest::newRow("3 rows just fit under, 8 over") << 689 << true;
        QTest::newRow("not 3 rows under") << 700 << true;
        QTest::newRow("bottom of the work area") << 1028 << true;
    }

    void byCaret()
    {
        QFETCH(int, bottom);
        QFETCH(bool, above);
        QCOMPARE(aboveCaret(bottom), above);
        // Before (no wanted height, no step), only where the fewest rows did
        // not fit under it.
        QCOMPARE(boxside::above(roomUnder(bottom), roomOver(bottom), kNeeded, 0, 0),
            roomUnder(bottom) < kNeeded && roomOver(bottom) > roomUnder(bottom));
    }

    // As many whole rows on both sides: under it, though there is more room
    // over it.
    void tiesGoUnder()
    {
        QCOMPARE(roomUnder(540), 488); // 339 + 2 rows + 29
        QCOMPARE(roomOver(540), 516); // 339 + 2 rows + 57
        QVERIFY(!aboveCaret(540));
        QVERIFY(!boxside::above(500, 500, kNeeded, kWanted, kRow));
        QVERIFY(!boxside::above(kWanted, kWanted, kNeeded, kWanted, kRow));
        QVERIFY(!boxside::above(kWanted, 5000, kNeeded, kWanted, kRow)); // all of it fits under it
        QVERIFY(!boxside::above(kWanted + 1, 5000, kNeeded, kWanted, kRow));
        QVERIFY(boxside::above(kWanted - 1, kWanted, kNeeded, kWanted, kRow)); // 7 rows under, 8 over
        // Without a step, the pixels.
        QVERIFY(boxside::above(500, 520, kNeeded, kWanted, 0));
        QVERIFY(!boxside::above(500, 500, kNeeded, kWanted, 0));
    }

    // With room for the fewest rows on neither side, the window shows them
    // anyway, pushed onto the work area over the box: on the side with more
    // room, the less of the box it covers.
    void neitherFits()
    {
        QVERIFY(boxside::above(200, 250, kNeeded, kWanted, kRow));
        QVERIFY(!boxside::above(250, 200, kNeeded, kWanted, kRow));
        QVERIFY(!boxside::above(250, 250, kNeeded, kWanted, kRow));
        QVERIFY(boxside::above(-20, 100, kNeeded, kWanted, kRow)); // a box below the work area
        QVERIFY(boxside::above(kNeeded - 1, kNeeded, kNeeded, kWanted, kRow));
    }

    // The height it decides by is the window's own: what ClipboardWindow.qml
    // makes of the room on that side, at every size, so that more of it shows
    // on the side it picks.
    void shownIsTheWindowsHeight()
    {
        for (const Size& size : kSizes) {
            const int needed = size.chrome + kMinRows * size.row;
            const int wanted = size.chrome + kMaxRows * size.row;
            for (int room = -50; room <= 2 * wanted; ++room) {
                const int shown = boxside::shown(room, needed, wanted, size.row);
                if (room < needed)
                    QCOMPARE(shown, room);
                else
                    QCOMPARE(shown, size.chrome + qmlRows(room, size.chrome, size.row) * size.row);
                QVERIFY2(room < needed || shown <= room, size.name); // it fits there
            }
        }
    }

    // Without a wanted height and a step (the launcher, which never sets
    // them), exactly the rule from before: over the box where the fewest
    // rows do not fit under it and there is more room over it.
    void oldRuleWithout()
    {
        for (const int needed : {0, 1, kNeeded, 700}) {
            for (int under = -100; under <= 1100; under += 7) {
                for (int over = -100; over <= 1100; over += 5) {
                    const bool before = under < needed && over > under;
                    if (boxside::above(under, over, needed, 0, 0) != before
                        || boxside::above(under, over, needed, 0, kRow) != before
                        || boxside::above(under, over, needed, needed, kRow) != before)
                        QFAIL(qPrintable(u"needed %1, under %2, over %3"_s.arg(needed).arg(under).arg(over)));
                }
            }
        }
    }

    void constexprToo()
    {
        static_assert(!boxside::above(639, 365, 339, 639, 60));
        static_assert(boxside::above(378, 626, 339, 639, 60));
        static_assert(boxside::shown(378, 339, 639, 60) == 339);
        static_assert(boxside::shown(626, 339, 639, 60) == 579);
        static_assert(boxside::shown(5000, 339, 639, 60) == 639);
        static_assert(boxside::shown(200, 339, 639, 60) == 200);
    }
};

QTEST_APPLESS_MAIN(PlacementTest)
#include "tst_placement.moc"
