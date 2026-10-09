#include "ClipboardWatcher.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QImage>

#include <shellapi.h>
#include <shlobj.h>

#include <latch>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr wchar_t kClassName[] = L"WinShun.ClipboardWatcher";
constexpr UINT kWriteMessage = WM_APP + 1; // lParam: a WriteJob
constexpr UINT_PTR kReadTimer = 1;
constexpr UINT kSettleMs = 40; // programs often put their formats up one after another
constexpr qsizetype kMaxText = 2 * 1024 * 1024; // characters: 4 MB, Windows' own limit for its history
constexpr qsizetype kFormattedUpTo = 1024 * 1024; // characters: longer text keeps no HTML / RTF
constexpr SIZE_T kMaxFormatBytes = 16 * 1024 * 1024;
constexpr SIZE_T kMaxImageBytes = 256 * 1024 * 1024;
constexpr qint64 kMaxPixels = 100'000'000;

struct Formats {
    UINT html = ::RegisterClipboardFormatW(L"HTML Format");
    UINT rtf = ::RegisterClipboardFormatW(L"Rich Text Format");
    UINT png = ::RegisterClipboardFormatW(L"PNG");
    UINT pngMime = ::RegisterClipboardFormatW(L"image/png");
    UINT dropEffect = ::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    // Not for clipboard history (Windows' own documented formats).
    UINT excludeMonitoring = ::RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    UINT canIncludeInHistory = ::RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    UINT viewerIgnore = ::RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    UINT ours = ::RegisterClipboardFormatW(L"WinShun.ClipboardEntry"); // see ClipWrite::id
};

const Formats& formats()
{
    static const Formats f;
    return f;
}

class ClipboardLock {
public:
    explicit ClipboardLock(bool open)
        : m_open(open)
    {
    }
    ~ClipboardLock() { close(); }
    void close()
    {
        if (std::exchange(m_open, false))
            ::CloseClipboard();
    }

private:
    bool m_open;
};

// A clipboard format's bytes; empty (and *tooBig set) beyond `maxBytes`.
QByteArray readBytes(UINT format, SIZE_T maxBytes, bool* tooBig = nullptr)
{
    const HANDLE handle = ::GetClipboardData(format);
    if (!handle)
        return {};
    const SIZE_T size = ::GlobalSize(handle);
    if (size > maxBytes) {
        if (tooBig)
            *tooBig = true;
        return {};
    }
    const void* data = ::GlobalLock(handle);
    if (!data)
        return {};
    QByteArray bytes(static_cast<const char*>(data), static_cast<qsizetype>(size));
    ::GlobalUnlock(handle);
    return bytes;
}

QByteArray withoutTrailingNuls(QByteArray bytes)
{
    while (bytes.endsWith('\0'))
        bytes.chop(1);
    return bytes;
}

QString readText(qint64* tooBig)
{
    const HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
    if (!handle)
        return {};
    const SIZE_T size = ::GlobalSize(handle);
    const auto* data = static_cast<const wchar_t*>(::GlobalLock(handle));
    if (!data)
        return {};
    const std::size_t length = ::wcsnlen(data, size / sizeof(wchar_t));
    QString text;
    if (static_cast<qsizetype>(length) > kMaxText)
        *tooBig = static_cast<qint64>(length * sizeof(wchar_t));
    else
        text = QString::fromWCharArray(data, static_cast<qsizetype>(length));
    ::GlobalUnlock(handle);
    return text;
}

QStringList readFiles()
{
    const auto drop = static_cast<HDROP>(::GetClipboardData(CF_HDROP));
    if (!drop)
        return {};
    QStringList files;
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        if (::DragQueryFileW(drop, i, path.data(), length + 1) == length)
            files.append(QString::fromWCharArray(path.c_str(), length));
    }
    return files;
}

