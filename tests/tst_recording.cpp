#include "Recording.h"
#include "GifExport.h"
#include "KeyStream.h"
#include "RecordingPointer.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
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

} // namespace

class TestRecording : public QObject {
    Q_OBJECT
private slots:
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
                ComPtr<IWICMetadataQueryReader> metadata;
                frame->GetMetadataQueryReader(&metadata);
                if (metadata && SUCCEEDED(metadata->GetMetadataByName(L"/grctlext/Delay", &value))) {
                    if (value.vt == VT_UI2) totalDelay += value.uiVal * 10;
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
        QImageReader animation(gif.path);
        QCOMPARE(animation.loopCount(), -1);
        for (UINT i = 0; i < count; ++i) {
            const QImage frame = animation.read();
            QVERIFY2(!frame.isNull(), qPrintable(animation.errorString()));
            QCOMPARE(frame.size(), QSize(320, 180)); // Qt composites GIF update rectangles into full pictures
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

    void encodedVideo_data()
    {
        QTest::addColumn<bool>("software");
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("audio");
        QTest::addColumn<int>("fps");
        QTest::addColumn<bool>("clickHighlights");
        QTest::newRow("hardware") << false << QSize(640, 360) << false << 30 << false;
        QTest::newRow("hardware-aac") << false << QSize(640, 360) << true << 30 << false;
        QTest::newRow("software-odd-region") << true << QSize(641, 361) << false << 30 << false;
        QTest::newRow("smaller-file") << false << QSize(640, 360) << false << 15 << false;
        QTest::newRow("gpu-click-highlight") << false << QSize(640, 360) << false << 30 << true;
    }

    void encodedVideo()
    {
        QFETCH(bool, software);
        QFETCH(QSize, size);
        QFETCH(bool, audio);
        QFETCH(int, fps);
        QFETCH(bool, clickHighlights);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Options options;
        options.path = temporary.filePath(u"sample.mp4"_s);
        options.region = QRect(QPoint(), size);
        options.software = software;
        options.fps = fps;
        if (fps == 15) options.quality = 70;
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
        QVERIFY(result.frames >= 5);
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
};

QTEST_MAIN(TestRecording)
#include "tst_recording.moc"
