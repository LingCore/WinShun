#include "CaptureImage.h"
#include "CapturePointer.h"
#include "IdleGraphics.h"

#include <QElapsedTimer>
#include <QPainter>
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
    void hiddenGraphicsAreReleasedAndTheWindowReopens()
    {
        QQmlEngine engine;
        auto window = makeWindow(engine);
        QVERIFY(window);
        auto* frozen = window->findChild<CaptureImage*>(u"frozen"_s);
        QVERIFY(frozen);
        frozen->setImage(pixels());
        const WId handle = window->winId();
        const QImage before = window->grabWindow();
        QVERIFY(!before.isNull());
        QSignalSpy invalidated(window.get(), &QQuickWindow::sceneGraphInvalidated);
        releaseIdleGraphics(window.get(), std::chrono::milliseconds(80));

        window->hide();
        QTest::qWait(20);
        QVERIFY(window->isPersistentGraphics());
        window->show();
        QTest::qWait(100);
        QVERIFY(window->isPersistentGraphics());
        QVERIFY(window->isPersistentSceneGraph());
        QCOMPARE(invalidated.size(), 0); // quick reopening kept its scene graph

        window->hide();
        QTRY_VERIFY_WITH_TIMEOUT(!window->isPersistentGraphics(), 1500);
        QVERIFY(!window->isPersistentSceneGraph());
        const bool threadedD3d = window->rendererInterface()->graphicsApi() == QSGRendererInterface::Direct3D11
            && qEnvironmentVariable("QSG_RENDER_LOOP") != u"basic";
        if (threadedD3d)
            QTRY_VERIFY_WITH_TIMEOUT(!invalidated.isEmpty(), 1500);

        window->show();
        if (QGuiApplication::platformName() != u"offscreen")
            QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(window->isPersistentGraphics(), 1500);
        QVERIFY(window->isPersistentSceneGraph());
        QCOMPARE(window->winId(), handle); // native frame/placement stay attached
        const QImage after = window->grabWindow();
        QCOMPARE(after.size(), before.size());
        // The frozen image survives its texture and scene graph being recreated.
        QCOMPARE(after.copy(2, 2, 100, 80), before.copy(2, 2, 100, 80));
    }

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

    // GitHub #461: the pen's tip as the pointer, as wide as the stroke.
    void brushTipIsTheStrokesSizeAndColour()
    {
        const QColor red(229, 57, 53);
        for (qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
            for (int pixels : {3, 6, 11, 36, 96}) {
                const QCursor cursor = brushCursor(red, pixels, dpr);
                const QImage image = cursor.pixmap().toImage();
                QCOMPARE(cursor.pixmap().devicePixelRatio(), dpr);
                QCOMPARE(image.width(), image.height());
                QCOMPARE(image.width() % 2, 1);
                const int c = image.width() / 2;
                QCOMPARE(qRound(cursor.hotSpot().x() * dpr), c); // the hotspot is the disc's middle
                QCOMPARE(cursor.hotSpot().x(), cursor.hotSpot().y());
                QCOMPARE(image.pixelColor(c, c), red);
                // Across its middle row: the tip's colour for as many pixels as it
                // is wide (those more ink than ring: its round edge is antialiased).
                int width = 0;
                for (int x = 0; x < image.width(); ++x) {
                    const QRgb pixel = image.pixel(x, c);
                    width += qAlpha(pixel) == 255 && qRed(pixel) > 200 && qGreen(pixel) < 156;
                }
                QVERIFY2(std::abs(width - pixels) <= 1, qPrintable(u"%1 px at %2: %3"_s.arg(pixels).arg(dpr).arg(width)));
                // Ringed white just past its edge (lighter than the ink, nothing
                // showing through); the corners are see-through.
                const QRgb ring = image.pixel(c + (pixels + 1) / 2, c);
                QVERIFY(qAlpha(ring) >= 240);
                QVERIFY(qGreen(ring) > red.green());
                QCOMPARE(qAlpha(image.pixel(0, 0)), 0);
                // A small tip has ticks to be found by, a large one none: one
                // pixel into where the right one starts, two rings and a gap out.
                const int edge = std::max(1, qRound(dpr));
                const int tickAt = c + int(std::ceil(pixels / 2.0 + 2 * edge)) + 2 * edge + 1;
                const bool ticks = tickAt < image.width() && image.pixel(tickAt, c) == qRgb(255, 255, 255);
                QCOMPARE(ticks, pixels < 12 * dpr);
            }
        }
        // See-through ink stays see-through over the picture (the marker's).
        const QColor ink(255, 235, 59, 128);
        const QImage marker = brushCursor(ink, 24, 1.0).pixmap().toImage();
        QCOMPARE(qAlpha(marker.pixel(marker.width() / 2, marker.height() / 2)), 128);

        CapturePointer pointer;
        pointer.setDpr(1.5);
        const qint64 cross = pointer.cursor().pixmap().cacheKey();
        pointer.setBrushColor(red);
        pointer.setBrushSize(11);
        const QCursor tip = pointer.cursor();
        QVERIFY(tip.pixmap().cacheKey() != cross);
        QCOMPARE(tip.pixmap().toImage().pixelColor(tip.pixmap().width() / 2, tip.pixmap().height() / 2), red);
        pointer.setShape(Qt::IBeamCursor); // only the cross gives way to it
        QVERIFY(pointer.cursor().pixmap().cacheKey() != tip.pixmap().cacheKey());
        pointer.setShape(Qt::SizeAllCursor);
        QCOMPARE(pointer.cursor().shape(), Qt::SizeAllCursor);
        pointer.setShape(Qt::CrossCursor);
        QCOMPARE(pointer.cursor().pixmap().cacheKey(), tip.pixmap().cacheKey()); // drawn once
        pointer.setBrushSize(0);
        QCOMPARE(pointer.cursor().pixmap().cacheKey(), cross);

        // To look at them: WINSHUN_CURSOR_SHEET=<file.png> saves the cross and
        // the tips (pen, marker, redaction) over light, mid and dark ground.
        if (const QString sheet = qEnvironmentVariable("WINSHUN_CURSOR_SHEET"); !sheet.isEmpty()) {
            const qreal dpr = 1.5;
            QList<QImage> pictures {pointer.cursor().pixmap().toImage()};
            for (int logical : {2, 4, 7, 12, 24}) // the pen's widths, logical px
                pictures.append(brushCursor(red, qRound(logical * dpr), dpr).pixmap().toImage());
            pictures.append(brushCursor(ink, qRound(24 * dpr), dpr).pixmap().toImage());
            pictures.append(brushCursor(QColor(128, 128, 128, 128), qRound(36 * dpr), dpr).pixmap().toImage());
            const int cell = 96;
            QImage out(cell * int(pictures.size()), cell * 3, QImage::Format_RGB32);
            QPainter painter(&out);
            const QList<QColor> grounds {QColor(250, 250, 250), QColor(120, 144, 156), QColor(24, 26, 30)};
            for (int row = 0; row < 3; ++row) {
                painter.fillRect(0, row * cell, out.width(), cell, grounds[row]);
                for (int i = 0; i < pictures.size(); ++i) {
                    const QImage& picture = pictures[i];
                    painter.drawImage(i * cell + (cell - picture.width()) / 2, row * cell + (cell - picture.height()) / 2, picture);
                }
            }
            painter.end();
            QVERIFY(out.save(sheet));
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
