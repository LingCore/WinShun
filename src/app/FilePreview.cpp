#include "FilePreview.h"
#include "SystemTheme.h"

#include "FileIconProvider.h"
#include "FileText.h"
#include "platform/ComWorker.h"
#include "platform/ExternalPreview.h"
#include "platform/PreviewHost.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QTransform>
#include <QtMath>

#include <windows.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;
using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr std::size_t kTextBytes = 1u << 20; // the start of a text file shown: 1 MB
constexpr qsizetype kPieceChars = 2000; // a longer line goes in pieces (one row of the list each)
constexpr int kLargestPicture = 4096; // pixels on the longer side; larger ones are scaled down as decoded
constexpr int kThumbnailPx = 512;
constexpr int kFolderItemsCounted = 10000;

// Kinds Windows Imaging Component reads, with the codecs it comes with or a
// common add-on (HEIF, WebP, AVIF, RAW image extensions from the Store).
bool isPicture(const QString& suffix)
{
    static const QStringList kTypes {u"jpg"_s, u"jpeg"_s, u"jpe"_s, u"jfif"_s, u"png"_s, u"gif"_s, u"bmp"_s, u"dib"_s,
        u"tif"_s, u"tiff"_s, u"ico"_s, u"webp"_s, u"heic"_s, u"heif"_s, u"avif"_s, u"jxl"_s, u"jxr"_s, u"wdp"_s, u"hdp"_s,
        u"dds"_s, u"dng"_s, u"cr2"_s, u"cr3"_s, u"nef"_s, u"arw"_s, u"orf"_s, u"rw2"_s, u"raf"_s};
    return kTypes.contains(suffix);
}

// Text whatever handler a program may have put on it (an editor's, a web
// view's): read here at once. The content search's kinds join these.
bool isText(const QString& suffix, const QStringList& more)
{
    static const QStringList kTypes {u"txt"_s, u"text"_s, u"md"_s, u"markdown"_s, u"log"_s, u"ini"_s, u"cfg"_s,
        u"conf"_s, u"config"_s, u"json"_s, u"jsonc"_s, u"json5"_s, u"xml"_s, u"yaml"_s, u"yml"_s, u"toml"_s, u"csv"_s,
        u"tsv"_s, u"c"_s, u"h"_s, u"cc"_s, u"cpp"_s, u"cxx"_s, u"hpp"_s, u"hh"_s, u"hxx"_s, u"inl"_s, u"cs"_s, u"java"_s,
        u"kt"_s, u"kts"_s, u"py"_s, u"pyw"_s, u"js"_s, u"mjs"_s, u"cjs"_s, u"ts"_s, u"tsx"_s, u"jsx"_s, u"go"_s,
        u"rs"_s, u"rb"_s, u"php"_s, u"lua"_s, u"pl"_s, u"swift"_s, u"dart"_s, u"sh"_s, u"bash"_s, u"zsh"_s, u"ps1"_s,
        u"psm1"_s, u"psd1"_s, u"bat"_s, u"cmd"_s, u"sql"_s, u"html"_s, u"htm"_s, u"css"_s, u"scss"_s, u"less"_s,
        u"vue"_s, u"svelte"_s, u"qml"_s, u"cmake"_s, u"gradle"_s, u"properties"_s, u"reg"_s, u"srt"_s, u"ass"_s,
        u"vtt"_s, u"lrc"_s, u"tex"_s, u"bib"_s, u"rst"_s, u"adoc"_s, u"diff"_s, u"patch"_s, u"gitignore"_s,
        u"gitattributes"_s, u"editorconfig"_s, u"env"_s, u"nfo"_s, u"inf"_s, u"manifest"_s, u"sln"_s, u"vcxproj"_s,
        u"csproj"_s, u"props"_s, u"targets"_s, u"svg"_s};
    return kTypes.contains(suffix) || more.contains(suffix);
}

