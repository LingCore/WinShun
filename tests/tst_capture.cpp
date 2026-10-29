// The screen capture's pure parts (src/capture): the geometry (Qt's buffer
// scale, edges and handles, nudges, where the toolbar goes, what a click
// picks), the file names, the HDR tone mapping and HDR file, the PrtScn
// value kept, and the session's state machine driven with physical pixels,
// without windows.
#include "CaptureDiagnostics.h"
#include "CaptureSession.h"
#include "Geometry.h"
#include "Hdr.h"
#include "PrintScreen.h"
#include "WindowSnapshot.h"

#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace ws::capture;
using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace {

// A JPEG XR's pixels as half floats, RGBA (WIC); empty when it cannot be read.
std::vector<std::uint16_t> readJxr(const QString& path)
{
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED); // a guiless test: none yet
    const auto uninit = qScopeGuard([init] {
        if (SUCCEEDED(init))
            ::CoUninitialize();
    });
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return {};
    ComPtr<IWICBitmapDecoder> decoder;
    const QString native = QDir::toNativeSeparators(path);
    if (FAILED(factory->CreateDecoderFromFilename(reinterpret_cast<const wchar_t*>(native.utf16()), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder)))
        return {};
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0;
    UINT height = 0;
    WICPixelFormatGUID format {};
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || FAILED(frame->GetPixelFormat(&format))
        || !::IsEqualGUID(format, GUID_WICPixelFormat64bppRGBAHalf))
        return {};
    std::vector<std::uint16_t> pixels(std::size_t(width) * height * 4);
    if (FAILED(frame->CopyPixels(nullptr, width * 8, UINT(pixels.size() * 2), reinterpret_cast<BYTE*>(pixels.data()))))
        return {};
    return pixels;
}

} // namespace

namespace {

// One monitor of 1000 × 800 physical pixels at the virtual desktop's origin.
struct Rig {
    CaptureScreen screen {0};
    CaptureSession session;
    std::vector<QRect> tops;
    std::vector<std::vector<QRect>> children;

    explicit Rig(qreal dpr = 1.0)
    {
        screen.setGeometry(QRect(0, 0, 1000, 800), dpr, QSize(qRound(1000 / dpr), qRound(800 / dpr)));
        QImage image(1000, 800, QImage::Format_RGB32);
        image.fill(qRgb(0x12, 0x34, 0x56));
        image.setPixel(10, 10, qRgb(0xAB, 0xCD, 0xEF));
        screen.setImage(image);
    }
    void begin()
    {
        CaptureSession::Windows windows;
        windows.tops = tops;
        windows.children = [this](std::size_t i) { return i < children.size() ? children[i] : std::vector<QRect> {}; };
        session.begin({&screen}, std::move(windows));
    }
    void drag(QPoint from, QPoint to)
    {
        session.pointerMove(0, from);
        session.pointerPress(0, from, Qt::LeftButton);
        session.pointerMove(0, (from + to) / 2);
        session.pointerMove(0, to);
        session.pointerRelease(0, to, Qt::LeftButton);
    }
    void click(QPoint at)
    {
        session.pointerMove(0, at);
        session.pointerPress(0, at, Qt::LeftButton);
        session.pointerRelease(0, at, Qt::LeftButton);
    }
};

} // namespace

class TestCapture : public QObject {
    Q_OBJECT

private slots:
    void longCaptureFrameStaysOnScreen()
    {
        for (const QRect screen : {QRect(0, 0, 2560, 1600), QRect(-1920, -1080, 1920, 1080)}) {
            for (int thickness : {2, 3, 4}) {
                const auto full = longCaptureBorder(screen, screen, thickness);
                for (const QRect& strip : full) {
                    QVERIFY(screen.contains(strip));
                    QVERIFY(!strip.isEmpty());
                }
                QCOMPARE(full[0].top(), screen.top());
                QCOMPARE(full[1].bottom(), screen.bottom());
                QCOMPARE(full[2].left(), screen.left());
                QCOMPARE(full[3].right(), screen.right());
                const QRect inner = screen.adjusted(40, 40, -40, -40);
                for (const QRect& strip : longCaptureBorder(inner, screen, thickness)) {
                    QVERIFY(screen.contains(strip));
                    QVERIFY(!inner.intersects(strip));
                }
                const QRect corner(screen.topLeft(), QSize(600, 440));
                for (const QRect& strip : longCaptureBorder(corner, screen, thickness))
                    QVERIFY(screen.contains(strip));
            }
        }
    }

