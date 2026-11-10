// Real clipboard IO on a private window station, never the user's clipboard.
#include "PinImage.h"
#include "ClipImage.h"
#include "ImageOrigin.h"
#include "platform/ClipboardWatcher.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonDocument>
#include <QMimeData>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using namespace ws;
namespace pin = ws::captureplugin::pinimage;

namespace {
QByteArray unicodeBytes(const QString& text)
{
    return QByteArray(reinterpret_cast<const char*>(text.utf16()), (text.size() + 1) * sizeof(char16_t));
}

struct DelayedText {
    QByteArray bytes;
    int rendered = 0;
    int unexpectedRenders = 0;

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
            ::SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* offer = reinterpret_cast<DelayedText*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_RENDERFORMAT && wParam != CF_UNICODETEXT && offer) {
            ++offer->unexpectedRenders;
            return 0;
        }
        if (message == WM_RENDERFORMAT && wParam == CF_UNICODETEXT && offer) {
            ++offer->rendered;
            const HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(offer->bytes.size()));
            if (memory) {
                if (void* data = ::GlobalLock(memory)) {
                    std::memcpy(data, offer->bytes.constData(), static_cast<std::size_t>(offer->bytes.size()));
                    ::GlobalUnlock(memory);
                    if (::SetClipboardData(CF_UNICODETEXT, memory))
                        return 0;
                }
                ::GlobalFree(memory);
            }
            return 0;
        }
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
};
} // namespace

class TestPinClipboard : public QObject {
    Q_OBJECT

    QImage m_image {32, 24, QImage::Format_ARGB32_Premultiplied};

    void write(ClipWrite data)
    {
        ClipboardWatcher writer({}, {}); // writing works when history is off
        std::atomic<int> result = -1;
        writer.write(std::move(data), [&](bool ok) { result = ok ? 1 : 0; });
        QTRY_COMPARE_WITH_TIMEOUT(result.load(), 1, 3000);
    }

    pin::ClipboardPicture read(pin::LastCapture last = {})
    {
        return std::async(std::launch::async, [this, last] { return pin::readClipboard(last); }).get();
    }

    void writeFormats(const std::vector<std::pair<UINT, QByteArray>>& formats)
    {
        const HWND window = ::CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(window);
        const auto destroy = qScopeGuard([window] { ::DestroyWindow(window); });
        QVERIFY(::OpenClipboard(window));
        const auto close = qScopeGuard([] { ::CloseClipboard(); });
        ::EmptyClipboard();
        for (const auto& [format, bytes] : formats) {
            const HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(bytes.size()));
            QVERIFY(handle);
            void* data = ::GlobalLock(handle);
            if (!data) {
                ::GlobalFree(handle);
                QFAIL("GlobalLock failed");
            }
            std::memcpy(data, bytes.constData(), static_cast<std::size_t>(bytes.size()));
            ::GlobalUnlock(handle);
            const bool set = ::SetClipboardData(format, handle) != nullptr;
            if (!set)
                ::GlobalFree(handle);
            QVERIFY(set);
        }
    }

    static QByteArray dib(bool v5)
    {
        const quint32 pixels[] {qRgb(220, 60, 30), qRgb(20, 30, 40)};
        QByteArray bytes;
        if (v5) {
            BITMAPV5HEADER header {};
            header.bV5Size = sizeof header;
            header.bV5Width = 2;
            header.bV5Height = 1;
            header.bV5Planes = 1;
            header.bV5BitCount = 32;
            header.bV5Compression = BI_BITFIELDS;
            header.bV5RedMask = 0x00FF0000;
            header.bV5GreenMask = 0x0000FF00;
            header.bV5BlueMask = 0x000000FF;
            header.bV5AlphaMask = 0xFF000000;
            header.bV5CSType = LCS_sRGB;
            bytes = QByteArray(reinterpret_cast<const char*>(&header), sizeof header);
        } else {
            BITMAPINFOHEADER header {sizeof(BITMAPINFOHEADER)};
            header.biWidth = 2;
            header.biHeight = 1;
            header.biPlanes = 1;
            header.biBitCount = 32;
            header.biCompression = BI_RGB;
            bytes = QByteArray(reinterpret_cast<const char*>(&header), sizeof header);
        }
        bytes.append(reinterpret_cast<const char*>(pixels), sizeof pixels);
        return bytes;
    }

