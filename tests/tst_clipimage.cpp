#include "ClipFiles.h"
#include "ClipImage.h"
#include "ClipThumbnailProvider.h"
#include "ColorSwatch.h"
#include "PhosphorGlyph.h"
#include "SystemTheme.h"
#include "TextLineSpacing.h"
#include "platform/ClipboardWatcher.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImageReader>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickTextureFactory>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <windows.h>
#include <shlobj.h> // DROPFILES
#include <wincodec.h>
#include <wrl/client.h>
#undef small // rpcndr.h's, for MIDL: a name below

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

using namespace ws;
using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace {

// A JPEG XR file of `image`, by Windows Imaging Component: a picture Windows
// reads and no Qt image plugin does (as WebP is on a user's PC, where Win顺
// ships none for it).
bool writeJpegXr(const QImage& image, const QString& path)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto uninitialize = qScopeGuard([com] {
        if (SUCCEEDED(com))
            ::CoUninitialize();
    });
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32); // BGRA in memory
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))
        && SUCCEEDED(wic->CreateStream(&stream))
        && SUCCEEDED(stream->InitializeFromFilename(path.toStdWString().c_str(), GENERIC_WRITE))
        && SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &encoder))
        && SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
        && SUCCEEDED(encoder->CreateNewFrame(&frame, &options)) && SUCCEEDED(frame->Initialize(options.Get()))
        && SUCCEEDED(frame->SetSize(static_cast<UINT>(pixels.width()), static_cast<UINT>(pixels.height())))
        && SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA
        && SUCCEEDED(frame->WritePixels(static_cast<UINT>(pixels.height()), static_cast<UINT>(pixels.bytesPerLine()),
            static_cast<UINT>(pixels.sizeInBytes()), const_cast<BYTE*>(pixels.constBits())))
        && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

QByteArray fixture(const QString& name)
{
    QFile file(QStringLiteral(WINSHUN_TEST_DATA "/") + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray still(const char* format)
{
    QImage image(16, 10, QImage::Format_ARGB32);
    image.fill(qRgba(35, 120, 210, 255));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, format) ? bytes : QByteArray();
}

bool putGlobal(UINT format, const void* bytes, std::size_t size)
{
    HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, size);
    void* data = handle ? ::GlobalLock(handle) : nullptr;
    if (!data) {
        if (handle)
            ::GlobalFree(handle);
        return false;
    }
    std::memcpy(data, bytes, size);
    ::GlobalUnlock(handle);
    if (::SetClipboardData(format, handle))
        return true;
    ::GlobalFree(handle);
    return false;
}

// Files copied by `owner`, as a file manager copies them: CF_HDROP, no text.
bool copyFiles(HWND owner, const QStringList& paths)
{
    QByteArray drop(sizeof(DROPFILES), '\0');
    auto* header = reinterpret_cast<DROPFILES*>(drop.data());
    header->pFiles = sizeof(DROPFILES);
    header->fWide = TRUE;
    for (const QString& path : paths) {
        const QString native = QDir::toNativeSeparators(path);
        drop.append(reinterpret_cast<const char*>(native.utf16()), (native.size() + 1) * 2);
    }
    drop.append(2, '\0');
    if (!::OpenClipboard(owner))
        return false;
    ::EmptyClipboard();
    const bool ok = putGlobal(CF_HDROP, drop.constData(), static_cast<std::size_t>(drop.size()));
    ::CloseClipboard();
    return ok;
}

// Another program putting back what it borrowed the clipboard from, as
// 有道词典's 划词 does: its own window owns the write, and it stays running.
int putBack(const QString& text)
{
    const HWND owner = ::CreateWindowExW(
        0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    bool opened = false;
    for (int attempt = 0; owner && attempt < 50 && !opened; ++attempt) {
        opened = ::OpenClipboard(owner);
        if (!opened)
            ::Sleep(20);
    }
    if (!opened)
        return 2;
    ::EmptyClipboard();
    const bool ok = putGlobal(CF_UNICODETEXT, text.utf16(), static_cast<std::size_t>(text.size() + 1) * 2);
    ::CloseClipboard();
    ::Sleep(2000); // the owner stays while the watcher looks at it
    ::DestroyWindow(owner);
    return ok ? 0 : 3;
}

QByteArray clipboardBytes(const wchar_t* name)
{
    if (!::OpenClipboard(nullptr))
        return {};
    const auto close = qScopeGuard([] { ::CloseClipboard(); });
    const HANDLE handle = ::GetClipboardData(::RegisterClipboardFormatW(name));
    const void* data = handle ? ::GlobalLock(handle) : nullptr;
    if (!data)
        return {};
    const QByteArray bytes(static_cast<const char*>(data), static_cast<qsizetype>(::GlobalSize(handle)));
    ::GlobalUnlock(handle);
    return bytes;
}

} // namespace

QQuickItem* itemNamed(QQuickItem* root, const QString& name)
{
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems())
        if (QQuickItem* found = itemNamed(child, name))
            return found;
    return nullptr;
}

