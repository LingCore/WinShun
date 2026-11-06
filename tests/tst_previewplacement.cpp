#include "PreviewPlacement.h"

#include <QTest>

using namespace ws;

namespace {

constexpr QSize kText {820, 870}; // a long text file's window on a 1080p screen

// As FilePreview's: as large as it would like to be, within the room.
auto fitting(QSize want)
{
    return [want](QSize room) {
        return want.boundedTo(room).expandedTo(QSize(std::min(360, room.width()), std::min(280, room.height())));
    };
}

QRect centred(const QRect& area, int width, int top, int height)
{
    return {area.left() + (area.width() - width) / 2, top, width, height};
}

} // namespace

Q_DECLARE_METATYPE(ws::PreviewSide)

class TestPreviewPlacement : public QObject {
    Q_OBJECT
private slots:
    void beside_data()
    {
        QTest::addColumn<QRect>("area");
        QTest::addColumn<QRect>("launcher");
        QTest::addColumn<PreviewSide>("side");
        const QRect fullHd(0, 0, 1920, 1040);
        // GitHub #186: the launcher in the middle of a 1080p screen, low
        // down; there was room on both sides, yet it was covered.
        QTest::newRow("#186 middle of 1080p") << fullHd << centred(fullHd, 755, 400, 560) << PreviewSide::Right;
        QTest::newRow("middle of 1080p, high") << fullHd << centred(fullHd, 755, 120, 560) << PreviewSide::Right;
        // 2560×1440 at 150 %, a 48 px taskbar.
        const QRect qhd(0, 0, 1707, 928);
        QTest::newRow("middle of 1440p at 150%") << qhd << centred(qhd, 755, 300, 500) << PreviewSide::Right;
        QTest::newRow("at the left edge") << fullHd << QRect(8, 200, 755, 560) << PreviewSide::Right;
        QTest::newRow("at the right edge") << fullHd << QRect(1157, 200, 755, 560) << PreviewSide::Left;
        QTest::newRow("more room on the left") << fullHd << QRect(1000, 200, 755, 560) << PreviewSide::Left;
        // A monitor left of the main one (negative coordinates).
        const QRect second(-1920, 0, 1920, 1040);
        QTest::newRow("monitor on the left") << second << centred(second, 755, 400, 560) << PreviewSide::Right;
        // Wide and short at the top: below it.
        const QRect small(0, 0, 1366, 728);
        QTest::newRow("wide, short, at the top") << small << QRect(8, 8, 1350, 200) << PreviewSide::Below;
    }

    void beside()
    {
        QFETCH(QRect, area);
        QFETCH(QRect, launcher);
        QFETCH(PreviewSide, side);
        const auto [rect, chosen] = placePreview(area, launcher, PreviewSide::None, kText, fitting(kText));
        QCOMPARE(chosen, side);
        QVERIFY2(!rect.intersects(launcher), "over the list while there is room beside it");
        QVERIFY(area.adjusted(8, 8, -8, -8).contains(rect));
        QVERIFY(rect.width() >= 360 && rect.height() >= 280);
        switch (side) {
        case PreviewSide::Right: QCOMPARE(rect.left(), launcher.right() + 1 + 8); break;
        case PreviewSide::Left: QCOMPARE(rect.right(), launcher.left() - 1 - 8); break;
        case PreviewSide::Below: QCOMPARE(rect.top(), launcher.bottom() + 1 + 8); break;
        default: break;
        }
        // Level with the launcher, unless that would take it off the screen.
        if (side != PreviewSide::Below)
            QCOMPARE(rect.top(), std::min(launcher.top(), area.bottom() - 8 + 1 - rect.height()));
    }

    void noRoomCoversTheFarSideOnly_data()
    {
        QTest::addColumn<QRect>("area");
        QTest::addColumn<QRect>("launcher");
        QTest::addColumn<bool>("toRight");
        const QRect small(0, 0, 1366, 728);
        QTest::newRow("middle of 1366x768") << small << centred(small, 755, 200, 500) << true;
        QTest::newRow("a bit right on 1366x768") << small << QRect(330, 200, 755, 500) << false;
        QTest::newRow("as wide as the screen") << QRect(0, 0, 1280, 680) << QRect(8, 8, 1264, 664) << true;
    }