private slots:
    void initTestCase() { m_image.fill(QColor(220, 60, 30)); }

    void watcherDiagnosticsContainMetadataWithoutPayloads()
    {
        const QString text = u"隐私测试：2026年11月2日 / PRIVATE_CLIPBOARD_CONTENT"_s;
        std::promise<ClipCapture> promise;
        auto captured = promise.get_future();
        ClipboardWatcher watcher({}, {.captured = [&](ClipCapture capture) { promise.set_value(std::move(capture)); }});
        watcher.setOptions({true, false});
        QTRY_VERIFY_WITH_TIMEOUT(watcher.listening(), 2000);
        QVERIFY(watcher.diagnosticStatus()[u"recent_reads"_s].toArray().isEmpty());
        const UINT privateFormat = ::RegisterClipboardFormatW(L"PRIVATE_FORMAT_NAME_DO_NOT_EXPORT");
        writeFormats({{CF_UNICODETEXT, unicodeBytes(text)}, {privateFormat, QByteArray("PRIVATE_FORMAT_PAYLOAD")}});
        const DWORD sequence = ::GetClipboardSequenceNumber();
        QTRY_VERIFY_WITH_TIMEOUT(captured.wait_for(0ms) == std::future_status::ready, 3000);
        const ClipCapture capture = captured.get();
        QCOMPARE(capture.text, text);
        QTRY_COMPARE_WITH_TIMEOUT(watcher.diagnosticStatus()[u"read_attempts"_s].toInteger(), 1, 3000);
        const auto status = watcher.diagnosticStatus();
        const auto event = status[u"recent_reads"_s].toArray().last().toObject();
        QCOMPARE(event[u"outcome"_s].toString(), u"captured"_s);
        QCOMPARE(event[u"sequence_before"_s].toInteger(), sequence);
        QCOMPARE(event[u"sequence_opened"_s].toInteger(), sequence);
        QCOMPARE(event[u"sequence_after"_s].toInteger(), sequence);
        QCOMPARE(event[u"owner_pid_read"_s].toInteger(), 0);
        QVERIFY(event[u"foreground_fallback"_s].toBool()); // writeFormats destroys its owner window
        const auto unicode = event[u"unicode_text"_s].toObject();
        QCOMPARE(unicode[u"utf16_units"_s].toInteger(), text.size());
        QVERIFY(unicode[u"bytes"_s].toInteger() >= unicodeBytes(text).size());
        QVERIFY(unicode[u"terminated"_s].toBool());
        QVERIFY(!unicode[u"odd_bytes"_s].toBool());
        bool sawUnicode = false, sawPrivate = false;
        for (const auto format : event[u"formats"_s].toArray()) {
            const auto item = format.toObject();
            if (item[u"id"_s].toInt() == CF_UNICODETEXT)
                sawUnicode = item[u"name"_s].toString() == u"CF_UNICODETEXT"_s;
            if (item[u"id"_s].toInt() == static_cast<int>(privateFormat)) {
                sawPrivate = true;
                QVERIFY(!item.contains(u"name"_s));
            }
        }
        QVERIFY(sawUnicode && sawPrivate);
        const QByteArray json = QJsonDocument(status).toJson();
        QVERIFY(!json.contains(text.toUtf8()));
        QVERIFY(!json.contains("PRIVATE_"));
        QVERIFY(!json.contains(capture.hash.toHex()));
        QVERIFY(!json.contains(capture.hash.toBase64()));
        QVERIFY(!json.contains("hash"));
        QVERIFY(!json.contains(QCoreApplication::applicationDirPath().toUtf8()));
    }

    void watcherDiagnosticsRespectPrivacyFlags_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("outcome");
        QTest::newRow("excluded-monitoring") << u"ExcludeClipboardContentFromMonitorProcessing"_s << u"monitoring-excluded"_s;
        QTest::newRow("viewer-ignore") << u"Clipboard Viewer Ignore"_s << u"monitoring-excluded"_s;
        QTest::newRow("excluded-history") << u"CanIncludeInClipboardHistory"_s << u"history-excluded"_s;
    }

    void watcherDiagnosticsRespectPrivacyFlags()
    {
        QFETCH(QString, format);
        QFETCH(QString, outcome);
        std::atomic<int> captures = 0;
        ClipboardWatcher watcher({}, {.captured = [&](ClipCapture) { ++captures; }});
        watcher.setOptions({true, false});
        QTRY_VERIFY_WITH_TIMEOUT(watcher.listening(), 2000);
        writeFormats({{CF_UNICODETEXT, unicodeBytes(u"PRIVATE_PASSWORD_DO_NOT_READ"_s)},
            {::RegisterClipboardFormatW(qUtf16Printable(format)), QByteArray(sizeof(DWORD), '\0')}});
        QTRY_COMPARE_WITH_TIMEOUT(watcher.diagnosticStatus()[u"read_attempts"_s].toInteger(), 1, 3000);
        const auto event = watcher.diagnosticStatus()[u"recent_reads"_s].toArray().last().toObject();
        QCOMPARE(event[u"outcome"_s].toString(), outcome);
        QVERIFY(!event.contains(u"unicode_text"_s));
        QVERIFY(!event.contains(u"formats"_s));
        QVERIFY(!event.contains(u"source_exe"_s));
        QCOMPARE(captures.load(), 0);
    }

    void watcherDiagnosticsKeepOnlyTheOutcomeForExcludedApps()
    {
        // The writer's window is gone by the time it is read: the source is the program in front.
        DWORD pid = 0;
        if (const HWND foreground = ::GetForegroundWindow())
            ::GetWindowThreadProcessId(foreground, &pid);
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        const HANDLE process = pid ? ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
        const bool known = process && ::QueryFullProcessImageNameW(process, 0, path, &size);
        if (process)
            ::CloseHandle(process);
        if (!known)
            QSKIP("no program in front to exclude");
        std::atomic<int> captures = 0;
        ClipboardWatcher watcher({}, {.captured = [&](ClipCapture) { ++captures; }});
        watcher.setOptions({true, false, {QFileInfo(QString::fromWCharArray(path)).fileName()}});
        QTRY_VERIFY_WITH_TIMEOUT(watcher.listening(), 2000);
        writeFormats({{CF_UNICODETEXT, unicodeBytes(u"EXCLUDED_APP_TEXT"_s)}});
        QTRY_COMPARE_WITH_TIMEOUT(watcher.diagnosticStatus()[u"read_attempts"_s].toInteger(), 1, 3000);
        const auto event = watcher.diagnosticStatus()[u"recent_reads"_s].toArray().last().toObject();
        QCOMPARE(event[u"outcome"_s].toString(), u"app-excluded"_s);
        QVERIFY(!event.contains(u"formats"_s));
        QVERIFY(!event.contains(u"source_exe"_s));
        QVERIFY(!event.contains(u"source_pid"_s));
        QCOMPARE(captures.load(), 0);
    }

    void watcherDiagnosticsBoundRecordsAndFormatsDuringConcurrentSnapshots()
    {
        ClipboardWatcher watcher({}, {});
        watcher.setOptions({true, false});
        QTRY_VERIFY_WITH_TIMEOUT(watcher.listening(), 2000);
        std::atomic<bool> valid = true;
        std::jthread snapshots([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                const auto status = watcher.diagnosticStatus();
                const auto recent = status[u"recent_reads"_s].toArray();
                if (recent.size() > status[u"max_read_records"_s].toInt())
                    valid = false;
                for (const auto event : recent) {
                    if (event.toObject()[u"formats"_s].toArray().size() > status[u"max_formats_per_read"_s].toInt())
                        valid = false;
                }
                std::this_thread::sleep_for(1ms);
            }
        });
        const int limit = watcher.diagnosticStatus()[u"max_read_records"_s].toInt();
        for (int i = 1; i <= limit + 3; ++i) {
            writeFormats({{CF_UNICODETEXT, unicodeBytes(QString::number(i))}});
            QTRY_COMPARE_WITH_TIMEOUT(watcher.diagnosticStatus()[u"read_attempts"_s].toInteger(), i, 3000);
        }
        QCOMPARE(watcher.diagnosticStatus()[u"recent_reads"_s].toArray().size(), limit);
        std::vector<std::pair<UINT, QByteArray>> offers;
        const int formatLimit = watcher.diagnosticStatus()[u"max_formats_per_read"_s].toInt();
        for (int i = 0; i < formatLimit + 4; ++i) {
            const QString name = u"PRIVATE_FORMAT_%1"_s.arg(i);
            offers.emplace_back(::RegisterClipboardFormatW(qUtf16Printable(name)), QByteArray("private"));
        }
        writeFormats(offers);
        QTRY_COMPARE_WITH_TIMEOUT(watcher.diagnosticStatus()[u"read_attempts"_s].toInteger(), limit + 4, 3000);
        const auto status = watcher.diagnosticStatus();
        const auto recent = status[u"recent_reads"_s].toArray();
        QCOMPARE(recent.size(), limit);
        QCOMPARE(recent.last().toObject()[u"formats"_s].toArray().size(), formatLimit);
        QVERIFY(recent.last().toObject()[u"formats_truncated"_s].toBool());
        QVERIFY(!QJsonDocument(status).toJson().contains("PRIVATE_FORMAT"));
        snapshots.request_stop();
        snapshots.join();
        QVERIFY(valid.load());
    }

    void watcherDiagnosticsReportBusyClipboardAndRecover()
    {
        ClipboardWatcher watcher({}, {});
        watcher.setOptions({true, false});
        QTRY_VERIFY_WITH_TIMEOUT(watcher.listening(), 2000);
        writeFormats({{CF_UNICODETEXT, unicodeBytes(u"recover after lock"_s)}});
        const HWND window = ::CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(window);
        const auto destroy = qScopeGuard([window] { ::DestroyWindow(window); });
        QVERIFY(::OpenClipboard(window));
        bool open = true;
        const auto close = qScopeGuard([&] {
            if (open)
                ::CloseClipboard();
        });
        QTRY_VERIFY_WITH_TIMEOUT(!watcher.diagnosticStatus()[u"recent_reads"_s].toArray().isEmpty(), 3000);
        const auto failed = watcher.diagnosticStatus()[u"recent_reads"_s].toArray().last().toObject();
        QCOMPARE(failed[u"outcome"_s].toString(), u"open-failed"_s);
        QCOMPARE(failed[u"open_attempts"_s].toInt(), 10);
        QVERIFY(failed[u"last_open_error"_s].toInteger() != ERROR_SUCCESS);
        QCOMPARE(failed[u"blocker_pid"_s].toInteger(), ::GetCurrentProcessId());
        QCOMPARE(failed[u"blocker_exe"_s].toString(), QFileInfo(QCoreApplication::applicationFilePath()).fileName());
        QVERIFY(!failed.contains(u"unicode_text"_s));
        // The retries of the one held clipboard stay one record.
        QTRY_VERIFY_WITH_TIMEOUT(watcher.diagnosticStatus()[u"recent_reads"_s].toArray().last().toObject()[u"read_tries"_s].toInt() >= 2, 3000);
        int failedRecords = 0;
        for (const auto record : watcher.diagnosticStatus()[u"recent_reads"_s].toArray())
            failedRecords += record.toObject()[u"outcome"_s].toString() == u"open-failed"_s;
        QCOMPARE(failedRecords, 1);
        ::CloseClipboard();
        open = false;
        QTRY_COMPARE_WITH_TIMEOUT(watcher.diagnosticStatus()[u"recent_reads"_s].toArray().last().toObject()[u"outcome"_s].toString(), u"captured"_s, 3000);
        QVERIFY(watcher.diagnosticStatus()[u"recent_reads"_s].toArray().last().toObject()[u"retry"_s].toInt() >= 1);
    }

    void watcherCapturesDelayedUnicodeWithoutExtraRendering()
    {
        DelayedText offer {unicodeBytes(u"延迟渲染：2026年11月2日"_s)};
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSW type {};
        type.lpfnWndProc = &DelayedText::windowProc;
        type.hInstance = instance;
        type.lpszClassName = L"WinShun.TestDelayedText";
        QVERIFY(::RegisterClassW(&type));
        const auto unregister = qScopeGuard([&] { ::UnregisterClassW(type.lpszClassName, instance); });
        const HWND window = ::CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, instance, &offer);
        QVERIFY(window);
        const auto destroy = qScopeGuard([window] { ::DestroyWindow(window); });
        std::promise<ClipCapture> promise;
        auto captured = promise.get_future();
        std::atomic<bool> fulfilled = false;
        ClipboardWatcher watcher({}, {.captured = [&](ClipCapture capture) {
            if (!fulfilled.exchange(true))
                promise.set_value(std::move(capture));
        }});
        watcher.setOptions({true, false});
        QTRY_VERIFY_WITH_TIMEOUT(watcher.listening(), 2000);
        QVERIFY(::OpenClipboard(window));
        QVERIFY(::EmptyClipboard());
        ::SetClipboardData(CF_UNICODETEXT, nullptr);
        ::SetClipboardData(::RegisterClipboardFormatW(L"PRIVATE_DELAYED_FORMAT_DO_NOT_READ"), nullptr);
        ::CloseClipboard();
        QTRY_VERIFY_WITH_TIMEOUT(captured.wait_for(0ms) == std::future_status::ready, 3000);
        QCOMPARE(captured.get().text, u"延迟渲染：2026年11月2日"_s);
        QCOMPARE(offer.rendered, 1);
        QCOMPARE(offer.unexpectedRenders, 0);
        QTRY_VERIFY_WITH_TIMEOUT(watcher.diagnosticStatus()[u"read_attempts"_s].toInteger() >= 1, 3000);
        const auto event = watcher.diagnosticStatus()[u"recent_reads"_s].toArray().first().toObject();
        QCOMPARE(event[u"outcome"_s].toString(), u"captured"_s);
        // Windows does not guarantee a sequence change for every rendering.
        // Record the observed values without imposing a discard policy.
        QVERIFY(event[u"sequence_opened"_s].toInteger() > 0);
        QVERIFY(event[u"sequence_after"_s].toInteger() > 0);
        QCOMPARE(event[u"owner_pid_read"_s].toInteger(), ::GetCurrentProcessId());
        QVERIFY(!event[u"foreground_fallback"_s].toBool());
    }

    void bitmapBeforeAnyCapture()
    {
        write({.imageData = encodeClipImage(m_image).data});
        const auto picture = read();
        QCOMPARE(picture.status, pin::ClipboardStatus::Image);
        QCOMPARE(picture.image.convertToFormat(QImage::Format_ARGB32), m_image.convertToFormat(QImage::Format_ARGB32));
    }

    void nativeBitmaps_data()
    {
        QTest::addColumn<bool>("v5");
        QTest::newRow("CF_DIB") << false;
        QTest::newRow("CF_DIBV5") << true;
    }

    void nativeBitmaps()
    {
        QFETCH(bool, v5);
        writeFormats({{v5 ? CF_DIBV5 : CF_DIB, dib(v5)}});
        const auto picture = read();
        QCOMPARE(picture.status, pin::ClipboardStatus::Image);
        QCOMPARE(picture.image.size(), QSize(2, 1));
        QCOMPARE(picture.image.pixelColor(0, 0), QColor(220, 60, 30));
    }

    void damagedV5FallsBackToDib()
    {
        writeFormats({{CF_DIBV5, QByteArray("damaged")}, {CF_DIB, dib(false)}});
        const auto picture = read();
        QCOMPARE(picture.status, pin::ClipboardStatus::Image);
        QCOMPARE(picture.image.size(), QSize(2, 1));
    }

    void asynchronousOwnCopyRetainsTheCapture()
    {
        write({.text = u"before capture"_s});
        const pin::LastCapture last {u"capture-a"_s, ::GetClipboardSequenceNumber()};
        QCOMPARE(read(last).status, pin::ClipboardStatus::LastCapture); // copy still pending
        write({.imageData = encodeClipImage(m_image).data, .imageOrigin = last.id});
        QVERIFY(::GetClipboardSequenceNumber() != last.clipboardSequence);
        QCOMPARE(read(last).status, pin::ClipboardStatus::LastCapture); // write completed later

        // Pixel equality is not provenance: an external copy of the same pixels
        // is a new clipboard picture, and an older capture's identity is too.
        write({.imageData = encodeClipImage(m_image).data});
        QCOMPARE(read(last).status, pin::ClipboardStatus::Image);
        write({.imageData = encodeClipImage(m_image).data, .imageOrigin = u"older-capture"_s});
        QCOMPARE(read(last).status, pin::ClipboardStatus::Image);
    }

    void externalCopyReplacesOwnImage()
    {
        const pin::LastCapture last {u"last-capture"_s, 0};
        write({.imageData = encodeClipImage(m_image).data, .imageOrigin = last.id});
        QCOMPARE(read(last).status, pin::ClipboardStatus::LastCapture);
        QImage other(17, 19, QImage::Format_ARGB32_Premultiplied);
        other.fill(Qt::blue);
        write({.imageData = encodeClipImage(other).data});
        const auto picture = read(last);
        QCOMPARE(picture.status, pin::ClipboardStatus::Image);
        QCOMPARE(picture.image.convertToFormat(QImage::Format_ARGB32), other.convertToFormat(QImage::Format_ARGB32));
        write({.text = u"text has no picture"_s});
        QCOMPARE(read(last).status, pin::ClipboardStatus::Empty); // caller may restore the capture
    }

    void qtFallbackCarriesTheSameOrigin()
    {
        auto* data = new QMimeData;
        data->setImageData(m_image);
        data->setData(QString::fromLatin1(kImageOriginMime), QByteArray("qt-fallback\0", 12));
        QGuiApplication::clipboard()->setMimeData(data);
        auto result = std::async(std::launch::async, [this] {
            return pin::readClipboard({u"qt-fallback"_s, 0});
        });
        // Pump Windows delayed-render messages while the worker reads Qt's offer.
        QTRY_VERIFY_WITH_TIMEOUT(result.wait_for(0ms) == std::future_status::ready, 3000);
        QCOMPARE(result.get().status, pin::ClipboardStatus::LastCapture);
        QGuiApplication::clipboard()->clear();
    }

    void copiedFilesUseTheProductionWicDecoder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString folder = dir.filePath(u"图片"_s);
        QVERIFY(QDir().mkpath(folder));
        const QString path = folder + u"/照片.tiff"_s;
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray tiff = QByteArray::fromBase64(
            "SUkqAAgAAAAKAAABBAABAAAAAQAAAAEBBAABAAAAAQAAAAIBAwADAAAAhgAAAAMBAwABAAAAAQAAAAYBAwABAAAAAgAAABEBBAABAAAAjAAAABUBAwABAAAAAwAAABYBBAABAAAAAQAAABcBBAABAAAAAwAAABwBAwABAAAAAQAAAAAAAAAIAAgACADcPB4=");
        QCOMPARE(file.write(tiff), tiff.size());
        file.close();
        const QString text = dir.filePath(u"first.txt"_s);
        QFile first(text);
        QVERIFY(first.open(QIODevice::WriteOnly));
        QVERIFY(first.write("not a picture") > 0);
        first.close();
        // No Qt image plugins: TIFF is exactly the format rejected by the PR's
        // canRead() gate. WIC must still read it, after skipping a non-image file.
        const QStringList paths = QCoreApplication::libraryPaths();
        QCoreApplication::setLibraryPaths({dir.path()});
        const auto restore = qScopeGuard([paths] { QCoreApplication::setLibraryPaths(paths); });
        QVERIFY(!QImageReader(path).canRead());
        write({.files = {text, path}});
        const auto picture = read();
        QCOMPARE(picture.status, pin::ClipboardStatus::Image);
        QCOMPARE(picture.image.size(), QSize(1, 1));
        QCOMPARE(picture.image.pixelColor(0, 0), QColor(220, 60, 30));
    }

    void malformedPictureDoesNotPretendToBeAnEmptyClipboard()
    {
        const HWND window = ::CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(window);
        const auto destroy = qScopeGuard([window] { ::DestroyWindow(window); });
        const HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, 16);
        QVERIFY(handle);
        void* bytes = ::GlobalLock(handle);
        QVERIFY(bytes);
        std::memset(bytes, 0, 16);
        ::GlobalUnlock(handle);
        QVERIFY(::OpenClipboard(window));
        ::EmptyClipboard();
        const bool set = ::SetClipboardData(::RegisterClipboardFormatW(L"PNG"), handle) != nullptr;
        ::CloseClipboard();
        QVERIFY(set);
        QCOMPARE(read().status, pin::ClipboardStatus::Unreadable);
    }

    void busyClipboardIsReported()
    {
        const HWND window = ::CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(window);
        const auto destroy = qScopeGuard([window] { ::DestroyWindow(window); });
        QVERIFY(::OpenClipboard(window));
        const auto close = qScopeGuard([] { ::CloseClipboard(); });
        QCOMPARE(read().status, pin::ClipboardStatus::Busy);
    }

    void placement_data()
    {
        QTest::addColumn<qreal>("scale");
        for (const qreal scale : {1.0, 1.25, 1.5, 1.75, 2.0})
            QTest::newRow(qPrintable(QString::number(scale))) << scale;
    }

    void placement()
    {
        QFETCH(qreal, scale);
        const QRect work(-qRound(1920 * scale), -qRound(1200 * scale), qRound(1920 * scale), qRound(1160 * scale));
        QImage large(4000, 3000, QImage::Format_RGB888);
        large.fill(Qt::green);
        // Its own pixels stay (#269): the pin shows a picture larger than the
        // screen fitted, it is not stored fitted. Its middle is at the cursor.
        for (const QPoint cursor : {work.center(), work.topLeft(), work.bottomRight()}) {
            const auto placed = pin::prepare(large, {cursor, work});
            QVERIFY(!placed.image.isNull());
            QCOMPARE(placed.image.size(), large.size());
            QCOMPARE(placed.origin, cursor - QPoint(large.width() / 2, large.height() / 2));
            QCOMPARE(placed.anchor, cursor);
            QCOMPARE(placed.image.format(), QImage::Format_RGB32);
        }
        const auto small = pin::prepare(m_image, {work.center(), work});
        QCOMPARE(small.image.size(), m_image.size());
        QCOMPARE(small.origin, work.center() - QPoint(m_image.width() / 2, m_image.height() / 2));
    }
};