// A DIB (CF_DIB, CF_DIBV5) is a BMP file without its 14-byte file header:
// give it one and Qt reads it, top-down, bit fields, alpha and all.
QImage imageFromDib(const QByteArray& dib)
{
    if (dib.size() < static_cast<qsizetype>(sizeof(BITMAPINFOHEADER)))
        return {};
    BITMAPINFOHEADER info;
    std::memcpy(&info, dib.constData(), sizeof info);
    if (info.biSize < sizeof(BITMAPINFOHEADER) || info.biWidth <= 0 || info.biHeight == 0)
        return {};
    qint64 colors = info.biClrUsed;
    if (colors == 0 && info.biBitCount <= 8)
        colors = 1ll << info.biBitCount;
    qint64 offset = info.biSize + colors * 4;
    // Masks after a plain header with BI_BITFIELDS; some programs leave them
    // after a V5 header too, where they are already inside it.
    const qint64 stride = ((static_cast<qint64>(info.biWidth) * info.biBitCount + 31) / 32) * 4;
    const qint64 pixels = stride * std::abs(static_cast<qint64>(info.biHeight));
    if (info.biCompression == BI_BITFIELDS && (info.biSize == sizeof(BITMAPINFOHEADER) || dib.size() - offset == pixels + 12))
        offset += 12;
    BITMAPFILEHEADER file {};
    file.bfType = 0x4D42; // "BM"
    file.bfSize = static_cast<DWORD>(sizeof file + dib.size());
    file.bfOffBits = static_cast<DWORD>(sizeof file + offset);
    QByteArray bmp(reinterpret_cast<const char*>(&file), sizeof file);
    bmp += dib;
    return QImage::fromData(bmp, "BMP");
}

QByteArray pixelHash(const QImage& image)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    QCryptographicHash hash(QCryptographicHash::Sha1);
    const qint32 size[] {argb.width(), argb.height()};
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(size), sizeof size));
    for (int y = 0; y < argb.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(argb.constScanLine(y)), argb.width() * 4));
    return hash.result();
}

std::wstring processPath(DWORD pid)
{
    if (pid == 0)
        return {};
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    std::wstring path(MAX_PATH * 4, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!::QueryFullProcessImageNameW(process, 0, path.data(), &size))
        size = 0;
    ::CloseHandle(process);
    path.resize(size);
    return path;
}

HGLOBAL globalCopy(const void* data, std::size_t size)
{
    const HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, size);
    if (!handle)
        return nullptr;
    void* target = ::GlobalLock(handle);
    if (!target) {
        ::GlobalFree(handle);
        return nullptr;
    }
    std::memcpy(target, data, size);
    ::GlobalUnlock(handle);
    return handle;
}

// Bottom-up 32-bit rows: what a DIB holds, from an ARGB32 image (whose
// bytes are B, G, R, A in memory).
QByteArray dibPixels(const QImage& argb)
{
    QByteArray pixels;
    pixels.reserve(static_cast<qsizetype>(argb.width()) * argb.height() * 4);
    for (int y = argb.height() - 1; y >= 0; --y)
        pixels.append(reinterpret_cast<const char*>(argb.constScanLine(y)), argb.width() * 4);
    return pixels;
}

} // namespace

struct ClipboardWatcher::WriteJob {
    ClipWrite data;
    std::function<void(bool)> done;
};

ClipboardWatcher::ClipboardWatcher(Options options, Callbacks callbacks)
    : m_callbacks(std::move(callbacks))
{
    setOptions(std::move(options)); // before the thread reads what is on the clipboard
    std::latch ready(1);
    m_thread = std::thread([this, &ready] {
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = &ClipboardWatcher::wndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kClassName;
        ::RegisterClassExW(&wc);
        const HWND hwnd = ::CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        if (hwnd && !::AddClipboardFormatListener(hwnd))
            qWarning() << "Clipboard history: AddClipboardFormatListener failed:" << ::GetLastError();
        m_window = hwnd;
        ready.count_down();
        if (!hwnd)
            return;
        // What is on the clipboard already (copied before Win顺 started).
        m_startSequence = ::GetClipboardSequenceNumber();
        ::PostMessageW(hwnd, WM_CLIPBOARDUPDATE, 0, 0);
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&msg);
    });
    ready.wait();
}

ClipboardWatcher::~ClipboardWatcher()
{
    if (const HWND hwnd = m_window.load())
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0); // the window ends the thread's message loop
    if (m_thread.joinable())
        m_thread.join();
}

void ClipboardWatcher::setOptions(Options options)
{
    for (QString& app : options.excludedApps) {
        app = app.trimmed().toLower();
        if (app.endsWith(u".exe"))
            app.chop(4);
    }
    const std::lock_guard lock(m_mutex);
    m_options = std::move(options);
}

