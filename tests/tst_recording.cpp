#include "Recording.h"
#include "CursorArt.h"
#include "CursorTrail.h"
#include "GifExport.h"
#include "KeyStream.h"
#include "RecordingPointer.h"
#include "RecordingWorker.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

using namespace Qt::StringLiterals;
using namespace ws::capture::recording;
using Microsoft::WRL::ComPtr;

namespace {

// Every frame of an MP4 as 32-bit BGRX, top row first, with its time. On
// the caller's thread, which has COM.
HRESULT forEachFrame(const QString& path, const std::function<void(qint64 ms, const unsigned char* top, LONG pitch)>& frame)
{
    HRESULT hr = ::MFStartup(MF_VERSION);
    if (FAILED(hr)) return hr;
    const auto shutdown = qScopeGuard([] { ::MFShutdown(); });
    ComPtr<IMFAttributes> attributes;
    ::MFCreateAttributes(&attributes, 1);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    hr = ::MFCreateSourceReaderFromURL(reinterpret_cast<const wchar_t*>(path.utf16()), attributes.Get(), &reader);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaType> type;
    ::MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
    if (FAILED(hr)) return hr;
    type.Reset();
    reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
    UINT32 width = 0, height = 0;
    ::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    const LONG stride = LONG(::MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, width * 4));
    for (int i = 0; i < 1000; ++i) {
        DWORD flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &time, &sample);
        if (FAILED(hr)) return hr;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        sample->GetBufferByIndex(0, &buffer);
        ComPtr<IMF2DBuffer> twoD;
        BYTE* top = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buffer.As(&twoD))) {
            hr = twoD->Lock2D(&top, &pitch);
            if (FAILED(hr)) return hr;
            frame(time / 10000, top, pitch);
            twoD->Unlock2D();
        } else {
            DWORD length = 0;
            hr = buffer->Lock(&top, nullptr, &length);
            if (FAILED(hr)) return hr;
            frame(time / 10000, stride < 0 ? top + qint64(height - 1) * -stride : top, stride);
            buffer->Unlock();
        }
    }
    return S_OK;
}

// The last frame of a sample()'s MP4 (`size` × `scale`), as a player shows
// it at the region's size: a doubled video halved again, each pixel the
// mean of its 2×2 block. Null where it does not decode.
QImage lastFrame(const QString& path, QSize size, int scale)
{
    QImage frame(size, QImage::Format_RGB32);
    HRESULT decoded = E_FAIL;
    std::jthread reader([&] {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com)) return;
        const auto uninit = qScopeGuard([] { ::CoUninitialize(); });
        decoded = forEachFrame(path, [&](qint64, const unsigned char* top, LONG pitch) {
            for (int y = 0; y < size.height(); ++y) {
                auto* out = reinterpret_cast<QRgb*>(frame.scanLine(y));
                for (int x = 0; x < size.width(); ++x) {
                    int sum[3] {};
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx)
                            for (int c = 0; c < 3; ++c)
                                sum[c] += top[qint64(y * scale + dy) * pitch + (x * scale + dx) * 4 + c];
                    const int n = scale * scale;
                    out[x] = qRgb((sum[2] + n / 2) / n, (sum[1] + n / 2) / n, (sum[0] + n / 2) / n);
                }
            }
        });
    });
    reader.join();
    return SUCCEEDED(decoded) ? frame : QImage();
}

// The brightness (Y) plane of an MP4's last frame, `size` rows of its
// width, as the decoder gives it: nothing converted. Empty where it does
// not decode.
QByteArray lastLuma(const QString& path, QSize size)
{
    QByteArray luma;
    std::jthread reading([&] {
        if (FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        const auto uninit = qScopeGuard([] { ::CoUninitialize(); });
        if (FAILED(::MFStartup(MF_VERSION))) return;
        const auto shutdown = qScopeGuard([] { ::MFShutdown(); });
        ComPtr<IMFSourceReader> reader;
        ComPtr<IMFMediaType> type;
        if (FAILED(::MFCreateSourceReaderFromURL(reinterpret_cast<const wchar_t*>(path.utf16()), nullptr, &reader))
            || FAILED(::MFCreateMediaType(&type)))
            return;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get()))) return;
        for (int i = 0; i < 1000; ++i) {
            DWORD flags = 0;
            ComPtr<IMFSample> sample;
            if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, nullptr, &sample))
                || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
                return;
            if (!sample) continue;
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMF2DBuffer> twoD;
            BYTE* top = nullptr;
            LONG pitch = 0;
            if (FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&twoD)) || FAILED(twoD->Lock2D(&top, &pitch))) return;
            luma.resize(qsizetype(size.width()) * size.height());
            for (int y = 0; y < size.height(); ++y)
                std::memcpy(luma.data() + qsizetype(y) * size.width(), top + qint64(y) * pitch, size_t(size.width()));
            twoD->Unlock2D();
        }
    });
    reading.join();
    return luma;
}

// Rows of sample()'s picture away from its moving block, and from the
// encoder's blocks around it.
bool still(int y, int height)
{
    return y < height / 2 - 4 || y >= height / 2 + 40;
}

double decibels(double squares, qint64 count)
{
    return 10 * std::log10(255.0 * 255.0 * double(count) / std::max(squares, 1e-9));
}

// How close an MP4's last frame is to sample()'s picture in brightness, in
// dB: what the encoder's quality decides.
double brightnessCloseness(const QString& path, const QImage& backdrop)
{
    const QSize size = backdrop.size();
    const QByteArray decoded = lastLuma(path, size);
    if (decoded.isEmpty()) return 0;
    // The picture's own Y, as the recording makes it (studio-range BT.709).
    const QImage bgra = backdrop.convertToFormat(QImage::Format_RGB32);
    QByteArray expected(qsizetype(size.width()) * size.height(), 0), chroma(expected.size() / 2, 0);
    bgraToNv12(bgra.constBits(), int(bgra.bytesPerLine()), size.width(), size.height(),
        reinterpret_cast<unsigned char*>(expected.data()), size.width(), reinterpret_cast<unsigned char*>(chroma.data()), size.width());
    double squares = 0;
    qint64 count = 0;
    for (int y = 0; y < size.height(); ++y) {
        if (!still(y, size.height())) continue;
        for (int x = 0; x < size.width(); ++x, ++count) {
            const qsizetype at = qsizetype(y) * size.width() + x;
            squares += std::pow(double(uchar(decoded[at])) - uchar(expected[at]), 2);
        }
    }
    return decibels(squares, count);
}

// How close a picture is to sample()'s in colour, in dB: what 4:2:0 video
// takes from small coloured text.
double colourCloseness(const QImage& frame, const QImage& backdrop)
{
    double squares = 0;
    qint64 count = 0;
    for (int y = 0; y < backdrop.height(); ++y) {
        if (!still(y, backdrop.height())) continue;
        const auto* a = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
        const auto* b = reinterpret_cast<const QRgb*>(backdrop.constScanLine(y));
        for (int x = 0; x < backdrop.width(); ++x, ++count)
            squares += (std::pow(qRed(a[x]) - qRed(b[x]), 2) + std::pow(qGreen(a[x]) - qGreen(b[x]), 2)
                + std::pow(qBlue(a[x]) - qBlue(b[x]), 2)) / 3;
    }
    return decibels(squares, count);
}

} // namespace

