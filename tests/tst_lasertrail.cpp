// The presenter's laser pointer (src/capture/LaserTrail; GitHub #189,
// docs/design-presenter.md 2.7): full width where newest, nothing at the
// life's end, gone a life after the last point, strokes kept apart, close
// points merged, the outline and its bounds where the points are.
#include "LaserTrail.h"

#include <QTest>

using namespace ws::capture::laser;

class TestLaserTrail : public QObject {
    Q_OBJECT

private slots:
    void tapersWithAge()
    {
        Trail trail(10);
        trail.add({0, 0}, 0, false);
        QCOMPARE(trail.widthAt(trail.points().front(), 0), 10.0);
        QCOMPARE(trail.widthAt(trail.points().front(), kLifeMs / 2), 5.0);
        QCOMPARE(trail.widthAt(trail.points().front(), kLifeMs), 0.0);
        QCOMPARE(trail.widthAt(trail.points().front(), kLifeMs * 3), 0.0);
    }

    void goneAfterItsLife()
    {
        Trail trail(8);
        for (int i = 0; i < 20; ++i)
            trail.add(QPointF(i * 10, 0), i * 10, i > 0);
        QVERIFY(trail.visible(190));
        QVERIFY(trail.visible(190 + kLifeMs - 1));
        QVERIFY(!trail.visible(190 + kLifeMs));
        trail.prune(100 + kLifeMs); // the points of 0..100 ms go
        QCOMPARE(int(trail.points().size()), 9);
        QVERIFY(!trail.points().front().joined); // the first left starts the trail
        trail.prune(190 + kLifeMs);
        QVERIFY(trail.points().empty());
        QVERIFY(trail.outline(1000).isEmpty());
    }

    void heldStillStaysNew()
    {
        Trail trail(8);
        trail.add({5, 5}, 0, false);
        trail.add({5.4, 5.2}, 300, true); // closer than kMinStep: the same point, fresh again
        QCOMPARE(int(trail.points().size()), 1);
        QCOMPARE(trail.points().front().time, 300);
        trail.hold(900);
        QVERIFY(trail.visible(900 + kLifeMs - 1));
        QCOMPARE(trail.widthAt(trail.points().front(), 900), 8.0);
    }

    void strokesApart()
    {
        Trail trail(8);
        trail.add({0, 0}, 0, false);
        trail.add({100, 0}, 10, true);
        trail.add({0, 100}, 20, false); // pressed again: no band back to the last stroke
        QVERIFY(trail.points()[1].joined);
        QVERIFY(!trail.points()[2].joined);
        const QPainterPath path = trail.outline(20);
        QVERIFY(path.contains(QPointF(50, 0))); // the band of the first stroke
        QVERIFY(!path.contains(QPointF(50, 50))); // nothing between the strokes
        QVERIFY(path.contains(QPointF(0, 100)));
        // Where a point's dot lies over one band only: filled, not cancelled out.
        QVERIFY(path.contains(QPointF(2, 0)));
        QVERIFY(path.contains(QPointF(98, 1)));
    }

    void boundsCoverTheOutline()
    {
        Trail trail(12);
        trail.add({10, 10}, 0, false);
        trail.add({200, 40}, 50, true);
        trail.add({220, 300}, 100, true);
        const QRectF bounds = trail.bounds(100);
        QVERIFY(bounds.contains(trail.outline(100).boundingRect().adjusted(0.5, 0.5, -0.5, -0.5)));
        QVERIFY(bounds.right() <= 220 + 6 + 1e-9); // the newest point at full width only
    }

    void keepsAtMostTheCap()
    {
        Trail trail(8);
        for (int i = 0; i < kMaxPoints + 100; ++i)
            trail.add(QPointF(i * 2, 0), 0, i > 0);
        QCOMPARE(int(trail.points().size()), kMaxPoints);
        QVERIFY(!trail.points().front().joined);
    }
};

QTEST_GUILESS_MAIN(TestLaserTrail)
#include "tst_lasertrail.moc"
