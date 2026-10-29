#include "CaptureImage.h"
#include "CapturePointer.h"

#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGTextureProvider>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <memory>

using namespace Qt::StringLiterals;
using namespace ws::capture;

namespace {

QImage pixels()
{
    QImage image(300, 240, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            row[x] = qRgb(x % 256, y, 36);
    }
    return image;
}

// The frozen CaptureImage and loupe geometry from CaptureOverlay, comparing
// the old per-move texture creation against the painted item's backing store.
std::unique_ptr<QQuickWindow> makeWindow(QQmlEngine& engine, bool legacy = false)
{
    QQmlComponent component(&engine);
    QByteArray source = R"(
        import QtQuick
        import CaptureUiTest
        Window {
            id: window
            width: 480; height: 360; color: "black"
            property real dpr: screen.devicePixelRatio
            property point pixel: Qt.point(150, 120)
            CaptureImage {
                id: frozen; objectName: "frozen"
                width: 300 / window.dpr; height: 240 / window.dpr
            }
            Rectangle {
                x: loupe.x; y: loupe.y; width: loupe.width; height: loupe.height
                color: "black"
            }
            CaptureLoupe {
                id: loupe; objectName: "loupe"
                x: 240; y: 100
                width: 15 * Math.round(11 * window.dpr) / window.dpr; height: width
            }
            CapturePointer {
                objectName: "pointer"
                anchors.fill: parent
                dpr: window.dpr
                MouseArea { anchors.fill: parent; hoverEnabled: true; cursorShape: undefined }
            }
        }
    )";
    if (legacy) {
        const auto from = source.indexOf("            CaptureLoupe {");
        const auto to = source.indexOf("            CapturePointer {", from);
        source.replace(from, to - from, R"(
            CaptureImage {
                id: loupe; objectName: "loupe"
                x: 240; y: 100
                width: 15 * Math.round(11 * window.dpr) / window.dpr; height: width
            }
        )");
    }
    component.setData(source, QUrl());
    if (component.isError())
        qWarning().noquote() << component.errorString();
    auto window = std::unique_ptr<QQuickWindow>(qobject_cast<QQuickWindow*>(component.create()));
    if (window) {
        const bool native = QGuiApplication::platformName() != u"offscreen";
        window->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
        window->setPosition(native ? QPoint(32, 32) : QPoint(-20000, -20000));
        window->findChild<CaptureImage*>(u"frozen"_s)->setImage(pixels());
        if (auto* loupe = window->findChild<CaptureLoupe*>(u"loupe"_s))
            loupe->setImage(pixels().copy(QRect(143, 113, 15, 15)));
        window->show();
        if (native && !QTest::qWaitForWindowExposed(window.get(), 3000))
            return {};
        QCoreApplication::processEvents();
        (void)window->grabWindow();
    }
    return window;
}

} // namespace