int main(int argc, char** argv)
{
    const QString requested = qEnvironmentVariable("WINSHUN_PIN_TEST_STATION");
    if (requested.isEmpty()) {
        QCoreApplication runner(argc, argv);
        const HWINSTA original = ::GetProcessWindowStation();
        QString name = u"WinShun-PinTest-%1"_s.arg(::GetCurrentProcessId());
        HWINSTA station = ::CreateWindowStationW(qUtf16Printable(name), CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
        if (!station) {
            station = ::CreateWindowStationW(nullptr, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
            if (!station && ::GetLastError() == ERROR_ALREADY_EXISTS)
                station = ::CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
            wchar_t generated[256] {};
            DWORD size = 0;
            if (station && ::GetUserObjectInformationW(station, UOI_NAME, generated, sizeof generated, &size))
                name = QString::fromWCharArray(generated);
        }
        // Non-administrators share the logon session's noninteractive station.
        // Serialize tests there, use a unique desktop, and require an empty
        // clipboard in the child. Never borrow WinSta0 or replace existing data.
        USEROBJECTFLAGS stationFlags {};
        DWORD size = 0;
        if (!station || name.compare(u"WinSta0"_s, Qt::CaseInsensitive) == 0
            || !::GetUserObjectInformationW(station, UOI_FLAGS, &stationFlags, sizeof stationFlags, &size)
            || (stationFlags.dwFlags & WSF_VISIBLE)) {
            qCritical("A noninteractive clipboard station is required");
            if (station)
                ::CloseWindowStation(station);
            return 2;
        }
        const QString mutexName = u"Local\\WinShun.PinClipboardTest.%1"_s.arg(name);
        const HANDLE mutex = ::CreateMutexW(nullptr, FALSE, qUtf16Printable(mutexName));
        if (!mutex)
            return 2;
        const DWORD waited = ::WaitForSingleObject(mutex, 5000);
        const auto unlock = qScopeGuard([mutex, waited] {
            if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED)
                ::ReleaseMutex(mutex);
            ::CloseHandle(mutex);
        });
        if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED)
            return 2;
        const QString desktopName = u"PinClipboardTest-%1"_s.arg(::GetCurrentProcessId());
        HDESK desktop = nullptr;
        if (station && ::SetProcessWindowStation(station))
            desktop = ::CreateDesktopW(qUtf16Printable(desktopName), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        ::SetProcessWindowStation(original);
        const auto cleanup = qScopeGuard([&] {
            if (desktop)
                ::CloseDesktop(desktop);
            if (station)
                ::CloseWindowStation(station);
        });
        if (!desktop) {
            qCritical() << "Test desktop creation failed:" << ::GetLastError();
            return 2; // never fall back to the user's clipboard
        }
        QProcess child;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(u"WINSHUN_PIN_TEST_STATION"_s, name);
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        const std::wstring target = (name + u'\\' + desktopName).toStdWString();
        child.setCreateProcessArgumentsModifier([&](QProcess::CreateProcessArguments* arguments) {
            arguments->startupInfo->lpDesktop = const_cast<wchar_t*>(target.c_str());
        });
        child.start(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
        if (!child.waitForStarted(5000) || !child.waitForFinished(60000))
            return 2;
        return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : 2;
    }
    wchar_t name[256] {};
    DWORD size = 0;
    USEROBJECTFLAGS flags {};
    if (!::GetUserObjectInformationW(::GetProcessWindowStation(), UOI_NAME, name, sizeof name, &size)
        || requested != QString::fromWCharArray(name) || requested.compare(u"WinSta0"_s, Qt::CaseInsensitive) == 0
        || !::GetUserObjectInformationW(::GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof flags, &size)
        || (flags.dwFlags & WSF_VISIBLE))
        return 2;
    QGuiApplication app(argc, argv);
    if (!::OpenClipboard(nullptr)) {
        qCritical() << "Test clipboard unavailable:" << ::GetLastError();
        return 2;
    }
    const bool empty = ::CountClipboardFormats() == 0;
    ::CloseClipboard();
    if (!empty) {
        qCritical("The noninteractive clipboard contains existing data; leaving it intact");
        return 2;
    }
    TestPinClipboard test;
    const int result = QTest::qExec(&test, argc, argv);
    QGuiApplication::clipboard()->clear();
    if (::OpenClipboard(nullptr)) {
        ::EmptyClipboard();
        ::CloseClipboard();
    }
    return result;
}

#include "tst_pinclipboard.moc"