void ClipboardWatcher::write(ClipWrite data, std::function<void(bool)> done)
{
    const HWND hwnd = m_window.load();
    auto* job = new WriteJob {std::move(data), std::move(done)};
    if (!hwnd || !::PostMessageW(hwnd, kWriteMessage, 0, reinterpret_cast<LPARAM>(job))) {
        if (job->done)
            job->done(false);
        delete job;
    }
}

LRESULT CALLBACK ClipboardWatcher::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<ClipboardWatcher*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CLIPBOARDUPDATE:
        ::SetTimer(hwnd, kReadTimer, kSettleMs, nullptr); // read once they are done
        return 0;
    case WM_TIMER:
        if (wParam == kReadTimer) {
            ::KillTimer(hwnd, kReadTimer);
            if (self)
                self->read();
        }
        return 0;
    case kWriteMessage: {
        auto* job = reinterpret_cast<WriteJob*>(lParam);
        const bool ok = self && self->writeNow(job->data);
        if (job->done)
            job->done(ok);
        delete job;
        return 0;
    }
    case WM_CLOSE:
        ::RemoveClipboardFormatListener(hwnd);
        if (self)
            self->m_window = nullptr;
        ::DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool ClipboardWatcher::openClipboard()
{
    // Another program may hold it for a moment (it is writing it, or reading
    // it too, like us).
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (::OpenClipboard(m_window.load()))
            return true;
        ::Sleep(attempt < 3 ? 10 : 30);
    }
    return false;
}

QString ClipboardWatcher::programName(const std::wstring& path)
{
    if (path.empty())
        return {};
    if (const auto it = m_programNames.find(path); it != m_programNames.end())
        return it->second;
    QString name;
    DWORD ignored = 0;
    if (const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored); size > 0) {
        std::vector<char> info(size);
        struct Translation {
            WORD language;
            WORD codePage;
        };
        Translation* translations = nullptr;
        UINT length = 0;
        if (::GetFileVersionInfoW(path.c_str(), 0, size, info.data())
            && ::VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length)
            && length >= sizeof(Translation)) {
            wchar_t key[64];
            ::swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].language,
                translations[0].codePage);
            wchar_t* description = nullptr;
            if (::VerQueryValueW(info.data(), key, reinterpret_cast<void**>(&description), &length) && length > 1)
                name = QString::fromWCharArray(description).trimmed();
        }
    }
    if (name.isEmpty())
        name = QFileInfo(QString::fromStdWString(path)).completeBaseName();
    m_programNames.emplace(path, name);
    return name;
}