QString sizeText(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString timeText(const FILETIME& time)
{
    ULARGE_INTEGER t;
    t.LowPart = time.dwLowDateTime;
    t.HighPart = time.dwHighDateTime;
    if (t.QuadPart == 0)
        return {};
    const qint64 ms = static_cast<qint64>((t.QuadPart - 116444736000000000ULL) / 10000);
    return QDateTime::fromMSecsSinceEpoch(ms).toLocalTime().toString(u"yyyy-MM-dd HH:mm"_s);
}

QString typeName(const QString& path, bool isDir)
{
    SHFILEINFOW info {};
    const DWORD attributes = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    // By the name alone (SHGFI_USEFILEATTRIBUTES): never touches the file.
    if (::SHGetFileInfoW(reinterpret_cast<const wchar_t*>(path.utf16()), attributes, &info, sizeof info,
            SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
        return QString::fromWCharArray(info.szTypeName);
    return {};
}

// The first frame, decoded by Windows Imaging Component and turned upright
// as the camera says (EXIF orientation); at most `largest` pixels on its
// longer side. `original`: its size as stored.
QImage decodePicture(const std::wstring& path, QSize* original, int largest = kLargestPicture)
{
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return {};
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return {};
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return {};
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0)
        return {};
    USHORT orientation = 1;
    ComPtr<IWICMetadataQueryReader> metadata;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata))) {
        PROPVARIANT value;
        ::PropVariantInit(&value);
        // The property system's name reads it from JPEG, TIFF and HEIF alike.
        if (SUCCEEDED(metadata->GetMetadataByName(L"System.Photo.Orientation", &value)) && value.vt == VT_UI2)
            orientation = value.uiVal;
        ::PropVariantClear(&value);
    }
    ComPtr<IWICBitmapSource> source = frame;
    UINT w = width;
    UINT h = height;
    if (largest > 0 && std::max(width, height) > static_cast<UINT>(largest)) {
        const double scale = static_cast<double>(largest) / std::max(width, height);
        w = std::max(1u, static_cast<UINT>(width * scale + 0.5));
        h = std::max(1u, static_cast<UINT>(height * scale + 0.5));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler))
            || FAILED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeHighQualityCubic)))
            return {};
        source = scaler;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
            WICBitmapPaletteTypeMedianCut)))
        return {};
    QImage image(static_cast<int>(w), static_cast<int>(h), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()
        || FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(image.bytesPerLine()), static_cast<UINT>(image.sizeInBytes()),
            image.bits())))
        return {};
    if (original)
        *original = QSize(static_cast<int>(width), static_cast<int>(height));
    switch (orientation) {
    case 2: return image.flipped(Qt::Horizontal);
    case 3: return image.flipped(Qt::Horizontal | Qt::Vertical);
    case 4: return image.flipped(Qt::Vertical);
    case 5: return image.transformed(QTransform().rotate(90)).flipped(Qt::Horizontal);
    case 6: return image.transformed(QTransform().rotate(90));
    case 7: return image.transformed(QTransform().rotate(90)).flipped(Qt::Vertical);
    case 8: return image.transformed(QTransform().rotate(270));
    default: return image;
    }
}


// The file's Zone.Identifier stream says it came from the internet (or an
// untrusted site): ZoneId 3 or 4.
bool fromTheInternet(const std::wstring& path)
{
    const HANDLE file = ::CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    char buffer[1024] {};
    DWORD read = 0;
    const BOOL ok = ::ReadFile(file, buffer, sizeof buffer - 1, &read, nullptr);
    ::CloseHandle(file);
    if (!ok)
        return false;
    const std::string_view text(buffer, read);
    const std::size_t at = text.find("ZoneId=");
    if (at == std::string_view::npos || at + 7 >= text.size())
        return false;
    const char zone = text[at + 7];
    return zone == '3' || zone == '4';
}

// What a folder holds directly: its folders and its files, counted up to a
// limit (a folder of a million files would take long).
std::pair<int, int> countFolder(const QString& path, bool* more, const std::function<bool()>& stale)
{
    int folders = 0;
    int files = 0;
    *more = false;
    const std::wstring pattern = (QString(path).replace(u'/', u'\\') + (path.endsWith(u'\\') ? u"*"_s : u"\\*"_s)).toStdWString();
    WIN32_FIND_DATAW data;
    const HANDLE find = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return {0, 0};
    do {
        const std::wstring_view name(data.cFileName);
        if (name == L"." || name == L"..")
            continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ++folders;
        else
            ++files;
        if (folders + files >= kFolderItemsCounted) {
            *more = true;
            break;
        }
    } while (!stale() && ::FindNextFileW(find, &data));
    ::FindClose(find);
    return {folders, files};
}

} // namespace

QImage decodePictureFile(const QString& path, int largest, QSize* original)
{
    QString native = path;
    native.replace(u'/', u'\\');
    return decodePicture(native.toStdWString(), original, largest);
}