    void noRoomCoversTheFarSideOnly()
    {
        QFETCH(QRect, area);
        QFETCH(QRect, launcher);
        QFETCH(bool, toRight);
        const auto [rect, side] = placePreview(area, launcher, PreviewSide::None, kText, fitting(kText));
        QCOMPARE(side, toRight ? PreviewSide::Over : PreviewSide::OverLeft);
        const QRect inner = area.adjusted(8, 8, -8, -8);
        QVERIFY(inner.contains(rect));
        QCOMPARE(rect.width(), 480); // readable, and no wider than that
        const int names = launcher.width() * 40 / 100;
        if (toRight) {
            QCOMPARE(rect.right(), inner.right());
            QVERIFY2(rect.left() >= launcher.left() + names, "over the names");
        } else {
            QCOMPARE(rect.left(), inner.left());
            QVERIFY2(rect.right() <= launcher.right() - names, "over the names");
        }
    }

    void staysOnItsSide()
    {
        const QRect area(0, 0, 1920, 1040);
        const QRect launcher(700, 200, 755, 560); // more room on the left
        QCOMPARE(placePreview(area, launcher, PreviewSide::None, kText, fitting(kText)).side, PreviewSide::Left);
        // Chosen before (a small picture first): kept while it has room.
        const auto [rect, side] = placePreview(area, launcher, PreviewSide::Right, kText, fitting(kText));
        QCOMPARE(side, PreviewSide::Right);
        QVERIFY(!rect.intersects(launcher));
        // No longer any room there: chosen again.
        const QRect atTheRight(1157, 200, 755, 560);
        QCOMPARE(placePreview(area, atTheRight, PreviewSide::Right, kText, fitting(kText)).side, PreviewSide::Left);
    }

    void smallFileStaysSmall()
    {
        const QRect area(0, 0, 1920, 1040);
        const QRect launcher = centred(area, 755, 400, 560);
        const QSize facts(560, 460);
        const auto [rect, side] = placePreview(area, launcher, PreviewSide::None, kText, fitting(facts));
        QCOMPARE(side, PreviewSide::Right);
        QCOMPARE(rect.size(), facts);
        QCOMPARE(rect.top(), launcher.top());
    }

    void portraitBottomUsesSpaceAbove()
    {
        const QRect area(-1080, -120, 1080, 1880);
        const QRect launcher(-1040, 1530, 1000, 220);
        const auto [rect, side] = placePreview(area, launcher, PreviewSide::None, kText, fitting(kText));
        QCOMPARE(side, PreviewSide::Above);
        QVERIFY(area.adjusted(8, 8, -8, -8).contains(rect));
        QVERIFY(!rect.intersects(launcher));
        QCOMPARE(rect.bottom(), launcher.top() - 9);
    }

    void smallMovesDoNotFlipSides()
    {
        const QRect area(0, 0, 1920, 1040);
        PreviewSide side = PreviewSide::Right;
        // Cross the 360px room boundary repeatedly, as while dragging near
        // an edge. A small overlap is allowed; the screen edge is not.
        for (int x : {790, 800, 812, 799, 820, 800, 790}) {
            const QRect launcher(x, 150, 755, 550);
            const auto placed = placePreview(area, launcher, side, kText, fitting(kText));
            QCOMPARE(placed.side, PreviewSide::Right);
            QCOMPARE(placed.rect.width(), previewplacement::kSmallest.width());
            QVERIFY(area.adjusted(8, 8, -8, -8).contains(placed.rect));
            QVERIFY(placed.rect.intersected(launcher).width() <= previewplacement::kSideTolerance);
            side = placed.side;
        }
        QCOMPARE(placePreview(area, QRect(1157, 150, 755, 550), side, kText, fitting(kText)).side, PreviewSide::Left);
    }

    void overlayKeepsItsEdge()
    {
        const QRect area(0, 0, 1280, 680);
        PreviewSide side = PreviewSide::Over;
        for (int x : {260, 270, 255, 275}) {
            const auto placed = placePreview(area, QRect(x, 80, 755, 560), side, kText, fitting(kText));
            QCOMPARE(placed.side, side);
            QCOMPARE(placed.rect.right(), area.right() - 8);
            QVERIFY(area.contains(placed.rect));
        }
    }

    void noLauncherInTheMiddle()
    {
        const QRect area(0, 0, 1920, 1040);
        const auto [rect, side] = placePreview(area, QRect(), PreviewSide::None, kText, fitting(QSize(560, 460)));
        QCOMPARE(side, PreviewSide::Over);
        QCOMPARE(rect.size(), QSize(560, 460));
        QVERIFY(qAbs(rect.center().x() - area.center().x()) <= 1 && qAbs(rect.center().y() - area.center().y()) <= 1);
    }
};

QTEST_APPLESS_MAIN(TestPreviewPlacement)
#include "tst_previewplacement.moc"