void ClipboardWatcher::read()
{
    const DWORD sequence = ::GetClipboardSequenceNumber();
    if (sequence == m_lastSequence)
        return;
    Options options;
    {
        const std::lock_guard lock(m_mutex);
        options = m_options;
    }
    const Formats& f = formats();
    // Our own write, put back by another program (one that restores the
    // clipboard after using it). Our writes themselves are not read back.
    const bool ours = ::IsClipboardFormatAvailable(f.ours);
    // Not for the history: passwords and the like. Known without opening
    // the clipboard, which would get in their way.
    if (!ours
        && (!options.record || ::IsClipboardFormatAvailable(f.excludeMonitoring)
            || ::IsClipboardFormatAvailable(f.viewerIgnore))) {
        m_lastSequence = sequence;
        return;
    }
    if (!openClipboard()) { // held by another program for long: try again in a moment
        if (++m_readRetries <= 10)
            ::SetTimer(m_window.load(), kReadTimer, 200, nullptr);
        return;
    }
    m_readRetries = 0;
    m_lastSequence = sequence;
    const bool initial = sequence == m_startSequence;
    ClipboardLock lock(true);
    if (ours) {
        const QByteArray id = readBytes(f.ours, 64);
        lock.close();
        qint64 value = 0;
        if (id.size() >= static_cast<qsizetype>(sizeof value))
            std::memcpy(&value, id.constData(), sizeof value);
        if (value > 0 && !initial && m_callbacks.reused)
            m_callbacks.reused(value);
        return;
    }
    if (::IsClipboardFormatAvailable(f.canIncludeInHistory)) {
        const QByteArray flag = readBytes(f.canIncludeInHistory, 64);
        DWORD value = 1;
        if (flag.size() >= static_cast<qsizetype>(sizeof value))
            std::memcpy(&value, flag.constData(), sizeof value);
        if (value == 0)
            return;
    }

    // Where it came from: the clipboard's owner, else the program in front.
    DWORD pid = 0;
    if (const HWND owner = ::GetClipboardOwner())
        ::GetWindowThreadProcessId(owner, &pid);
    if (pid == 0) {
        if (const HWND foreground = ::GetForegroundWindow())
            ::GetWindowThreadProcessId(foreground, &pid);
    }
    const std::wstring path = processPath(pid);
    if (!path.empty() && !options.excludedApps.isEmpty()) {
        QString exe = QFileInfo(QString::fromStdWString(path)).fileName().toLower();
        if (exe.endsWith(u".exe"))
            exe.chop(4);
        if (options.excludedApps.contains(exe))
            return;
    }

    ClipCapture capture;
    QByteArray png;
    QByteArray dib;
    qint64 tooBig = 0;
    if (::IsClipboardFormatAvailable(CF_HDROP)) {
        const QStringList files = readFiles();
        if (!files.isEmpty()) {
            capture.kind = ClipKind::Files;
            capture.text = files.join(u'\n');
        }
    }
    if (capture.text.isEmpty() && ::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        QString text = readText(&tooBig);
        if (!text.trimmed().isEmpty()) {
            capture.kind = kindOfText(text);
            capture.text = std::move(text);
            if (capture.text.size() <= kFormattedUpTo) {
                if (::IsClipboardFormatAvailable(f.html))
                    capture.html = withoutTrailingNuls(readBytes(f.html, kMaxFormatBytes));
                if (::IsClipboardFormatAvailable(f.rtf))
                    capture.rtf = withoutTrailingNuls(readBytes(f.rtf, kMaxFormatBytes));
            }
        }
    }
    // A picture only when there is no text: Excel and Word offer a picture of
    // what was copied as well, which they draw only when asked for it.
    bool imageTooBig = false;
    if (capture.text.isEmpty() && tooBig == 0 && options.images) {
        if (::IsClipboardFormatAvailable(f.png))
            png = readBytes(f.png, kMaxImageBytes, &imageTooBig);
        else if (::IsClipboardFormatAvailable(f.pngMime))
            png = readBytes(f.pngMime, kMaxImageBytes, &imageTooBig);
        if (png.isEmpty() && !imageTooBig) {
            if (::IsClipboardFormatAvailable(CF_DIBV5))
                dib = readBytes(CF_DIBV5, kMaxImageBytes, &imageTooBig);
            else if (::IsClipboardFormatAvailable(CF_DIB))
                dib = readBytes(CF_DIB, kMaxImageBytes, &imageTooBig);
        }
    }
    lock.close(); // the slow part (pictures) without holding the clipboard

    if (tooBig > 0 || imageTooBig) {
        if (m_callbacks.skippedTooBig)
            m_callbacks.skippedTooBig(tooBig);
        return;
    }
    if (!png.isEmpty() || !dib.isEmpty()) {
        const QImage image = png.isEmpty() ? imageFromDib(dib) : QImage::fromData(png, "PNG");
        if (image.isNull())
            return;
        if (static_cast<qint64>(image.width()) * image.height() > kMaxPixels) {
            if (m_callbacks.skippedTooBig)
                m_callbacks.skippedTooBig(static_cast<qint64>(image.width()) * image.height() * 4);
            return;
        }
        capture.kind = ClipKind::Image;
        capture.width = image.width();
        capture.height = image.height();
        capture.hash = pixelHash(image);
        if (png.isEmpty()) {
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
        }
        capture.png = std::move(png);
    }
    if (capture.text.isEmpty() && capture.png.isEmpty())
        return;
    capture.sourcePath = QString::fromStdWString(path);
    capture.source = programName(path);
    capture.initial = initial;
    if (m_callbacks.captured)
        m_callbacks.captured(std::move(capture));
}

