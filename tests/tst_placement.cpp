#include "BoxSide.h"
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
