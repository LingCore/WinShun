#include "ClipboardWatcher.h"
#include "ImageOrigin.h"

#include "ClipImage.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <latch>
#include <memory>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr wchar_t kClassName[] = L"WinShun.ClipboardWatcher";
constexpr UINT kWriteMessage = WM_APP + 1; // lParam: a WriteJob
constexpr UINT kOptionsMessage = WM_APP + 2;
constexpr UINT_PTR kReadTimer = 1;
constexpr UINT_PTR kIdleTimer = 2;
constexpr UINT kIdleMs = 1000; // an off module needs a worker only while a copy is being written
constexpr UINT kSettleMs = 40; // programs often put their formats up one after another
constexpr qsizetype kMaxText = 2 * 1024 * 1024; // characters: 4 MB, Windows' own limit for its history
constexpr qsizetype kFormattedUpTo = 1024 * 1024; // characters: longer text keeps no HTML / RTF
constexpr SIZE_T kMaxFormatBytes = 16 * 1024 * 1024;
constexpr qsizetype kMaxObjectBytes = 32 * 1024 * 1024; // an equation's or an object's formats all together
constexpr SIZE_T kMaxImageBytes = 256 * 1024 * 1024;

struct Formats {
    UINT html = ::RegisterClipboardFormatW(L"HTML Format");
    UINT rtf = ::RegisterClipboardFormatW(L"Rich Text Format");
    UINT png = ::RegisterClipboardFormatW(L"PNG");
    UINT pngMime = ::RegisterClipboardFormatW(L"image/png");
    UINT dropEffect = ::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    // An equation or an embedded object (GitHub #58).
    UINT mathMl = ::RegisterClipboardFormatW(L"MathML");
    UINT objectDescriptor = ::RegisterClipboardFormatW(L"Object Descriptor");
    UINT embeddedObject = ::RegisterClipboardFormatW(L"Embedded Object");
    UINT embedSource = ::RegisterClipboardFormatW(L"Embed Source");
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

// CF_ENHMETAFILE's bits (it is a handle, not memory); empty (and *tooBig
// set) beyond `maxBytes`.
QByteArray metafileBits(SIZE_T maxBytes, bool* tooBig)
{
    const auto emf = static_cast<HENHMETAFILE>(::GetClipboardData(CF_ENHMETAFILE));
    const UINT size = emf ? ::GetEnhMetaFileBits(emf, 0, nullptr) : 0;
    if (size > maxBytes)
        *tooBig = true;
    if (size == 0 || size > maxBytes)
        return {};
    QByteArray bits(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (::GetEnhMetaFileBits(emf, size, reinterpret_cast<BYTE*>(bits.data())) != size)
        return {};
    return bits;
}

// Whether an Object Descriptor names a class of an equation editor's
// (Equation 3.0, MathType, WPS's 公式编辑器: "Equation.3", "Equation.DSMT4",
// "Equation.KSEE3"), by its ProgID.
bool equationClass(const QByteArray& descriptor)
{
    OBJECTDESCRIPTOR object {};
    if (descriptor.size() < static_cast<qsizetype>(sizeof object))
        return false;
    std::memcpy(&object, descriptor.constData(), sizeof object);
    wchar_t clsid[40];
    if (::StringFromGUID2(object.clsid, clsid, 40) == 0)
        return false;
    const std::wstring key = L"CLSID\\" + std::wstring(clsid) + L"\\ProgID";
    wchar_t progId[128];
    DWORD size = sizeof progId;
    return ::RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, progId, &size) == ERROR_SUCCESS
        && ::_wcsnicmp(progId, L"Equation.", 9) == 0;
}

// Whether what is on the (open) clipboard is an equation or an embedded
// object, which pastes back as one only from its own formats (GitHub #58).
// Word offers MathML for an equation copied alone (not for text with one in
// it, which its RTF pastes back as it was); a container offers "Embedded
// Object" for an object it holds; an equation editor may offer "Embed Source"
// of a class of its own. Not "Embed Source" by itself: Word, Excel and
// PowerPoint offer it with whatever they copy, and reading it has them
// write a whole document.
bool isObject(const Formats& f)
{
    if (::IsClipboardFormatAvailable(f.mathMl) && ::IsClipboardFormatAvailable(CF_ENHMETAFILE))
        return true;
    if (!::IsClipboardFormatAvailable(f.objectDescriptor))
        return false;
    return ::IsClipboardFormatAvailable(f.embeddedObject)
        || (::IsClipboardFormatAvailable(f.embedSource) && equationClass(readBytes(f.objectDescriptor, 64 * 1024)));
}

// The name an equation's or an object's format is kept under
// (ClipFormat::name); empty for the rest. Not kept: what links to the
// document it came from (Link Source, ObjectLink ...), OLE's own (DataObject,
// Ole Private Data, OLE 1's Native and OwnerLink), what Windows makes from
// the others (CF_TEXT, CF_METAFILEPICT ...), and the pictures, which are
// written from the entry's PNG.
QString objectFormatName(UINT format)
{
    if (format == CF_UNICODETEXT)
        return u"CF_UNICODETEXT"_s;
    if (format == CF_ENHMETAFILE)
        return u"CF_ENHMETAFILE"_s;
    static const QStringList kept {u"Object Descriptor"_s, u"Embedded Object"_s, u"Embed Source"_s,
        u"Rich Text Format"_s, u"HTML Format"_s, u"MathML"_s, u"MathML Presentation"_s};
    wchar_t name[64];
    const int length = format >= 0xC000 ? ::GetClipboardFormatNameW(format, name, 64) : 0;
    const QString s = QString::fromWCharArray(name, std::max(length, 0));
    return length > 0 && kept.contains(s) ? s : QString();
}

// An equation's or an object's formats, in the order the program put them
// (the clipboard is open); none if one is too big, or all of them more than
// kMaxObjectBytes: some of them might paste as something else.
QList<ClipFormat> readObjectFormats()
{
    QList<ClipFormat> formats;
    qsizetype total = 0;
    for (UINT format = ::EnumClipboardFormats(0); format != 0; format = ::EnumClipboardFormats(format)) {
        QString name = objectFormatName(format);
        if (name.isEmpty())
            continue;
        bool tooBig = false;
        QByteArray data = format == CF_ENHMETAFILE ? metafileBits(kMaxFormatBytes, &tooBig)
                                                   : readBytes(format, kMaxFormatBytes, &tooBig);
        if (tooBig)
            return {};
        if (data.isEmpty())
            continue; // not given after all: Word draws no HTML of an equation
        total += data.size();
        if (total > kMaxObjectBytes)
            return {};
        formats.append({std::move(name), std::move(data)});
    }
    return formats;
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

// A kept format's number here (ClipFormat::name; CF_ENHMETAFILE aside).
UINT formatId(const QString& name)
{
    if (name == u"CF_UNICODETEXT"_s)
        return CF_UNICODETEXT;
    return ::RegisterClipboardFormatW(qUtf16Printable(name));
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

namespace {
ClipboardWatcher::Options normalizedOptions(ClipboardWatcher::Options options)
{
    for (QString& app : options.excludedApps) {
        app = app.trimmed().toLower();
        if (app.endsWith(u".exe"))
            app.chop(4);
    }
    return options;
}
}

ClipboardWatcher::ClipboardWatcher(Options options, Callbacks callbacks)
    : m_callbacks(std::move(callbacks))
    , m_options(normalizedOptions(std::move(options)))
{
    if (m_options.record) {
        std::lock_guard lock(m_lifecycleMutex);
        startWorker(true);
    }
}

void ClipboardWatcher::startWorker(bool captureInitial)
{
    if (m_window.load())
        return;
    if (m_thread.joinable())
        m_thread.join();
    std::latch ready(1);
    m_running = true;
    m_thread = std::thread([this, &ready, captureInitial] {
        const auto ended = qScopeGuard([this] { m_running = false; });
        m_lastSequence = 0;
        m_startSequence = 0;
        m_readRetries = 0;
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = &ClipboardWatcher::wndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kClassName;
        ::RegisterClassExW(&wc);
        const HWND hwnd = ::CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        m_window = hwnd;
        if (hwnd)
            configureWatching(hwnd);
        ready.count_down();
        if (!hwnd)
            return;
        // What is on the clipboard already (copied before Win顺 started).
        if (captureInitial && m_listening) {
            m_lastSequence = 0;
            m_startSequence = ::GetClipboardSequenceNumber();
            ::PostMessageW(hwnd, WM_CLIPBOARDUPDATE, 0, 0);
        }
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&msg);
        if (::IsWindow(hwnd))
            ::SendMessageW(hwnd, WM_CLOSE, 0, 0);
        m_programNames = decltype(m_programNames){};
    });
    ready.wait();
}

ClipboardWatcher::~ClipboardWatcher()
{
    std::lock_guard lock(m_lifecycleMutex);
    if (const HWND hwnd = m_window.load())
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0); // the window ends the thread's message loop
    if (m_thread.joinable())
        m_thread.join();
}

void ClipboardWatcher::setOptions(Options options)
{
    const bool record = options.record;
    {
        const std::lock_guard lock(m_mutex);
        m_options = normalizedOptions(std::move(options));
    }
    std::lock_guard lock(m_lifecycleMutex);
    if (record)
        startWorker(false);
    if (const HWND window = m_window.load())
        ::PostMessageW(window, kOptionsMessage, 0, 0);
}

bool ClipboardWatcher::recordingEnabled()
{
    std::lock_guard lock(m_mutex);
    return m_options.record;
}

void ClipboardWatcher::configureWatching(HWND window)
{
    if (recordingEnabled()) {
        ::KillTimer(window, kIdleTimer);
        if (!m_listening) {
            m_lastSequence = ::GetClipboardSequenceNumber(); // do not backfill copies made while disabled
            m_listening = ::AddClipboardFormatListener(window) != FALSE;
            if (!m_listening)
                qWarning() << "Clipboard history: AddClipboardFormatListener failed:" << ::GetLastError();
        }
    } else {
        if (m_listening.exchange(false))
            ::RemoveClipboardFormatListener(window);
        ::KillTimer(window, kReadTimer);
        ::SetTimer(window, kIdleTimer, kIdleMs, nullptr);
    }
}

void ClipboardWatcher::write(ClipWrite data, std::function<void(bool)> done)
{
    auto job = std::make_unique<WriteJob>(WriteJob{std::move(data), std::move(done)});
    {
        std::lock_guard lock(m_lifecycleMutex);
        startWorker(false);
        ++m_pendingWrites;
        if (const HWND hwnd = m_window.load(); hwnd && ::PostMessageW(hwnd, kWriteMessage, 0, reinterpret_cast<LPARAM>(job.get()))) {
            job.release();
            return;
        }
        --m_pendingWrites;
    }
    if (job->done)
        job->done(false);
}

LRESULT CALLBACK ClipboardWatcher::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<ClipboardWatcher*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case kOptionsMessage:
        if (self)
            self->configureWatching(hwnd);
        return 0;
    case WM_CLIPBOARDUPDATE:
        if (self && self->m_listening)
            ::SetTimer(hwnd, kReadTimer, kSettleMs, nullptr); // read once they are done
        return 0;
    case WM_TIMER:
        if (wParam == kReadTimer) {
            ::KillTimer(hwnd, kReadTimer);
            if (self && self->m_listening)
                self->read();
        } else if (wParam == kIdleTimer && self) {
            std::unique_lock lock(self->m_lifecycleMutex, std::try_to_lock);
            if (lock.owns_lock() && !self->recordingEnabled() && self->m_pendingWrites == 0) {
                ::KillTimer(hwnd, kIdleTimer);
                ::SendMessageW(hwnd, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    case kWriteMessage: {
        auto* job = reinterpret_cast<WriteJob*>(lParam);
        const bool ok = self && self->writeNow(job->data);
        if (job->done)
            job->done(ok);
        delete job;
        if (self) {
            --self->m_pendingWrites;
            if (!self->recordingEnabled())
                ::SetTimer(hwnd, kIdleTimer, kIdleMs, nullptr);
        }
        return 0;
    }
    case WM_CLOSE:
        if (self && self->m_listening.exchange(false))
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
    QList<QByteArray> encodedImages;
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
    // An equation or an embedded object: its own formats too, to paste it
    // back from (pictures are what the history lists it by).
    QList<ClipFormat> object;
    if (capture.kind != ClipKind::Files && tooBig == 0 && options.images && isObject(f))
        object = readObjectFormats();
    // A picture only when there is no text (or it is an equation): Excel and
    // Word offer a picture of what was copied as well, which they draw only
    // when asked for it.
    bool imageTooBig = false;
    if ((capture.text.isEmpty() || !object.isEmpty()) && tooBig == 0 && options.images) {
        SIZE_T remaining = kMaxImageBytes;
        for (const ClipImageOffer& offer : clipImageOffers()) {
            const QString name = offer.name.toString();
            const UINT format = ::RegisterClipboardFormatW(qUtf16Printable(name));
            if (!::IsClipboardFormatAvailable(format))
                continue;
            QByteArray bytes = readBytes(format, remaining, &imageTooBig);
            remaining -= static_cast<SIZE_T>(bytes.size());
            if (!bytes.isEmpty())
                encodedImages.append(std::move(bytes));
        }
        if (::IsClipboardFormatAvailable(CF_DIBV5))
            dib = readBytes(CF_DIBV5, remaining, &imageTooBig);
        if (dib.isEmpty() && ::IsClipboardFormatAvailable(CF_DIB))
            dib = readBytes(CF_DIB, remaining, &imageTooBig);
    }
    lock.close(); // the slow part (pictures) without holding the clipboard

    DecodedClipImage picture;
    for (QByteArray& bytes : encodedImages) {
        auto candidate = decodeClipImage(std::move(bytes));
        imageTooBig = imageTooBig || candidate.tooBig;
        if (!candidate.firstFrame.isNull()) {
            picture = std::move(candidate);
            break;
        }
    }
    if (picture.firstFrame.isNull() && !dib.isEmpty())
        picture = encodeClipImage(imageFromDib(dib));
    if (!object.isEmpty()) {
        // Listed by the picture it came with, else by its metafile drawn
        // (Word gives an equation no other); pasted from its formats. With
        // no picture to show, it is kept as it would be without them.
        if (picture.firstFrame.isNull()) {
            const auto metafile = std::ranges::find(object, u"CF_ENHMETAFILE"_s, &ClipFormat::name);
            if (metafile != object.cend())
                picture = encodeClipImage(imageFromEmf(metafile->data));
        }
        if (!picture.firstFrame.isNull()) {
            imageTooBig = false;
            // Its text stays (Word's linear form of an equation): the search
            // finds it by that, Shift+Enter pastes it. HTML and RTF are
            // among its formats.
            capture.html.clear();
            capture.rtf.clear();
            capture.formats = packClipFormats(object);
        } else {
            imageTooBig = imageTooBig && capture.text.isEmpty(); // else its text, as before
        }
    }
    if (tooBig > 0 || (picture.firstFrame.isNull() && (imageTooBig || picture.tooBig))) {
        if (m_callbacks.skippedTooBig)
            m_callbacks.skippedTooBig(tooBig);
        return;
    }
    if (!picture.firstFrame.isNull()) {
        capture.kind = ClipKind::Image;
        capture.width = picture.firstFrame.width();
        capture.height = picture.firstFrame.height();
        capture.hash = std::move(picture.hash);
        capture.imageFormat = picture.format;
        capture.animated = picture.animated;
        capture.imageData = std::move(picture.data);
    }
    if (capture.text.isEmpty() && capture.imageData.isEmpty())
        return;
    if (capture.hash.isEmpty()) // here, not on the GUI thread: megabytes of text take a while
        capture.hash = clipHash(capture.kind, capture.text);
    capture.sourcePath = QString::fromStdWString(path);
    capture.source = programName(path);
    capture.initial = initial;
    if (m_callbacks.captured)
        m_callbacks.captured(std::move(capture));
}

bool ClipboardWatcher::writeNow(const ClipWrite& data)
{
    if (data.empty())
        return false; // an empty request must never erase the user's clipboard
    const Formats& f = formats();
    std::vector<std::pair<UINT, HANDLE>> items; // memory, a metafile for CF_ENHMETAFILE
    const auto add = [&](UINT format, const void* bytes, std::size_t size) {
        if (const HGLOBAL handle = globalCopy(bytes, size))
            items.emplace_back(format, handle);
    };
    const auto release = [](UINT format, HANDLE handle) {
        if (format == CF_ENHMETAFILE)
            ::DeleteEnhMetaFile(static_cast<HENHMETAFILE>(handle));
        else
            ::GlobalFree(handle);
    };
    const auto fail = [&] {
        for (const auto& [format, handle] : items)
            release(format, handle);
        return false;
    };
    // An equation's or an embedded object's own formats first, as the
    // program that copied it put them: Word and WPS paste it from them, the
    // picture after them is for the rest.
    for (const ClipFormat& format : unpackClipFormats(data.formats)) {
        if (format.name == u"CF_ENHMETAFILE"_s) {
            if (const HENHMETAFILE emf = ::SetEnhMetaFileBits(static_cast<UINT>(format.data.size()),
                    reinterpret_cast<const BYTE*>(format.data.constData())))
                items.emplace_back(CF_ENHMETAFILE, emf);
        } else if (const UINT id = formatId(format.name)) {
            add(id, format.data.constData(), static_cast<std::size_t>(format.data.size()));
        }
    }
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
    if (!data.imagePath.isEmpty() || !data.imageData.isEmpty()) {
        QByteArray encoded = data.imageData;
        if (encoded.isEmpty()) {
            QFile file(data.imagePath);
            if (!file.open(QIODevice::ReadOnly) || file.size() > kClipMaxImageBytes)
                return fail();
            encoded = file.readAll();
        }
        const auto picture = decodeClipImage(std::move(encoded));
        if (picture.firstFrame.isNull())
            return fail();
        // Original formats precede the still fallback so capable targets
        // receive all frames. Simpler editors can still paste the first frame.
        if (picture.format != ClipImageFormat::Png) {
            for (const ClipImageOffer& offer : clipImageOffers()) {
                if (offer.format != picture.format)
                    continue;
                const QString name = offer.name.toString();
                add(::RegisterClipboardFormatW(qUtf16Printable(name)), picture.data.constData(),
                    static_cast<std::size_t>(picture.data.size()));
            }
        }
        const QByteArray png = picture.format == ClipImageFormat::Png ? picture.data : encodeClipImage(picture.firstFrame).data;
        if (png.isEmpty())
            return fail();
        add(f.png, png.constData(), static_cast<std::size_t>(png.size()));
        add(f.pngMime, png.constData(), static_cast<std::size_t>(png.size()));
        const QImage image = picture.firstFrame.convertToFormat(QImage::Format_ARGB32);
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
    // WINSHUN_TEST_NO_CLIP_HISTORY (live tests): Windows' own history (25 items,
    // the oldest pushed out by every copy) and our watcher leave this copy alone.
    if (qEnvironmentVariableIsSet("WINSHUN_TEST_NO_CLIP_HISTORY")) {
        const DWORD zero = 0;
        add(::RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), &zero, sizeof zero);
        add(::RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"), &zero, sizeof zero);
    }
    // Ours: its entry moves up instead of being added again (none for a
    // joined paste, which is not recorded).
    const qint64 id = data.id;
    add(f.ours, &id, sizeof id);
    if (!data.imageOrigin.isEmpty()) {
        const QByteArray origin = data.imageOrigin.toUtf8() + '\0';
        add(::RegisterClipboardFormatW(kImageOriginFormat), origin.constData(), static_cast<std::size_t>(origin.size()));
    }

    if (items.size() <= 1 || !openClipboard())
        return fail();
    ::EmptyClipboard();
    bool ok = true;
    for (const auto& [format, handle] : items) {
        if (!::SetClipboardData(format, handle)) { // the clipboard owns it once this works
            release(format, handle);
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
