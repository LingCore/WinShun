#include "GraphicsLoss.h"

#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

using namespace ws::capture;

// The capture's count of graphics thrown away under its overlays
// ("graphics_lost" in its journal): a lost device reaches it as Qt Quick's
// scene graph torn down (QSGThreadedRenderLoop::handleDeviceLoss), the same
// sceneGraphInvalidated a window gives up its graphics with, which is how
// it is brought about here on the real D3D11 renderer.
class GraphicsLossTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11); }

    // Shown, hidden and shown again, the graphics kept as the overlays keep
    // theirs: nothing. Given up: once, counted from begin(); drawn anew
    // after that, which loses nothing more.
    void countsWhatIsThrownAway()
    {
        GraphicsLoss loss;
        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        window.setColor(Qt::darkCyan);
        window.setGeometry(40, 40, 120, 80);
        loss.watch(&window);
        QSignalSpy swapped(&window, &QQuickWindow::frameSwapped);
        loss.begin();
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(swapped.count() >= 1, 5000);
        window.hide();
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(swapped.count() >= 2, 5000);
        QCOMPARE(loss.since(), 0);

        window.setPersistentGraphics(false);
        window.setPersistentSceneGraph(false);
        window.hide();
        window.releaseResources();
        QTRY_COMPARE_WITH_TIMEOUT(loss.since(), 1, 5000);

        const qsizetype before = swapped.count();
        window.setPersistentGraphics(true);
        window.setPersistentSceneGraph(true);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(swapped.count() > before, 5000);
        QCOMPARE(loss.since(), 1);
        loss.begin(); // the next capture
        QCOMPARE(loss.since(), 0);
    }
};

QTEST_MAIN(GraphicsLossTest)
#include "tst_graphicsloss.moc"