    void regionRecordingAction()
    {
        Rig rig;
        rig.begin();
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {499, 399});
        QVERIFY(rig.session.key(Qt::Key_R, Qt::ControlModifier));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Record);

        Rig tiny;
        tiny.begin();
        QSignalSpy none(&tiny.session, &CaptureSession::finished);
        tiny.drag({100, 100}, {119, 119});
        tiny.session.act(static_cast<int>(CaptureSession::Action::Record));
        QCOMPARE(none.count(), 0);
        QVERIFY(tiny.session.active());
    }

    void initTestCase()
    {
        qRegisterMetaType<CaptureSession::Action>();
        qRegisterMetaType<ColorFormat>("ws::capture::ColorFormat");
    }

    // ---- geometry --------------------------------------------------------

    void bufferScaleRounds()
    {
        // 2560 px at 150 %: 1706.67 logical units, Qt's window 1707 wide,
        // its buffer 2561 → the real scale is 2560/1707 · … only when exact.
        QCOMPARE(bufferScale(1707, 1.5), std::round(1707 * 1.5) / 1707);
        QCOMPARE(bufferScale(1000, 1.0), 1.0);
        QCOMPARE(bufferScale(0, 1.25), 1.25); // no size yet: the DPR itself
        QVERIFY(std::abs(toLogical(2560, 1707, 1.5) * bufferScale(1707, 1.5) - 2560) < 1e-9);
    }

    void toPhysicalRoundsBack()
    {
        for (const double dpr : {1.0, 1.25, 1.5, 1.75, 2.0, 2.25}) {
            for (int p = 0; p < 3000; p += 7)
                QCOMPARE(toPhysical(p / dpr, dpr), p);
        }
    }

    void spanRectIncludesBothPixels()
    {
        QCOMPARE(spanRect({10, 20}, {10, 20}), QRect(10, 20, 1, 1));
        QCOMPARE(spanRect({30, 40}, {10, 20}), QRect(10, 20, 21, 21));
    }

    void moveIntoKeepsOnScreen()
    {
        const QRect screen(0, 0, 100, 100);
        QCOMPARE(moveInto(QRect(-5, -5, 10, 10), screen), QRect(0, 0, 10, 10));
        QCOMPARE(moveInto(QRect(95, 95, 10, 10), screen), QRect(90, 90, 10, 10));
        QCOMPARE(moveInto(QRect(10, 10, 300, 20), screen), QRect(0, 10, 100, 20)); // too wide: cut
    }

    void handleAtEdgesAndCorners()
    {
        const QRect sel(100, 100, 200, 100); // edge lines x 100 and 300, y 100 and 200
        const int reach = 6;
        QCOMPARE(handleAt(sel, {100, 100}, reach), Handle::TopLeft);
        QCOMPARE(handleAt(sel, {299, 199}, reach), Handle::BottomRight);
        QCOMPARE(handleAt(sel, {300, 100}, reach), Handle::TopRight); // just outside the right edge
        QCOMPARE(handleAt(sel, {94, 150}, reach), Handle::Left);
        QCOMPARE(handleAt(sel, {92, 150}, reach), Handle::None); // centre 92.5: 7.5 away
        QCOMPARE(handleAt(sel, {200, 205}, reach), Handle::Bottom);
        QCOMPARE(handleAt(sel, {200, 150}, reach), Handle::Move);
        QCOMPARE(handleAt(sel, {500, 500}, reach), Handle::None);
        QCOMPARE(handleAt(QRect(), {0, 0}, reach), Handle::None);
        // Tiny: every pixel is near a corner; the nearer edges win, not
        // always the top left.
        const QRect tiny(50, 50, 3, 3);
        QCOMPARE(handleAt(tiny, {52, 51}, reach), Handle::TopRight);
        QCOMPARE(handleAt(tiny, {50, 52}, reach), Handle::BottomLeft);
        QCOMPARE(handleAt(tiny, {52, 52}, reach), Handle::BottomRight);
    }

    void dragHandleResizesAndFlips()
    {
        const QRect screen(0, 0, 1000, 800);
        const QRect start(100, 100, 200, 100);
        QCOMPARE(dragHandle(start, Handle::Right, {50, 999}, screen), QRect(100, 100, 250, 100));
        QCOMPARE(dragHandle(start, Handle::TopLeft, {-20, -30}, screen), QRect(80, 70, 220, 130));
        // Past the opposite edge: it becomes that edge.
        QCOMPARE(dragHandle(start, Handle::Left, {250, 0}, screen), QRect(300, 100, 50, 100));
        // Onto the opposite edge: one pixel stays.
        QCOMPARE(dragHandle(start, Handle::Bottom, {0, -100}, screen), QRect(100, 100, 200, 1));
        // Kept on the screen.
        QCOMPARE(dragHandle(start, Handle::BottomRight, {5000, 5000}, screen), QRect(100, 100, 900, 700));
        QCOMPARE(dragHandle(start, Handle::Move, {-500, 0}, screen), QRect(0, 100, 200, 100));
        // At the screen's right edge, a pixel wide grows inwards.
        QCOMPARE(dragHandle(QRect(990, 0, 10, 10), Handle::Left, {50, 0}, screen), QRect(999, 0, 1, 10));
    }

    void nudgeMovesAndResizes()
    {
        const QRect screen(0, 0, 100, 100);
        QCOMPARE(nudge(QRect(10, 10, 20, 20), 1, 0, false, screen), QRect(11, 10, 20, 20));
        QCOMPARE(nudge(QRect(0, 10, 20, 20), -1, 0, false, screen), QRect(0, 10, 20, 20)); // at the edge
        QCOMPARE(nudge(QRect(10, 10, 20, 20), 1, 1, true, screen), QRect(10, 10, 21, 21));
        QCOMPARE(nudge(QRect(10, 10, 1, 1), -1, -1, true, screen), QRect(10, 10, 1, 1)); // never under a pixel
        QCOMPARE(nudge(QRect(10, 10, 90, 20), 1, 0, true, screen), QRect(10, 10, 90, 20)); // the screen's edge
    }

    void toolbarBelowAboveInside()
    {
        const QRect screen(0, 0, 1000, 800);
        const QSize bar(200, 40);
        ToolbarPlace p = placeToolbar(QRect(100, 100, 300, 200), bar, screen, 8);
        QCOMPARE(p.where, ToolbarPlace::Below);
        QCOMPARE(p.topLeft, QPoint(200, 308)); // right edges aligned
        p = placeToolbar(QRect(100, 700, 300, 90), bar, screen, 8);
        QCOMPARE(p.where, ToolbarPlace::Above);
        QCOMPARE(p.topLeft, QPoint(200, 652));
        p = placeToolbar(QRect(0, 0, 1000, 800), bar, screen, 8);
        QCOMPARE(p.where, ToolbarPlace::Inside);
        QCOMPARE(p.topLeft, QPoint(800, 752));
        p = placeToolbar(QRect(0, 100, 50, 50), bar, screen, 8); // narrow at the left: kept on the screen
        QCOMPARE(p.topLeft.x(), 0);
    }

    void sizeLabelAndMagnifierStayOnScreen()
    {
        const QRect screen(0, 0, 1000, 800);
        QCOMPARE(placeSizeLabel(QRect(100, 100, 50, 50), QSize(80, 20), screen, 6), QPoint(100, 74));
        QCOMPARE(placeSizeLabel(QRect(100, 0, 50, 50), QSize(80, 20), screen, 6), QPoint(100, 6)); // no room above
        QCOMPARE(placeMagnifier({100, 100}, QSize(150, 200), screen, 20), QPoint(121, 121));
        QCOMPARE(placeMagnifier({990, 790}, QSize(150, 200), screen, 20), QPoint(820, 570)); // flipped
    }

    // Only the capture's own surfaces (pins, countdown) stay out of the window
    // list; Win顺's settings window and other windows are picked like any app's.
    void ownWindowsAreDetectedExceptCaptureSurfaces()
    {
        QVERIFY(WindowSnapshot::isCaptureSurfaceClass(L"WinShun.Pin"));
        QVERIFY(WindowSnapshot::isCaptureSurfaceClass(L"WinShun.Countdown"));
        QVERIFY(!WindowSnapshot::isCaptureSurfaceClass(L"Qt6120QWindowIcon")); // the settings window
        QVERIFY(!WindowSnapshot::isCaptureSurfaceClass(L"Notepad"));
        QVERIFY(!WindowSnapshot::isCaptureSurfaceClass(L""));
    }

    void magnifierAtEveryScaleStaysWhole()
    {
        // The magnifier is as wide as its picture: 15 cells of round(11 * dpr) screen pixels, never clipped by an edge.
        const QRect screen(0, 0, 2560, 1600);
        for (double dpr : {1.0, 1.25, 1.5, 1.75}) {
            const int cell = std::max(1, int(std::lround(11 * dpr)));
            const QSize box(15 * cell + 2, 15 * cell + 70);
            for (QPoint c : {QPoint(1280, 800), QPoint(2559, 1599), QPoint(0, 0), QPoint(2500, 10)}) {
                const QPoint p = placeMagnifier(c, box, screen, int(std::lround(20 * dpr)));
                QVERIFY(screen.contains(QRect(p, box)));
            }
        }
    }

    void hitChainPicksTopmost()
    {
        const QRect screen(0, 0, 1000, 800);
        const std::vector<QRect> windows {QRect(100, 100, 400, 300), QRect(50, 50, 800, 600)};
        const auto children = [](std::size_t i) {
            return i == 0 ? std::vector<QRect> {QRect(100, 100, 400, 300), QRect(120, 120, 200, 100), QRect(130, 130, 50, 20)}
                          : std::vector<QRect> {};
        };
        // Smallest first; a child as big as its window is left out; the screen last.
        std::vector<QRect> chain = hitChain(windows, children, {140, 135}, screen);
        QCOMPARE(chain.size(), std::size_t(4));
        QCOMPARE(chain[0], QRect(130, 130, 50, 20));
        QCOMPARE(chain[1], QRect(120, 120, 200, 100));
        QCOMPARE(chain[2], QRect(100, 100, 400, 300));
        QCOMPARE(chain[3], screen);
        // Under the first window only the second one.
        chain = hitChain(windows, children, {60, 60}, screen);
        QCOMPARE(chain.size(), std::size_t(2));
        QCOMPARE(chain[0], QRect(50, 50, 800, 600));
        // A window reaching off the screen is cut to it.
        chain = hitChain({QRect(-100, -100, 300, 300)}, {}, {10, 10}, screen);
        QCOMPARE(chain[0], QRect(0, 0, 200, 200));
        QVERIFY(hitChain(windows, children, {2000, 10}, screen).empty());
    }

    void fileNamesCountUp()
    {
        const QDateTime when(QDate(2026, 10, 19), QTime(9, 5, 7));
        QCOMPARE(screenshotFileName(when, {}), u"WinShun_20261019_090507.png"_s);
        const QStringList taken {u"WinShun_20261019_090507.png"_s, u"WinShun_20261019_090507_1.png"_s};
        QCOMPARE(screenshotFileName(when, [&](const QString& n) { return taken.contains(n); }),
            u"WinShun_20261019_090507_2.png"_s);
    }

    void colorHexUpperCase()
    {
        QCOMPARE(colorHex(qRgb(0xAB, 0x0C, 0x01)), u"#AB0C01"_s);
    }

    // ---- pins ----------------------------------------------------------------

    void zoomStepsPassThroughOneAndStopAtTheEnds()
    {
        QCOMPARE(nextZoom(1.0, 1), 1.1); // a notch is 10%
        QCOMPARE(nextZoom(1.0, -1), 0.9);
        QCOMPARE(nextZoom(1.1, -1), 1.0); // back to exactly 100%
        QCOMPARE(nextZoom(0.9, 1), 1.0);
        QCOMPARE(nextZoom(1.05, -1), 1.0); // between steps (a corner drag): the next one that way
        QCOMPARE(nextZoom(1.05, 1), 1.1);
        QCOMPARE(nextZoom(1.0, 10), 2.0);
        QCOMPARE(nextZoom(2.0, 1), 3.0); // past 200% the whole multiples
        QCOMPARE(nextZoom(1.0, 100), 8.0);
        QCOMPARE(nextZoom(1.0, -100), 0.1);
        QCOMPARE(nextZoom(8.0, 1), 8.0);
        QCOMPARE(nextZoom(0.1, -1), 0.1);
        QCOMPARE(nextZoom(1.0, 30, 2.5), 2.0); // the window budget stops it
        QCOMPARE(nextZoom(2.0, 1, 2.5), 2.0);
        // Every 10% from 10% to 200%, then crisp whole multiples.
        const std::vector<double>& steps = zoomSteps();
        for (int tenth = 1; tenth <= 20; ++tenth)
            QCOMPARE(steps[std::size_t(tenth - 1)], tenth / 10.0);
        for (double z : steps)
            QVERIFY(z <= 2.0 || z == std::floor(z));
        QVERIFY(std::ranges::is_sorted(steps));
    }

    void maxZoomKeepsTheWindowInBudget()
    {
        QCOMPARE(maxZoom({100, 100}, 24'000'000), 8.0);
        const double z = maxZoom({2560, 1600}, 24'000'000);
        QVERIFY(z > 2.4 && z < 2.5);
        QVERIFY(qint64(2560 * z) * qint64(1600 * z) <= 24'000'000 + 10'000);
        QCOMPARE(maxZoom({20000, 20000}, 24'000'000), 1.0); // never below its own size
    }

    void zoomedSizeIsExactAtOne()
    {
        QCOMPARE(zoomedSize({333, 201}, 1.0), QSize(333, 201));
        QCOMPARE(zoomedSize({333, 201}, 2.0), QSize(666, 402));
        QCOMPARE(zoomedSize({333, 201}, 0.5), QSize(167, 101));
        QCOMPARE(zoomedSize({5, 5}, 0.1), QSize(1, 1));
    }

    void zoomAboutKeepsThePixelUnderTheCursor()
    {
        const QPoint origin(1000, 500);
        const QPoint cursor(1100, 550); // the picture's pixel (100, 50)
        const QPoint twice = zoomAbout(origin, cursor, 1.0, 2.0);
        QCOMPARE(twice, QPoint(900, 450)); // pixel 100 now spans 1100..1101 on the screen
        // Back to 100% from anywhere lands on whole pixels where it started.
        QCOMPARE(zoomAbout(twice, cursor, 2.0, 1.0), origin);
        const QPoint odd = zoomAbout(origin, cursor, 1.0, 1.25);
        const QPoint back = zoomAbout(odd, cursor, 1.25, 1.0);
        QVERIFY((back - origin).manhattanLength() <= 1);
        // The cursor on the picture's top left pixel keeps that corner there.
        QCOMPARE(zoomAbout(origin, origin, 1.0, 3.0), QPoint(999, 499));
    }

    void opacityStepsByTenths()
    {
        QCOMPARE(nextOpacity(1.0, 1), 1.0);
        QCOMPARE(nextOpacity(1.0, -1), 0.9);
        QCOMPARE(nextOpacity(0.2, -5), 0.1);
        QCOMPARE(nextOpacity(0.55, 1), 0.7); // 0.55 rounds to 0.6 first
    }

    void clickThroughPinTakesTheMouseWithCtrlOver()
    {
        // A pin that is not click-through never lets the mouse through.
        QVERIFY(!pinLetsMouseThrough(false, false, false, false));
        QVERIFY(!pinLetsMouseThrough(false, true, true, false));
        // Click-through: through, except with Ctrl held and the cursor over it.
        QVERIFY(pinLetsMouseThrough(true, false, false, false));
        QVERIFY(pinLetsMouseThrough(true, false, true, false)); // over it, no Ctrl
        QVERIFY(pinLetsMouseThrough(true, true, false, false)); // Ctrl, cursor elsewhere
        QVERIFY(!pinLetsMouseThrough(true, true, true, false)); // both: reachable
        // Dragged or its menu up: it keeps the mouse, Ctrl or not.
        QVERIFY(!pinLetsMouseThrough(true, false, false, true));
    }

    void pinFrameFollowsTheDpiThePictureDoesNot()
    {
        const QRect picture(100, 200, 400, 300);
        const PinFrame at100 = pinFrame(1.0);
        const PinFrame at150 = pinFrame(1.5);
        const PinFrame at200 = pinFrame(2.0);
        QCOMPARE(at100.border, 1);
        QCOMPARE(at150.border, 2);
        QCOMPARE(at200.border, 2);
        QCOMPARE(at100.shadow, 8);
        QCOMPARE(at150.shadow, 12);
        QCOMPARE(at200.shadow, 16);
        // Moved to a monitor at another scale: the window grows round the
        // picture, which keeps its place and its physical size.
        const QRect w100 = pinWindowRect(picture, at100);
        const QRect w150 = pinWindowRect(picture, at150);
        QCOMPARE(w100, QRect(91, 191, 418, 318));
        QCOMPARE(w150, QRect(86, 186, 428, 328));
        QCOMPARE(w150.adjusted(at150.margin(), at150.margin(), -at150.margin(), -at150.margin()), picture);
    }

    // ---- OCR -------------------------------------------------------------------

    void ocrUpscalesSmallText()
    {
        QCOMPARE(ocrUpscale({400, 200}, 1.0), 2.0); // 100%: twice
        QCOMPARE(ocrUpscale({400, 200}, 1.25), 2.0);
        QCOMPARE(ocrUpscale({400, 200}, 1.5), 1.0); // 150% and up: as it is
        QCOMPARE(ocrUpscale({400, 200}, 2.0), 1.0);
        QCOMPARE(ocrUpscale({1200, 200}, 1.0), 2000.0 / 1200); // no wider than the reader takes
        QCOMPARE(ocrUpscale({1700, 200}, 1.0), 1.0); // under a quarter more: not worth it
        QCOMPARE(ocrUpscale({}, 1.0), 1.0);
    }

    void placeBesideTriesEachSide()
    {
        const QRect area(0, 0, 1000, 800);
        const QSize box(200, 100);
        QCOMPARE(placeBeside({100, 100, 300, 200}, box, area, 10), QPoint(410, 100)); // right
        QCOMPARE(placeBeside({700, 100, 290, 200}, box, area, 10), QPoint(490, 100)); // left
        QCOMPARE(placeBeside({50, 100, 900, 200}, box, area, 10), QPoint(50, 310)); // under
        QCOMPARE(placeBeside({50, 650, 900, 140}, box, area, 10), QPoint(50, 540)); // over
        QCOMPARE(placeBeside({10, 10, 980, 780}, box, area, 10), QPoint(400, 350)); // the middle
        QCOMPARE(placeBeside({100, 750, 300, 40}, box, area, 10), QPoint(410, 700)); // kept in the area
    }

    // ---- the Start menu ----------------------------------------------------------

    void changedRegionFindsTheFlyoutWithoutItsShadow()
    {
        QImage without(400, 300, QImage::Format_RGB32);
        without.fill(qRgb(30, 30, 30));
        QImage with = without.copy();
        for (int i = 0; i < 40; ++i) // a few sparse changes elsewhere (a caret, a clock)
            with.setPixel((i * 37) % 400, (i * 53) % 300, qRgb(200, 200, 200));
        // A shadow 6 pixels round the flyout: darker, by a little.
        for (int y = 44; y < 256; ++y)
            for (int x = 94; x < 306; ++x)
                with.setPixel(x, y, qRgb(20, 20, 20));
        // The flyout: a 2 px border and busy contents (text over acrylic).
        for (int y = 50; y < 250; ++y)
            for (int x = 100; x < 300; ++x) {
                const bool edge = x < 102 || x >= 298 || y < 52 || y >= 248;
                with.setPixel(x, y, edge ? qRgb(60, 60, 60) : ((x / 3 + y / 5) % 2 ? qRgb(40, 40, 40) : qRgb(220, 220, 220)));
            }
        QCOMPARE(changedRegion(with, without, with.rect(), 4, 0.3), QRect(100, 50, 200, 200));
        // Cut to the area asked about (the work area: not the taskbar).
        QCOMPARE(changedRegion(with, without, QRect(0, 0, 400, 150), 4, 0.3), QRect(100, 50, 200, 100));
        QCOMPARE(changedRegion(without, without, without.rect(), 4, 0.3), QRect());
        QCOMPARE(changedRegion(with, without.scaled(10, 10), with.rect(), 4, 0.3), QRect());
    }

    // ---- HDR -----------------------------------------------------------------

    void hdrMapsWhiteAndBlack()
    {
        // scRGB 1.0 is 80 nits: with the SDR white at 80, 0.5 stays the sRGB
        // of 0.5 (under the knee); the brightest pixel (8.0) ends at white,
        // and 1.0 is rolled off below it to make room. With nothing above
        // white, nothing is rolled off: 1.0 is white.
        const auto half = [](float f) { // float → half for the plain values used here
            if (f == 0)
                return std::uint16_t(0);
            int e;
            const float m = std::frexp(f, &e); // f = m · 2^e, m in [0.5, 1)
            const int exp = e - 1 + 15;
            const auto mant = static_cast<std::uint16_t>(std::lround((m * 2 - 1) * 1024));
            return static_cast<std::uint16_t>((exp << 10) | mant);
        };
        QCOMPARE(halfToFloat(half(1.0f)), 1.0f);
        QCOMPARE(halfToFloat(half(0.5f)), 0.5f);
        QCOMPARE(encodeSrgb(0.0f), std::uint8_t(0));
        QCOMPARE(encodeSrgb(1.0f), std::uint8_t(255));
        std::uint16_t pixels[4 * 4] {}; // 4 pixels, RGBA
        const float values[4] {0.0f, 0.5f, 1.0f, 8.0f};
        for (int i = 0; i < 4; ++i) {
            pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = half(values[i]);
            pixels[i * 4 + 3] = half(1.0f);
        }
        const QImage out = mapScRgbToSdr(pixels, 4, 1, 4 * 4 * 2, 80.0);
        QCOMPARE(out.size(), QSize(4, 1));
        QCOMPARE(qRed(out.pixel(0, 0)), 0);
        QVERIFY(std::abs(qRed(out.pixel(1, 0)) - 188) <= 2); // sRGB(0.5) = 188, under the knee
        QVERIFY(qRed(out.pixel(2, 0)) > 230 && qRed(out.pixel(2, 0)) < 255);
        QCOMPARE(qRed(out.pixel(3, 0)), 255);
        QCOMPARE(qRed(mapScRgbToSdr(pixels, 3, 1, 4 * 4 * 2, 80.0).pixel(2, 0)), 255);
        // A brighter SDR white (200 nits) makes the same value darker.
        const QImage dimmer = mapScRgbToSdr(pixels, 4, 1, 4 * 4 * 2, 200.0);
        QVERIFY(qRed(dimmer.pixel(2, 0)) < 255);
    }

    // GitHub #133: the HDR file beside a saved picture. Every half comes back
    // from floatToHalf as it was; the frame is kept and cut as captured; what
    // was drawn over it goes in at the SDR white level; and the JPEG XR holds
    // the half floats as they are, above 1.0 too (12.5 = 1000 nits).
    void hdrFileKeepsTheFrame()
    {
        for (std::uint32_t h = 0; h < 0x10000; ++h) {
            if ((h & 0x7C00) == 0x7C00 && (h & 0x3FF))
                continue; // NaN
            QCOMPARE(floatToHalf(halfToFloat(std::uint16_t(h))), std::uint16_t(h));
        }
        QCOMPARE(floatToHalf(1e6f), std::uint16_t(0x7C00)); // beyond the largest half: infinity
        QCOMPARE(floatToHalf(65519.0f), std::uint16_t(0x7BFF)); // rounds down to 65504
        QCOMPARE(decodeSrgb(0), 0.0f);
        QCOMPARE(decodeSrgb(255), 1.0f);
        for (int v : {1, 50, 188, 254})
            QCOMPARE(int(encodeSrgb(decodeSrgb(std::uint8_t(v)))), v);

        // 5x3, rows padded to 8 pixels, each pixel's red its index / 10 (up to 1.4), the rest of row 0 HDR.
        constexpr int kW = 5;
        constexpr int kH = 3;
        constexpr int kPitch = 8 * 4;
        std::vector<std::uint16_t> source(std::size_t(kPitch) * kH, 0xFFFF);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                std::uint16_t* p = &source[std::size_t(y) * kPitch + std::size_t(x) * 4];
                p[0] = floatToHalf((y * kW + x) / 10.0f);
                p[1] = floatToHalf(y == 0 ? 12.5f : 0.25f);
                p[2] = floatToHalf(0.5f);
                p[3] = floatToHalf(1.0f);
            }
        const HdrPixels frame = HdrPixels::fromScRgb(source.data(), kW, kH, kPitch * 2, 240.0);
        QCOMPARE(frame.width, kW);
        QCOMPARE(frame.rgba.size(), std::size_t(kW * kH * 4));
        QCOMPARE(frame.rgba[(1 * kW + 2) * 4], floatToHalf(0.7f));
        HdrPixels cut = frame.copy(QRect(1, 1, 3, 2));
        QCOMPARE(QSize(cut.width, cut.height), QSize(3, 2));
        QCOMPARE(cut.sdrWhiteNits, 240.0);
        QCOMPARE(cut.rgba[0], floatToHalf(0.6f)); // (1, 1)
        QCOMPARE(cut.rgba[(1 * 3 + 2) * 4], floatToHalf(1.3f)); // (3, 2)
        QVERIFY(frame.copy(QRect(3, 0, 3, 1)).isNull()); // not inside
        QVERIFY(frame.copy(QRect()).isNull());

        // Drawn over: one pixel changed, to 50% grey; at 240 nits SDR white that is 3x its linear value.
        QImage plain(3, 2, QImage::Format_RGB32);
        plain.fill(qRgb(10, 20, 30));
        QImage drawn = plain.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        drawn.setPixel(1, 0, qRgb(188, 188, 188));
        const std::vector<std::uint16_t> before = cut.rgba;
        paintSdrOver(cut, plain, drawn);
        for (std::size_t i = 0; i < cut.rgba.size(); ++i) {
            if (i / 4 != 1)
                QCOMPARE(cut.rgba[i], before[i]);
        }
        QVERIFY(std::abs(halfToFloat(cut.rgba[4]) - decodeSrgb(188) * 3.0f) < 0.005f);
        QCOMPARE(cut.rgba[7], floatToHalf(1.0f));

        // The JPEG XR, read back through WIC.
        QTemporaryDir dir;
        const QString jxr = dir.filePath(u"frame.jxr"_s);
        QVERIFY(writeJxr(jxr, frame));
        QCOMPARE(readJxr(jxr), frame.rgba);
        QVERIFY(!writeJxr(dir.filePath(u"none/there.jxr"_s), frame)); // no folder: no file, no crash
        QVERIFY(!writeJxr(dir.filePath(u"empty.jxr"_s), HdrPixels()));
        QVERIFY(!QFileInfo::exists(dir.filePath(u"empty.jxr"_s)));
    }

    // ---- PrtScn ----------------------------------------------------------

    void printScreenValueRoundTrips()
    {
        std::optional<DWORD> value;
        QCOMPARE(ws::printscreen::encode(std::nullopt), u"absent"_s);
        QVERIFY(ws::printscreen::decode(u"absent"_s, &value));
        QVERIFY(!value);
        QVERIFY(ws::printscreen::decode(ws::printscreen::encode(DWORD {1}), &value));
        QCOMPARE(value, std::optional<DWORD>(1));
        QVERIFY(!ws::printscreen::decode(u"yes"_s, &value));
        QVERIFY(!ws::printscreen::decode(QString(), &value));
    }

    // ---- the session -----------------------------------------------------

    void dragSelectsAndEnterCopies()
    {
        Rig rig;
        rig.begin();
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {299, 199});
        QVERIFY(rig.session.hasSelection());
        QCOMPARE(rig.session.selectionRect(), QRect(100, 100, 200, 100));
        QCOMPARE(rig.screen.selection(), QRect(100, 100, 200, 100));
        QVERIFY(rig.screen.dimmed());
        QVERIFY(!rig.screen.adjusting());
        QVERIFY(rig.session.key(Qt::Key_Return, Qt::NoModifier));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Copy);
        QCOMPARE(finished[0][2].toRect(), QRect(100, 100, 200, 100));
    }

    void dragBeyondTheScreenIsClamped()
    {
        Rig rig;
        rig.begin();
        rig.session.pointerMove(0, {900, 700});
        rig.session.pointerPress(0, {900, 700}, Qt::LeftButton);
        rig.session.pointerMove(0, {1500, 1200}); // the mouse grabbed past the edge
        rig.session.pointerRelease(0, {1500, 1200}, Qt::LeftButton);
        QCOMPARE(rig.session.selectionRect(), QRect(900, 700, 100, 100));
    }

    void clickPicksWindowAndControl()
    {
        Rig rig;
        rig.tops = {QRect(100, 100, 400, 300)};
        rig.children = {{QRect(120, 120, 100, 40)}};
        rig.begin();
        rig.session.pointerMove(0, {130, 130});
        QCOMPARE(rig.screen.hover(), QRect(120, 120, 100, 40)); // the control
        rig.session.wheel(0, {130, 130}, 1); // out: the window
        QCOMPARE(rig.screen.hover(), QRect(100, 100, 400, 300));
        rig.session.wheel(0, {130, 130}, 1); // out: the screen
        QCOMPARE(rig.screen.hover(), QRect(0, 0, 1000, 800));
        rig.session.wheel(0, {130, 130}, 5); // no further
        QCOMPARE(rig.screen.hover(), QRect(0, 0, 1000, 800));
        rig.session.wheel(0, {130, 130}, -1);
        rig.click({131, 131}); // a twitch under the drag threshold is still a click
        QCOMPARE(rig.session.selectionRect(), QRect(100, 100, 400, 300));
        QCOMPARE(rig.screen.hover(), QRect()); // no outline over a selection
    }

    void clickOnDesktopPicksTheScreen()
    {
        Rig rig;
        rig.begin();
        rig.click({500, 500});
        QCOMPARE(rig.session.selectionRect(), QRect(0, 0, 1000, 800));
    }

    void handlesResizeAndInsideMoves()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {299, 199});
        // The right edge line is at x 300: a press on pixel 300 takes it.
        rig.drag({300, 150}, {350, 150});
        QCOMPARE(rig.session.selectionRect(), QRect(100, 100, 250, 100));
        rig.drag({200, 150}, {210, 160}); // inside: moved
        QCOMPARE(rig.session.selectionRect(), QRect(110, 110, 250, 100));
        rig.session.pointerMove(0, {200, 150});
        QCOMPARE(rig.screen.cursorShape(), int(Qt::SizeAllCursor));
        rig.session.pointerMove(0, {110, 110});
        QCOMPARE(rig.screen.cursorShape(), int(Qt::SizeFDiagCursor));
    }

    void clickOutsideKeepsSelectionDragOutsideReplacesIt()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {199, 199});
        rig.click({600, 600});
        QCOMPARE(rig.session.selectionRect(), QRect(100, 100, 100, 100));
        rig.drag({600, 600}, {699, 649});
        QCOMPARE(rig.session.selectionRect(), QRect(600, 600, 100, 50));
    }

    void arrowsNudgeAndShiftResizes()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {199, 199});
        rig.session.key(Qt::Key_Right, Qt::NoModifier);
        rig.session.key(Qt::Key_Down, Qt::NoModifier);
        QCOMPARE(rig.session.selectionRect(), QRect(101, 101, 100, 100));
        rig.session.key(Qt::Key_Left, Qt::ShiftModifier);
        QCOMPARE(rig.session.selectionRect(), QRect(101, 101, 99, 100));
        rig.session.key(Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(rig.session.selectionRect(), QRect(101, 101, 99, 101));
    }

    void arrowsWithoutSelectionMoveTheCursor()
    {
        Rig rig;
        rig.begin();
        QSignalSpy moves(&rig.session, &CaptureSession::cursorMoveRequested);
        rig.session.pointerMove(0, {0, 0});
        rig.session.key(Qt::Key_Left, Qt::NoModifier); // already at the edge
        rig.session.key(Qt::Key_Down, Qt::NoModifier);
        QCOMPARE(moves.count(), 2);
        QCOMPARE(moves[0][0].toPoint(), QPoint(0, 0));
        QCOMPARE(moves[1][0].toPoint(), QPoint(0, 1));
    }

    void escapeClearsThenCancels()
    {
        Rig rig;
        rig.begin();
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {199, 199});
        rig.session.key(Qt::Key_Escape, Qt::NoModifier);
        QVERIFY(!rig.session.hasSelection());
        QVERIFY(rig.screen.dimmed()); // back to hover: outside the prospective selection stays dim
        QCOMPARE(finished.count(), 0);
        rig.session.pointerPress(0, {50, 50}, Qt::RightButton); // right click: the same
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Cancel);
    }

    void escapeDuringDragGoesBack()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {199, 199});
        rig.session.pointerMove(0, {199, 150});
        rig.session.pointerPress(0, {199, 150}, Qt::LeftButton); // the right edge
        rig.session.pointerMove(0, {400, 150});
        QVERIFY(rig.screen.adjusting());
        rig.session.key(Qt::Key_Escape, Qt::NoModifier);
        QCOMPARE(rig.session.selectionRect(), QRect(100, 100, 100, 100));
        QVERIFY(!rig.screen.adjusting());
    }

    void doubleClickInsideCopiesAsIs()
    {
        Rig rig;
        rig.begin();
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {199, 199});
        // A double click: press, release, then the second press (Adjusting: Move).
        rig.click({150, 150});
        rig.session.pointerPress(0, {150, 150}, Qt::LeftButton);
        rig.session.doubleClick(0, {150, 150});
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Copy);
        QCOMPARE(finished[0][2].toRect(), QRect(100, 100, 100, 100));
    }

    void ctrlSSavesAndColourCopies()
    {
        Rig rig;
        rig.begin();
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        QSignalSpy colors(&rig.session, &CaptureSession::colorCopied);
        rig.session.pointerMove(0, {10, 10});
        QCOMPARE(rig.screen.color(), u"#ABCDEF"_s);
        QCOMPARE(rig.screen.loupe().size(), QSize(CaptureScreen::kLoupeCells, CaptureScreen::kLoupeCells));
        QCOMPARE(rig.screen.loupe().pixel(CaptureScreen::kLoupeCells / 2, CaptureScreen::kLoupeCells / 2),
            qRgb(0xAB, 0xCD, 0xEF));
        const qint64 loupe = rig.screen.loupe().cacheKey();
        rig.screen.setColorFormat(ColorFormat::Rgb);
        QCOMPARE(rig.screen.loupe().cacheKey(), loupe); // changing text does not upload the same pixels again
        rig.screen.setColorFormat(ColorFormat::Hex);
        QVERIFY(rig.session.key(Qt::Key_C, Qt::NoModifier));
        QCOMPARE(colors.count(), 1);
        QCOMPARE(colors[0][0].toString(), u"#ABCDEF"_s);
        QCOMPARE(finished.count(), 1); // and the capture ends, as with Ctrl+C
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Cancel);
        rig.begin();
        QVERIFY(!rig.session.key(Qt::Key_S, Qt::NoModifier)); // S alone: nothing
        rig.drag({100, 100}, {199, 199});
        QVERIFY(rig.session.key(Qt::Key_S, Qt::ControlModifier));
        QCOMPARE(finished.count(), 2);
        QCOMPARE(finished[1][0].value<CaptureSession::Action>(), CaptureSession::Action::Save);
    }

    void magnifierCanBeTurnedOff()
    {
        Rig rig;
        rig.begin();
        rig.session.setMagnifier(false);
        rig.session.pointerMove(0, {50, 50});
        QVERIFY(!rig.screen.magnifierShown());
        rig.session.setMagnifier(true);
        rig.session.pointerMove(0, {60, 60});
        QVERIFY(rig.screen.magnifierShown());
    }

    void magnifierOnlyOverASelectionMade()
    {
        Rig rig;
        rig.begin();
        rig.session.pointerMove(0, {50, 50});
        QVERIFY(rig.screen.magnifierShown()); // picking: everywhere
        rig.session.pointerMove(0, {100, 100});
        rig.session.pointerPress(0, {100, 100}, Qt::LeftButton);
        rig.session.pointerMove(0, {300, 300});
        QVERIFY(rig.screen.magnifierShown()); // drawing: everywhere
        rig.session.pointerMove(0, {199, 199});
        rig.session.pointerRelease(0, {199, 199}, Qt::LeftButton);
        QVERIFY(rig.screen.magnifierShown()); // made: over it
        rig.session.pointerMove(0, {200, 150});
        QVERIFY(rig.screen.magnifierShown()); // and on its frame
        rig.session.pointerMove(0, {204, 150});
        QVERIFY(!rig.screen.magnifierShown()); // on the handle beyond it: not until it is dragged
        rig.session.pointerPress(0, {204, 150}, Qt::LeftButton);
        rig.session.pointerMove(0, {230, 150});
        QVERIFY(rig.screen.magnifierShown());
        rig.session.pointerRelease(0, {230, 150}, Qt::LeftButton);
        rig.session.pointerMove(0, {300, 150});
        QVERIFY(!rig.screen.magnifierShown()); // beside it: gone
        QSignalSpy colors(&rig.session, &CaptureSession::colorCopied);
        QVERIFY(rig.session.key(Qt::Key_C, Qt::NoModifier));
        QCOMPARE(colors.count(), 0); // no colour copied that is not shown
        rig.session.pointerMove(0, {150, 150});
        QVERIFY(rig.screen.magnifierShown());
        rig.session.mouseLeft(0); // onto the toolbar
        QVERIFY(!rig.screen.magnifierShown());
        rig.session.pointerMove(0, {150, 151});
        QVERIFY(rig.screen.magnifierShown());
        rig.session.pointerMove(0, {101, 150});
        for (int i = 0; i < 20; ++i)
            rig.session.key(Qt::Key_Right, Qt::NoModifier); // the selection moves away from the cursor
        QCOMPARE(rig.session.selectionRect().left(), 120);
        QVERIFY(!rig.screen.magnifierShown());
        rig.session.key(Qt::Key_Escape, Qt::NoModifier); // back to picking, the cursor where it was
        QVERIFY(rig.screen.magnifierShown());
        rig.drag({100, 100}, {199, 199});
        rig.session.pointerMove(0, {300, 300});
        rig.session.pointerPress(0, {300, 300}, Qt::LeftButton); // beside it: a click keeps it
        QVERIFY(!rig.screen.magnifierShown());
        rig.session.pointerMove(0, {400, 400}); // a drag draws a new one
        QVERIFY(rig.screen.magnifierShown());
    }

    void f3PinsAndTheToolbarReads()
    {
        Rig rig;
        rig.begin();
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {199, 149});
        QVERIFY(rig.session.key(Qt::Key_F3, Qt::NoModifier));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Pin);
        QCOMPARE(finished[0][2].toRect(), QRect(100, 100, 100, 50));
        rig.begin();
        rig.drag({10, 20}, {59, 39});
        rig.session.act(int(CaptureSession::Action::Ocr)); // the toolbar's 识别文字
        QCOMPARE(finished.count(), 2);
        QCOMPARE(finished[1][0].value<CaptureSession::Action>(), CaptureSession::Action::Ocr);
        QCOMPARE(finished[1][2].toRect(), QRect(10, 20, 50, 20));
        rig.session.act(99); // not an action
        QCOMPARE(finished.count(), 2);
    }

    void ctrlLStartsALongScreenshotWithoutAnnotations()
    {
        Rig rig;
        rig.begin();
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {499, 399});
        drawRect(rig, {150, 150}, {300, 250});
        QVERIFY(rig.session.key(Qt::Key_L, Qt::ControlModifier));
        QCOMPARE(finished.count(), 0); // 先完成或放弃标注
        QVERIFY(!rig.screen.notice().isEmpty());
        rig.session.act(int(CaptureSession::Action::Long)); // the toolbar's 长截图 alike
        QCOMPARE(finished.count(), 0);
        rig.begin();
        rig.drag({10, 20}, {49, 319}); // 40 px wide: too narrow to scroll
        QVERIFY(rig.session.key(Qt::Key_L, Qt::ControlModifier));
        QCOMPARE(finished.count(), 0);
        QVERIFY(!rig.screen.notice().isEmpty());
        rig.begin();
        rig.drag({10, 20}, {209, 319});
        QVERIFY(rig.session.key(Qt::Key_L, Qt::ControlModifier));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Long);
        QCOMPARE(finished[0][2].toRect(), QRect(10, 20, 200, 300));
    }

    void flyoutIsPickedOverTheWindows()
    {
        Rig rig;
        rig.tops = {QRect(0, 0, 1000, 700)}; // a window under the Start menu
        rig.children = {{QRect(50, 50, 200, 100)}};
        rig.begin();
        rig.session.pointerMove(0, {60, 60});
        QCOMPARE(rig.screen.hover(), QRect(50, 50, 200, 100));
        // Found once the menu closed, while the cursor is already there.
        rig.session.setFlyout(QRect(40, 40, 300, 400));
        QCOMPARE(rig.screen.hover(), QRect(40, 40, 300, 400)); // no controls of its own
        rig.session.pointerMove(0, {600, 600}); // beside it: the window again
        QCOMPARE(rig.screen.hover(), QRect(0, 0, 1000, 700));
        rig.session.pointerMove(0, {60, 60});
        rig.session.wheel(0, {60, 60}, 1);
        QCOMPARE(rig.screen.hover(), QRect(0, 0, 1000, 800)); // out: the screen
        rig.session.end();
        rig.begin(); // the next capture has none
        rig.session.pointerMove(0, {60, 60});
        QCOMPARE(rig.screen.hover(), QRect(50, 50, 200, 100));
    }

    void copyOnSelectFinishesAtOnce()
    {
        Rig rig;
        rig.session.setCopyOnSelect(true);
        rig.begin();
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {199, 199});
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][2].toRect(), QRect(100, 100, 100, 100));
    }

    void logicalInputAtFractionalScale()
    {
        // 150 %: Qt's logical position × 1.5 is the physical pixel again.
        Rig rig(1.5);
        rig.begin();
        rig.session.mouseMoved(0, 101 / 1.5, 51 / 1.5);
        QCOMPARE(rig.screen.cursor(), QPoint(101, 51));
        // The handle reach, 6 px at 100 %, is 9 px at 150 %.
        rig.drag({100, 100}, {199, 199});
        rig.session.pointerMove(0, {208, 150});
        QCOMPARE(rig.screen.cursorShape(), int(Qt::SizeHorCursor));
    }

    void endClearsState()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {199, 199});
        rig.session.end();
        QVERIFY(!rig.session.active());
        QCOMPARE(rig.screen.selection(), QRect());
        QVERIFY(!rig.screen.cursorHere());
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.session.key(Qt::Key_Return, Qt::NoModifier); // nothing after the end
        QCOMPARE(finished.count(), 0);
    }

    // ---- annotating (AnnotationModel under the session) -------------------
    void polylineSurvivesDoubleClickRouting()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {700, 500});
        QVERIFY(rig.session.key(Qt::Key_L, {}));
        auto* model = rig.session.annotation();
        model->setPanel({{u"lineMode"_s, u"polyline"_s}});
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.click({150, 150});
        rig.session.pointerMove(0, {350, 170});
        QVERIFY(model->pathActive());
        QCOMPARE(model->document().items.first().points.last(), QPointF(350.5, 170.5));
        rig.click({350, 170});
        // Qt delivers the second press before doubleClick.
        rig.session.pointerPress(0, {350, 170}, Qt::LeftButton);
        rig.session.doubleClick(0, {350, 170});
        rig.session.pointerRelease(0, {350, 170}, Qt::LeftButton);
        QVERIFY(!model->pathActive());
        QCOMPARE(model->itemCount(), 1);
        QCOMPARE(model->document().items.first().points.size(), 2);
        QVERIFY(finished.isEmpty());
        rig.click({450, 250});
        rig.click({550, 300});
        rig.session.pointerPress(0, {550, 300}, Qt::RightButton);
        QVERIFY(!model->pathActive());
        QCOMPARE(model->itemCount(), 2);
        QVERIFY(rig.session.hasSelection());
        QVERIFY(finished.isEmpty());
    }

    // A rectangle drawn with the R key and the mouse, then the tool put away.
    static void drawRect(Rig& rig, QPoint from, QPoint to)
    {
        rig.session.key(Qt::Key_R, Qt::NoModifier);
        rig.drag(from, to);
        rig.session.key(Qt::Key_R, Qt::NoModifier); // the same key: tool off
    }

    void toolKeysAndDrawingGoToTheAnnotations()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {499, 399});
        AnnotationModel* annot = rig.session.annotation();
        QVERIFY(annot->active());
        QCOMPARE(annot->selectionRect(), QRect(100, 100, 400, 300));
        QVERIFY(rig.session.key(Qt::Key_R, Qt::NoModifier));
        QCOMPARE(annot->tool(), 0);
        rig.drag({150, 150}, {300, 250});
        QCOMPARE(annot->document().items.size(), 1);
        QVERIFY(rig.session.hasSelection()); // drawing inside never made a new selection
        QCOMPARE(rig.session.selectionRect(), QRect(100, 100, 400, 300));
        QVERIFY(rig.session.key(Qt::Key_R, Qt::NoModifier)); // again: off
        QCOMPARE(annot->tool(), -1);
        // Ctrl+Z is the annotations' while a selection is up
        QVERIFY(rig.session.key(Qt::Key_Z, Qt::ControlModifier));
        QCOMPARE(annot->document().items.size(), 0);
    }

    void escapePutsSelectionAndAnnotationsDownTogether()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {499, 399});
        drawRect(rig, {150, 150}, {300, 250});
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.session.key(Qt::Key_Escape, Qt::NoModifier);
        QVERIFY(!rig.session.hasSelection());
        QVERIFY(rig.session.hasAnnotations()); // kept, waiting for Ctrl+Z
        QVERIFY(!rig.screen.notice().isEmpty());
        QCOMPARE(finished.count(), 0);
        rig.session.key(Qt::Key_Z, Qt::ControlModifier); // back as they were
        QVERIFY(rig.session.hasSelection());
        QCOMPARE(rig.session.selectionRect(), QRect(100, 100, 400, 300));
        QCOMPARE(rig.session.annotation()->document().items.size(), 1);
        rig.session.key(Qt::Key_Escape, Qt::NoModifier); // down again
        rig.session.key(Qt::Key_Escape, Qt::NoModifier); // out
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Cancel);
        QVERIFY(rig.session.hasAnnotations()); // the capture reads them for its draft
    }

    void escapeDuringAStrokeOnlyDropsTheStroke()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {499, 399});
        rig.session.key(Qt::Key_E, Qt::NoModifier);
        rig.session.pointerMove(0, {150, 150});
        rig.session.pointerPress(0, {150, 150}, Qt::LeftButton);
        rig.session.pointerMove(0, {250, 250});
        QVERIFY(rig.session.annotation()->gestureActive());
        rig.session.key(Qt::Key_Escape, Qt::NoModifier);
        QVERIFY(!rig.session.annotation()->gestureActive());
        rig.session.pointerRelease(0, {250, 250}, Qt::LeftButton);
        QCOMPARE(rig.session.annotation()->document().items.size(), 0);
        QVERIFY(rig.session.hasSelection()); // the selection stayed
    }

    void doubleClickOnAnObjectNeverCopies()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {499, 399});
        drawRect(rig, {150, 150}, {300, 250});
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        // Qt: press, release, then the second press as a press and a double click
        rig.session.pointerMove(0, {150, 200});
        rig.session.pointerPress(0, {150, 200}, Qt::LeftButton); // on the rectangle's left edge
        rig.session.pointerRelease(0, {150, 200}, Qt::LeftButton);
        rig.session.pointerPress(0, {150, 200}, Qt::LeftButton);
        rig.session.doubleClick(0, {150, 200});
        rig.session.pointerRelease(0, {150, 200}, Qt::LeftButton);
        QCOMPARE(finished.count(), 0);
        // on the bare picture it still copies
        rig.session.pointerMove(0, {400, 350});
        rig.session.pointerPress(0, {400, 350}, Qt::LeftButton);
        rig.session.pointerRelease(0, {400, 350}, Qt::LeftButton);
        rig.session.pointerPress(0, {400, 350}, Qt::LeftButton);
        rig.session.doubleClick(0, {400, 350});
        QCOMPARE(finished.count(), 1);
    }

    void aPressOutsideTheSelectionKeepsItWhileAnnotated()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {499, 399});
        drawRect(rig, {150, 150}, {300, 250});
        rig.drag({600, 500}, {800, 700}); // would have drawn a new selection
        QCOMPARE(rig.session.selectionRect(), QRect(100, 100, 400, 300));
        QVERIFY(!rig.screen.notice().isEmpty()); // 已有标注：…
        QCOMPARE(rig.session.annotation()->document().items.size(), 1);
    }

    void hHidesTheToolbar()
    {
        Rig rig;
        rig.begin();
        QVERIFY(!rig.session.key(Qt::Key_H, Qt::NoModifier)); // nothing selected: not taken
        rig.drag({100, 100}, {499, 399});
        QSignalSpy hidden(&rig.session, &CaptureSession::toolbarHiddenChanged);
        QVERIFY(rig.session.key(Qt::Key_H, Qt::NoModifier));
        QVERIFY(rig.session.toolbarHidden());
        QVERIFY(rig.session.key(Qt::Key_H, Qt::NoModifier));
        QVERIFY(!rig.session.toolbarHidden());
        QCOMPARE(hidden.count(), 2);
    }

    void aNewCaptureStartsClean()
    {
        Rig rig;
        rig.begin();
        rig.drag({100, 100}, {499, 399});
        drawRect(rig, {150, 150}, {300, 250});
        rig.session.end();
        QVERIFY(!rig.session.annotation()->active());
        rig.begin();
        QVERIFY(!rig.session.hasAnnotations());
        rig.drag({100, 100}, {499, 399});
        QCOMPARE(rig.session.annotation()->document().items.size(), 0);
    }

    void annotationsFollowTheStylesGiven()
    {
        Rig rig;
        rig.session.setStyles(ws::capture::annot::StyleMemory::fromJson(
            u"{\"version\":1,\"rect\":{\"color\":\"#1E88E5\",\"width\":7,\"fill\":\"none\"}}"_s));
        rig.begin();
        rig.drag({100, 100}, {499, 399});
        drawRect(rig, {150, 150}, {300, 250});
        const auto& item = rig.session.annotation()->document().items.first();
        QCOMPARE(item.style.color, qRgb(0x1E, 0x88, 0xE5));
        QCOMPARE(item.style.width, 7);
        QVERIFY(!rig.session.stylesChanged()); // nothing was changed on the panel
    }

    // ---- the mouse buttons after a selection (#46) -----------------------------

    void defaultButtonsKeepTheOldBehaviour()
    {
        Rig rig;
        rig.begin();
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {299, 299});
        // middle and the side buttons do nothing by default
        for (Qt::MouseButton b : {Qt::MiddleButton, Qt::BackButton, Qt::ForwardButton})
            rig.session.pointerPress(0, {150, 150}, b);
        QCOMPARE(finished.count(), 0);
        QVERIFY(rig.session.hasSelection());
        // the right button steps back: selection down, then the capture ends
        rig.session.pointerPress(0, {150, 150}, Qt::RightButton);
        QVERIFY(!rig.session.hasSelection());
        QCOMPARE(finished.count(), 0);
        rig.session.pointerPress(0, {150, 150}, Qt::RightButton);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Cancel);
    }

    void buttonsRunTheirActions_data()
    {
        QTest::addColumn<int>("button");
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("action");
        using A = CaptureSession::Action;
        const struct {
            Qt::MouseButton button;
            const char* name;
            A action;
        } rows[] {{Qt::MiddleButton, "copy", A::Copy}, {Qt::BackButton, "save", A::Save},
            {Qt::ForwardButton, "pin", A::Pin}, {Qt::RightButton, "ocr", A::Ocr}, {Qt::MiddleButton, "cancel", A::Cancel},
            {Qt::MiddleButton, "ocrcopy", A::OcrCopy}, {Qt::BackButton, "ocrsearch", A::OcrSearch},
            {Qt::ForwardButton, "ocrweb", A::OcrWeb}};
        for (const auto& r : rows)
            QTest::newRow(r.name) << int(r.button) << QString::fromLatin1(r.name) << int(r.action);
    }
    void buttonsRunTheirActions()
    {
        QFETCH(int, button);
        QFETCH(QString, name);
        QFETCH(int, action);
        Rig rig;
        rig.begin();
        CaptureSession::MouseActions actions;
        const auto b = Qt::MouseButton(button);
        (b == Qt::MiddleButton ? actions.middle : b == Qt::BackButton ? actions.back : b == Qt::ForwardButton ? actions.forward : actions.right) = name;
        rig.session.setMouseActions(actions);
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.session.pointerPress(0, {50, 50}, b); // no selection yet: the right button steps back, the others nothing
        rig.session.pointerRelease(0, {50, 50}, b);
        QCOMPARE(finished.count(), b == Qt::RightButton ? 1 : 0);
        if (b == Qt::RightButton) { // the capture ended; start again
            rig.begin();
            finished.clear();
        }
        rig.drag({100, 100}, {299, 299});
        rig.session.pointerPress(0, {150, 150}, b);
        QCOMPARE(finished.count(), 0); // it acts when the same button is let go of on the capture
        rig.session.pointerRelease(0, {150, 150}, b);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action(action));
        if (CaptureSession::Action(action) != CaptureSession::Action::Cancel) // a cancel takes nothing
            QCOMPARE(finished[0][2].toRect(), QRect(100, 100, 200, 200));
    }

    // 识别文字并搜索 / 网页搜索 and the OCR window's 网页搜索 hand the words over as one line.
    void ocrWordsBecomeOneLine()
    {
        QCOMPARE(ocrOneLine(u"  第一行\n  第二行\t\tthird \r\n"_s), u"第一行 第二行 third"_s);
        QVERIFY(ocrOneLine(u" \n\t "_s).isEmpty());
        QCOMPARE(ocrOneLine(QString(1000, u'x')).size(), 256);
        QCOMPARE(ocrOneLine(u"x"_s, 3), u"x"_s);
        QCOMPARE(ocrOneLine(u"abcdef"_s, 3), u"abc"_s);
    }

    void hiddenToolbarButtonsAreTheSessionsToTell()
    {
        Rig rig;
        QSignalSpy changed(&rig.session, &CaptureSession::hiddenButtonsChanged);
        QVERIFY(rig.session.hiddenButtons().isEmpty());
        rig.session.setHiddenButtons({u"pin"_s, u"ocr"_s});
        QCOMPARE(rig.session.hiddenButtons(), (QStringList {u"pin"_s, u"ocr"_s}));
        QCOMPARE(changed.count(), 1);
        rig.session.setHiddenButtons({u"pin"_s, u"ocr"_s}); // the same: no signal
        QCOMPARE(changed.count(), 1);
        // The keys of a hidden button still work.
        rig.begin();
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {299, 299});
        rig.session.key(Qt::Key_F3, Qt::NoModifier);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Pin);
    }

    void unsetButtonsOrBadNamesDoNothing()
    {
        Rig rig;
        rig.begin();
        CaptureSession::MouseActions actions;
        actions.middle = u"nonsense"_s; // unknown: the default (none)
        actions.back = u"back"_s; // back is the right button's alone
        actions.right = u"none"_s;
        rig.session.setMouseActions(actions);
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {299, 299});
        for (Qt::MouseButton b : {Qt::MiddleButton, Qt::BackButton, Qt::ForwardButton, Qt::RightButton})
            rig.session.pointerPress(0, {150, 150}, b);
        QCOMPARE(finished.count(), 0);
        QVERIFY(rig.session.hasSelection()); // right set to none: the selection stays; Esc still steps back
        rig.session.key(Qt::Key_Escape, Qt::NoModifier);
        QVERIFY(!rig.session.hasSelection());
    }

    void whileAnnotatingTheRightButtonStepsBackAndTheRestSleep()
    {
        Rig rig;
        rig.begin();
        CaptureSession::MouseActions actions;
        actions.right = u"copy"_s;
        actions.middle = u"copy"_s;
        actions.back = u"save"_s;
        rig.session.setMouseActions(actions);
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {499, 399});
        rig.session.key(Qt::Key_R, Qt::NoModifier); // a tool is on
        for (Qt::MouseButton b : {Qt::MiddleButton, Qt::BackButton, Qt::ForwardButton})
            rig.session.pointerPress(0, {150, 150}, b);
        QCOMPARE(finished.count(), 0);
        rig.session.pointerPress(0, {150, 150}, Qt::LeftButton); // a stroke under way
        QVERIFY(rig.session.annotation()->gestureActive());
        rig.session.pointerPress(0, {200, 200}, Qt::MiddleButton);
        QCOMPARE(finished.count(), 0);
        rig.session.pointerPress(0, {200, 200}, Qt::RightButton); // back(): the stroke is dropped, not copied
        QCOMPARE(finished.count(), 0);
        QVERIFY(!rig.session.annotation()->gestureActive());
        QVERIFY(rig.session.hasSelection());
    }

    void aPressedButtonActsOnlyOnItsRelease()
    {
        Rig rig;
        rig.begin();
        CaptureSession::MouseActions actions;
        actions.middle = u"copy"_s;
        actions.back = u"save"_s;
        rig.session.setMouseActions(actions);
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {299, 299});
        // the phase changed between press and release (a tool turned on): void
        rig.session.pointerPress(0, {150, 150}, Qt::MiddleButton);
        rig.session.key(Qt::Key_R, Qt::NoModifier);
        rig.session.pointerRelease(0, {150, 150}, Qt::MiddleButton);
        QCOMPARE(finished.count(), 0);
        rig.session.key(Qt::Key_R, Qt::NoModifier); // tool off
        // moved before the release: void
        rig.session.pointerPress(0, {150, 150}, Qt::MiddleButton);
        rig.session.pointerMove(0, {190, 190});
        rig.session.pointerRelease(0, {190, 190}, Qt::MiddleButton);
        QCOMPARE(finished.count(), 0);
        // another button between: void; and a different button's release does nothing
        rig.session.pointerPress(0, {150, 150}, Qt::MiddleButton);
        rig.session.pointerRelease(0, {150, 150}, Qt::BackButton);
        QCOMPARE(finished.count(), 0);
        rig.session.pointerPress(0, {150, 150}, Qt::BackButton);
        rig.session.pointerRelease(0, {150, 150}, Qt::MiddleButton);
        QCOMPARE(finished.count(), 0);
        // a still release of the same button acts
        rig.session.pointerPress(0, {150, 150}, Qt::BackButton);
        rig.session.pointerRelease(0, {150, 150}, Qt::BackButton);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Save);
    }

    // ---- stage 7 -----------------------------------------------------------------------------

    void shiftRBringsBackTheLastSelectionOnItsMonitor()
    {
        CaptureScreen first {0};
        CaptureScreen second {1};
        first.setGeometry(QRect(0, 0, 1000, 800), 1.0, QSize(1000, 800));
        second.setGeometry(QRect(1000, 0, 1000, 800), 1.0, QSize(1000, 800));
        QImage image(1000, 800, QImage::Format_RGB32);
        image.fill(Qt::gray);
        first.setImage(image);
        second.setImage(image);
        CaptureSession session;
        QSignalSpy selections(&session, &CaptureSession::selectionChanged);
        // Nothing remembered: a word, no selection.
        session.begin({&first, &second}, {});
        session.pointerMove(0, {50, 50});
        QVERIFY(session.key(Qt::Key_R, Qt::ShiftModifier));
        QVERIFY(!session.hasSelection());
        QVERIFY(!first.flash().isEmpty());
        session.end();
        // The last capture was on the second monitor, the cursor is on the first.
        session.setLastSelection(QRect(1100, 100, 300, 200));
        session.begin({&first, &second}, {});
        session.pointerMove(0, {50, 50});
        selections.clear();
        QVERIFY(session.key(Qt::Key_R, Qt::ShiftModifier));
        QVERIFY(session.hasSelection());
        QCOMPARE(session.selectionScreen(), 1);
        QCOMPARE(session.selectionRect(), QRect(100, 100, 300, 200));
        QCOMPARE(second.selection(), QRect(100, 100, 300, 200));
        QVERIFY(first.selection().isNull());
        QVERIFY(selections.count() >= 1);
        // With a selection there is nothing to bring back: the key is not taken.
        QVERIFY(!session.key(Qt::Key_R, Qt::ShiftModifier) || session.selectionRect() == QRect(100, 100, 300, 200));
        session.end();
        // Without Shift, R is a tool's key, not this one's.
        session.begin({&first, &second}, {});
        QVERIFY(!session.key(Qt::Key_R, Qt::NoModifier));
        QVERIFY(!session.hasSelection());
    }

    void shiftChangesTheColourFormatAndCCopiesIt()
    {
        Rig rig;
        rig.begin();
        QSignalSpy colors(&rig.session, &CaptureSession::colorCopied);
        QSignalSpy formats(&rig.session, &CaptureSession::colorFormatChanged);
        // Shift alone, as in PixPin: pressed and let go of with nothing between.
        const auto tapShift = [&rig] {
            rig.session.key(Qt::Key_Shift, Qt::ShiftModifier);
            rig.session.keyReleased(Qt::Key_Shift);
        };
        rig.session.pointerMove(0, {10, 10});
        QCOMPARE(rig.screen.colorText(), u"#ABCDEF"_s);
        tapShift();
        QCOMPARE(rig.session.colorFormat(), ColorFormat::Rgb);
        QCOMPARE(rig.screen.colorText(), u"rgb(171, 205, 239)"_s);
        QCOMPARE(rig.screen.color(), u"#ABCDEF"_s); // the swatch keeps its hex
        QVERIFY(rig.screen.flash().contains(u"RGB"_s));
        QVERIFY(rig.session.key(Qt::Key_C, Qt::NoModifier));
        QCOMPARE(colors.count(), 1);
        QCOMPARE(colors[0][0].toString(), u"rgb(171, 205, 239)"_s);
        rig.begin(); // C ended it; the format stays
        rig.session.pointerMove(0, {10, 10});
        QCOMPARE(rig.screen.colorText(), u"rgb(171, 205, 239)"_s);
        tapShift();
        QCOMPARE(rig.screen.colorText(), u"hsl(210, 68%, 80%)"_s);
        tapShift();
        QCOMPARE(rig.screen.colorText(), u"#ABCDEF"_s);
        QCOMPARE(formats.count(), 3);
        // Shift with another key, a click or the wheel between: not alone.
        rig.session.key(Qt::Key_Shift, Qt::ShiftModifier);
        rig.session.key(Qt::Key_R, Qt::ShiftModifier);
        rig.session.keyReleased(Qt::Key_Shift);
        rig.session.key(Qt::Key_Shift, Qt::ShiftModifier);
        rig.session.pointerPress(0, {10, 10}, Qt::LeftButton, Qt::ShiftModifier);
        rig.session.pointerRelease(0, {10, 10}, Qt::LeftButton);
        rig.session.keyReleased(Qt::Key_Shift);
        rig.session.key(Qt::Key_Escape, Qt::NoModifier); // the click picked the screen: back to picking
        rig.session.key(Qt::Key_Shift, Qt::ShiftModifier);
        rig.session.wheelAngle(0, {10, 10}, 120, 0);
        rig.session.keyReleased(Qt::Key_Shift);
        rig.session.key(Qt::Key_Shift, Qt::ShiftModifier | Qt::ControlModifier); // Ctrl+Shift
        rig.session.keyReleased(Qt::Key_Shift);
        QCOMPARE(formats.count(), 3);
        // The format outlasts the capture (the app keeps it in the settings).
        rig.session.setColorFormat(ColorFormat::Hsl);
        rig.session.end();
        rig.begin();
        rig.session.pointerMove(0, {10, 10});
        QCOMPARE(rig.screen.colorText(), u"hsl(210, 68%, 80%)"_s);
        // Ctrl+C is still the picture's copy; Shift+C copies its words (PixPin's 识别文字并复制).
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        QVERIFY(rig.session.key(Qt::Key_C, Qt::ControlModifier));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Copy);
        rig.begin();
        rig.drag({100, 100}, {299, 199});
        QVERIFY(rig.session.key(Qt::Key_C, Qt::ShiftModifier));
        QCOMPARE(finished.count(), 2);
        QCOMPARE(finished[1][0].value<CaptureSession::Action>(), CaptureSession::Action::OcrCopy);
        QCOMPARE(finished[1][2].toRect(), QRect(100, 100, 200, 100));
        QCOMPARE(rig.session.colorFormat(), ColorFormat::Hsl); // and leaves the format alone
    }

    void qAndDAsk()
    {
        Rig rig;
        rig.begin();
        QSignalSpy finished(&rig.session, &CaptureSession::finished);
        rig.drag({100, 100}, {299, 299});
        QVERIFY(rig.session.key(Qt::Key_Q, Qt::NoModifier));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Qr);
        QCOMPARE(finished[0][1].toInt(), 0);
        QCOMPARE(finished[0][2].toRect(), QRect(100, 100, 200, 200));
        // Without a selection the window under the cursor is read, as for the other keys.
        rig.begin();
        finished.clear();
        rig.session.pointerMove(0, {400, 400});
        QVERIFY(rig.session.key(Qt::Key_Q, Qt::NoModifier));
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Qr);
        // D ends this capture for the delayed one: it needs no selection and takes no picture.
        rig.begin();
        finished.clear();
        QVERIFY(rig.session.key(Qt::Key_D, Qt::NoModifier));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Delay);
        QVERIFY(!rig.session.key(Qt::Key_D, Qt::ControlModifier)); // Ctrl+D is the annotation's copy
        rig.begin();
        finished.clear();
        rig.session.act(int(CaptureSession::Action::Delay));
        QCOMPARE(finished.count(), 1);
    }

    void controlsFoundLaterJoinTheHover()
    {
        Rig rig;
        rig.tops = {QRect(100, 100, 600, 500)};
        rig.children = {{}};
        QList<QPair<std::size_t, QPoint>> asked;
        CaptureSession::Windows windows;
        windows.tops = rig.tops;
        windows.children = [&rig](std::size_t i) { return rig.children[i]; };
        windows.probe = [&asked](std::size_t i, QPoint p) { asked.append({i, p}); };
        rig.session.begin({&rig.screen}, std::move(windows));
        rig.session.pointerMove(0, {250, 250});
        QCOMPARE(rig.screen.hover(), QRect(100, 100, 600, 500)); // the window: nothing finer known
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked[0].first, std::size_t(0)); // asked about the window the point is in, at that point
        QCOMPARE(asked[0].second, QPoint(250, 250));
        // The probe answers: a control; the hover is worked out again at the cursor.
        rig.children[0] = {QRect(100, 100, 200, 500), QRect(150, 200, 300, 120)};
        rig.session.refreshHover();
        QCOMPARE(rig.screen.hover(), QRect(150, 200, 300, 120)); // the smallest holding the point
        rig.session.pointerMove(0, {110, 400});
        QCOMPARE(rig.screen.hover(), QRect(100, 100, 200, 500));
        // Outside every window: no question.
        asked.clear();
        rig.session.pointerMove(0, {900, 700});
        QVERIFY(asked.isEmpty());
        // Once a selection is made, the hover is not asked about.
        rig.drag({300, 300}, {399, 399});
        asked.clear();
        rig.session.pointerMove(0, {250, 250});
        QVERIFY(asked.isEmpty());
        rig.session.refreshHover(); // and refreshing then changes nothing
        QCOMPARE(rig.session.selectionRect(), QRect(300, 300, 100, 100));
    }

    void hoverDimsEveryMonitorWithoutMakingASelection()
    {
        Rig rig;
        rig.tops = {QRect(100, 100, 600, 500)};
        CaptureScreen other(1);
        other.setGeometry(QRect(1000, 0, 1000, 800), 1.0, QSize(1000, 800));
        rig.session.begin({&rig.screen, &other}, {rig.tops, {}, {}});
        rig.session.pointerMove(0, {200, 200});
        QVERIFY(rig.screen.dimmed());
        QVERIFY(other.dimmed());
        QVERIFY(other.hover().isEmpty());
        QCOMPARE(rig.screen.hover(), QRect(100, 100, 600, 500));
        QVERIFY(!rig.session.hasSelection());
        QVERIFY(rig.screen.selection().isEmpty());
        rig.session.end();
        QVERIFY(!rig.screen.dimmed());
        QVERIFY(!other.dimmed());
    }

    void copyAndPinActions()
    {
        for (int way = 0; way < 4; ++way) {
            Rig rig;
            rig.session.setPinOnCopy(way != 3);
            rig.session.setCopyOnSelect(way == 1);
            CaptureSession::MouseActions actions;
            actions.middle = u"copypin"_s;
            rig.session.setMouseActions(actions);
            rig.begin();
            QSignalSpy finished(&rig.session, &CaptureSession::finished);
            rig.drag({100, 100}, {199, 199});
            if (way == 0)
                rig.session.key(Qt::Key_Return, Qt::NoModifier);
            else if (way == 2)
                rig.session.act(static_cast<int>(CaptureSession::Action::Copy));
            else if (way == 3) {
                rig.session.pointerPress(0, {150, 150}, Qt::MiddleButton);
                QCOMPARE(finished.count(), 0); // act on release, not on the window underneath
                rig.session.pointerRelease(0, {150, 150}, Qt::MiddleButton);
            }
            QCOMPARE(finished.count(), 1);
            QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::CopyPin);
            QCOMPARE(finished[0][2].toRect(), QRect(100, 100, 100, 100));
        }
    }

    void doubleLeftFollowsTheSetting()
    {
        for (const QString& name : {u"none"_s, u"save"_s}) {
            Rig rig;
            rig.begin();
            CaptureSession::MouseActions actions;
            actions.doubleLeft = name;
            rig.session.setMouseActions(actions);
            QSignalSpy finished(&rig.session, &CaptureSession::finished);
            rig.drag({100, 100}, {199, 199});
            rig.click({150, 150});
            rig.session.pointerPress(0, {150, 150}, Qt::LeftButton);
            rig.session.doubleClick(0, {150, 150});
            QCOMPARE(finished.count(), name == u"none" ? 0 : 1);
            if (name == u"save")
                QCOMPARE(finished[0][0].value<CaptureSession::Action>(), CaptureSession::Action::Save);
        }
    }

    // The diagnostics' bookkeeping (CaptureDiagnostics.h): a tick that comes
    // after more than 150 ms is a stall of the time over 50; five are recorded
    // one by one, the rest only counted; the longest is kept with where it was.
    void diagStallWatch()
    {
        diag::StallWatch watch;
        QVERIFY(!watch.note(50, 100, u"hover"_s));
        QVERIFY(!watch.note(150, 150, u"hover"_s)); // at the line: not yet
        const auto first = watch.note(375 + 50, 200, u"hover"_s);
        QVERIFY(first);
        QCOMPARE(first->ms, qint64(375));
        QVERIFY(first->record);
        for (int i = 1; i < diag::kMaxStallEvents; ++i)
            QVERIFY(watch.note(200, 300 + i, u"dragging"_s)->record);
        const auto sixth = watch.note(600, 900, u"selected"_s);
        QVERIFY(sixth);
        QVERIFY(!sixth->record);
        QCOMPARE(sixth->ms, qint64(550));
        QCOMPARE(watch.count(), 6);
        QCOMPARE(watch.longestMs(), qint64(550));
        QCOMPARE(watch.longestAtMs(), qint64(900));
        QCOMPARE(watch.longestPhase(), u"selected"_s);
        QVERIFY(watch.noteHidden());
        QVERIFY(!watch.noteHidden()); // one event per capture, every tick counted
        QCOMPARE(watch.summary()[u"hidden_ticks"_s].toInt(), 2);
        watch.reset();
        QCOMPARE(watch.count(), 0);
        QCOMPARE(watch.hiddenTicks(), 0);
    }

    void diagSmallParts()
    {
        QVERIFY(!diag::cursorHidden(CURSOR_SHOWING));
        QVERIFY(diag::cursorHidden(0));
        QVERIFY(diag::cursorHidden(CURSOR_SHOWING | CURSOR_SUPPRESSED));
        QCOMPARE(diag::fileNameOf(u"%SystemRoot%\\cursors\\cross_rl.cur"_s), u"cross_rl.cur"_s);
        QCOMPARE(diag::fileNameOf({}), QString());
        QVERIFY(diag::frameStats({}).isEmpty());
        std::vector<qint64> frames;
        for (int i = 1; i <= 100; ++i)
            frames.push_back(i * 1'000'000);
        const QJsonObject stats = diag::frameStats(frames);
        QCOMPARE(stats[u"count"_s].toInt(), 100);
        QCOMPARE(stats[u"max_ms"_s].toDouble(), 100.0);
        QCOMPARE(stats[u"p95_ms"_s].toDouble(), 95.0);
    }

    // A monochrome cursor (no colour bitmap; the mask is twice as tall) and a
    // colour one read back as such, with their hotspots.
    void diagCursorKinds()
    {
        const auto make = [](bool colour) {
            const int bytes = 32 * 32 / 8;
            std::vector<BYTE> bits(static_cast<std::size_t>(bytes) * 2, 0xFF);
            ICONINFO info {};
            info.fIcon = FALSE;
            info.xHotspot = 7;
            info.yHotspot = 9;
            info.hbmMask = ::CreateBitmap(32, colour ? 32 : 64, 1, 1, bits.data());
            info.hbmColor = colour ? ::CreateBitmap(32, 32, 1, 32, nullptr) : nullptr;
            const HICON cursor = ::CreateIconIndirect(&info);
            ::DeleteObject(info.hbmMask);
            if (info.hbmColor)
                ::DeleteObject(info.hbmColor);
            return cursor;
        };
        const HICON mono = make(false);
        const HICON colour = make(true);
        QVERIFY(mono && colour);
        QCOMPARE(diag::describeBitmaps(mono), u"monochrome 32x32"_s);
        QCOMPARE(diag::describeBitmaps(colour), u"colour 32bpp 32x32"_s);
        const QJsonObject facts = diag::cursorFacts(colour, CURSOR_SHOWING);
        QCOMPARE(facts[u"kind"_s].toString(), u"colour 32bpp"_s);
        QCOMPARE(facts[u"size"_s].toString(), u"32x32"_s);
        QCOMPARE(facts[u"hotspot_x"_s].toInt(), 7);
        QCOMPARE(facts[u"hotspot_y"_s].toInt(), 9);
        QCOMPARE(facts[u"flags"_s].toInt(), 1);
        QCOMPARE(diag::cursorFacts(nullptr, 0)[u"kind"_s].toString(), u"none"_s);
        ::DestroyIcon(mono);
        ::DestroyIcon(colour);

        // The system's own pointers and the settings are read without a fuss.
        const QJsonObject pointer = diag::pointerFacts();
        QVERIFY(pointer[u"cursor_base_size"_s].toInt() >= 16);
        QVERIFY(!pointer[u"system_cross"_s].toString().isEmpty());
        QVERIFY(!pointer[u"crosshair_file"_s].toString().contains(u'\\'));
        QVERIFY(diag::secondsSinceProcessStart() >= 0);
    }

    void diagSessionPhase()
    {
        Rig rig;
        rig.begin();
        QCOMPARE(rig.session.phaseName(), u"hover"_s);
        QCOMPARE(rig.session.msSinceRelease(), qint64(-1));
        rig.session.pointerPress(0, {100, 100}, Qt::LeftButton);
        rig.session.pointerMove(0, {199, 199});
        QCOMPARE(rig.session.phaseName(), u"dragging"_s);
        QCOMPARE(rig.session.msSinceRelease(), qint64(-1));
        rig.session.pointerRelease(0, {199, 199}, Qt::LeftButton);
        QCOMPARE(rig.session.phaseName(), u"selected"_s);
        QVERIFY(rig.session.msSinceRelease() >= 0);
        rig.begin();
        QCOMPARE(rig.session.msSinceRelease(), qint64(-1));
    }
};

QTEST_GUILESS_MAIN(TestCapture)
#include "tst_capture.moc"
