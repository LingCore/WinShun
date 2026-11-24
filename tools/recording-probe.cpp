#include "Recording.h"
#include "RecordingPointer.h"
#include "platform/KeyListener.h"

#include <QGuiApplication>
#include <QBackingStore>
#include <QPainter>
#include <QTimer>
#include <QWindow>
#include <windows.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QScopeGuard>
#include <QScreen>
#include <QTextStream>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <thread>
#include <cstring>

using namespace Qt::StringLiterals;

QJsonObject inspect(const QString& path, const QString& imagePath, bool findClick)
{
    using Microsoft::WRL::ComPtr;
    QJsonObject result {{u"file"_s, path}, {u"bytes"_s, QFileInfo(path).size()}};
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return {{u"error"_s, qint64(com)}};
    const auto uninit = qScopeGuard([] { ::CoUninitialize(); });
    const HRESULT started = ::MFStartup(MF_VERSION);
    if (FAILED(started)) return {{u"error"_s, qint64(started)}};
    const auto shutdown = qScopeGuard([] { ::MFShutdown(); });
    ComPtr<IMFAttributes> attributes;
    ::MFCreateAttributes(&attributes, 1);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = ::MFCreateSourceReaderFromURL(reinterpret_cast<const wchar_t*>(path.utf16()), attributes.Get(), &reader);
    if (FAILED(hr)) return {{u"error"_s, qint64(hr)}};
    PROPVARIANT value {};
    if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value))) {
        result[u"durationMs"_s] = qint64(value.uhVal.QuadPart / 10000);
        ::PropVariantClear(&value);
    }
    ComPtr<IMFMediaType> audio;
    result[u"hasAudio"_s] = SUCCEEDED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &audio));
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    ComPtr<IMFMediaType> type;
    ::MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
    if (FAILED(hr)) return {{u"error"_s, qint64(hr)}};
    type.Reset();
    reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
    UINT32 width = 0, height = 0;
    ::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    result[u"width"_s] = int(width);
    result[u"height"_s] = int(height);
    UINT32 fps = 0, denominator = 0;
    if (SUCCEEDED(::MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &fps, &denominator)) && denominator)
        result[u"fps"_s] = double(fps) / denominator;
    int frames = 0;
    LONGLONG lastTime = 0;
    bool foundClick = false;
    const auto saveImage = [&](const QImage& image) {
        if (findClick) {
            int colored = 0;
            for (int y = 0; y < image.height(); ++y) {
                const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
                for (int x = 0; x < image.width(); ++x) {
                    const QRgb pixel = row[x];
                    if (qRed(pixel) > 210 && qGreen(pixel) > 110 && qGreen(pixel) < 230 && qBlue(pixel) < 170
                        && qRed(pixel) - qBlue(pixel) > 70) ++colored;
                }
            }
            if (colored < 150) return; // ignore isolated ClearType / codec colour fringes
            foundClick = true;
            result[u"clickFrameMs"_s] = lastTime / 10000;
            result[u"clickPixels"_s] = colored;
        }
        result[u"imageSaved"_s] = image.save(imagePath);
    };
    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        LONGLONG timestamp = 0;
        hr = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &timestamp, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample) continue;
        lastTime = timestamp;
        ++frames;
        if ((findClick ? !foundClick : frames == 1 || frames == 30) && !imagePath.isEmpty()) {
            ComPtr<IMFMediaBuffer> buffer;
            sample->GetBufferByIndex(0, &buffer);
            ComPtr<IMF2DBuffer> twoD;
            if (SUCCEEDED(buffer.As(&twoD))) {
                BYTE* top = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(twoD->Lock2D(&top, &pitch))) {
                    QImage image(int(width), int(height), QImage::Format_RGB32);
                    for (UINT32 y = 0; y < height; ++y)
                        std::memcpy(image.scanLine(int(y)), top + qint64(y) * pitch, size_t(width) * 4);
                    twoD->Unlock2D();
                    saveImage(image);
                }
            } else {
                BYTE* data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                    type.Reset();
                    reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
                    const LONG stride = LONG(::MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, width * 4));
                    QImage image(int(width), int(height), QImage::Format_RGB32);
                    if (length >= UINT32(std::abs(stride)) * height) {
                        for (UINT32 y = 0; y < height; ++y)
                            std::memcpy(image.scanLine(int(y)), data + qint64(y) * stride, size_t(width) * 4);
                        saveImage(image);
                    }
                    buffer->Unlock();
                }
            }
        }
    }
    result[u"decodedFrames"_s] = frames;
    result[u"lastFrameMs"_s] = lastTime / 10000;
    if (FAILED(hr)) result[u"decodeError"_s] = qint64(hr);
    return result;
}