class TestRecording : public QObject {
    Q_OBJECT
private slots:
    void isolatedEncoding()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        for (const bool software : {false, true}) {
            Options options;
            options.path = temporary.filePath(software ? u"software.mp4"_s : u"hardware.mp4"_s);
            options.region = QRect(0, 0, 641, 361);
            options.software = software;
            options.systemAudio = true;
            Control control;
            const WorkerProgram program {QCoreApplication::applicationFilePath(), {}};
            const auto result = recordIsolated(program, options, control, {}, 600);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QVERIFY(result.frames >= 10);
            QCOMPARE(result.path, options.path);
            HRESULT decoded = E_FAIL;
            int frames = 0;
            std::jthread reader([&] {
                const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (FAILED(com)) return;
                const auto uninit = qScopeGuard([] { ::CoUninitialize(); });
                decoded = forEachFrame(result.path, [&](qint64, const unsigned char*, LONG) { ++frames; });
            });
            reader.join();
            QVERIFY(SUCCEEDED(decoded));
            QVERIFY(frames >= 10);
            control.stop = true; // the production Stop and save path leaves this set
            const auto gif = exportGifIsolated(program, result.path,
                {temporary.filePath(software ? u"software.gif"_s : u"hardware.gif"_s), 15, 1280}, control, {});
            QVERIFY2(gif.error.isEmpty(), qPrintable(gif.error));
            QVERIFY(QFileInfo::exists(result.path));
            QImageReader image(gif.path);
            QVERIFY(image.canRead());
            QVERIFY(image.imageCount() >= 5);
            QCOMPARE(image.size(), QSize(642, 362));
        }
    }

    void isolatedHardwareFailureFallsBack()
    {
        QTemporaryDir temporary;
        Options options;
        options.path = temporary.filePath(u"fallback.mp4"_s);
        options.region = QRect(0, 0, 320, 180);
        Control control;
        const WorkerProgram program {QCoreApplication::applicationFilePath(), {u"--test-fail-hardware"_s}};
        const auto result = recordIsolated(program, options, control, {}, 400);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.path, options.path);
        QVERIFY(result.frames > 0);
        QVERIFY(!result.hardware);
        QCOMPARE(QDir(temporary.path()).entryList({u"*.partial.mp4"_s}, QDir::Files).size(), 0);
    }

    void isolatedStallIsBoundedAndCancellable()
    {
        QTemporaryDir temporary;
        Options options;
        options.path = temporary.filePath(u"stalled.mp4"_s);
        options.region = QRect(0, 0, 320, 180);
        options.software = true;
        const WorkerProgram program {QCoreApplication::applicationFilePath(), {u"--test-stall-worker"_s}, 200};
        Control control;
        QElapsedTimer elapsed;
        elapsed.start();
        const auto failure = recordIsolated(program, options, control, {}, 400);
        QVERIFY(!failure.error.isEmpty());
        QVERIFY(elapsed.elapsed() < 3000);
        QVERIFY(!QFileInfo::exists(options.path));
        std::jthread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); control.stop = true; });
        const auto cancelled = recordIsolated(program, options, control, {}, 400);
        QVERIFY(cancelled.error.isEmpty());
        QVERIFY(cancelled.path.isEmpty());
    }

    // The screen itself, read each way: Windows.Graphics.Capture, and GDI,
    // which Windows 10 1809 (LTSC 2019) gets because the first cannot make a
    // capture item for a display there (asking threw E_NOINTERFACE). A solid
    // window under the region must come out in its colour; it is layered, as
    // menus and translucent windows are, which GDI reads without CAPTUREBLT.
    void screenFrames_data()
    {
        QTest::addColumn<bool>("gdi");
        QTest::newRow("windows-graphics-capture") << false;
        QTest::newRow("gdi") << true;
    }

    void screenFrames()
    {
        QFETCH(bool, gdi);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Options options;
        options.path = temporary.filePath(u"screen.mp4"_s);
        options.software = true;
        options.cursor = false; // the pointer must not cover the sampled colour
        options.screenByGdi = gdi;
        Control control;
        Result result;
        HRESULT decoded = E_FAIL;
        int frames = 0;
        std::array<int, 3> bgr {};
        std::jthread worker([&] {
            // Physical pixels, as the recording's region and frames are.
            ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            MONITORINFO info {};
            info.cbSize = sizeof info;
            if (!::GetMonitorInfoW(::MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info)) return;
            QRect block(info.rcWork.left + 100, info.rcWork.top + 100, 320, 180);
            POINT pointer {};
            if (::GetPhysicalCursorPos(&pointer) && block.adjusted(-40, -40, 40, 40).contains(pointer.x, pointer.y))
                block.translate(420, 0);
            const HBRUSH brush = ::CreateSolidBrush(RGB(40, 110, 200));
            WNDCLASSW type {};
            type.lpfnWndProc = ::DefWindowProcW;
            type.hInstance = ::GetModuleHandleW(nullptr);
            type.hbrBackground = brush;
            type.lpszClassName = L"WinShunRecordingTestBlock";
            ::RegisterClassW(&type);
            const HWND window = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, type.lpszClassName, L"",
                WS_POPUP, block.x(), block.y(), block.width(), block.height(), nullptr, nullptr, type.hInstance, nullptr);
            const auto cleanup = qScopeGuard([&] {
                if (window) ::DestroyWindow(window);
                ::UnregisterClassW(type.lpszClassName, type.hInstance);
                ::DeleteObject(brush);
            });
            if (!window) return;
            ::SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
            ::ShowWindow(window, SW_SHOWNOACTIVATE);
            ::UpdateWindow(window);
            for (int i = 0; i < 20; ++i) { // until DWM has shown it
                MSG message;
                while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) ::DispatchMessageW(&message);
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            options.region = block;
            std::jthread stop([&] { std::this_thread::sleep_for(std::chrono::milliseconds(800)); control.stop = true; });
            result = record(options, control, {});
            if (!result.error.isEmpty() || result.path.isEmpty()) return;
            const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(com)) { decoded = com; return; }
            const auto uninit = qScopeGuard([] { ::CoUninitialize(); });
            decoded = forEachFrame(result.path, [&](qint64, const unsigned char* top, LONG pitch) {
                ++frames;
                const unsigned char* centre = top + qint64(block.height() / 2) * pitch + (block.width() / 2) * 4;
                bgr = {centre[0], centre[1], centre[2]};
            });
        });
        worker.join();
        QVERIFY2(!options.region.isEmpty(), "no test window on the primary display");
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.path, options.path);
        QVERIFY(result.screen.startsWith(gdi ? u"GDI"_s : u"Windows.Graphics.Capture"_s));
        QVERIFY(SUCCEEDED(decoded));
        QVERIFY2(frames >= 5, qPrintable(QString::number(frames)));
        // BT.709 studio range there and back: a few levels either way.
        QVERIFY2(std::abs(bgr[0] - 200) <= 12 && std::abs(bgr[1] - 110) <= 12 && std::abs(bgr[2] - 40) <= 12,
            qPrintable(u"centre BGR %1 %2 %3"_s.arg(bgr[0]).arg(bgr[1]).arg(bgr[2])));
    }

    void pauseClock()
    {
        Timeline clock(100);
        QCOMPARE(clock.elapsed(90), 0);
        QCOMPARE(clock.elapsed(150), 50);
        clock.pause(160);
        clock.pause(170);
        QCOMPARE(clock.elapsed(1000), 60);
        clock.resume(460);
        clock.resume(500);
        QCOMPARE(clock.elapsed(510), 110);
        clock.pause(520);
        QCOMPARE(clock.elapsed(2000), 120);
        clock.resume(1520);
        QCOMPARE(clock.elapsed(1620), 220);
    }

    // GitHub #214: the CPU's NV12, for a driver without D3D11 video.
    void cpuNv12()
    {
        const auto convert = [](std::array<std::array<unsigned char, 3>, 4> rgb) {
            std::array<unsigned char, 16> bgra {};
            for (int i = 0; i < 4; ++i) {
                const int at = (i % 2) * 4 + (i / 2) * 8; // a 2×2 block, pitch 8
                bgra[at] = rgb[i][2];
                bgra[at + 1] = rgb[i][1];
                bgra[at + 2] = rgb[i][0];
                bgra[at + 3] = 255;
            }
            std::array<unsigned char, 6> out {};
            bgraToNv12(bgra.data(), 8, 2, 2, out.data(), 2, out.data() + 4, 2);
            return out;
        };
        const auto solid = [&](unsigned char r, unsigned char g, unsigned char b) {
            return convert({{{r, g, b}, {r, g, b}, {r, g, b}, {r, g, b}}});
        };
        using Nv12 = std::array<unsigned char, 6>;
        QCOMPARE(solid(255, 255, 255), (Nv12 {235, 235, 235, 235, 128, 128}));
        QCOMPARE(solid(0, 0, 0), (Nv12 {16, 16, 16, 16, 128, 128}));
        QCOMPARE(solid(128, 128, 128), (Nv12 {126, 126, 126, 126, 128, 128}));
        QCOMPARE(solid(255, 0, 0), (Nv12 {63, 63, 63, 63, 102, 240})); // BT.709 red
        const auto mixed = convert({{{255, 255, 255}, {0, 0, 0}, {0, 0, 0}, {255, 255, 255}}});
        QCOMPARE(mixed, (Nv12 {235, 16, 16, 235, 128, 128}));
    }

    // Twice the region's size where H.264 (level 5.2) takes it.
    void sharpScaleLimits()
    {
        QCOMPARE(sharpScale(QSize(1920, 1080), 30), 2);
        QCOMPARE(sharpScale(QSize(1920, 1080), 60), 2); // 3840 × 2160 at 60
        QCOMPARE(sharpScale(QSize(1080, 1920), 30), 2);
        QCOMPARE(sharpScale(QSize(2048, 1152), 30), 2); // 4096 × 2304: the largest frame
        QCOMPARE(sharpScale(QSize(2048, 1152), 60), 1); // too many blocks a second
        QCOMPARE(sharpScale(QSize(2050, 400), 30), 1);
        QCOMPARE(sharpScale(QSize(2560, 1440), 30), 1);
        QCOMPARE(sharpScale(QSize(641, 361), 60), 2);
        // Asked for where it cannot be: refused before anything is opened.
        QTemporaryDir temporary;
        Options options;
        options.path = temporary.filePath(u"large.mp4"_s);
        options.region = QRect(0, 0, 2560, 1440);
        options.scale = 2;
        QVERIFY(!sample(options, 100).error.isEmpty());
        QVERIFY(!QFileInfo::exists(options.path));
    }

    // Twice the size is asked for, and the way to encode cannot make it: the
    // region's own size then, never a failed recording.
    void doubledWhereItCan_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("software");
        QTest::addColumn<bool>("noVideoDevice");
        QTest::addColumn<int>("scale");
        QTest::newRow("gpu") << QSize(640, 360) << false << false << 0; // 2, or what refused it is said
        QTest::newRow("software") << QSize(640, 360) << true << false << 2;
        QTest::newRow("software-large-region") << QSize(2000, 1100) << true << false << 1; // too slow doubled
        QTest::newRow("no-video-device") << QSize(640, 360) << false << true << 1; // the CPU makes each frame (#214)
    }

    void doubledWhereItCan()
    {
        QFETCH(QSize, size);
        QFETCH(bool, software);
        QFETCH(bool, noVideoDevice);
        QFETCH(int, scale);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Options options;
        options.path = temporary.filePath(u"doubled.mp4"_s);
        options.region = QRect(QPoint(), size);
        options.software = software;
        options.noVideoDevice = noVideoDevice;
        options.scale = 2;
        Result result;
        std::jthread worker([&] { result = sample(options, 300); });
        worker.join();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        if (scale)
            QCOMPARE(result.scale, scale);
        else
            QVERIFY2(result.scale == 2 || !result.refused.isEmpty(), "a GPU that refuses the doubled size must say why");
        QVERIFY2(!lastFrame(result.path, size, result.scale).isNull(), qPrintable(result.refused));
    }

    // What the panel says after a recording the computer did not keep up with.
    void fallenShort()
    {
        QCOMPARE(fallenShortFps(600, 10'000, 60), 0);
        QCOMPARE(fallenShortFps(545, 10'000, 60), 0); // nine tenths and more: kept up
        QCOMPARE(fallenShortFps(410, 10'000, 60), 41);
        QCOMPARE(fallenShortFps(410, 10'000, 30), 0);
        QCOMPARE(fallenShortFps(20, 1'000, 60), 0); // too short to tell
        QCOMPARE(fallenShortFps(2, 10'000, 15), 1);
    }

    // An encoder that takes no quality gets a bitrate: more for each
    // quality, more for a higher rate, within what a level allows.
    void bitrateByQuality()
    {
        const QSize size(1920, 1080);
        int previous = 0;
        for (const int quality : kQualities) {
            const int rate = videoBitrate(size, 30, quality);
            QVERIFY2(rate >= previous * 3 / 2, qPrintable(u"%1 at %2"_s.arg(rate).arg(quality)));
            previous = rate;
        }
        QVERIFY(videoBitrate(size, 60, 80) > videoBitrate(size, 30, 80));
        QVERIFY(videoBitrate(size, 60, 80) < videoBitrate(size, 30, 80) * 2); // frames closer together differ less
        QVERIFY(videoBitrate(size, 15, 80) < videoBitrate(size, 30, 80));
        QCOMPARE(videoBitrate(QSize(3840, 2160), 60, 100), 60'000'000);
        QCOMPARE(videoBitrate(QSize(16, 16), 15, 0), 500'000);

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        qint64 bytes[2] {};
        for (int i = 0; i < 2; ++i) {
            Options options;
            options.path = temporary.filePath(u"bitrate-%1.mp4"_s.arg(i));
            options.region = QRect(0, 0, 640, 360);
            options.software = true;
            options.bitrateOnly = true;
            options.quality = i ? kQualities.back() : kQualities.front();
            Result result;
            std::jthread worker([&] { result = sample(options, 600); });
            worker.join();
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QVERIFY2(result.rate.startsWith(u"bitrate "), qPrintable(result.rate));
            QVERIFY(!lastFrame(result.path, options.region.size(), 1).isNull());
            bytes[i] = result.bytes;
        }
        QVERIFY2(bytes[1] > bytes[0], qPrintable(u"%1, %2"_s.arg(bytes[0]).arg(bytes[1])));
    }

    // What the panel's 画质 choices come to, on small dark, coloured and
    // light-on-dark text: each one closer to the screen than the one before,
    // and 超清's doubled size far closer in colour than any quality alone.
    void clarityByQuality_data()
    {
        QTest::addColumn<bool>("software");
        QTest::newRow("hardware") << false;
        QTest::newRow("software") << true;
    }

    void clarityByQuality()
    {
        QFETCH(bool, software);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QSize size(640, 360);
        const QImage backdrop = sampleBackdrop(size);
        QString seen;
        struct Closeness {
            double brightness = 0;
            double colour = 0;
        };
        const auto record = [&](int quality, int scale, Closeness& close, qint64& bytes) {
            Options options;
            options.path = temporary.filePath(u"q%1-x%2.mp4"_s.arg(quality).arg(scale));
            options.region = QRect(QPoint(), size);
            options.software = software;
            options.quality = quality;
            options.scale = scale;
            Result result;
            std::jthread worker([&] { result = sample(options, 600); });
            worker.join();
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QCOMPARE(result.scale, scale);
            QCOMPARE(result.rate, u"quality %1"_s.arg(quality));
            const QImage frame = lastFrame(result.path, size, scale);
            QVERIFY(!frame.isNull());
            close = {scale == 1 ? brightnessCloseness(result.path, backdrop) : 0, colourCloseness(frame, backdrop)};
            bytes = result.bytes;
            seen += u"q%1 x%2: %3 dB, colour %4 dB, %5 bytes; "_s.arg(quality).arg(scale)
                .arg(close.brightness, 0, 'f', 1).arg(close.colour, 0, 'f', 1).arg(bytes);
        };
        Closeness previous, plain;
        qint64 previousBytes = 0;
        for (const int quality : kQualities) {
            qint64 bytes = 0;
            record(quality, 1, plain, bytes);
            if (QTest::currentTestFailed()) return;
            QVERIFY2(plain.brightness >= previous.brightness + 0.8, qPrintable(seen));
            QVERIFY2(bytes > previousBytes, qPrintable(seen));
            previous = plain;
            previousBytes = bytes;
        }
        QVERIFY2(plain.brightness >= 45, qPrintable(seen)); // 超清: no visible loss in brightness
        Closeness doubled;
        qint64 bytes = 0;
        record(kQualities.back(), 2, doubled, bytes);
        if (QTest::currentTestFailed()) return;
        QVERIFY2(doubled.colour >= plain.colour + 8 && doubled.colour >= 38, qPrintable(seen));
        QVERIFY2(bytes < previousBytes * 4, qPrintable(seen)); // four times the pixels, far less than four times the file
        qInfo().noquote() << seen;
    }

    void neverOverwrite()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(u"keep.mp4"_s);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("existing user file");
        file.close();
        Options options;
        options.path = path;
        options.region = QRect(0, 0, 640, 360);
        QVERIFY(!sample(options, 100).error.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("existing user file"));
    }

    void clickQueue()
    {
        ClickStream input(QRect(-200, -100, 80, 60));
        input.push(QPoint(-180, -80), false, 100);
        input.push(QPoint(-170, -70), true, 110);
        input.push(QPoint(-120, -70), false, 120); // just outside the right edge
        auto batch = input.take();
        QCOMPARE(batch.count, size_t(2));
        QCOMPARE(batch.clicks[0].point, QPoint(20, 20));
        QVERIFY(!batch.clicks[0].secondary);
        QVERIFY(batch.clicks[1].secondary);
        QCOMPARE(input.take().count, size_t(0));
        input.push(QPoint(-180, -80), false, 150);
        input.discardBefore(200); // pause/resume discards queued and delayed input
        input.push(QPoint(-180, -80), false, 199);
        QCOMPARE(input.take().count, size_t(0));
        for (int i = 0; i < 30; ++i) input.push(QPoint(-180, -80), false, 300 + i);
        batch = input.take();
        QCOMPARE(batch.count, ClickStream::capacity);
        QCOMPARE(batch.clicks[0].time, 314);
        QCOMPARE(batch.clicks[batch.count - 1].time, 329);
    }

    void clickLifetime()
    {
        ClickPulses pulses;
        for (int i = 0; i < 12; ++i) pulses.add({QPoint(i, i), 100 + i, false});
        QCOMPARE(pulses.active(120).size(), size_t(8));
        QCOMPARE(pulses.active(120).front().point, QPoint(4, 4));
        QCOMPARE(pulses.active(ClickPulses::duration + 110).size(), size_t(1));
        QVERIFY(pulses.active(ClickPulses::duration + 111).empty());
        pulses.add({QPoint(), 200, true});
        pulses.clear();
        QVERIFY(pulses.active(201).empty());
    }

    void cancelledBeforeStart()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Options options;
        options.path = temporary.filePath(u"cancelled.mp4"_s);
        options.region = QRect(0, 0, 640, 360);
        Control control;
        control.stop = true;
        const auto result = record(options, control, {});
        QVERIFY(result.error.isEmpty());
        QVERIFY(result.path.isEmpty());
        QCOMPARE(result.frames, quint64(0));
        QVERIFY(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }

    void discardDuringRecording()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Options options;
        options.path = temporary.filePath(u"discarded.mp4"_s);
        options.region = QRect(0, 0, 640, 360);
        Control control;
        Result result;
        std::jthread worker([&] {
            result = sample(options, 1200, control, [&](const Progress& progress) {
                if (progress.frames >= 2) {
                    control.discard = true;
                    control.stop = true;
                }
            });
        });
        worker.join();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.path.isEmpty());
        QVERIFY(!QFileInfo::exists(options.path));
        QVERIFY(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }

    void animatedGif()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Options options;
        options.path = temporary.filePath(u"source.mp4"_s);
        options.region = QRect(0, 0, 640, 360);
        Result source, result, cancelled, duplicate;
        const GifOptions gif {temporary.filePath(u"animated.gif"_s), 15, 320};
        HRESULT decoded = E_FAIL;
        UINT count = 0, width = 0, height = 0;
        int transparentFrames = 0, subFrames = 0;
        qint64 totalDelay = 0;
        QByteArray firstPixels;
        QByteArray loopData;
        std::jthread worker([&] {
            source = sample(options, 600);
            if (!source.error.isEmpty()) return;
            Control control;
            int previous = -1;
            result = exportGif(source.path, gif, control, [&](const Progress& value) {
                if (value.percent >= previous) previous = value.percent;
                else control.discard = true; // progress may never run backwards
            });
            if (!result.error.isEmpty() || result.path.isEmpty()) return;
            duplicate = exportGif(source.path, gif, control);
            Control cancel;
            cancelled = exportGif(source.path, {temporary.filePath(u"cancelled.gif"_s), 10, 320}, cancel,
                [&](const Progress&) { cancel.discard = true; });
            decoded = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(decoded)) return;
            const auto uninit = qScopeGuard([] { ::CoUninitialize(); });
            ComPtr<IWICImagingFactory> factory;
            decoded = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            if (FAILED(decoded)) return;
            ComPtr<IWICBitmapDecoder> decoder;
            decoded = factory->CreateDecoderFromFilename(reinterpret_cast<const wchar_t*>(gif.path.utf16()), nullptr,
                GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
            if (FAILED(decoded)) return;
            decoder->GetFrameCount(&count);
            ComPtr<IWICMetadataQueryReader> global;
            decoder->GetMetadataQueryReader(&global);
            PROPVARIANT value {};
            if (global && SUCCEEDED(global->GetMetadataByName(L"/appext/Data", &value))) {
                if (value.vt == (VT_VECTOR | VT_UI1)) loopData = QByteArray(reinterpret_cast<const char*>(value.caub.pElems), value.caub.cElems);
                ::PropVariantClear(&value);
            }
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IWICBitmapFrameDecode> frame;
                decoded = decoder->GetFrame(i, &frame);
                if (FAILED(decoded)) return;
                UINT frameWidth = 0, frameHeight = 0;
                frame->GetSize(&frameWidth, &frameHeight);
                if (i == 0) { width = frameWidth; height = frameHeight; }
                if (!frameWidth || !frameHeight || frameWidth > width || frameHeight > height) { decoded = E_UNEXPECTED; return; }
                if (i > 0 && (frameWidth < width || frameHeight < height)) ++subFrames;
                ComPtr<IWICMetadataQueryReader> metadata;
                frame->GetMetadataQueryReader(&metadata);
                if (metadata && SUCCEEDED(metadata->GetMetadataByName(L"/grctlext/Delay", &value))) {
                    if (value.vt == VT_UI2) totalDelay += value.uiVal * 10;
                    ::PropVariantClear(&value);
                }
                if (i > 0 && metadata && SUCCEEDED(metadata->GetMetadataByName(L"/grctlext/TransparencyFlag", &value))) {
                    if (value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE) ++transparentFrames;
                    ::PropVariantClear(&value);
                }
                if (i == 0) {
                    ComPtr<IWICFormatConverter> converter;
                    factory->CreateFormatConverter(&converter);
                    decoded = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
                    if (FAILED(decoded)) return;
                    firstPixels.resize(width * height * 4);
                    decoded = converter->CopyPixels(nullptr, width * 4, UINT(firstPixels.size()), reinterpret_cast<BYTE*>(firstPixels.data()));
                    if (FAILED(decoded)) return;
                }
            }
        });
        worker.join();
        QVERIFY2(source.error.isEmpty(), qPrintable(source.error));
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.path, gif.path);
        QVERIFY2(SUCCEEDED(decoded), qPrintable(u"GIF decode HRESULT %1"_s.arg(quint32(decoded), 8, 16, QChar(u'0'))));
        QCOMPARE(QSize(int(width), int(height)), QSize(320, 180));
        QVERIFY(count >= 8 && count <= 10);
        QCOMPARE(result.frames, quint64(count));
        QVERIFY2(transparentFrames > 0 || subFrames > 0, "GIF delta frames should encode unchanged regions compactly");
        QImageReader animation(gif.path);
        QCOMPARE(animation.loopCount(), -1);
        for (UINT i = 0; i < count; ++i) {
            const QImage frame = animation.read();
            QVERIFY2(!frame.isNull(), qPrintable(animation.errorString()));
            QCOMPARE(frame.size(), QSize(320, 180)); // Qt composites GIF update rectangles into full pictures
            const QRgb corner = frame.pixel(3, 180 / 4);
            QVERIFY2(qRed(corner) > 210 && qGreen(corner) > 210 && qBlue(corner) > 210,
                qPrintable(u"frame %1 static pixel drifted: %2,%3,%4"_s.arg(i).arg(qRed(corner)).arg(qGreen(corner)).arg(qBlue(corner))));
        }
        QVERIFY2(totalDelay >= 560 && totalDelay <= 730, qPrintable(QString::number(totalDelay)));
        QCOMPARE(loopData, QByteArray::fromHex("03010000")); // WIC omits the sub-block terminator when reading
        QVERIFY(firstPixels.size() == 320 * 180 * 4);
        const auto* rgb = reinterpret_cast<const unsigned char*>(firstPixels.constData()) + (180 / 4 * 320 + 3) * 4;
        QVERIFY2(rgb[0] > 215 && rgb[1] > 215 && rgb[2] > 215, "GIF must contain the decoded top-down image, not an empty frame");
        QVERIFY(!duplicate.error.isEmpty());
        QVERIFY(cancelled.path.isEmpty());
        QVERIFY(!QFileInfo::exists(temporary.filePath(u"cancelled.gif"_s)));
        QVERIFY(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden).filter(u".winshun-gif"_s).isEmpty());
        QVERIFY(QFileInfo::exists(source.path)); // failure and cancellation never remove the source
        QCOMPARE(result.percent, 100);
        QCOMPARE(gifSize(QSize(1920, 1080), 1280), QSize(1280, 720));
        QCOMPARE(gifSize(QSize(200, 100), 1280), QSize(200, 100));
    }

    // A GIF that keeps the region's size is made from a doubled video: the
    // same size as from a plain one, its small coloured text closer to the screen's.
    void gifFromDoubledVideo()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QSize size(641, 361); // odd: a plain video is padded to 642 × 362, a doubled one needs none
        const QImage backdrop = sampleBackdrop(size);
        double colour[2] {};
        for (const int scale : {1, 2}) {
            Options options;
            options.path = temporary.filePath(u"x%1.mp4"_s.arg(scale));
            options.region = QRect(QPoint(), size);
            options.software = true;
            options.quality = kQualities.back();
            options.scale = scale;
            Result video, gif;
            std::jthread worker([&] {
                video = sample(options, 400);
                if (!video.error.isEmpty()) return;
                Control control;
                gif = exportGif(video.path, {temporary.filePath(u"x%1.gif"_s.arg(scale)), 15, 1280, {}, video.scale}, control);
            });
            worker.join();
            QVERIFY2(video.error.isEmpty(), qPrintable(video.error));
            QCOMPARE(video.scale, scale);
            QVERIFY2(gif.error.isEmpty(), qPrintable(gif.error));
            const QImage first = QImageReader(gif.path).read().convertToFormat(QImage::Format_RGB32);
            QCOMPARE(first.size(), scale == 2 ? size : QSize(642, 362));
            colour[scale - 1] = colourCloseness(first, backdrop);
        }
        QVERIFY2(colour[1] >= colour[0] + 3, qPrintable(u"plain %1 dB, doubled %2 dB"_s.arg(colour[0]).arg(colour[1])));
        qInfo().noquote() << "GIF colour: from a plain video" << colour[0] << "dB, from a doubled one" << colour[1] << "dB";
    }

    void encodedVideo_data()
    {
        QTest::addColumn<bool>("software");
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("audio");
        QTest::addColumn<int>("fps");
        QTest::addColumn<bool>("clickHighlights");
        QTest::addColumn<bool>("noVideoDevice");
        QTest::newRow("hardware") << false << QSize(640, 360) << false << 30 << false << false;
        QTest::newRow("hardware-aac") << false << QSize(640, 360) << true << 30 << false << false;
        QTest::newRow("software-odd-region") << true << QSize(641, 361) << false << 30 << false << false;
        QTest::newRow("15-frames") << false << QSize(640, 360) << false << 15 << false << false;
        QTest::newRow("60-frames") << false << QSize(640, 360) << false << 60 << false << false;
        QTest::newRow("software-60-frames-aac") << true << QSize(640, 360) << true << 60 << false << false;
        QTest::newRow("gpu-click-highlight") << false << QSize(640, 360) << false << 30 << true << false;
        // GitHub #214: hardware asked for, but the driver has no D3D11 video.
        QTest::newRow("no-video-device-odd-region") << false << QSize(641, 361) << false << 30 << false << true;
        QTest::newRow("no-video-device-click-highlight") << false << QSize(640, 360) << false << 30 << true << true;
    }

    void encodedVideo()
    {
        QFETCH(bool, software);
        QFETCH(QSize, size);
        QFETCH(bool, audio);
        QFETCH(int, fps);
        QFETCH(bool, clickHighlights);
        QFETCH(bool, noVideoDevice);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Options options;
        options.path = temporary.filePath(u"sample.mp4"_s);
        options.region = QRect(QPoint(), size);
        options.software = software;
        options.noVideoDevice = noVideoDevice;
        options.fps = fps;
        options.systemAudio = audio; // sample() generates a tone, never opens an endpoint
        options.clickHighlights = clickHighlights;
        Result result;
        HRESULT decoded = E_FAIL;
        QSize actual;
        qint64 duration = 0;
        QByteArray pixels;
        QByteArray lastPixels;
        bool hasAudio = false;
        HRESULT audioDecoded = E_FAIL;
        int audioPeak = 0;
        UINT32 actualFps = 0, fpsDenominator = 0;
        std::jthread worker([&] {
            result = sample(options, 600);
            if (!result.error.isEmpty()) return;
            const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(com)) { decoded = com; return; }
            const auto uninit = qScopeGuard([] { ::CoUninitialize(); });
            decoded = ::MFStartup(MF_VERSION);
            if (FAILED(decoded)) return;
            const auto shutdown = qScopeGuard([] { ::MFShutdown(); });
            ComPtr<IMFAttributes> attributes;
            ::MFCreateAttributes(&attributes, 1);
            attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            ComPtr<IMFSourceReader> reader;
            decoded = ::MFCreateSourceReaderFromURL(reinterpret_cast<const wchar_t*>(result.path.utf16()), attributes.Get(), &reader);
            if (FAILED(decoded)) return;
            PROPVARIANT value {};
            if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value))) {
                duration = qint64(value.uhVal.QuadPart) / 10000;
                ::PropVariantClear(&value);
            }
            ComPtr<IMFMediaType> type;
            ::MFCreateMediaType(&type);
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            decoded = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
            if (FAILED(decoded)) return;
            type.Reset();
            reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
            UINT32 width = 0, height = 0;
            ::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
            actual = QSize(int(width), int(height));
            ::MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &actualFps, &fpsDenominator);
            for (int i = 0; i < 100; ++i) {
                DWORD flags = 0;
                ComPtr<IMFSample> frame;
                decoded = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, nullptr, &frame);
                if (FAILED(decoded) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
                if (!frame) continue;
                ComPtr<IMFMediaBuffer> buffer;
                frame->GetBufferByIndex(0, &buffer);
                QByteArray framePixels;
                ComPtr<IMF2DBuffer> twoD;
                if (SUCCEEDED(buffer.As(&twoD))) {
                    BYTE* top = nullptr;
                    LONG pitch = 0;
                    decoded = twoD->Lock2D(&top, &pitch);
                    if (SUCCEEDED(decoded)) {
                        framePixels.resize(actual.width() * actual.height() * 4);
                        for (int y = 0; y < actual.height(); ++y)
                            std::memcpy(framePixels.data() + y * actual.width() * 4, top + qint64(y) * pitch, size_t(actual.width()) * 4);
                        twoD->Unlock2D();
                    }
                } else {
                    BYTE* data = nullptr;
                    DWORD length = 0;
                    decoded = buffer->Lock(&data, nullptr, &length);
                    if (SUCCEEDED(decoded)) {
                        type.Reset();
                        reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
                        const LONG stride = LONG(::MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, UINT32(actual.width() * 4)));
                        framePixels.resize(actual.width() * actual.height() * 4);
                        for (int y = 0; y < actual.height(); ++y)
                            std::memcpy(framePixels.data() + y * actual.width() * 4, data + qint64(y) * stride, size_t(actual.width()) * 4);
                        buffer->Unlock();
                    }
                }
                if (FAILED(decoded)) break;
                if (pixels.isEmpty()) pixels = framePixels;
                lastPixels = std::move(framePixels);
                if (!clickHighlights) break;
            }
            type.Reset();
            hasAudio = SUCCEEDED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &type));
            if (audio && hasAudio) {
                type.Reset();
                ::MFCreateMediaType(&type);
                type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                audioDecoded = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type.Get());
                if (FAILED(audioDecoded)) return;
                for (int i = 0; i < 20 && audioPeak < 500; ++i) {
                    DWORD flags = 0;
                    ComPtr<IMFSample> frame;
                    audioDecoded = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &frame);
                    if (FAILED(audioDecoded) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
                    if (!frame) continue;
                    ComPtr<IMFMediaBuffer> buffer;
                    audioDecoded = frame->ConvertToContiguousBuffer(&buffer);
                    if (FAILED(audioDecoded)) break;
                    BYTE* data = nullptr;
                    DWORD length = 0;
                    audioDecoded = buffer->Lock(&data, nullptr, &length);
                    if (FAILED(audioDecoded)) break;
                    for (DWORD at = 0; at + sizeof(qint16) <= length; at += sizeof(qint16)) {
                        qint16 amplitude = 0;
                        std::memcpy(&amplitude, data + at, sizeof amplitude);
                        audioPeak = std::max(audioPeak, std::abs(int(amplitude)));
                    }
                    buffer->Unlock();
                }
            }
        });
        worker.join();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY2(SUCCEEDED(decoded), qPrintable(u"Decode HRESULT 0x%1"_s.arg(quint32(decoded), 8, 16, QChar(u'0'))));
        QCOMPARE(actual, QSize((size.width() + 1) & ~1, (size.height() + 1) & ~1));
        QVERIFY(fpsDenominator > 0);
        QVERIFY(std::abs(double(actualFps) / fpsDenominator - fps) < 0.05); // MP4 timestamps use a rational time base
        // On time: 60 a second are 16.7 ms apart, closer than Sleep()'s 15.6 ms tick tells apart.
        QVERIFY2(result.frames >= quint64(fps * 600 / 1000 * 8 / 10), qPrintable(QString::number(result.frames)));
        QCOMPARE(hasAudio, audio);
        if (audio) {
            QVERIFY(SUCCEEDED(audioDecoded));
            QVERIFY2(audioPeak >= 500, "AAC must decode to the generated tone, not silence");
        }
        QVERIFY2(duration >= 500 && duration <= 850, qPrintable(QString::number(duration)));
        QVERIFY(pixels.size() >= actual.width() * actual.height() * 4);
        // A dark/black frame can still be a valid MP4: check decoded content.
        const auto* rgb = reinterpret_cast<const unsigned char*>(pixels.constData()) + (actual.height() / 4 * actual.width() + 5) * 4;
        QVERIFY2(rgb[0] > 225 && rgb[1] > 225 && rgb[2] > 225,
            qPrintable(u"decoded BGR: %1 %2 %3, size %4"_s.arg(rgb[0]).arg(rgb[1]).arg(rgb[2]).arg(pixels.size())));
        // The sample's moving block, rgb(40, 110, 200), starts near the left
        // edge: colours come through each conversion, not just brightness.
        const auto* block = reinterpret_cast<const unsigned char*>(pixels.constData()) + ((actual.height() / 2 + 17) * actual.width() + 45) * 4;
        QVERIFY2(std::abs(block[0] - 200) < 24 && std::abs(block[1] - 110) < 24 && std::abs(block[2] - 40) < 24,
            qPrintable(u"decoded block BGR: %1 %2 %3"_s.arg(block[0]).arg(block[1]).arg(block[2])));
        if (noVideoDevice) {
            QVERIFY(!result.hardware);
            QVERIFY2(result.encoder.contains(u"NV12 on the CPU"_s), qPrintable(result.encoder));
        }
        QVERIFY(result.bytes < 500'000);
        if (clickHighlights) {
            const auto orangePixels = [](const QByteArray& data) {
                int count = 0;
                for (qsizetype i = 0; i + 3 < data.size(); i += 4) {
                    const auto* rgb = reinterpret_cast<const unsigned char*>(data.constData() + i);
                    if (rgb[2] > 210 && rgb[1] > 110 && rgb[1] < 230 && rgb[0] < 170 && rgb[2] - rgb[0] > 70) ++count;
                }
                return count;
            };
            QVERIFY2(orangePixels(pixels) > 20, "The GPU click ring must survive MP4 encoding");
            QVERIFY2(orangePixels(lastPixels) == 0, "The click ring must fade out without altering the source frame");
        }
    }

    // GitHub #98: "Ctrl + S" at 100 ms in Shortcuts mode, through the GPU
    // overlay and the encoder: there in the frames after it, gone 2.5 s
    // (and a fade) later, and gone at once with a pause.
    void keycapsInFrames_data()
    {
        QTest::addColumn<bool>("pause");
        QTest::newRow("shown-then-gone") << false;
        QTest::newRow("cleared-by-a-pause") << true;
    }

    void keycapsInFrames()
    {
        QFETCH(bool, pause);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Options options;
        options.path = temporary.filePath(u"keys.mp4"_s);
        options.region = QRect(0, 0, 640, 360);
        options.keys.mode = ws::capture::keycaps::Mode::Shortcuts;
        options.keyFont = QGuiApplication::font();
        options.keyInput = std::make_shared<KeyStream>(); // the hook's queue, empty here
        // The row: 36 px high (unit), 18 px from the bottom right corner;
        // its "S" keycap the 26 px before its 5 px padding.
        const QRect spot(594, 312, 20, 22);
        Control control;
        std::atomic<qint64> pausedAt {-1};
        Result result;
        HRESULT decoded = E_FAIL;
        QList<QPair<qint64, double>> frames; // time, the dark share of the spot
        std::jthread worker([&] {
            result = sample(options, pause ? 1200 : 3200, control, [&](const Progress& progress) {
                if (pause && pausedAt < 0 && progress.elapsedMs >= 250) {
                    pausedAt = progress.elapsedMs;
                    control.paused = true;
                }
            });
            if (!result.error.isEmpty()) return;
            decoded = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(decoded)) return;
            const auto uninit = qScopeGuard([] { ::CoUninitialize(); });
            decoded = forEachFrame(result.path, [&](qint64 ms, const unsigned char* top, LONG pitch) {
                int dark = 0;
                for (int y = spot.top(); y <= spot.bottom(); ++y) {
                    const unsigned char* row = top + qint64(y) * pitch;
                    for (int x = spot.left(); x <= spot.right(); ++x) {
                        const unsigned char* bgr = row + x * 4;
                        if (bgr[0] + bgr[1] + bgr[2] < 3 * 120) ++dark;
                    }
                }
                frames.append({ms, double(dark) / (spot.width() * spot.height())});
            });
        });
        if (pause) {
            QTRY_VERIFY_WITH_TIMEOUT(pausedAt.load() >= 0, 10000);
            QTest::qSleep(300);
            control.paused = false;
        }
        worker.join();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY2(SUCCEEDED(decoded), qPrintable(u"Decode HRESULT 0x%1"_s.arg(quint32(decoded), 8, 16, QChar(u'0'))));
        // Ended: what the hook hands over now is not kept (a GIF's export).
        ws::capture::keycaps::Key late;
        late.vk = 'A';
        late.time = monotonicTime();
        options.keyInput->push(late);
        QCOMPARE(options.keyInput->take().count, size_t(0));
        QString seen = u"paused at %1 ms; "_s.arg(pausedAt.load());
        for (const auto& [ms, dark] : frames)
            seen += u"%1:%2 "_s.arg(ms).arg(dark, 0, 'f', 2);
        int shown = 0, gone = 0;
        for (const auto& [ms, dark] : frames) {
            if (ms < 90) {
                QVERIFY2(dark < 0.3, qPrintable(seen)); // nothing before the key (the sample's own text: ~0.13)
            } else if (ms >= 190 && ms <= (pause ? 240 : 2500)) {
                QVERIFY2(dark > 0.6, qPrintable(seen));
                ++shown;
            } else if (pause ? ms > pausedAt + 40 : ms >= 2850) {
                QVERIFY2(dark < 0.3, qPrintable(seen));
                ++gone;
            }
        }
        QVERIFY2(shown >= (pause ? 1 : 60), qPrintable(seen));
        QVERIFY2(gone >= (pause ? 20 : 8), qPrintable(seen));
    }

    // The trail's line: smoothed through the pointer's places, young (wide,
    // opaque) at the pointer, and drawn back into it, the tail first, when
    // the pointer stops.
    void trailLine()
    {
        CursorTrail trail;
        trail.move({10, 10}, 0);
        QVERIFY(trail.line(0).empty()); // one place: no line
        trail.move({10.2, 10.2}, 100'000); // under half a pixel: not a move
        QVERIFY(trail.line(100'000).empty());
        for (int i = 1; i <= 10; ++i)
            trail.move({10.0 + 10 * i, 10}, i * 100'000); // 10 px every 10 ms
        const qint64 now = 1'000'000;
        const auto line = trail.line(now);
        QVERIFY(line.size() >= 11);
        QCOMPARE(line.front().at, QPointF(10, 10));
        QCOMPARE(line.back().at, QPointF(110, 10));
        QCOMPARE(line.front().place, 0.0f);
        QCOMPARE(line.back().place, 1.0f);
        QCOMPARE(line.back().life, 1.0f);
        QVERIFY(std::abs(line.front().life - 0.8f) < 1e-4f);
        for (size_t i = 1; i < line.size(); ++i) {
            QVERIFY(line[i].at.x() > line[i - 1].at.x());
            QVERIFY(line[i].place >= line[i - 1].place && line[i].life >= line[i - 1].life);
        }
        const auto later = trail.line(now + 4'500'000); // the first 60 ms of it gone
        QVERIFY(!later.empty());
        QCOMPARE(later.front().at, QPointF(70, 10));
        QCOMPARE(later.back().at, QPointF(110, 10));
        QVERIFY(std::abs(later.back().life - 0.1f) < 1e-4f);
        QVERIFY(trail.line(now + CursorTrail::lifetime).empty());
        trail.clear();
        QVERIFY(trail.line(now).empty());

        // A turn: rounded, through the midpoints, never through the corner.
        trail.move({0, 0}, 0);
        trail.move({100, 0}, 10'000);
        trail.move({100, 100}, 20'000);
        const auto turn = trail.line(20'000);
        QVERIFY(std::none_of(turn.begin(), turn.end(), [](const auto& point) { return point.at == QPointF(100, 0); }));
        QVERIFY(std::any_of(turn.begin(), turn.end(), [](const auto& point) {
            return std::abs(point.at.x() - 87.5) < 1e-6 && std::abs(point.at.y() - 12.5) < 1e-6;
        }));
        QCOMPARE(turn.back().at, QPointF(100, 100));
    }

    void trailColorsFromPointer()
    {
        const std::array<QRgb, 2> stops {qRgb(200, 0, 0), qRgb(0, 0, 200)};
        QCOMPARE(trailColorAt(stops, 0), stops[0]);
        QCOMPARE(trailColorAt(stops, 1), stops[1]);
        QCOMPARE(trailColorAt(stops, 0.5f), qRgb(100, 0, 100));
        QCOMPARE(trailColorAt({}, 0.5f), neutralTrailColors()[1]);

        // Windows' black and white arrows: the grey.
        QImage plain(32, 32, QImage::Format_ARGB32_Premultiplied);
        plain.fill(Qt::transparent);
        QPainter painter(&plain);
        painter.fillRect(4, 4, 10, 20, Qt::white);
        painter.fillRect(14, 4, 4, 20, Qt::black);
        painter.end();
        QCOMPARE(trailColors(plain), neutralTrailColors());

        // A lime pointer (Settings › Mouse pointer › custom colour): its own
        // hue, kept to the middle lightness at the pointer.
        QImage lime = plain;
        painter.begin(&lime);
        painter.fillRect(4, 4, 10, 20, QColor(0x8C, 0xE0, 0x1E));
        painter.end();
        const auto own = trailColors(lime);
        QCOMPARE(own.size(), size_t(1));
        const QColor tip = QColor::fromRgb(own.back());
        QVERIFY2(tip.hsvHue() >= 75 && tip.hsvHue() <= 100, qPrintable(tip.name()));
        const auto linear = [](int channel) {
            const double value = channel / 255.0;
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        const double light = 0.2126 * linear(tip.red()) + 0.7152 * linear(tip.green()) + 0.0722 * linear(tip.blue());
        QVERIFY2(light > 0.27 && light < 0.31, qPrintable(QString::number(light)));
    }

    // A pointer's bitmaps as Windows draws them: monochrome by AND and XOR,
    // colour without alpha by its mask, colour with alpha by the alpha.
    void cursorPictureFromBits()
    {
        constexpr QRgb black = 0xff000000u, white = 0xffffffffu;
        const auto row = [](QImage& image, int y, std::initializer_list<QRgb> pixels) {
            int x = 0;
            for (QRgb pixel : pixels) image.setPixel(x++, y, pixel);
        };
        QImage mask(4, 2, QImage::Format_ARGB32);
        row(mask, 0, {black, black, white, white}); // AND
        row(mask, 1, {black, white, black, white}); // XOR
        const auto mono = cursorPicture(QImage(), mask, QPoint(1, 0));
        QCOMPARE(mono.image.size(), QSize(4, 1));
        QCOMPARE(mono.hotSpot, QPoint(1, 0));
        QCOMPARE(mono.image.pixel(0, 0), black);
        QCOMPARE(mono.image.pixel(1, 0), white);
        QCOMPARE(qAlpha(mono.image.pixel(2, 0)), 0); // the screen as it is
        QCOMPARE(qAlpha(mono.image.pixel(3, 0)), 0);
        QVERIFY(!mono.invert.isNull());
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(mono.invert.constScanLine(0)), 4), QByteArray("\0\0\0\xff", 4));

        QImage color(4, 1, QImage::Format_ARGB32);
        row(color, 0, {0x00000000u, 0x00ff0000u, 0x00000000u, 0x00ffffffu});
        QImage andMask(4, 1, QImage::Format_ARGB32);
        row(andMask, 0, {black, black, white, white});
        const auto masked = cursorPicture(color, andMask, {});
        QCOMPARE(masked.image.pixel(0, 0), black);
        QCOMPARE(masked.image.pixel(1, 0), qRgb(255, 0, 0));
        QCOMPARE(qAlpha(masked.image.pixel(2, 0)), 0);
        QCOMPARE(qAlpha(masked.image.pixel(3, 0)), 0);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(masked.invert.constScanLine(0)), 4), QByteArray("\0\0\0\xff", 4));

        QImage alpha(2, 1, QImage::Format_ARGB32);
        row(alpha, 0, {qRgba(255, 0, 0, 128), qRgba(0, 0, 255, 255)});
        QImage none(2, 1, QImage::Format_ARGB32);
        row(none, 0, {white, white});
        const auto shaded = cursorPicture(alpha, none, {});
        QCOMPARE(shaded.image.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(qAlpha(shaded.image.pixel(0, 0)), 128);
        QCOMPARE(shaded.image.pixel(1, 0), qRgb(0, 0, 255));
        QVERIFY(shaded.invert.isNull());
        QVERIFY(shaded.id != masked.id && masked.id != mono.id);
    }

    // On this desktop: the standard pointers at the size Windows shows them
    // (whatever the handle's), a program's own at its own size, and an
    // animated one stepping with the time.
    void cursorShapes()
    {
        CursorShapes shapes(1.5);
        QCOMPARE(shapes.systemSize(), systemCursorSize(1.5));
        const CursorPicture* arrow = shapes.arrow();
        QVERIFY(arrow && !arrow->image.isNull());
        QCOMPARE(arrow->image.width(), shapes.systemSize());
        const CursorPicture* beam = shapes.picture(::LoadCursorW(nullptr, IDC_IBEAM), 0);
        QVERIFY(beam && !beam->image.isNull());
        QCOMPARE(beam->image.width(), shapes.systemSize());
        QCOMPARE(shapes.picture(::LoadCursorW(nullptr, IDC_ARROW), 0), arrow); // kept, not read again

        std::array<BYTE, 32 * 32 / 8> andBits {}, xorBits {};
        andBits.fill(0xff);
        HCURSOR own = ::CreateCursor(::GetModuleHandleW(nullptr), 3, 4, 32, 32, andBits.data(), xorBits.data());
        QVERIFY(own);
        const CursorPicture* made = shapes.picture(own, 0);
        QVERIFY(made);
        QCOMPARE(made->image.size(), QSize(32, 32));
        QCOMPARE(made->hotSpot, QPoint(3, 4));
        ::DestroyCursor(own);

        const auto busy = static_cast<HCURSOR>(::LoadImageW(nullptr, LR"(C:\Windows\Cursors\aero_busy.ani)", IMAGE_CURSOR, 48, 48, LR_LOADFROMFILE));
        if (!busy)
            QSKIP("no aero_busy.ani");
        const CursorPicture* first = shapes.picture(busy, 1'000'000);
        const CursorPicture* again = shapes.picture(busy, 1'100'000); // within its first frame
        const CursorPicture* next = shapes.picture(busy, 1'000'000 + 2'000'000); // 200 ms on
        ::DestroyCursor(busy);
        QVERIFY(first && again && next);
        QCOMPARE(again, first);
        QVERIFY(next != first);
        QCOMPARE(next->image.size(), QSize(48, 48));
    }

    // Pointer files read as Windows shows them: a .cur's picture made for
    // the size as it is, else the next larger (or the largest) one resized;
    // an .ani's frames in its steps' order and time.
    void cursorFileFramesFromFiles()
    {
        const auto put16 = [](QByteArray& out, quint32 value) { out.append(char(value & 0xff)).append(char(value >> 8 & 0xff)); };
        const auto put32 = [&put16](QByteArray& out, quint32 value) { put16(out, value & 0xffff); put16(out, value >> 16); };
        struct Frame {
            int width;
            int blue; // tells the frames apart
            int hotX, hotY;
        };
        // A .cur: 32-bit DIBs, bottom row first, then an empty AND mask.
        // Each pixel is (x * 8, y * 8, blue); the diagonal is transparent.
        const auto cur = [&](const std::vector<Frame>& frames) {
            QByteArray head, body;
            put16(head, 0);
            put16(head, 2);
            put16(head, quint32(frames.size()));
            qsizetype offset = 6 + 16 * qsizetype(frames.size());
            for (const Frame& frame : frames) {
                QByteArray dib;
                put32(dib, 40);
                put32(dib, quint32(frame.width));
                put32(dib, quint32(frame.width * 2));
                put16(dib, 1);
                put16(dib, 32);
                for (int i = 0; i < 6; ++i)
                    put32(dib, 0);
                for (int y = frame.width - 1; y >= 0; --y)
                    for (int x = 0; x < frame.width; ++x)
                        put32(dib, x == y ? 0u : qRgba(x * 8, y * 8, frame.blue, 255));
                dib.append(QByteArray(((frame.width + 31) / 32) * 4 * frame.width, '\0'));
                head.append(char(frame.width)).append(char(frame.width)).append('\0').append('\0');
                put16(head, quint32(frame.hotX));
                put16(head, quint32(frame.hotY));
                put32(head, quint32(dib.size()));
                put32(head, quint32(offset + body.size()));
                body.append(dib);
            }
            return head + body;
        };
        const QByteArray file = cur({{16, 16, 2, 3}, {24, 24, 3, 6}});

        const CursorFrames exact = cursorFileFrames(file, 24);
        QCOMPARE(exact.pictures.size(), size_t(1));
        const CursorPicture& own = exact.pictures.front();
        QCOMPARE(own.image.size(), QSize(24, 24));
        QCOMPARE(own.hotSpot, QPoint(3, 6));
        QVERIFY(own.invert.isNull());
        QCOMPARE(own.image.pixel(5, 7), qRgba(40, 56, 24, 255)); // as made, not drawn again
        QCOMPARE(qAlpha(own.image.pixel(9, 9)), 0);
        QCOMPARE(exact.ends, std::vector<qint64>({10'000'000}));

        const auto check = [&](int size, int blue, QPoint hotSpot) {
            const CursorFrames read = cursorFileFrames(file, size);
            QCOMPARE(read.pictures.size(), size_t(1));
            const QImage& image = read.pictures.front().image;
            QCOMPARE(image.size(), QSize(size, size));
            QCOMPARE(read.pictures.front().hotSpot, hotSpot);
            QVERIFY(std::abs(qBlue(image.pixel(size - 2, 1)) - blue) <= 2);
        };
        check(20, 24, QPoint(3, 5)); // the next larger, made smaller
        check(12, 16, QPoint(2, 2));
        check(32, 24, QPoint(4, 8)); // none larger: the largest

        // An .ani of two frames in three steps: 1, 0, 1 for 2, 4, 6 jiffies.
        QByteArray chunks = "anih", list;
        put32(chunks, 36);
        for (quint32 value : {36u, 2u, 3u, 0u, 0u, 0u, 0u, 10u, 1u})
            put32(chunks, value);
        chunks.append("rate");
        put32(chunks, 12);
        for (quint32 value : {2u, 4u, 6u})
            put32(chunks, value);
        chunks.append("seq ");
        put32(chunks, 12);
        for (quint32 value : {1u, 0u, 1u})
            put32(chunks, value);
        list.append("fram");
        for (const QByteArray& icon : {cur({{16, 16, 1, 1}}), cur({{16, 99, 1, 1}})}) {
            list.append("icon");
            put32(list, quint32(icon.size()));
            list.append(icon);
        }
        chunks.append("LIST");
        put32(chunks, quint32(list.size()));
        chunks.append(list);
        QByteArray ani = "RIFF";
        put32(ani, quint32(4 + chunks.size()));
        ani.append("ACON").append(chunks);

        const CursorFrames animated = cursorFileFrames(ani, 16);
        QCOMPARE(animated.pictures.size(), size_t(3));
        QCOMPARE(animated.ends, std::vector<qint64>({333'333, 1'000'000, 2'000'000}));
        QCOMPARE(qBlue(animated.pictures[0].image.pixel(14, 1)), 99);
        QCOMPARE(qBlue(animated.pictures[1].image.pixel(14, 1)), 16);
        QCOMPARE(animated.pictures[2].id, animated.pictures[0].id); // one frame, one GPU copy

        QVERIFY(cursorFileFrames(QByteArray("not a pointer"), 16).pictures.empty());
        QVERIFY(cursorFileFrames(ani.first(40), 16).pictures.empty());
    }

    // PointerOverlay on a GPU texture, read back: the trail in its colour,
    // no doubled alpha where it crosses itself, and a monochrome pointer
    // drawn as Windows draws it (black, white, the screen kept, inverted).
    void overlayDrawsTrailAndPointer()
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        HRESULT made = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(made))
            made = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &device, nullptr, &context);
        QVERIFY(SUCCEEDED(made));
        constexpr int width = 64, height = 40;
        std::vector<std::uint32_t> ground(size_t(width) * height, 0xffc86432u); // rgb(200, 100, 50)
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        const D3D11_SUBRESOURCE_DATA initial {ground.data(), UINT(width * 4), 0};
        ComPtr<ID3D11Texture2D> source;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, &initial, &source)));
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
        const auto readBack = [&](ID3D11Texture2D* texture) {
            std::vector<std::uint32_t> pixels(size_t(width) * height);
            context->CopyResource(staging.Get(), texture);
            D3D11_MAPPED_SUBRESOURCE mapped {};
            if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                for (int y = 0; y < height; ++y)
                    std::memcpy(pixels.data() + size_t(y) * width, static_cast<const char*>(mapped.pData) + size_t(y) * mapped.RowPitch, width * 4);
                context->Unmap(staging.Get(), 0);
            }
            return pixels;
        };
        const auto alike = [](std::uint32_t pixel, QRgb want) {
            return std::abs(qRed(pixel) - qRed(want)) <= 6 && std::abs(qGreen(pixel) - qGreen(want)) <= 6
                && std::abs(qBlue(pixel) - qBlue(want)) <= 6;
        };
        const auto name = [](std::uint32_t pixel) { return u"%1"_s.arg(pixel & 0xffffffu, 6, 16, QChar(u'0')); };

        PointerOverlay overlay(device.Get(), context.Get(), QSize(width, height), 1);
        constexpr QRgb black = 0xff000000u, white = 0xffffffffu;
        QImage mask(4, 2, QImage::Format_ARGB32);
        for (int x = 0; x < 4; ++x) {
            mask.setPixel(x, 0, x < 2 ? black : white); // AND
            mask.setPixel(x, 1, x % 2 ? white : black); // XOR
        }
        const CursorPicture pointer = cursorPicture(QImage(), mask, QPoint(1, 0));
        const std::vector<CursorTrail::Point> straight {{QPointF(4, 30), 1, 0}, {QPointF(60, 30), 1, 1}};
        const std::array<QRgb, 1> green {qRgb(0, 200, 0)};
        auto pixels = readBack(overlay.draw(source.Get(), {}, 0, {}, TrailDraw {straight, 8, green}, CursorDraw {&pointer, QPoint(11, 10)}));
        const auto at = [&](int x, int y) { return pixels[size_t(y) * width + x]; };
        QVERIFY2(alike(at(10, 10), qRgb(0, 0, 0)), qPrintable(name(at(10, 10))));
        QVERIFY2(alike(at(11, 10), qRgb(255, 255, 255)), qPrintable(name(at(11, 10))));
        QVERIFY2(alike(at(12, 10), qRgb(200, 100, 50)), qPrintable(name(at(12, 10))));
        QVERIFY2(alike(at(13, 10), qRgb(55, 155, 205)), qPrintable(name(at(13, 10)))); // inverted
        QVERIFY2(alike(at(30, 30), qRgb(0, 200, 0)), qPrintable(name(at(30, 30))));
        QVERIFY2(alike(at(30, 20), qRgb(200, 100, 50)), qPrintable(name(at(30, 20))));
        // The source frame is kept as it was (the overlay draws on a copy).
        QVERIFY(readBack(source.Get()) == ground);

        // Half faded, crossing itself at (30, 20): half green there, as
        // everywhere else on it, not three quarters.
        const std::vector<CursorTrail::Point> crossing {{QPointF(10, 6), 0.5f, 0}, {QPointF(50, 34), 0.5f, 0.33f},
            {QPointF(50, 6), 0.5f, 0.66f}, {QPointF(10, 34), 0.5f, 1}};
        pixels = readBack(overlay.draw(source.Get(), {}, 0, {}, TrailDraw {crossing, 16, green}));
        const QRgb half = qRgb(100, 150, 25);
        QVERIFY2(alike(at(30, 20), half), qPrintable(name(at(30, 20))));
        QVERIFY2(alike(at(20, 13), half), qPrintable(name(at(20, 13))));
        // Nothing to draw: the source itself, no copy.
        QCOMPARE(overlay.draw(source.Get(), {}, 0, {}, TrailDraw {}, CursorDraw {}), source.Get());
    }

    // The trail through the GPU overlay and the encoder: there while the
    // sample's pointer moves (250 ms), drawn back into it within half a
    // second of its stop; the sample draws no pointer of the desktop's.
    void trailInFrames()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Options options;
        options.path = temporary.filePath(u"trail.mp4"_s);
        options.region = QRect(0, 0, 640, 360);
        options.cursorTrail = true;
        Result result;
        HRESULT decoded = E_FAIL;
        QList<QPair<qint64, int>> frames; // time, the trail's pixels in the top half
        std::jthread worker([&] {
            result = sample(options, 1000);
            if (!result.error.isEmpty()) return;
            decoded = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(decoded)) return;
            const auto uninit = qScopeGuard([] { ::CoUninitialize(); });
            decoded = forEachFrame(result.path, [&](qint64 ms, const unsigned char* top, LONG pitch) {
                int magenta = 0;
                for (int y = 0; y < 180; ++y) {
                    const unsigned char* line = top + qint64(y) * pitch;
                    for (int x = 0; x < 640; ++x) {
                        const unsigned char* bgr = line + x * 4;
                        if (bgr[2] > 150 && bgr[0] > 150 && bgr[1] < 110) ++magenta;
                    }
                }
                frames.append({ms, magenta});
            });
        });
        worker.join();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY2(SUCCEEDED(decoded), qPrintable(u"Decode HRESULT 0x%1"_s.arg(quint32(decoded), 8, 16, QChar(u'0'))));
        QString seen;
        for (const auto& [ms, count] : frames)
            seen += u"%1:%2 "_s.arg(ms).arg(count);
        int shown = 0, gone = 0, moving = 0, fading = 1 << 30;
        for (const auto& [ms, count] : frames) {
            if (ms >= 100 && ms <= 280) {
                QVERIFY2(count > 300, qPrintable(seen));
                ++shown;
                moving = std::max(moving, count);
            } else if (ms >= 420 && ms <= 480) {
                fading = std::min(fading, count);
            } else if (ms >= 800) {
                QVERIFY2(count == 0, qPrintable(seen));
                ++gone;
            }
        }
        QVERIFY2(shown >= 3 && gone >= 3, qPrintable(seen));
        QVERIFY2(fading < moving / 2, qPrintable(seen)); // drawing back, thinner and fainter
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.contains(u"--recording-worker"_s)) {
        if (args.contains(u"--test-stall-worker"_s)) { ::Sleep(30'000); return 9; }
        if (args.contains(u"--test-fail-hardware"_s)) {
            QByteArray bytes(65536, '\0');
            DWORD count = 0;
            QElapsedTimer waiting;
            waiting.start();
            while (waiting.elapsed() < 3000) {
                if (!::PeekNamedPipe(::GetStdHandle(STD_INPUT_HANDLE), bytes.data(), DWORD(bytes.size()), &count, nullptr, nullptr)) return 8;
                if (QByteArrayView(bytes.constData(), count).contains('\n')) break;
                ::Sleep(5);
            }
            const auto json = QJsonDocument::fromJson(bytes.first(qsizetype(count)).split('\n').first()).object();
            if (!json.value(u"software"_s).toBool()) return 7;
        }
        return recordingWorkerMain();
    }
    TestRecording test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_recording.moc"