// What the worker found out about a file.
struct FilePreview::Loaded {
    Kind kind = Message;
    QImage image; // Picture; Facts: the thumbnail or icon
    QSize imageSize; // Picture: as decoded
    QString summary;
    QStringList facts;
    QStringList lines;
    QVector<int> numbers;
    int lineCount = 0;
    int rows = 0; // Text: lines as shown, long ones wrapped (about)
    QString message;
    QUuid handler; // Handler: the preview handler's CLSID
};

// The pictures QML shows ("image://preview/<n>"): only the latest one is kept.
class FilePreview::Images : public QQuickImageProvider {
public:
    Images()
        : QQuickImageProvider(QQuickImageProvider::Image)
    {
    }
    QImage requestImage(const QString& id, QSize* size, const QSize&) override
    {
        QMutexLocker lock(&m_mutex);
        const QImage image = id == m_id ? m_image : QImage();
        if (size)
            *size = image.size();
        return image;
    }
    void set(const QString& id, QImage image)
    {
        QMutexLocker lock(&m_mutex);
        m_id = id;
        m_image = std::move(image);
    }

private:
    QMutex m_mutex;
    QString m_id;
    QImage m_image;
};

FilePreview::FilePreview(QObject* parent)
    : QObject(parent)
    , m_worker(new ComWorker)
    , m_host(std::make_unique<PreviewHost>())
    , m_images(std::make_shared<Images>())
{
    connect(m_host.get(), &PreviewHost::failed, this, &FilePreview::handlerFailed);
    m_slowTimer.setSingleShot(true);
    m_slowTimer.setInterval(250);
    connect(&m_slowTimer, &QTimer::timeout, this, [this] {
        setSlow(true);
        // Taking a while: the window now, saying so, rather than nothing at all.
        if (m_open && !m_external && m_kind == Loading && (!m_window || !m_window->isVisible()))
            showWindow({});
    });
    // Seer 2.8 has the window Win顺 talks to, takes the file and shows
    // nothing: ours then, and that Seer is not asked again. Told to hide
    // all the same: one that was only slow must not stay on the screen.
    m_externalCheck.setSingleShot(true);
    m_externalCheck.setInterval(1500);
    connect(&m_externalCheck, &QTimer::timeout, this, [this] {
        if (!m_open || m_external != extpreview::Engine::Seer || extpreview::answered(*m_external))
            return;
        qWarning("WinShun: Seer (no version told) showed nothing; previewing with Win顺's own while it runs");
        extpreview::close(*m_external);
        extpreview::passOver(*m_external);
        m_external.reset();
        load(m_item);
    });
}

FilePreview::~FilePreview()
{
    // m_worker is left running: a thumbnail or a picture codec may still be
    // busy in it at exit, and nothing should wait for that.
}

QQuickImageProvider* FilePreview::imageProvider()
{
    // The engine takes ownership of what it is given: a forwarder, so that
    // the pictures stay ours.
    class Forward : public QQuickImageProvider {
    public:
        explicit Forward(std::shared_ptr<Images> images)
            : QQuickImageProvider(QQuickImageProvider::Image)
            , m_images(std::move(images))
        {
        }
        QImage requestImage(const QString& id, QSize* size, const QSize& requested) override
        {
            return m_images->requestImage(id, size, requested);
        }

    private:
        std::shared_ptr<Images> m_images;
    };
    return new Forward(m_images);
}

void FilePreview::setLauncherWindow(QWindow* launcher)
{
    m_launcher = launcher;
    // It grows into the search panel and back, gets rows as one types, is
    // dragged: the preview goes along (once a frame at most).
    m_relayout.setSingleShot(true);
    m_relayout.setInterval(0);
    connect(&m_relayout, &QTimer::timeout, this, [this] {
        if (m_open && !m_external && m_window && m_window->isVisible() && m_kind != Loading)
            showWindow(m_kind == Picture ? m_imageSize : QSize());
    });
    for (auto signal : {&QWindow::xChanged, &QWindow::yChanged, &QWindow::widthChanged, &QWindow::heightChanged})
        connect(launcher, signal, &m_relayout, qOverload<>(&QTimer::start));
}

void FilePreview::setDark(bool dark)
{
    m_dark = dark;
}

void FilePreview::setTextExtensions(QStringList extensions)
{
    for (QString& e : extensions)
        e = e.toLower();
    m_textExtensions = std::move(extensions);
}