class Fixture : public QWindow {
public:
    Fixture() : m_store(this)
    {
        setTitle(u"WinShun recording test — generated content"_s);
        setFlags(Qt::Window | Qt::WindowStaysOnTopHint);
        resize(800, 500);
        setPosition(80, 80);
        QObject::connect(&m_tick, &QTimer::timeout, this, [this] { ++m_frame; draw(); });
        m_tick.start(33);
    }
    void exposeEvent(QExposeEvent*) override { draw(); }
    void draw()
    {
        if (!isExposed()) return;
        m_store.resize(size());
        m_store.beginPaint(QRect(QPoint(), size()));
        QPainter painter(m_store.paintDevice());
        painter.fillRect(QRect(QPoint(), size()), QColor(248, 249, 251));
        painter.setPen(QColor(28, 33, 42));
        painter.setFont(QFont(u"Microsoft YaHei UI"_s, 11));
        for (int y = 30; y < height(); y += 27)
            painter.drawText(20, y, u"清晰的中文小字 ABC 0123456789  —  C++20 / Windows.Graphics.Capture"_s);
        painter.fillRect((m_frame * 3) % (width() - 80), height() / 2, 70, 30, QColor(40, 110, 200));
        painter.end();
        m_store.endPaint();
        m_store.flush(QRect(QPoint(), size()));
    }
private:
    QBackingStore m_store;
    QTimer m_tick;
    int m_frame = 0;
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 4) {
        QTextStream(stderr) << "recording-probe sample|screen|fixture file.mp4 milliseconds [x y width height] [--software] [--fps 15|30|60] [--quality 0-100] [--scale 2] [--bitrate-only]\n"
                              "    [--system-audio] [--microphone] [--pause] [--clicks] [--trail] [--backdrop sample.png]\n"
                              "recording-probe inspect file.mp4 frame.png [--find-click]\n";
        return 2;
    }
    if (args[1] == u"inspect") {
        QJsonObject result;
        std::jthread worker([&] { result = inspect(args[2], args[3], args.contains(u"--find-click")); });
        worker.join();
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
        return result.contains(u"error"_s) || result.contains(u"decodeError"_s) ? 1 : 0;
    }
    ws::capture::recording::Options options;
    options.path = args[2];
    options.region = QRect(0, 0, 1280, 720);
    if (args.size() >= 8 && !args[4].startsWith(u"--"))
        options.region = QRect(args[4].toInt(), args[5].toInt(), args[6].toInt(), args[7].toInt());
    options.software = args.contains(u"--software");
    options.systemAudio = args.contains(u"--system-audio");
    options.microphone = args.contains(u"--microphone");
    options.clickHighlights = args.contains(u"--clicks");
    const auto valueAfter = [&](QStringView name, int otherwise) {
        const qsizetype at = args.indexOf(name);
        return at > 0 && at + 1 < args.size() ? args[at + 1].toInt() : otherwise;
    };
    options.fps = valueAfter(u"--fps", 30);
    options.quality = valueAfter(u"--quality", options.quality);
    options.scale = valueAfter(u"--scale", 1);
    options.bitrateOnly = args.contains(u"--bitrate-only");
    options.cursorTrail = args.contains(u"--trail");
    const int duration = args[3].toInt();
    // What sample's frames are made from, to hold a decoded frame against.
    if (const qsizetype at = args.indexOf(u"--backdrop"); at > 0 && at + 1 < args.size())
        ws::capture::recording::sampleBackdrop(options.region.size()).save(args[at + 1]);
    std::unique_ptr<ws::KeyListener> clicks;
    const auto listen = [&] {
        if (!options.clickHighlights) return true;
        options.clickInput = std::make_shared<ws::capture::recording::ClickStream>(options.region);
        ws::KeyListener::Callbacks callbacks;
        callbacks.mousePress = [input = options.clickInput](POINT point, bool secondary, std::uint32_t time) {
            input->pushInput(QPoint(point.x, point.y), secondary, time);
        };
        clicks = std::make_unique<ws::KeyListener>(std::move(callbacks));
        return clicks->isActive();
    };
    ws::capture::recording::Result result;
    if (args[1] == u"sample") {
        std::jthread worker([&] { result = ws::capture::recording::sample(options, duration); });
        worker.join();
    } else if (args[1] == u"fixture") {
        Fixture fixture;
        fixture.show();
        ws::capture::recording::Control control;
        std::jthread worker;
        QTimer::singleShot(600, &app, [&] {
            const HWND hwnd = reinterpret_cast<HWND>(fixture.winId());
            RECT client {};
            ::GetClientRect(hwnd, &client);
            POINT at {0, 0};
            ::ClientToScreen(hwnd, &at);
            options.region = QRect(at.x, at.y, client.right, client.bottom);
            options.pointerScale = fixture.devicePixelRatio();
            if (!listen()) { result.error = u"Raw Input registration failed"_s; app.quit(); return; }
            worker = std::jthread([&] {
                const auto start = std::chrono::steady_clock::now();
                result = ws::capture::recording::record(options, control, [&](const auto&) {
                    if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(duration))
                        control.stop = true;
                });
                QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
            });
        });
        QTimer::singleShot(duration + 15000, &app, [&] { control.stop = true; });
        app.exec();
        control.stop = true;
        if (worker.joinable()) worker.join();
    } else {
        if (auto* screen = QGuiApplication::primaryScreen()) options.pointerScale = screen->devicePixelRatio();
        if (!listen()) return 3;
        ws::capture::recording::Control control;
        const bool pause = args.contains(u"--pause");
        std::jthread stop([&](std::stop_token token) {
            const auto start = std::chrono::steady_clock::now();
            while (!token.stop_requested()) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                control.paused = pause && elapsed >= duration / 3 && elapsed < duration * 2 / 3;
                if (elapsed >= duration) { control.stop = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        std::jthread worker([&] { result = ws::capture::recording::record(options, control, {}); });
        worker.join();
    }
    QTextStream(stdout) << QJsonDocument(QJsonObject {{u"path"_s, result.path}, {u"error"_s, result.error},
        {u"durationMs"_s, result.elapsedMs}, {u"frames"_s, qint64(result.frames)}, {u"bytes"_s, result.bytes},
        {u"encoder"_s, result.encoder}, {u"hardware"_s, result.hardware}, {u"rate"_s, result.rate},
        {u"cancelled"_s, result.error.isEmpty() && !result.frames}}).toJson(QJsonDocument::Compact) << '\n';
    return result.error.isEmpty() ? 0 : 1;
}