class ClipImageTest : public QObject {
    Q_OBJECT

public:
    explicit ClipImageTest(bool isolated)
        : m_isolated(isolated)
    {
    }

private slots:
    void initTestCase()
    {
        const QUrl folder = QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR "/"));
        qmlRegisterSingletonType<SystemTheme>(
            "WinShun", 1, 0, "SystemTheme", [](QQmlEngine*, QJSEngine*) -> QObject* { return new SystemTheme; });
        qmlRegisterSingletonType(folder.resolved(QUrl(u"Theme.qml"_s)), "WinShun", 1, 0, "Theme");
        qmlRegisterType<ColorSwatch>("WinShun", 1, 0, "ColorSwatch");
        qmlRegisterType<PhosphorGlyph>("WinShun", 1, 0, "PhosphorGlyph");
        qmlRegisterType<TextLineSpacing>("WinShun", 1, 0, "TextLineSpacing"); // ClipPreview's selectable text
    }

    void codecsAndOriginals_data()
    {
        QTest::addColumn<QByteArray>("encoded");
        QTest::addColumn<ClipImageFormat>("format");
        QTest::addColumn<bool>("animated");
        QTest::newRow("png") << still("PNG") << ClipImageFormat::Png << false;
        QTest::newRow("jpeg") << still("JPEG") << ClipImageFormat::Jpeg << false;
        QTest::newRow("bmp") << still("BMP") << ClipImageFormat::Png << false;
        QTest::newRow("gif") << fixture(u"clipboard-animated.gif"_s) << ClipImageFormat::Gif << true;
    }

    void codecsAndOriginals()
    {
        QFETCH(QByteArray, encoded);
        QFETCH(ClipImageFormat, format);
        QFETCH(bool, animated);
        QVERIFY(!encoded.isEmpty());
        const auto decoded = decodeClipImage(encoded);
        QVERIFY(!decoded.firstFrame.isNull());
        QCOMPARE(decoded.format, format);
        QCOMPARE(decoded.animated, animated);
        QVERIFY(!decoded.hash.isEmpty());
        if (!encoded.startsWith("BM"))
            QCOMPARE(decoded.data, encoded);
        else
            QVERIFY(decoded.data.startsWith("\x89PNG"));

        QTemporaryDir dir;
        ClipStore store(dir.filePath(u"history"_s));
        QVERIFY(store.open(1'790'000'000'000));
        ClipCapture capture;
        capture.kind = ClipKind::Image;
        capture.imageData = decoded.data;
        capture.imageFormat = decoded.format;
        capture.animated = decoded.animated;
        capture.hash = decoded.hash;
        capture.width = decoded.firstFrame.width();
        capture.height = decoded.firstFrame.height();
        const qint64 id = store.add(capture, 1'790'000'000'000);
        QVERIFY(id > 0);
        const QString path = store.payload(id).imagePath;
        QVERIFY(path.endsWith(u'.' + clipImageExtension(format)));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), decoded.data);
        file.close();
        auto records = store.remove(std::span(&id, 1));
        QVERIFY(!store.clip(id));
        QVERIFY(QFile::exists(path));
        store.restore(std::move(records));
        QCOMPARE(store.clip(id)->imageFormat, format);
        QCOMPARE(store.clip(id)->animated, animated);
        store.close();
        QVERIFY(store.open(1'790'000'000'000));
        QCOMPARE(store.clip(id)->imageFormat, format);
        QCOMPARE(store.clip(id)->animated, animated);
        QCOMPARE(store.payload(id).imagePath, path);
        const auto exported = clipfiles::images(store, {store.clip(id)}, dir.filePath(u"export"_s), u"图片"_s);
        QCOMPARE(exported.error, clipfiles::Error::None);
        QCOMPARE(exported.paths.size(), 1);
        QFile copy(exported.paths.front());
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), decoded.data);
        copy.close();
        QCOMPARE(store.clearHistory(), 1);
        store.close(); // wait for the asynchronous file removal
        QVERIFY(!QFile::exists(path));
    }

    void animationIdentityUsesEveryFrame()
    {
        const auto first = decodeClipImage(fixture(u"clipboard-animated.gif"_s));
        const auto other = decodeClipImage(fixture(u"clipboard-other-animation.gif"_s));
        QVERIFY(first.animated && other.animated);
        QCOMPARE(first.firstFrame, other.firstFrame);
        QVERIFY(first.hash != other.hash);
        QCOMPARE(first.hash, decodeClipImage(first.data).hash);
        QVERIFY(first.hash != imagePixelHash(first.firstFrame));
        QCOMPARE(decodeClipImage(still("PNG")).hash, decodeClipImage(still("BMP")).hash);
    }

    void rotatedPhotosAndBadInputs()
    {
        const QByteArray jpeg = fixture(u"clipboard-oriented.jpg"_s);
        const auto rotated = decodeClipImage(jpeg);
        QCOMPARE(rotated.firstFrame.size(), QSize(10, 16));
        QCOMPARE(rotated.data, jpeg);
        for (const QByteArray& bytes : {QByteArray(), QByteArray("GIF89a"), QByteArray("not an image"),
                 QByteArray("<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'/>")})
            QVERIFY(decodeClipImage(bytes).firstFrame.isNull());
        const auto rejected = decodeClipImage(fixture(u"clipboard-oversized.png"_s));
        QVERIFY(rejected.tooBig);
        QVERIFY(rejected.firstFrame.isNull());
    }

    void nativeCaptureAndCopy_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("gif") << u"GIF"_s << fixture(u"clipboard-animated.gif"_s);
        QTest::newRow("gif-mime") << u"image/gif"_s << fixture(u"clipboard-animated.gif"_s);
        QTest::newRow("jpeg") << u"JFIF"_s << still("JPEG");
        QTest::newRow("jpeg-mime") << u"image/jpeg"_s << still("JPEG");
        QTest::newRow("png") << u"PNG"_s << still("PNG");
        QTest::newRow("bmp-mime") << u"image/bmp"_s << still("BMP");
        QTest::newRow("damaged-gif-falls-back") << u"GIF"_s << QByteArray("broken GIF");
    }

    void nativeCaptureAndCopy()
    {
        if (!m_isolated) {
            const QString reason
                = u"Private clipboard unavailable (Windows error %1); the user's clipboard is never used"_s.arg(
                    qEnvironmentVariable("WINSHUN_TEST_CLIP_IMAGE_ERROR"));
            QSKIP(qPrintable(reason));
        }
        QFETCH(QString, name);
        QFETCH(QByteArray, bytes);
        const HWND owner = ::CreateWindowExW(
            0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(owner);
        const auto destroy = qScopeGuard([owner] { ::DestroyWindow(owner); });
        const auto put = [](UINT format, const QByteArray& value) {
            HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(value.size()));
            if (!handle)
                return false;
            void* data = ::GlobalLock(handle);
            if (!data) {
                ::GlobalFree(handle);
                return false;
            }
            std::memcpy(data, value.constData(), static_cast<std::size_t>(value.size()));
            ::GlobalUnlock(handle);
            if (::SetClipboardData(format, handle))
                return true;
            ::GlobalFree(handle);
            return false;
        };
        QVERIFY(::OpenClipboard(owner));
        ::EmptyClipboard();
        ::CloseClipboard();
        QList<ClipCapture> captures;
        ClipboardWatcher watcher({.record = true}, {.captured = [this, &captures](ClipCapture capture) {
            QMetaObject::invokeMethod(
                this, [&captures, capture = std::move(capture)] { captures.append(capture); }, Qt::QueuedConnection);
        }});
        QVERIFY(watcher.listening());
        QVERIFY(::OpenClipboard(owner));
        ::EmptyClipboard();
        const bool offered = put(::RegisterClipboardFormatW(qUtf16Printable(name)), bytes);
        // Simulate programs offering a flattened preview alongside the original.
        const QByteArray png = still("PNG");
        const bool fallback = name == u"PNG" || name == u"image/bmp" || put(::RegisterClipboardFormatW(L"PNG"), png);
        ::CloseClipboard();
        QVERIFY(offered && fallback);
        QTRY_COMPARE_WITH_TIMEOUT(captures.size(), 1, 3000);
        const auto& capture = captures.front();
        const auto decoded = decodeClipImage(bytes.startsWith("broken") ? png : bytes);
        QCOMPARE(capture.imageData, decoded.data);
        QCOMPARE(capture.imageFormat, decoded.format);
        QCOMPARE(capture.animated, decoded.animated);
        QCOMPARE(capture.hash, decoded.hash);
        watcher.setOptions({});

        std::atomic<int> copied = -1;
        watcher.write({.imageData = capture.imageData}, [&](bool ok) { copied = ok ? 1 : 0; });
        QTRY_COMPARE_WITH_TIMEOUT(copied.load(), 1, 3000);
        const wchar_t* native = decoded.format == ClipImageFormat::Gif ? L"GIF"
            : decoded.format == ClipImageFormat::Jpeg                  ? L"JFIF"
                                                                       : L"PNG";
        QCOMPARE(clipboardBytes(native), decoded.data);
        QVERIFY(!decodeClipImage(clipboardBytes(L"image/png")).firstFrame.isNull());
    }

    // 有道词典's 划词 presses Ctrl+C in the program in front; files copied
    // there (no text) are the clipboard's for about a second, then 有道 puts
    // back what was there before, as its own write. Only that is recorded.
    void copyPutBackByAnotherProgramIsDropped()
    {
        if (!m_isolated)
            QSKIP("Private clipboard unavailable; the user's clipboard is never used");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(u"borrowed.md"_s);
        QVERIFY(QFile(file).open(QIODevice::WriteOnly));
        const HWND owner = ::CreateWindowExW(
            0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(owner);
        const auto destroy = qScopeGuard([owner] { ::DestroyWindow(owner); });
        QVERIFY(::OpenClipboard(owner));
        ::EmptyClipboard();
        ::CloseClipboard();
        QList<ClipCapture> captures;
        ClipboardWatcher watcher({.record = true}, {.captured = [this, &captures](ClipCapture capture) {
            QMetaObject::invokeMethod(
                this, [&captures, capture = std::move(capture)] { captures.append(capture); }, Qt::QueuedConnection);
        }});
        QVERIFY(watcher.listening());
        QTest::qWait(200); // started: what it finds there already is not held (ClipCapture::initial)

        QVERIFY(copyFiles(owner, {file}));
        QTest::qWait(400); // read by now (40 ms), and held
        QCOMPARE(captures.size(), 0);
        QProcess other;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"WINSHUN_TEST_CLIP_PUT_BACK"_s, u"what was there before"_s);
        other.setProcessEnvironment(environment);
        other.start(QCoreApplication::applicationFilePath(), {});
        QVERIFY(other.waitForStarted(5000));
        QTRY_COMPARE_WITH_TIMEOUT(captures.size(), 1, 3000);
        QCOMPARE(captures.front().kind, ClipKind::Text);
        QCOMPARE(captures.front().text, u"what was there before"_s);
        QTest::qWait(1800); // past the hold: the files never come
        QCOMPARE(captures.size(), 1);
        QVERIFY(other.waitForFinished(5000));
        QCOMPARE(other.exitCode(), 0);
    }

    // Files or a picture copied by the user: held a moment, then kept; at
    // once when the program copies again, or the history is looked at.
    void heldCopyIsKept()
    {
        if (!m_isolated)
            QSKIP("Private clipboard unavailable; the user's clipboard is never used");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QStringList files;
        for (const QString& name : {u"a.png"_s, u"b.pdf"_s, u"c.md"_s}) {
            files.append(dir.filePath(name));
            QVERIFY(QFile(files.back()).open(QIODevice::WriteOnly));
        }
        const HWND owner = ::CreateWindowExW(
            0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(owner);
        const auto destroy = qScopeGuard([owner] { ::DestroyWindow(owner); });
        QVERIFY(::OpenClipboard(owner));
        ::EmptyClipboard();
        ::CloseClipboard();
        QList<ClipCapture> captures;
        ClipboardWatcher watcher({.record = true}, {.captured = [this, &captures](ClipCapture capture) {
            QMetaObject::invokeMethod(
                this, [&captures, capture = std::move(capture)] { captures.append(capture); }, Qt::QueuedConnection);
        }});
        QVERIFY(watcher.listening());
        QTest::qWait(200); // started: what it finds there already is not held (ClipCapture::initial)

        // Nobody puts it back: kept after the hold.
        QVERIFY(copyFiles(owner, {files[0]}));
        QTest::qWait(700);
        QCOMPARE(captures.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(captures.size(), 1, 3000);
        QCOMPARE(captures[0].kind, ClipKind::Files);
        QCOMPARE(captures[0].text, QDir::toNativeSeparators(files[0]));

        // Copied again by the same program before the hold is over: the
        // first is kept at once, the second held in its turn.
        QVERIFY(copyFiles(owner, {files[1]}));
        QTest::qWait(200);
        QVERIFY(copyFiles(owner, {files[2]}));
        QTRY_COMPARE_WITH_TIMEOUT(captures.size(), 2, 600);
        QCOMPARE(captures[1].text, QDir::toNativeSeparators(files[1]));
        // The history looked at: the held one goes in now.
        watcher.recordHeldNow();
        QTRY_COMPARE_WITH_TIMEOUT(captures.size(), 3, 600);
        QCOMPARE(captures[2].text, QDir::toNativeSeparators(files[2]));
        // Win顺 writing the clipboard (a paste from the history, never read
        // back): the held one goes in first.
        QVERIFY(copyFiles(owner, {files[0]}));
        QTest::qWait(200);
        std::atomic<int> written = -1;
        watcher.write({.text = u"pasted from the history"_s}, [&](bool ok) { written = ok ? 1 : 0; });
        QTRY_COMPARE_WITH_TIMEOUT(written.load(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(captures.size(), 4, 600);
        QCOMPARE(captures[3].text, QDir::toNativeSeparators(files[0]));
        QTest::qWait(300);
        QCOMPARE(captures.size(), 4); // its own write is not recorded as a copy

        // Text is never held.
        QVERIFY(::OpenClipboard(owner));
        ::EmptyClipboard();
        const QString text = u"copied words"_s;
        QVERIFY(putGlobal(CF_UNICODETEXT, text.utf16(), static_cast<std::size_t>(text.size() + 1) * 2));
        ::CloseClipboard();
        QTRY_COMPARE_WITH_TIMEOUT(captures.size(), 5, 600);
        QCOMPARE(captures[4].text, text);
    }

    // The rows' thumbnails: the size and pixels Qt itself makes of a file
    // for a sourceSize, kept for the next request, errors as Qt reports them.
    void clipThumbnailsMatchQtAndAreKept()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(u"截图 #1 100%.png"_s);
        QImage picture(1380, 1080, QImage::Format_RGB32);
        for (int y = 0; y < picture.height(); ++y) {
            for (int x = 0; x < picture.width(); ++x)
                picture.setPixel(x, y, qRgb(x % 256, y % 256, (x * y) % 256));
        }
        QVERIFY(picture.save(path, "PNG"));
        QImage small(40, 30, QImage::Format_RGB32);
        small.fill(Qt::red);
        const QString smallPath = dir.filePath(u"small.png"_s);
        QVERIFY(small.save(smallPath));
        const QString broken = dir.filePath(u"broken.png"_s);
        QFile file(broken);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not an image");
        file.close();

        QQmlEngine engine;
        engine.addImageProvider(u"clipthumb"_s, new ClipThumbnailProvider);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nImage { asynchronous: true; sourceSize.width: 162; sourceSize.height: 102 }", QUrl());
        const auto shown = [&](const QString& source) {
            auto* image = qobject_cast<QQuickItem*>(component.create());
            image->setProperty("source", source);
            return std::unique_ptr<QQuickItem>(image);
        };
        // As QQuickPixmap reads the file itself for that sourceSize.
        QImageReader reader(path);
        reader.setAutoTransform(true);
        reader.setScaledSize(QSize(130, 102)); // fits 102 high, the aspect kept
        const QImage expected = reader.read();
        const QString url = ClipThumbnailProvider::url(path, 7);
        auto first = shown(url);
        QTRY_COMPARE(first->property("status").toInt(), 1); // Image.Ready
        QCOMPARE(QSizeF(first->implicitWidth(), first->implicitHeight()), QSizeF(130, 102));
        // Kept: asked again (a row scrolled back to), it is there at once,
        // before the request returns, the pixels Qt would have made.
        ClipThumbnailProvider provider;
        std::unique_ptr<QQuickImageResponse> again(
            provider.requestImageResponse(url.mid(QStringLiteral("image://clipthumb/").size()), QSize(162, 102)));
        std::unique_ptr<QQuickTextureFactory> factory(again->textureFactory());
        QVERIFY(factory);
        QCOMPARE(factory->image().convertToFormat(QImage::Format_RGB32), expected.convertToFormat(QImage::Format_RGB32));
        // Smaller than the slot: as it is, never enlarged.
        auto smallImage = shown(ClipThumbnailProvider::url(smallPath));
        QTRY_COMPARE(smallImage->property("status").toInt(), 1);
        QCOMPARE(QSizeF(smallImage->implicitWidth(), smallImage->implicitHeight()), QSizeF(40, 30));
        // Not a picture: an error naming the file, as Qt's own.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u".*Error decoding: .*broken\\.png.*"_s));
        auto none = shown(ClipThumbnailProvider::url(broken));
        QTRY_COMPARE(none->property("status").toInt(), 3); // Image.Error
        // The address: any path, and a file written again gets another one.
        QVERIFY(ClipThumbnailProvider::url(path, 1) != ClipThumbnailProvider::url(path, 2));
        QVERIFY(!url.contains(u'#') && !url.contains(u'%'));
    }

    // A picture copied as a file that no Qt decoder Win顺 ships reads (WebP,
    // HEIC ... on a user's PC: GitHub #373): its thumbnail by Windows' own
    // decoder, fitted to the slot as Qt's would be, not the file's icon.
    void clipThumbnailsOfPicturesQtCannotRead()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(u"照片 1.jxr"_s);
        QImage picture(400, 300, QImage::Format_ARGB32);
        picture.fill(qRgb(30, 144, 255));
        QVERIFY(writeJpegXr(picture, path));
        QVERIFY(!QImageReader(path).canRead()); // what makes it a test of Windows' decoder

        QQmlEngine engine;
        engine.addImageProvider(u"clipthumb"_s, new ClipThumbnailProvider);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nImage { asynchronous: true; sourceSize.width: 162; sourceSize.height: 102 }", QUrl());
        std::unique_ptr<QQuickItem> image(qobject_cast<QQuickItem*>(component.create()));
        QVERIFY(image);
        image->setProperty("source", ClipThumbnailProvider::url(path, 3));
        QTRY_COMPARE(image->property("status").toInt(), 1); // Image.Ready
        QCOMPARE(QSizeF(image->implicitWidth(), image->implicitHeight()), QSizeF(136, 102)); // fits 102 high

        ClipThumbnailProvider provider; // the pixels, kept from that request
        std::unique_ptr<QQuickImageResponse> again(provider.requestImageResponse(
            ClipThumbnailProvider::url(path, 3).mid(QStringLiteral("image://clipthumb/").size()), QSize(162, 102)));
        std::unique_ptr<QQuickTextureFactory> factory(again->textureFactory());
        QVERIFY(factory);
        const QColor middle = factory->image().pixelColor(68, 51);
        QVERIFY2(qAbs(middle.red() - 30) <= 8 && qAbs(middle.green() - 144) <= 8 && qAbs(middle.blue() - 255) <= 8,
            qPrintable(middle.name()));
    }

    // Qt may delete a response before its job is done: the engine that asked
    // is gone (a window's, each of these tests'), or it no longer wants the
    // picture. The job then has no one to give it to and touches nothing of
    // what was deleted (it wrote into it: a crash on a pool thread); it keeps
    // the picture for the next time.
    void clipThumbnailOutlivesItsResponse()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(u"大图.png"_s);
        QImage picture(3000, 2000, QImage::Format_RGB32);
        picture.fill(qRgb(200, 80, 40));
        QVERIFY(picture.save(path, "PNG"));
        ClipThumbnailProvider provider;
        const auto id = [&](int stamp) {
            return ClipThumbnailProvider::url(path, stamp).mid(QStringLiteral("image://clipthumb/").size());
        };
        constexpr int kFirst = 100, kAsked = 12;
        for (int stamp = kFirst; stamp < kFirst + kAsked; ++stamp)
            delete provider.requestImageResponse(id(stamp), QSize(162, 102)); // gone before its picture is read
        const auto allKept = [&] {
            for (int stamp = kFirst; stamp < kFirst + kAsked; ++stamp) {
                std::unique_ptr<QQuickImageResponse> again(provider.requestImageResponse(id(stamp), QSize(162, 102)));
                if (!std::unique_ptr<QQuickTextureFactory>(again->textureFactory()))
                    return false;
            }
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(allKept(), 20000);
    }

    void imageFileRowLoadsAtBothSizesAndReusesItsDelegate()
    {
        QTest::failOnWarning(QRegularExpression(u"TypeError|ReferenceError|Cannot assign|Binding loop"_s));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(u"手机截图 #+ 100%.JPG"_s);
        QImage phone(900, 1600, QImage::Format_RGB32);
        phone.fill(QColor(35, 120, 210));
        QVERIFY(phone.save(path, "JPEG"));
        const QUrl folder = QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR "/"));
        QQmlEngine engine;
        engine.addImageProvider(u"clipthumb"_s, new ClipThumbnailProvider);
        QQmlComponent component(&engine, folder.resolved(QUrl(u"ClipRow.qml"_s)));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.createWithInitialProperties({
            {u"index"_s, 0}, {u"clipId"_s, 1}, {u"kind"_s, 3},
            {u"title"_s, u"手机截图 #+ 100%.JPG"_s}, {u"detail"_s, u"Weixin · 刚刚"_s},
            {u"icon"_s, QUrl::fromLocalFile(path).toString()},
            {u"image"_s, ClipThumbnailProvider::url(path)}, {u"swatch"_s, QString()},
            {u"order"_s, 0}, {u"groupName"_s, QString()}, {u"pinned"_s, false},
            {u"missing"_s, false}, {u"movable"_s, false}, {u"afterPinned"_s, false}, {u"screenshot"_s, false}, {u"pasted"_s, false}, {u"fileCount"_s, 1},
        }));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* row = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(row);
        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
        window.setProperty("_q_showWithoutActivating", true);
        const int themeType = qmlTypeId("WinShun", 1, 0, "Theme");
        window.setColor(engine.singletonInstance<QObject*>(themeType)->property("background").value<QColor>());
        window.resize(760, 142);
        row->setParentItem(window.contentItem());
        row->setSize(QSizeF(760, 142));
        window.show();
        auto* thumbnail = row->findChild<QQuickItem*>(u"clipboardThumbnail"_s);
        auto* icon = row->findChild<QQuickItem*>(u"clipboardFileIcon"_s);
        QVERIFY(thumbnail);
        QVERIFY(icon);
        QVERIFY(row->property("picture").toBool());
        for (bool large : {false, true}) {
            row->setProperty("pictureSize", large ? u"large"_s : u"standard"_s);
            QTRY_COMPARE(thumbnail->property("status").toInt(), 1); // Image.Ready
            QVERIFY(thumbnail->isVisible());
            QVERIFY(!icon->isVisible());
            const qreal height = large ? 130 : 68;
            QVERIFY(qAbs(thumbnail->height() - height) <= 1);
            QVERIFY(qAbs(thumbnail->width() / thumbnail->height() - 900.0 / 1600) < 0.02);
        }
        const QImage rendered = window.grabWindow();
        QVERIFY(!rendered.isNull());
        const QPointF center = thumbnail->mapToScene(QPointF(thumbnail->width() / 2, thumbnail->height() / 2));
        const QColor pixel = rendered.pixelColor(qRound(center.x() * window.devicePixelRatio()),
            qRound(center.y() * window.devicePixelRatio()));
        QVERIFY(qAbs(pixel.red() - 35) <= 2 && qAbs(pixel.green() - 120) <= 2 && qAbs(pixel.blue() - 210) <= 2);
        const QString screenshot = qEnvironmentVariable("WINSHUN_CLIP_ROW_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(rendered.save(screenshot));
        // "small": in an icon's place, the row no higher than a text's (GitHub #328).
        row->setProperty("pictureSize", u"small"_s);
        QTRY_VERIFY(thumbnail->property("status").toInt() == 1 && thumbnail->height() <= 32 && thumbnail->width() <= 32);
        QVERIFY(thumbnail->isVisible());
        QVERIFY(qAbs(thumbnail->width() / thumbnail->height() - 900.0 / 1600) < 0.08);
        if (!screenshot.isEmpty())
            QVERIFY(window.grabWindow().save(screenshot + u".small.png"_s));
        // Several pictures copied as files: how many, on the thumbnail; not on the small one.
        auto* count = row->findChild<QQuickItem*>(u"clipboardPictureCount"_s);
        QVERIFY(count);
        row->setProperty("fileCount", 3);
        QVERIFY(!count->isVisible());
        row->setProperty("pictureSize", u"standard"_s);
        QTRY_VERIFY(count->isVisible());
        const QRectF badge = count->mapRectToItem(thumbnail, QRectF(QPointF(), count->size()));
        QVERIFY(QRectF(QPointF(), thumbnail->size()).contains(badge));
        if (!screenshot.isEmpty())
            QVERIFY(window.grabWindow().save(screenshot + u".count.png"_s));
        row->setProperty("fileCount", 1);
        QVERIFY(!count->isVisible());
        // A broken image keeps its file icon; changing the reused row clears the old picture.
        const QString broken = dir.filePath(u"broken.jpg"_s);
        QFile file(broken);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not an image");
        file.close();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u".*Error decoding:.*"_s));
        row->setProperty("image", ClipThumbnailProvider::url(broken));
        QTRY_COMPARE(thumbnail->property("status").toInt(), 3); // Image.Error
        QTRY_COMPARE(icon->property("status").toInt(), 1);
        QVERIFY(icon->isVisible());
        QVERIFY(!thumbnail->isVisible());
        QCOMPARE(icon->size(), QSizeF(32, 32));
        row->setProperty("image", QString());
        QVERIFY(!row->property("picture").toBool());
        QCOMPARE(thumbnail->property("status").toInt(), 0); // Image.Null
        QVERIFY(icon->isVisible());
        row->setProperty("kind", 0);
        QVERIFY(!icon->isVisible());
        row->setProperty("kind", 4);
        QVERIFY(row->property("picture").toBool()); // an owned image being written still reserves its row
    }

    // Text with a formula (GitHub #367): its look in the picture's place on
    // a picture's taller row, the text's sign not showing around it; plain
    // text keeps its sign.
    void textLookRow()
    {
        QTest::failOnWarning(QRegularExpression(u"TypeError|ReferenceError|Cannot assign|Binding loop"_s));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(u"7.png"_s);
        QImage look(330, 96, QImage::Format_RGB32); // three times a formula of 110 x 32
        look.fill(Qt::white);
        for (int y = 30; y < 66; ++y)
            std::fill_n(reinterpret_cast<QRgb*>(look.scanLine(y)) + 40, 250, qRgb(0, 0, 0));
        QVERIFY(look.save(path, "PNG"));
        const QUrl folder = QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR "/"));
        QQmlEngine engine;
        engine.addImageProvider(u"clipthumb"_s, new ClipThumbnailProvider);
        QQmlComponent component(&engine, folder.resolved(QUrl(u"ClipRow.qml"_s)));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.createWithInitialProperties({
            {u"index"_s, 0}, {u"clipId"_s, 7}, {u"kind"_s, 0},
            {u"title"_s, u"E=mc2"_s}, {u"detail"_s, u"WPS Office · 刚刚 · 5 字"_s},
            {u"icon"_s, QString()}, {u"image"_s, ClipThumbnailProvider::url(path, 1)}, {u"swatch"_s, QString()},
            {u"order"_s, 0}, {u"groupName"_s, QString()}, {u"pinned"_s, false},
            {u"missing"_s, false}, {u"movable"_s, false}, {u"afterPinned"_s, false}, {u"screenshot"_s, false}, {u"pasted"_s, false}, {u"fileCount"_s, 0},
        }));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* row = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(row);
        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
        window.setProperty("_q_showWithoutActivating", true);
        const int themeType = qmlTypeId("WinShun", 1, 0, "Theme");
        const QColor background = engine.singletonInstance<QObject*>(themeType)->property("background").value<QColor>();
        window.setColor(background);
        window.resize(760, 80);
        row->setParentItem(window.contentItem());
        row->setSize(QSizeF(760, 80));
        window.show();
        auto* thumbnail = row->findChild<QQuickItem*>(u"clipboardThumbnail"_s);
        auto* tile = row->findChild<QQuickItem*>(u"clipboardTextTile"_s);
        auto* slot = row->findChild<QQuickItem*>(u"clipboardIconSlot"_s);
        QVERIFY(thumbnail && tile && slot);
        QVERIFY(row->property("picture").toBool());
        QTRY_COMPARE(thumbnail->property("status").toInt(), 1); // Image.Ready
        QVERIFY(thumbnail->isVisible());
        QVERIFY(!tile->isVisible());
        QCOMPARE(slot->width(), 108.0); // a picture's slot ...
        QVERIFY(thumbnail->width() <= 108 && thumbnail->height() < slot->height()); // ... the formula whole in it
        // Above the formula, inside the slot: the row as it is beside the slot
        // (its highlight, if any), no tile's tint.
        const QImage rendered = window.grabWindow();
        const qreal dpr = window.devicePixelRatio();
        const QString screenshot = qEnvironmentVariable("WINSHUN_CLIP_ROW_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(rendered.save(screenshot + u".look.png"_s));
        const auto at = [&](QPointF point) {
            const QPointF scene = slot->mapToScene(point);
            return rendered.pixelColor(qRound(scene.x() * dpr), qRound(scene.y() * dpr));
        };
        const QColor pixel = at(QPointF(slot->width() / 2, 2));
        const QColor beside = at(QPointF(slot->width() + 8, 2));
        QVERIFY2(qAbs(pixel.red() - beside.red()) <= 2 && qAbs(pixel.green() - beside.green()) <= 2
                && qAbs(pixel.blue() - beside.blue()) <= 2,
            qPrintable(pixel.name() + u" beside "_s + beside.name()));
        // Plain text: the sign, in an icon's place.
        row->setProperty("image", QString());
        QVERIFY(!row->property("picture").toBool());
        QVERIFY(tile->isVisible());
        QCOMPARE(slot->width(), 32.0);
    }

    void animatedPreviewCanPauseAndReleasesHiddenImages()
    {
        QTest::failOnWarning(QRegularExpression(u"TypeError|ReferenceError|Cannot assign|Binding loop"_s));
        const QUrl folder = QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR "/"));
        QQmlEngine engine;
        QQmlComponent component(&engine, folder.resolved(QUrl(u"ClipPreview.qml"_s)));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* preview = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(preview);
        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
        window.setProperty("_q_showWithoutActivating", true);
        const int themeType = qmlTypeId("WinShun", 1, 0, "Theme");
        const auto* theme = engine.singletonInstance<QObject*>(themeType);
        QVERIFY(theme);
        window.setColor(theme->property("background").value<QColor>());
        window.resize(360, 300);
        preview->setParentItem(window.contentItem());
        preview->setSize(QSizeF(360, 300));
        QVariantMap info {{u"id"_s, u"1"_s}, {u"kind"_s, 4}, {u"width"_s, 160}, {u"height"_s, 100},
            {u"animated"_s, true},
            {u"image"_s, QUrl::fromLocalFile(QStringLiteral(WINSHUN_TEST_DATA "/clipboard-animated.gif")).toString()}};
        preview->setProperty("info", info);
        window.show();
        window.lower();
        QTRY_VERIFY(preview->findChild<QObject*>(u"clipboardAnimation"_s));
        auto* animation = preview->findChild<QObject*>(u"clipboardAnimation"_s);
        QTRY_COMPARE(animation->property("status").toInt(), 1); // Image.Ready
        QCOMPARE(animation->property("frameCount").toInt(), 2);
        auto* toggle = preview->findChild<QObject*>(u"clipboardAnimationToggle"_s);
        QVERIFY(toggle);
        if (animation->property("paused").toBool())
            QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        QTRY_VERIFY(!animation->property("paused").toBool());
        const int frame = animation->property("currentFrame").toInt();
        QTRY_VERIFY_WITH_TIMEOUT(animation->property("currentFrame").toInt() != frame, 2000);
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        QTRY_VERIFY(animation->property("paused").toBool());
        const int pausedFrame = animation->property("currentFrame").toInt();
        QTest::qWait(350);
        QCOMPARE(animation->property("currentFrame").toInt(), pausedFrame);
        const QString screenshot = qEnvironmentVariable("WINSHUN_CLIP_IMAGE_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window.grabWindow().save(screenshot));
        window.hide();
        QTRY_VERIFY(!preview->findChild<QObject*>(u"clipboardAnimation"_s));
    }

    // GitHub #414: a picture's words are folded under their heading unless
    // asked for, and a click on the picture asks for it large.
    void pictureWordsFoldAndPictureClick()
    {
        QTest::failOnWarning(QRegularExpression(u"TypeError|ReferenceError|Cannot assign|Binding loop"_s));
        const QUrl folder = QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR "/"));
        QQmlEngine engine;
        QQmlComponent component(&engine, folder.resolved(QUrl(u"ClipPreview.qml"_s)));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"wordsShown"_s, false}}));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* preview = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(preview);
        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::WindowDoesNotAcceptFocus);
        window.setProperty("_q_showWithoutActivating", true);
        window.resize(360, 420);
        preview->setParentItem(window.contentItem());
        preview->setSize(QSizeF(360, 420));
        const QString path = QStringLiteral(WINSHUN_TEST_DATA "/clipboard-oriented.jpg");
        const QSize size = QImageReader(path).size();
        preview->setProperty("info", QVariantMap {{u"id"_s, u"1"_s}, {u"kind"_s, 4}, {u"width"_s, size.width()},
            {u"height"_s, size.height()}, {u"image"_s, QUrl::fromLocalFile(path).toString()},
            {u"words"_s, u"图里的一行字"_s}});
        window.show();
        window.lower();

        auto* words = preview->findChild<QQuickItem*>(u"clipboardPictureWords"_s);
        auto* heading = preview->findChild<QQuickItem*>(u"clipboardPictureWordsToggle"_s);
        QVERIFY(words && heading);
        QVERIFY(heading->isVisible());
        QVERIFY(!words->isVisible()); // folded: the heading and 复制文字 only
        QSignalSpy toggled(preview, SIGNAL(wordsToggled()));
        QVERIFY(QMetaObject::invokeMethod(object.get(), "wordsToggled"));
        QCOMPARE(toggled.count(), 1);
        preview->setProperty("wordsShown", true);
        QVERIFY(words->isVisible());
        QCOMPARE(words->property("text").toString(), u"图里的一行字"_s);

        auto* zoom = preview->findChild<QQuickItem*>(u"clipboardPictureZoom"_s);
        QVERIFY(zoom);
        QVERIFY(!zoom->property("enabled").toBool()); // not in the hover's preview
        preview->setProperty("pictureZoomable", true);
        QTRY_VERIFY(zoom->property("enabled").toBool()); // once the picture is there
        QSignalSpy clicked(preview, SIGNAL(pictureClicked()));
        const QPoint middle = zoom->mapToScene(QPointF(zoom->width() / 2, zoom->height() / 2)).toPoint();
        QTest::mouseClick(&window, Qt::LeftButton, {}, middle);
        QTRY_COMPARE(clicked.count(), 1);
        window.hide();
    }

    // GitHub #414: the picture large beside the clipboard window, fitted in
    // the room there; the wheel's zoom and back to fitted with a new picture.
    void pictureViewerBesideTheWindow()
    {
        QTest::failOnWarning(QRegularExpression(u"TypeError|ReferenceError|Cannot assign|Binding loop"_s));
        const QUrl folder = QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR "/"));
        QQmlEngine engine;
        QQmlComponent hostComponent(&engine);
        hostComponent.setData("import QtQuick; Window { x: 100; y: 100; width: 600; height: 500; visible: true; flags: Qt.Tool }",
            QUrl(u"host.qml"_s));
        std::unique_ptr<QObject> host(hostComponent.create());
        QVERIFY2(host, qPrintable(hostComponent.errorString()));
        const QString path = QStringLiteral(WINSHUN_TEST_DATA "/clipboard-oriented.jpg");
        QQmlComponent component(&engine, folder.resolved(QUrl(u"ClipPictureViewer.qml"_s)));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        // A picture larger than the room on the right: fitted into it.
        const QSize shown(2880, 1704);
        std::unique_ptr<QObject> object(component.createWithInitialProperties({
            {u"host"_s, QVariant::fromValue(host.get())}, {u"hostRect"_s, QRectF(100, 100, 600, 500)},
            {u"availableArea"_s, QRectF(0, 0, 1920, 1040)}, {u"source"_s, QUrl::fromLocalFile(path).toString()},
            {u"pixels"_s, QSizeF(shown)}, {u"caption"_s, u"PNG"_s}}));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* viewer = qobject_cast<QQuickWindow*>(object.get());
        QVERIFY(viewer);
        QTRY_VERIFY(viewer->isVisible());
        const qreal dpr = viewer->devicePixelRatio();
        const qreal natural = shown.width() / dpr;
        const qreal fit = std::min({1.0, (1920 - 700 - 16 - 24) / natural, (1040 - 16 - 24 - 36) / (shown.height() / dpr)});
        QCOMPARE(viewer->x(), 708); // right of it, 8 apart
        QVERIFY(viewer->x() + viewer->width() <= 1920 - 8);
        QVERIFY(viewer->y() >= 8 && viewer->y() + viewer->height() <= 1040 - 8);
        QCOMPARE(viewer->width(), int(std::ceil(std::min(1920.0 - 700 - 16, natural * fit + 24))));
        const auto named = [viewer](const QString& name) { return itemNamed(viewer->contentItem(), name); };
        auto* view = named(u"clipboardPictureViewerView"_s);
        QVERIFY(view);
        QVERIFY(std::abs(view->property("scale").toReal() - view->property("fitScale").toReal()) < 1e-6);
        QQuickItem* image = nullptr;
        QTRY_VERIFY((image = named(u"clipboardPictureViewerImage"_s)));
        QVERIFY(image);
        QTRY_COMPARE(image->property("status").toInt(), 1); // Image.Ready

        const qreal fitted = view->property("scale").toReal();
        QVERIFY(QMetaObject::invokeMethod(view, "zoomAt", Q_ARG(QVariant, 2.0), Q_ARG(QVariant, 50.0), Q_ARG(QVariant, 50.0)));
        QVERIFY(std::abs(view->property("scale").toReal() - fitted * 2) < 1e-6);
        auto* caption = named(u"clipboardPictureViewerCaption"_s);
        QVERIFY(caption);
        QVERIFY(caption->property("text").toString().endsWith(u"%1%"_s.arg(qRound(fitted * 2 * 100))));
        object->setProperty("source", QUrl::fromLocalFile(QStringLiteral(WINSHUN_TEST_DATA "/clipboard-animated.gif")).toString());
        QCOMPARE(view->property("zoom").toReal(), 0.0); // a new picture: fitted again

        // No room beside it: over the middle of the screen.
        object->setProperty("hostRect", QRectF(300, 100, 1320, 500));
        QCOMPARE(viewer->x(), int(std::round((1920 - viewer->width()) / 2.0)));
        viewer->close();
    }