QString FilePreview::iconUrl() const
{
    return m_item.path.isEmpty() ? QString() : FileIconProvider::iconUrl(m_item.path, m_item.isDir);
}

bool FilePreview::showsElsewhere() const
{
    return !m_builtInOnly && extpreview::running().has_value();
}

bool FilePreview::isShowing(const QString& path) const
{
    return m_open && m_item.path.compare(path, Qt::CaseInsensitive) == 0;
}

void FilePreview::show(const Item& item)
{
    if (!m_builtInOnly) {
        if (const std::optional<extpreview::Engine> engine = extpreview::running()) {
            if (m_open && !m_external)
                close(); // ours, opened while none ran
            const bool known = extpreview::knownToShow(*engine); // first: busy with the file, Seer may not answer
            if (extpreview::show(*engine, item.path)) {
                m_open = true;
                m_external = engine;
                m_externalSince.start();
                m_item = item;
                if (!known)
                    m_externalCheck.start();
                return;
            }
        }
    }
    if (m_external)
        close();
    m_open = true;
    load(item);
}

void FilePreview::follow(const std::optional<Item>& item)
{
    if (!m_open)
        return;
    if (m_external) {
        // As QuickLook does on Explorer's selection: the next file, while it is up.
        if (item && m_item.path.compare(item->path, Qt::CaseInsensitive) != 0 && extpreview::switchTo(*m_external, item->path))
            m_item = *item;
        return;
    }
    if (!item) {
        ++m_generation;
        m_host->hide();
        m_item = {};
        m_kind = Message;
        m_message = tr("这一行没有可预览的文件");
        m_summary.clear();
        m_slowTimer.stop();
        setSlow(false);
        emit contentChanged();
        return;
    }
    if (m_item.path.compare(item->path, Qt::CaseInsensitive) != 0)
        load(*item);
}

void FilePreview::close()
{
    if (!m_open)
        return;
    m_open = false;
    m_side = -1;
    ++m_generation;
    m_slowTimer.stop();
    m_externalCheck.stop();
    if (m_external) {
        extpreview::close(*m_external);
        m_external.reset();
    }
    if (m_host)
        m_host->hide();
    if (m_window)
        m_window->hide();
    m_item = {};
    m_lines.setStringList({});
    m_lineNumbers.clear();
    m_images->set({}, {});
    m_imageUrl.clear();
    emit contentChanged();
}

void FilePreview::closeByUser()
{
    close();
    emit closedByUser();
}

void FilePreview::forgetClosedExternal()
{
    if (!m_open || !m_external || m_externalSince.elapsed() < 1500 || extpreview::visible(*m_external))
        return;
    m_open = false;
    m_external.reset();
    m_item = {};
}

bool FilePreview::foregroundIsOurs() const
{
    const HWND front = ::GetForegroundWindow();
    return m_open && !m_external && front
        && ((m_window && front == reinterpret_cast<HWND>(m_window->winId())) || m_host->owns(front));
}

bool FilePreview::ownsForeground() const
{
    if (!m_open)
        return false;
    const HWND front = ::GetForegroundWindow();
    if (!front)
        return false;
    if (m_external)
        return extpreview::owns(*m_external, front);
    return (m_window && front == reinterpret_cast<HWND>(m_window->winId())) || (m_host && m_host->owns(front));
}

void FilePreview::openFile()
{
    if (!m_item.path.isEmpty())
        emit openRequested(m_item.path);
}

void FilePreview::setSlow(bool slow)
{
    if (m_slow == slow)
        return;
    m_slow = slow;
    emit slowChanged();
}