class TestCaptureUi : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qmlRegisterType<CaptureImage>("CaptureUiTest", 1, 0, "CaptureImage");
        qmlRegisterType<CaptureLoupe>("CaptureUiTest", 1, 0, "CaptureLoupe");
        qmlRegisterType<CapturePointer>("CaptureUiTest", 1, 0, "CapturePointer");
    }

    void crosshairIsAlphaAndKeepsItsHotspot()
    {
        // Only black, white and see-through (left of the hotspot, at the
        // picture's edge); white under the hotspot.
        const auto checkPicture = [](const QCursor& cursor, qreal dpr) {
            const QPixmap pixmap = cursor.pixmap();
            QCOMPARE(pixmap.devicePixelRatio(), dpr);
            const QImage image = pixmap.toImage();
            const QPoint hot(qRound(cursor.hotSpot().x() * dpr), qRound(cursor.hotSpot().y() * dpr)); // as Qt's Windows plugin does
            QCOMPARE(qAlpha(image.pixel(0, hot.y() - std::max(1, qRound(2 * dpr)) - 2)), 0);
            QCOMPARE(image.pixel(hot), qRgb(255, 255, 255));
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const QRgb pixel = image.pixel(x, y);
                    QVERIFY(pixel == 0 || pixel == qRgb(0, 0, 0) || pixel == qRgb(255, 255, 255));
                }
            }
        };
        // Windows' pointer size: 32 as it comes, 80 on the machine this was written on.
        for (int base : {32, 80}) {
            CapturePointer pointer;
            pointer.setBaseSize(base);
            const int arm = qRound(10 * base / 32.0);
            const int half = qRound(8 * base / 32.0);
            const int serif = qRound(3 * base / 32.0);
            for (qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
                pointer.setShape(Qt::CrossCursor);
                pointer.setDpr(dpr);
                const QCursor cross = pointer.cursor();
                QCOMPARE(cross.shape(), Qt::BitmapCursor);
                QCOMPARE(cross.hotSpot(), QPoint(arm, arm));
                QCOMPARE(cross.pixmap().size(), QSize(2 * qRound(arm * dpr) + 1, 2 * qRound(arm * dpr) + 1));
                checkPicture(cross, dpr);
                pointer.setShape(Qt::IBeamCursor);
                const QCursor beam = pointer.cursor();
                QCOMPARE(beam.shape(), Qt::BitmapCursor);
                QCOMPARE(beam.hotSpot(), QPoint(serif, half));
                QCOMPARE(beam.pixmap().size(), QSize(2 * qRound(serif * dpr) + 1, 2 * qRound(half * dpr) + 1));
                checkPicture(beam, dpr);
                for (Qt::CursorShape shape : {Qt::SizeAllCursor, Qt::SizeHorCursor, Qt::SizeFDiagCursor,
                                             Qt::SizeBDiagCursor, Qt::ArrowCursor}) {
                    pointer.setShape(shape);
                    QCOMPARE(pointer.cursor().shape(), shape);
                }
                pointer.setShape(Qt::CrossCursor);
                QCOMPARE(pointer.cursor().pixmap().cacheKey(), cross.pixmap().cacheKey());
            }
        }
    }

    void loupeKeepsExactPixelsAtTheScreenEdges()
    {
        QQmlEngine engine;
        auto window = makeWindow(engine);
        QVERIFY(window);
        const QImage base = pixels();
        const qreal dpr = window->devicePixelRatio();
        auto* loupe = window->findChild<CaptureLoupe*>(u"loupe"_s);
        const int cell = qRound(11 * dpr);
        for (const QPoint at : {QPoint(150, 120), QPoint(0, 0), QPoint(299, 239), QPoint(4, 237)}) {
            window->setProperty("pixel", at);
            loupe->setImage(base.copy(QRect(at - QPoint(7, 7), QSize(15, 15))));
            const QImage frame = window->grabWindow();
            QVERIFY(!frame.isNull());
            for (int y = 0; y < 15; ++y) {
                for (int x = 0; x < 15; ++x) {
                    const QPoint sample = at + QPoint(x - 7, y - 7);
                    const QRgb expected = base.rect().contains(sample) ? base.pixel(sample) : qRgb(0, 0, 0);
                    const QPoint drawn(qRound(240 * dpr) + x * cell + cell / 2,
                                       qRound(100 * dpr) + y * cell + cell / 2);
                    QCOMPARE(frame.pixel(drawn), expected);
                    for (int offset = 0; offset < cell; ++offset) {
                        QCOMPARE(frame.pixel(qRound(240 * dpr) + x * cell + offset, drawn.y()), expected);
                        QCOMPARE(frame.pixel(drawn.x(), qRound(100 * dpr) + y * cell + offset), expected);
                    }
                }
            }
        }
        QImage replacement = base;
        replacement.fill(Qt::green);
        window->findChild<CaptureImage*>(u"frozen"_s)->setImage(replacement);
        window->setProperty("pixel", QPoint(150, 120));
        loupe->setImage(replacement.copy(QRect(143, 113, 15, 15)));
        QCOMPARE(window->grabWindow().pixel(qRound(240 * dpr) + 7 * cell + cell / 2,
                                           qRound(100 * dpr) + 7 * cell + cell / 2), qRgb(0, 255, 0));
        QEnterEvent entered(QPointF(20, 20), QPointF(20, 20), window->mapToGlobal(QPoint(20, 20)));
        QCoreApplication::sendEvent(window.get(), &entered);
        QTest::mouseMove(window.get(), QPoint(20, 20));
        if (QGuiApplication::platformName() == u"offscreen")
            QCOMPARE(window->cursor().shape(), Qt::BitmapCursor); // native hover is checked with real input in the preview
    }

    void loupeReusesItsTextureWhileMoving()
    {
        QQmlEngine engine;
        auto window = makeWindow(engine);
        QVERIFY(window);
        auto* loupe = window->findChild<QQuickItem*>(u"loupe"_s);
        struct Resources {
            const QSGTexture* previous = nullptr;
            const QRhiTexture* previousNative = nullptr;
            std::atomic<int> textures = 0;
            std::atomic<int> nativeTextures = 0;
        };
        const auto resources = std::make_shared<Resources>();
        const auto measurement = QObject::connect(window.get(), &QQuickWindow::beforeRendering, window.get(), [resources, loupe] {
            const auto* provider = loupe->textureProvider();
            const QSGTexture* texture = provider ? provider->texture() : nullptr;
            if (texture && texture != resources->previous) {
                ++resources->textures;
                resources->previous = texture;
            }
            const QRhiTexture* native = texture ? texture->rhiTexture() : nullptr;
            if (native && native != resources->previousNative) {
                ++resources->nativeTextures;
                resources->previousNative = native;
            }
        }, Qt::DirectConnection);
        QSignalSpy frames(window.get(), &QQuickWindow::frameSwapped);
        for (int step = 0; step < 48; ++step) {
            window->findChild<CaptureLoupe*>(u"loupe"_s)->setImage(pixels().copy(QRect(50 + step * 3, 90 + step, 15, 15)));
            if (QGuiApplication::platformName() == u"offscreen") {
                QVERIFY(!window->grabWindow().isNull());
            } else {
                const int before = frames.count();
                window->update();
                QTRY_VERIFY_WITH_TIMEOUT(frames.count() > before, 2000);
            }
        }
        QObject::disconnect(measurement);
        QCOMPARE(resources->textures.load(), 1);
        if (window->rendererInterface()->graphicsApi() != QSGRendererInterface::Software)
            QCOMPARE(resources->nativeTextures.load(), 1); // the GPU image itself is reused too
    }

    void compareLoupeFrameCost()
    {
        for (bool legacy : {true, false}) {
            QQmlEngine engine;
            auto window = makeWindow(engine, legacy);
            QVERIFY(window);
            const QImage base = pixels();
            auto* upload = legacy ? window->findChild<CaptureImage*>(u"loupe"_s) : nullptr;
            auto* paint = window->findChild<CaptureLoupe*>(u"loupe"_s);
            QList<qint64> times;
            for (int step = 0; step < 96; ++step) {
                const QPoint at(50 + step * 2, 60 + step);
                QElapsedTimer timer;
                timer.start();
                if (upload)
                    upload->setImage(base.copy(QRect(at - QPoint(7, 7), QSize(15, 15))));
                else
                    paint->setImage(base.copy(QRect(at - QPoint(7, 7), QSize(15, 15))));
                QVERIFY(!window->grabWindow().isNull());
                if (step >= 16)
                    times.append(timer.nsecsElapsed());
            }
            std::ranges::sort(times);
            qInfo().noquote() << u"Loupe %1, DPR %2: frame/readback p50 %3 ms, p95 %4 ms"_s
                .arg(legacy ? u"recreate"_s : u"reuse"_s).arg(window->devicePixelRatio())
                .arg(times[times.size() / 2] / 1e6, 0, 'f', 3)
                .arg(times[times.size() * 95 / 100] / 1e6, 0, 'f', 3);
        }
    }
};

QTEST_MAIN(TestCaptureUi)
#include "tst_captureui.moc"