private:
    bool m_isolated;
};

int main(int argc, char** argv)
{
    // copyPutBackByAnotherProgramIsDropped's other program, on the test's
    // private desktop (inherited).
    if (const QString text = qEnvironmentVariable("WINSHUN_TEST_CLIP_PUT_BACK"); !text.isEmpty())
        return putBack(text);
    const QString requestedStation = qEnvironmentVariable("WINSHUN_TEST_CLIP_IMAGE_STATION");
    if (requestedStation.isEmpty()) {
        // The child starts on a private desktop before any DLL can create a
        // window. Switching the current Qt thread's desktop is too late.
        QCoreApplication runner(argc, argv);
        const HWINSTA original = ::GetProcessWindowStation();
        QString name = u"WinShun-ClipboardImageTest-%1"_s.arg(::GetCurrentProcessId());
        HWINSTA station = ::CreateWindowStationW(qUtf16Printable(name), CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
        if (!station) {
            // Non-administrators may create an unnamed station only. Refuse
            // an existing one so parallel tests never share a clipboard.
            station = ::CreateWindowStationW(nullptr, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
            wchar_t generatedName[256] {};
            DWORD length = 0;
            if (station && ::GetUserObjectInformationW(station, UOI_NAME, generatedName, sizeof generatedName, &length))
                name = QString::fromWCharArray(generatedName);
        }
        HDESK desktop = nullptr;
        if (station && ::SetProcessWindowStation(station))
            desktop = ::CreateDesktopW(L"Default", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        const DWORD isolationError = desktop ? 0 : ::GetLastError();
        ::SetProcessWindowStation(original);
        const auto close = qScopeGuard([&] {
            if (desktop)
                ::CloseDesktop(desktop);
            if (station)
                ::CloseWindowStation(station);
        });
        if (!desktop)
            qWarning() << "Private clipboard unavailable; native tests will be skipped. Windows error:"
                       << isolationError;
        QProcess child;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"WINSHUN_TEST_CLIP_IMAGE_STATION"_s, desktop ? name : u"unavailable"_s);
        environment.insert(u"WINSHUN_TEST_CLIP_IMAGE_ERROR"_s, QString::number(isolationError));
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        const std::wstring desktopName = (name + u"\\Default"_s).toStdWString();
        if (desktop) {
            child.setCreateProcessArgumentsModifier([&](QProcess::CreateProcessArguments* arguments) {
                arguments->startupInfo->lpDesktop = const_cast<wchar_t*>(desktopName.c_str());
            });
        }
        child.start(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
        if (!child.waitForStarted(5000) || !child.waitForFinished(60000)) {
            qCritical() << child.errorString();
            return 1;
        }
        if (child.exitStatus() != QProcess::NormalExit) {
            qCritical("The test process on the private desktop ended abnormally (code %#x)",
                static_cast<unsigned>(child.exitCode()));
            return 1;
        }
        return child.exitCode();
    }

    wchar_t actualStation[256] {};
    DWORD length = 0;
    USEROBJECTFLAGS stationFlags {};
    const bool isolated = ::GetUserObjectInformationW(
                              ::GetProcessWindowStation(), UOI_NAME, actualStation, sizeof actualStation, &length)
        && requestedStation.compare(QString::fromWCharArray(actualStation), Qt::CaseInsensitive) == 0
        && requestedStation.compare(u"WinSta0"_s, Qt::CaseInsensitive) != 0
        && ::GetUserObjectInformationW(
            ::GetProcessWindowStation(), UOI_FLAGS, &stationFlags, sizeof stationFlags, &length)
        && !(stationFlags.dwFlags & WSF_VISIBLE);
    QGuiApplication app(argc, argv);
    // Match the application's Unicode-path font loading, not the offscreen
    // platform's default Latin-only font database.
    QFile font(QStringLiteral(WINSHUN_QML_DIR "/../../../resources/fonts/AlibabaPuHuiTi-3-55-Regular.ttf"));
    if (font.open(QIODevice::ReadOnly)) {
        const int id = QFontDatabase::addApplicationFontFromData(font.readAll());
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty())
            QGuiApplication::setFont(QFont(families.front()));
    }
    QGuiApplication::styleHints()->setColorScheme(
        qEnvironmentVariable("WINSHUN_CLIP_IMAGE_THEME") == u"dark" ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    const QString pluginRoot = qEnvironmentVariable("WINSHUN_CLIP_IMAGE_PLUGIN_ROOT");
    if (!pluginRoot.isEmpty())
        QCoreApplication::setLibraryPaths({pluginRoot});
    ClipImageTest test(isolated);
    return QTest::qExec(&test, argc, argv);
}

#include "tst_clipimage.moc"