void FilePreview::load(const Item& item)
{
    const quint64 generation = ++m_generation;
    m_item = item;
    m_kind = Loading;
    m_message.clear();
    m_summary.clear();
    m_facts.clear();
    setSlow(false);
    m_slowTimer.start();
    if (m_host)
        m_host->hide();
    emit contentChanged();

    const QStringList textTypes = m_textExtensions;
    // A handler failed on it a moment ago: what is known instead.
    const bool noHandler = m_noHandlerFor.compare(item.path, Qt::CaseInsensitive) == 0;
    QPointer<FilePreview> self(this);
    const auto stale = [self, generation] { return !self || self->m_generation.load() != generation; };
    m_worker->post([self, item, textTypes, noHandler, generation, stale] {
        if (stale())
            return;
        auto loaded = std::make_shared<Loaded>();
        const std::wstring native = QString(item.path).replace(u'/', u'\\').toStdWString();
        WIN32_FILE_ATTRIBUTE_DATA data {};
        if (!::GetFileAttributesExW(native.c_str(), GetFileExInfoStandard, &data)) {
            loaded->message = QCoreApplication::translate("ws::FilePreview", "找不到这个文件，可能已被移动或删除");
        } else {
            const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const qint64 bytes = (static_cast<qint64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            const QString suffix = isDir ? QString() : QFileInfo(item.path).suffix().toLower();
            const QString type = typeName(item.path, isDir);
            const QString modified = timeText(data.ftLastWriteTime);
            QStringList summary {type};
            if (!isDir)
                summary.append(sizeText(bytes));
            summary.append(modified);
            const auto fact = [&](const QString& label, const QString& value) {
                if (!value.isEmpty())
                    loaded->facts << label << value;
            };
            constexpr DWORD kNotHere = FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
            const bool cloudOnly = (data.dwFileAttributes & kNotHere) != 0;
            bool done = false;
            if (isDir) {
                bool more = false;
                const auto [folders, files] = countFolder(item.path, &more, stale);
                const QString holds = folders + files == 0 ? QCoreApplication::translate("ws::FilePreview", "空文件夹")
                    : more ? QCoreApplication::translate("ws::FilePreview", "%1 项以上").arg(kFolderItemsCounted)
                    : folders == 0 ? QCoreApplication::translate("ws::FilePreview", "%n 个文件", nullptr, files)
                    : files == 0 ? QCoreApplication::translate("ws::FilePreview", "%n 个文件夹", nullptr, folders)
                                 : QCoreApplication::translate("ws::FilePreview", "%1 个文件夹，%2 个文件").arg(folders).arg(files);
                summary.insert(1, holds);
                fact(QCoreApplication::translate("ws::FilePreview", "包含"), holds);
            } else if (!cloudOnly && isPicture(suffix)) {
                QSize original;
                QImage image = decodePicture(native, &original);
                if (!image.isNull()) {
                    loaded->kind = Picture;
                    loaded->imageSize = image.size();
                    summary.insert(1, u"%1 × %2"_s.arg(original.width()).arg(original.height()));
                    loaded->image = std::move(image);
                    done = true;
                }
            } else if (!cloudOnly && (isText(suffix, textTypes) || suffix.isEmpty() || noHandler || !PreviewHost::handlerFor(item.path))) {
                // A text kind; or no extension, or nothing else would show
                // it: text if it reads as text.
                const filetext::Head head = filetext::readHead(native, kTextBytes);
                if (head.status == filetext::Status::Ok && head.text.isEmpty()) {
                    loaded->kind = Message;
                    loaded->message = QCoreApplication::translate("ws::FilePreview", "空文件");
                    done = true;
                } else if (head.status == filetext::Status::Ok) {
                    loaded->kind = Text;
                    const QStringList lines = head.text.split(u'\n');
                    int count = static_cast<int>(lines.size());
                    if (count > 1 && lines.last().isEmpty())
                        --count; // the end of the last line, not a line
                    loaded->lineCount = count;
                    for (int i = 0; i < count; ++i) {
                        const QString& line = lines[i];
                        qsizetype at = 0;
                        do { // in pieces, never splitting a surrogate pair
                            qsizetype n = std::min(kPieceChars, line.size() - at);
                            if (at + n < line.size() && line[at + n - 1].isHighSurrogate())
                                --n;
                            loaded->lines.append(line.mid(at, n));
                            loaded->numbers.append(at == 0 ? i + 1 : 0);
                            loaded->rows += std::max<int>(1, static_cast<int>((n + 99) / 100));
                            at += n;
                        } while (at < line.size());
                    }
                    QString encoding;
                    switch (head.encoding) {
                    case filetext::HeadEncoding::Utf8: encoding = u"UTF-8"_s; break;
                    case filetext::HeadEncoding::Utf8Bom: encoding = u"UTF-8 BOM"_s; break;
                    case filetext::HeadEncoding::Utf16LE: encoding = u"UTF-16 LE"_s; break;
                    case filetext::HeadEncoding::Utf16BE: encoding = u"UTF-16 BE"_s; break;
                    case filetext::HeadEncoding::Ansi:
                        encoding = head.codePage == 936 ? u"GBK"_s : head.codePage == 950 ? u"Big5"_s : u"ANSI %1"_s.arg(head.codePage);
                        break;
                    }
                    summary.insert(1, encoding);
                    summary.insert(2, head.cut ? QCoreApplication::translate("ws::FilePreview", "前 %1 行").arg(count)
                                               : QCoreApplication::translate("ws::FilePreview", "%1 行").arg(count));
                    done = true;
                }
            }
            // Downloaded from the internet (the mark of the web): Explorer's
            // preview pane does not hand such a file to its handler (a
            // document can make it fetch from a server, and so give away the
            // user's sign-in, KB5070960); nor does Win顺.
            const bool fromInternet = !done && !isDir && !cloudOnly && fromTheInternet(native);
            if (!done && !isDir && !cloudOnly && !noHandler && !fromInternet) {
                if (const std::optional<QUuid> handler = PreviewHost::handlerFor(item.path)) {
                    loaded->kind = Handler;
                    loaded->handler = *handler;
                    done = true;
                }
            }
            if (!done) {
                loaded->kind = Facts;
                fact(QCoreApplication::translate("ws::FilePreview", "类型"), type);
                if (!isDir)
                    fact(QCoreApplication::translate("ws::FilePreview", "大小"), sizeText(bytes) + (bytes >= 1024 ? u" (%1 B)"_s.arg(QLocale().toString(bytes)) : QString()));
                fact(QCoreApplication::translate("ws::FilePreview", "修改时间"), modified);
                fact(QCoreApplication::translate("ws::FilePreview", "创建时间"), timeText(data.ftCreationTime));
                fact(QCoreApplication::translate("ws::FilePreview", "位置"), QFileInfo(item.path).absolutePath().replace(u'/', u'\\'));
                if (cloudOnly)
                    fact(QCoreApplication::translate("ws::FilePreview", "状态"), QCoreApplication::translate("ws::FilePreview", "仅在云端，未下载到这台电脑"));
                else if (noHandler)
                    fact(QCoreApplication::translate("ws::FilePreview", "说明"),
                        QCoreApplication::translate("ws::FilePreview", "预览程序没能显示这个文件"));
                else if (fromInternet && PreviewHost::handlerFor(item.path))
                    fact(QCoreApplication::translate("ws::FilePreview", "说明"),
                        QCoreApplication::translate("ws::FilePreview", "从网上下载的文件，为安全起见不预览内容（资源管理器也是这样）"));
                else if (!stale())
                    loaded->image = FileIconProvider::largeImage(item.path, kThumbnailPx);
            }
            summary.removeAll(QString());
            loaded->summary = summary.join(u"  ·  "_s);
        }
        QMetaObject::invokeMethod(self, [self, generation, loaded] {
            if (self)
                self->apply(generation, loaded);
        }, Qt::QueuedConnection);
    });
}

void FilePreview::apply(quint64 generation, std::shared_ptr<Loaded> loaded)
{
    if (generation != m_generation.load() || !m_open)
        return;
    m_slowTimer.stop();
    setSlow(false);
    m_kind = loaded->kind;
    m_summary = loaded->summary;
    m_facts = loaded->facts;
    m_message = loaded->message;
    m_imageSize = loaded->imageSize;
    m_lineNumbers = loaded->numbers;
    m_lineCount = loaded->lineCount;
    m_textRows = loaded->rows;
    m_lines.setStringList(loaded->lines);
    const QString id = QString::number(generation);
    m_imageUrl = loaded->image.isNull() ? QString() : u"image://preview/"_s + id;
    m_images->set(id, std::move(loaded->image));
    QSize wanted;
    if (m_kind == Picture)
        wanted = m_imageSize;
    showWindow(wanted);
    emit contentChanged();
    if (m_kind == Handler)
        startHandler(generation, loaded->handler);
}

// ---- the window --------------------------------------------------------------------

void FilePreview::showWindow(QSize wanted)
{
    if (!m_window) {
        if (!m_makeWindow)
            return;
        m_window = m_makeWindow();
        if (!m_window)
            return;
        // Moved without changing size: the handler's popup goes along too
        // (a new size reaches it from PreviewWindow.qml).
        connect(m_window, &QWindow::xChanged, this, &FilePreview::placeHandler);
        connect(m_window, &QWindow::yChanged, this, &FilePreview::placeHandler);
    }
    const QRect place = placeBeside(wanted);
    if (place.isValid() && m_window->geometry() != place)
        m_window->setGeometry(place);
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (!m_window->isVisible()) {
        // Not activated: the launcher keeps the focus (and so stays open).
        m_window->setProperty("_q_showWithoutActivating", true);
        m_window->show();
        m_window->setProperty("_q_showWithoutActivating", QVariant());
    }
    // Beside the launcher: just under it, both on top of other windows.
    // Over it (no room beside it): in front of it.
    if (m_overLauncher)
        ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    else if (m_launcher && m_launcher->isVisible())
        ::SetWindowPos(hwnd, reinterpret_cast<HWND>(m_launcher->winId()), 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    placeHandler(); // the handler's popup just above it again
}

// The window's place on the launcher's screen, all in logical pixels: beside
// the launcher where there is the most room for `wanted` (right, left or
// below; the work area, not the taskbar), as large as fits there; with no
// room beside it worth the name, over the middle of the screen, under it.
QRect FilePreview::placeBeside(QSize wanted)
{
    QScreen* screen = m_launcher ? m_launcher->screen() : QGuiApplication::primaryScreen();
    if (!screen)
        return {};
    const QRect area = screen->availableGeometry();
    const QRect launcher = m_launcher && m_launcher->isVisible() ? m_launcher->geometry() : QRect();
    constexpr int kGap = 8;
    constexpr int kHeader = 64; // the header row and a margin, about
    constexpr int kPad = 32;
    const int tallest = area.height() - 2 * kGap;
    // What it would like to be.
    QSize want;
    switch (m_kind) {
    case Picture:
        want = QSize(std::max(360, wanted.width() > 0 ? qCeil(wanted.width() / screen->devicePixelRatio()) + kPad : 720),
            std::max(280, wanted.height() > 0 ? qCeil(wanted.height() / screen->devicePixelRatio()) + kPad + kHeader : 540));
        break;
    case Text: {
        // As tall as the text, about (a line of the code font, by 界面大小).
        const QString size = SystemTheme::uiSize();
        const int line = size == u"compact" ? 18 : size == u"large" ? 22 : 20;
        want = QSize(820, std::clamp(kHeader + kPad + m_textRows * line, 280, tallest * 85 / 100));
        break;
    }
    case Handler: want = QSize(900, tallest * 90 / 100); break;
    case Loading:
        if (m_window && m_window->isVisible())
            return m_window->geometry(); // stays as it is until it is known what comes
        want = QSize(560, 460);
        break;
    default: want = QSize(560, 460); break;
    }
    want = want.boundedTo(QSize(area.width() - 2 * kGap, tallest));
    // `want` within `room`; a picture keeps its proportions (narrowed, it
    // is not as tall either: no bands of nothing above and below it).
    const auto fit = [&](QSize room) {
        const QSize bounded = want.boundedTo(room).expandedTo(QSize(std::min(360, room.width()), std::min(280, room.height())));
        if (m_kind != Picture || wanted.isEmpty())
            return bounded;
        const QSizeF picture = QSizeF(wanted) / screen->devicePixelRatio();
        const qreal scale = std::min({1.0, (bounded.width() - kPad) / picture.width(), (bounded.height() - kPad - kHeader) / picture.height()});
        return QSize(std::max(360, qCeil(picture.width() * scale) + kPad), std::max(280, qCeil(picture.height() * scale) + kPad + kHeader))
            .boundedTo(bounded);
    };

    QList<QRect> rooms;
    if (launcher.isValid()) {
        const int top = std::max(area.top() + kGap, launcher.top());
        rooms << QRect(QPoint(launcher.right() + 1 + kGap, top), QPoint(area.right() - kGap, area.bottom() - kGap))
              << QRect(QPoint(area.left() + kGap, top), QPoint(launcher.left() - 1 - kGap, area.bottom() - kGap))
              << QRect(QPoint(std::max(area.left() + kGap, launcher.left()), launcher.bottom() + 1 + kGap),
                     QPoint(area.right() - kGap, area.bottom() - kGap));
    }
    // The side: the one chosen when the preview opened, while the launcher
    // is where it was (the window jumping from side to side as the arrows
    // go through small and large files is hard to follow); else the one
    // with the most room for a document (not for this file, which may be
    // a small picture before a long text).
    const auto usable = [](const QRect& room) { return room.width() >= 360 && room.height() >= 280; };
    // (Its height is not where it is: it changes with the number of rows.)
    const bool moved = m_sideFor.topLeft() != launcher.topLeft() || m_sideFor.width() != launcher.width();
    if (m_side < 0 || moved || (m_side < rooms.size() && !usable(rooms[m_side]))) {
        const QSize document = QSize(820, tallest * 85 / 100).boundedTo(QSize(area.width() - 2 * kGap, tallest));
        qint64 bestArea = 0;
        m_side = static_cast<int>(rooms.size()); // none: over the middle
        // Right or left before below: the rows grow downwards as one types.
        for (int i = 0; i < rooms.size(); ++i) {
            if (!usable(rooms[i]) || (i == 2 && m_side < rooms.size()))
                continue;
            const QSize fits = document.boundedTo(rooms[i].size());
            const qint64 a = static_cast<qint64>(fits.width()) * fits.height();
            if (a > bestArea && a * 2 >= static_cast<qint64>(document.width()) * document.height()) {
                bestArea = a;
                m_side = i;
            }
        }
        m_sideFor = launcher;
    }
    m_overLauncher = m_side >= rooms.size();
    if (m_overLauncher) {
        // Too little room beside it (the search panel, a launcher in the
        // middle of a small screen): over the launcher's right part, in
        // front of it, so that the names on the left stay in sight.
        // At most 60 % of its width: the names stay in sight.
        QRect over(QPoint(), fit(launcher.isValid() ? QSize(std::max(560, launcher.width() * 6 / 10), want.height()) : want));
        if (launcher.isValid()) {
            over.moveRight(std::min(launcher.right() - 24, area.right() - kGap));
            over.moveTop(std::clamp(launcher.center().y() - want.height() / 2, area.top() + kGap, area.bottom() - kGap - want.height()));
            if (over.left() < area.left() + kGap)
                over.moveLeft(area.left() + kGap);
        } else {
            over.moveCenter(area.center());
        }
        return over;
    }
    const QRect& room = rooms[m_side];
    QRect best(room.topLeft(), fit(room.size()));
    // Up against the launcher: from its right edge rightwards, from its left edge leftwards.
    if (room.right() < launcher.left())
        best.moveRight(room.right());
    return best;
}

bool FilePreview::screenRect(QQuickItem* item, int* left, int* top, int* right, int* bottom) const
{
    if (!item || !m_window)
        return false;
    // Logical within the window to physical on the screen, by the window's
    // own scale (its monitor's), from where its client area is.
    POINT origin {0, 0};
    if (!::ClientToScreen(reinterpret_cast<HWND>(m_window->winId()), &origin))
        return false;
    const qreal dpr = m_window->devicePixelRatio();
    const QPointF at = item->mapToScene(QPointF(0, 0));
    *left = origin.x + qRound(at.x() * dpr);
    *top = origin.y + qRound(at.y() * dpr);
    *right = origin.x + qRound((at.x() + item->width()) * dpr);
    *bottom = origin.y + qRound((at.y() + item->height()) * dpr);
    return *right > *left && *bottom > *top;
}

void FilePreview::setHandlerArea(QQuickItem* area)
{
    m_handlerArea = area;
    placeHandler();
}

void FilePreview::placeHandler()
{
    int left, top, right, bottom;
    if (m_kind == Handler && screenRect(m_handlerArea, &left, &top, &right, &bottom))
        m_host->move(left, top, right, bottom);
}

void FilePreview::startHandler(quint64 generation, const QUuid& handler)
{
    int left, top, right, bottom;
    if (!m_window || !screenRect(m_handlerArea, &left, &top, &right, &bottom)) {
        // The area is laid out a moment later (the window just resized): then.
        QMetaObject::invokeMethod(this, [this, generation, handler] {
            if (generation != m_generation.load() || m_kind != Handler)
                return;
            int l, t, r, b;
            if (screenRect(m_handlerArea, &l, &t, &r, &b))
                m_host->show(generation, m_item.path, handler, l, t, r, b, reinterpret_cast<HWND>(m_window->winId()), m_dark);
            else
                handlerFailed(generation);
        }, Qt::QueuedConnection);
        return;
    }
    m_host->show(generation, m_item.path, handler, left, top, right, bottom, reinterpret_cast<HWND>(m_window->winId()), m_dark);
}

// The handler could not be made, would not take the file, or did not answer
// in time: what is known of the file instead (a thumbnail, its facts).
void FilePreview::handlerFailed(quint64 generation)
{
    if (generation != m_generation.load() || !m_open || m_external)
        return;
    m_noHandlerFor = m_item.path;
    load(m_item);
}

} // namespace ws