bool ClipboardWatcher::writeNow(const ClipWrite& data)
{
    const Formats& f = formats();
    std::vector<std::pair<UINT, HGLOBAL>> items;
    const auto add = [&](UINT format, const void* bytes, std::size_t size) {
        if (const HGLOBAL handle = globalCopy(bytes, size))
            items.emplace_back(format, handle);
    };
    if (!data.text.isEmpty())
        add(CF_UNICODETEXT, data.text.utf16(), (static_cast<std::size_t>(data.text.size()) + 1) * sizeof(char16_t));
    if (!data.html.isEmpty()) {
        const QByteArray html = data.html + '\0';
        add(f.html, html.constData(), static_cast<std::size_t>(html.size()));
    }
    if (!data.rtf.isEmpty()) {
        const QByteArray rtf = data.rtf + '\0';
        add(f.rtf, rtf.constData(), static_cast<std::size_t>(rtf.size()));
    }
    if (!data.files.isEmpty()) {
        std::wstring list;
        for (const QString& file : data.files) {
            list += file.toStdWString();
            list += L'\0';
        }
        list += L'\0';
        QByteArray drop(sizeof(DROPFILES), '\0');
        auto* header = reinterpret_cast<DROPFILES*>(drop.data());
        header->pFiles = sizeof(DROPFILES);
        header->fWide = TRUE;
        drop.append(reinterpret_cast<const char*>(list.data()), static_cast<qsizetype>(list.size() * sizeof(wchar_t)));
        add(CF_HDROP, drop.constData(), static_cast<std::size_t>(drop.size()));
        const DWORD effect = DROPEFFECT_COPY; // pasting from the history never moves files
        add(f.dropEffect, &effect, sizeof effect);
    }
    if (!data.imagePath.isEmpty()) {
        QFile file(data.imagePath);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        const QByteArray png = file.readAll();
        const QImage image = QImage::fromData(png, "PNG").convertToFormat(QImage::Format_ARGB32);
        if (image.isNull())
            return false;
        add(f.png, png.constData(), static_cast<std::size_t>(png.size()));
        const QByteArray pixels = dibPixels(image);
        // CF_DIB, 32 bits a pixel (most programs ignore the fourth byte) ...
        BITMAPINFOHEADER info {sizeof(BITMAPINFOHEADER)};
        info.biWidth = image.width();
        info.biHeight = image.height(); // bottom-up
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        info.biSizeImage = static_cast<DWORD>(pixels.size());
        QByteArray dib(reinterpret_cast<const char*>(&info), sizeof info);
        dib += pixels;
        add(CF_DIB, dib.constData(), static_cast<std::size_t>(dib.size()));
        // ... and CF_DIBV5 with its alpha, for those that read transparency.
        BITMAPV5HEADER v5 {sizeof(BITMAPV5HEADER)};
        v5.bV5Width = image.width();
        v5.bV5Height = image.height();
        v5.bV5Planes = 1;
        v5.bV5BitCount = 32;
        v5.bV5Compression = BI_BITFIELDS;
        v5.bV5SizeImage = static_cast<DWORD>(pixels.size());
        v5.bV5RedMask = 0x00FF0000;
        v5.bV5GreenMask = 0x0000FF00;
        v5.bV5BlueMask = 0x000000FF;
        v5.bV5AlphaMask = 0xFF000000;
        v5.bV5CSType = LCS_sRGB;
        v5.bV5Intent = LCS_GM_IMAGES;
        QByteArray dibV5(reinterpret_cast<const char*>(&v5), sizeof v5);
        dibV5 += pixels;
        add(CF_DIBV5, dibV5.constData(), static_cast<std::size_t>(dibV5.size()));
    }
    // Ours: its entry moves up instead of being added again (none for a
    // joined paste, which is not recorded).
    const qint64 id = data.id;
    add(f.ours, &id, sizeof id);

    if (items.size() <= 1 || !openClipboard()) {
        for (const auto& [format, handle] : items)
            ::GlobalFree(handle);
        return false;
    }
    ::EmptyClipboard();
    bool ok = true;
    for (const auto& [format, handle] : items) {
        if (!::SetClipboardData(format, handle)) { // the clipboard owns it once this works
            ::GlobalFree(handle);
            ok = false;
        }
    }
    ::CloseClipboard();
    // Known without reading it back: opening the clipboard for that would
    // get in the way of the paste that follows (the target opens it then).
    m_lastSequence = ::GetClipboardSequenceNumber();
    if (ok && id > 0 && m_callbacks.reused)
        m_callbacks.reused(id);
    return ok;
}

} // namespace ws
