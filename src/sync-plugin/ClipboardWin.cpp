#include "ClipboardWin.h"

#include "Util.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

namespace shunsync {

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kJobMessage = WM_APP + 1;
constexpr UINT_PTR kSettleTimer = 1;
constexpr UINT kSettleMs = 150; // a program writing its formats one after another: read once they are all there
constexpr std::size_t kMaxTextChars = 4u * 1024 * 1024;
constexpr std::size_t kMaxFiles = 20000;
constexpr std::uint64_t kMaxPixels = 50'000'000; // a CF_DIB of 200 MB is as far as it goes

HGLOBAL globalOf(const void* data, std::size_t size)
{
    const HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, std::max<std::size_t>(size, 1));
    if (!h)
        return nullptr;
    if (void* p = ::GlobalLock(h)) {
        if (size)
            std::memcpy(p, data, size);
        ::GlobalUnlock(h);
        return h;
    }
    ::GlobalFree(h);
    return nullptr;
}

ComPtr<IWICImagingFactory> wicFactory()
{
    ComPtr<IWICImagingFactory> factory;
    ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    return factory;
}

// A PNG's pixels on white, as a bottom-up 24-bit DIB (BITMAPINFOHEADER then rows).
std::optional<Bytes> dibOnWhite(std::string_view png)
{
    const ComPtr<IWICImagingFactory> factory = wicFactory();
    if (!factory)
        return std::nullopt;
    ComPtr<IStream> stream;
    stream.Attach(::SHCreateMemStream(reinterpret_cast<const BYTE*>(png.data()), static_cast<UINT>(png.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (!stream || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        || FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
        return std::nullopt;
    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || !width || !height || static_cast<std::uint64_t>(width) * height > kMaxPixels)
        return std::nullopt;
    std::vector<BYTE> pixels(static_cast<std::size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())))
        return std::nullopt;
    const std::size_t rowBytes = (static_cast<std::size_t>(width) * 3 + 3) & ~std::size_t(3);
    Bytes dib(sizeof(BITMAPINFOHEADER) + rowBytes * height, '\0');
    BITMAPINFOHEADER header {};
    header.biSize = sizeof header;
    header.biWidth = static_cast<LONG>(width);
    header.biHeight = static_cast<LONG>(height); // bottom-up: what every program reads
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(rowBytes * height);
    std::memcpy(dib.data(), &header, sizeof header);
    auto* out = reinterpret_cast<BYTE*>(dib.data() + sizeof header);
    for (UINT row = 0; row < height; ++row) {
        const BYTE* source = pixels.data() + static_cast<std::size_t>(height - 1 - row) * width * 4;
        BYTE* target = out + rowBytes * row;
        for (UINT x = 0; x < width; ++x, source += 4, target += 3) {
            const unsigned alpha = source[3];
            for (int c = 0; c < 3; ++c)
                target[c] = static_cast<BYTE>((source[c] * alpha + 255 * (255 - alpha) + 127) / 255);
        }
    }
    return dib;
}

// The clipboard's bitmap as a PNG (what was copied is opaque: a screenshot,
// a picture from a program that puts no PNG).
std::optional<Bytes> pngOfBitmap(HBITMAP bitmapHandle)
{
    const ComPtr<IWICImagingFactory> factory = wicFactory();
    if (!factory)
        return std::nullopt;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<IStream> stream;
    stream.Attach(::SHCreateMemStream(nullptr, 0));
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    UINT width = 0;
    UINT height = 0;
    if (!stream || FAILED(factory->CreateBitmapFromHBITMAP(bitmapHandle, nullptr, WICBitmapIgnoreAlpha, &bitmap))
        || FAILED(bitmap->GetSize(&width, &height)) || !width || !height || static_cast<std::uint64_t>(width) * height > kMaxPixels
        || FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom))
        || FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
        || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, &properties))
        || FAILED(frame->Initialize(properties.Get())) || FAILED(frame->SetSize(width, height)))
        return std::nullopt;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->WriteSource(converter.Get(), nullptr)) || FAILED(frame->Commit())
        || FAILED(encoder->Commit()))
        return std::nullopt;
    ULARGE_INTEGER size {};
    if (FAILED(::IStream_Size(stream.Get(), &size)) || size.QuadPart > kMaxImageBytes || FAILED(::IStream_Reset(stream.Get())))
        return std::nullopt;
    Bytes bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    if (FAILED(::IStream_Read(stream.Get(), bytes.data(), static_cast<ULONG>(bytes.size()))))
        return std::nullopt;
    return bytes;
}

std::wstring windowsLineEnds(const std::string& text)
{
    const std::wstring in = localsend::wide(text);
    std::wstring out;
    out.reserve(in.size() + in.size() / 16);
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'\r' && i + 1 < in.size() && in[i + 1] == L'\n')
            continue; // "\r\n" sent: one "\r\n" here
        if (in[i] == L'\n')
            out += L'\r';
        if (in[i] != L'\0')
            out += in[i];
    }
    return out;
}

} // namespace

std::pair<int, int> pngSize(std::string_view png)
{
    static constexpr std::string_view signature("\x89PNG\r\n\x1a\n", 8);
    if (png.size() < 24 || png.substr(0, 8) != signature || png.substr(12, 4) != "IHDR")
        return {0, 0};
    return {static_cast<int>(be32(png.substr(16))), static_cast<int>(be32(png.substr(20)))};
}

ClipboardWin::ClipboardWin(CopiedHandler copied, std::function<void(const std::string&)> log)
    : m_copied(std::move(copied)), m_log(std::move(log))
{
    m_originFormat = ::RegisterClipboardFormatW(L"ShunSync.Origin");
    m_pngFormat = ::RegisterClipboardFormatW(L"PNG");
    m_dropEffectFormat = ::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    m_excludeFormat = ::RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    m_historyFormat = ::RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    m_cloudFormat = ::RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
    m_ignoreFormat = ::RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
}

ClipboardWin::~ClipboardWin() { stop(); }

bool ClipboardWin::start()
{
    std::promise<bool> started;
    std::future<bool> ready = started.get_future();
    m_thread = std::thread([this, started = std::move(started)]() mutable { run(std::move(started)); });
    if (ready.get())
        return true;
    m_thread.join();
    return false;
}

void ClipboardWin::stop()
{
    if (!m_thread.joinable())
        return;
    if (m_hwnd)
        ::PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    m_thread.join();
    m_hwnd = nullptr;
    const std::lock_guard lock(m_mutex);
    for (const auto& job : m_jobs)
        job->done.set_value(false);
    m_jobs.clear();
}

void ClipboardWin::setWatching(bool on)
{
    const std::lock_guard lock(m_mutex);
    m_watching = on;
}

void ClipboardWin::run(std::promise<bool> started)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // WIC
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &ClipboardWin::windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"WinShunSyncClipboard";
    ::RegisterClassExW(&wc);
    m_threadId = ::GetCurrentThreadId();
    m_hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    if (!m_hwnd || !::AddClipboardFormatListener(m_hwnd)) {
        if (m_log)
            m_log("clipboard: not watched (" + std::to_string(::GetLastError()) + ")");
        if (m_hwnd)
            ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        started.set_value(false);
        if (SUCCEEDED(com))
            ::CoUninitialize();
        return;
    }
    m_lastSequence = ::GetClipboardSequenceNumber(); // what is there already is not sent
    started.set_value(true);
    MSG message;
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    if (SUCCEEDED(com))
        ::CoUninitialize();
}

LRESULT CALLBACK ClipboardWin::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* self = static_cast<ClipboardWin*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<ClipboardWin*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self)
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    switch (message) {
    case WM_CLIPBOARDUPDATE:
        ::SetTimer(hwnd, kSettleTimer, kSettleMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kSettleTimer) {
            ::KillTimer(hwnd, kSettleTimer);
            localsend::guarded("the clipboard", [self] { self->changedNow(); });
        }
        return 0;
    case kJobMessage: {
        std::deque<std::shared_ptr<Job>> jobs;
        {
            const std::lock_guard lock(self->m_mutex);
            jobs.swap(self->m_jobs);
        }
        for (const auto& job : jobs) {
            bool ok = false;
            localsend::guarded("a clipboard job", [&] { ok = job->work(); });
            job->done.set_value(ok);
        }
        return 0;
    }
    case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        ::RemoveClipboardFormatListener(hwnd);
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

bool ClipboardWin::onWindow(std::function<bool()> work)
{
    if (::GetCurrentThreadId() == m_threadId)
        return work();
    auto job = std::make_shared<Job>();
    job->work = std::move(work);
    std::future<bool> done = job->done.get_future();
    {
        const std::lock_guard lock(m_mutex);
        if (!m_hwnd)
            return false;
        m_jobs.push_back(job);
    }
    ::PostMessageW(m_hwnd, kJobMessage, 0, 0);
    // A program that renders what it copied only when asked can keep the
    // window's thread a while: not waited for longer than this.
    if (done.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        return false;
    return done.get();
}

void ClipboardWin::changedNow()
{
    {
        const std::lock_guard lock(m_mutex);
        if (!m_watching)
            return;
    }
    const DWORD sequence = ::GetClipboardSequenceNumber();
    if (sequence == m_lastSequence || sequence == m_ownSequence)
        return;
    bool busy = false;
    std::optional<Copied> copied = read(true, &busy);
    if (busy && m_retries++ < 5) {
        ::SetTimer(m_hwnd, kSettleTimer, 100, nullptr); // another program has it open: in a moment
        return;
    }
    m_retries = 0;
    m_lastSequence = sequence;
    if (copied && m_copied)
        m_copied(std::move(*copied));
}

std::optional<Copied> ClipboardWin::read(bool skipOurs, bool* busy)
{
    bool open = false;
    for (int tries = 0; tries < 10 && !open; ++tries) {
        open = ::OpenClipboard(m_hwnd) != 0;
        if (!open)
            ::Sleep(15);
    }
    if (!open) {
        if (busy)
            *busy = true;
        return std::nullopt;
    }
    struct Closer {
        ~Closer() { ::CloseClipboard(); }
    } closer;
    if (skipOurs && ::IsClipboardFormatAvailable(m_originFormat))
        return std::nullopt; // what came from another device: never sent on
    if (::IsClipboardFormatAvailable(m_excludeFormat) || ::IsClipboardFormatAvailable(m_ignoreFormat))
        return std::nullopt; // a password manager's
    const auto zeroDword = [](UINT format) {
        if (!::IsClipboardFormatAvailable(format))
            return false;
        const HANDLE h = ::GetClipboardData(format);
        const auto* value = h ? static_cast<const DWORD*>(::GlobalLock(h)) : nullptr;
        const bool zero = value && ::GlobalSize(h) >= sizeof(DWORD) && *value == 0;
        if (value)
            ::GlobalUnlock(h);
        return zero;
    };
    if (zeroDword(m_historyFormat) || zeroDword(m_cloudFormat))
        return std::nullopt;

    Copied c;
    if (const HANDLE h = ::GetClipboardData(CF_HDROP)) {
        const auto drop = static_cast<HDROP>(h);
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count && c.files.size() < kMaxFiles; ++i) {
            const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            ::DragQueryFileW(drop, i, path.data(), length + 1);
            path.resize(length);
            if (!path.empty())
                c.files.push_back(std::move(path));
        }
        if (!c.files.empty()) {
            c.kind = Copied::Kind::Files;
            return c;
        }
    }
    if (const HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
            const std::size_t max = std::min<std::size_t>(::GlobalSize(h) / sizeof(wchar_t), kMaxTextChars + 1);
            const std::size_t length = ::wcsnlen(p, max);
            if (length <= kMaxTextChars)
                c.text = localsend::narrow(std::wstring_view(p, length));
            ::GlobalUnlock(h);
        }
        if (!c.text.empty()) {
            c.kind = Copied::Kind::Text;
            return c;
        }
    }
    if (const HANDLE h = ::GetClipboardData(m_pngFormat)) {
        const SIZE_T size = ::GlobalSize(h);
        if (size > 0 && size <= kMaxImageBytes) {
            if (const auto* p = static_cast<const char*>(::GlobalLock(h))) {
                c.png.assign(p, size);
                ::GlobalUnlock(h);
            }
        }
        const auto [width, height] = pngSize(c.png);
        if (width > 0 && height > 0) {
            c.kind = Copied::Kind::Image;
            c.width = width;
            c.height = height;
            return c;
        }
        c.png.clear();
    }
    if (const HANDLE h = ::GetClipboardData(CF_BITMAP)) {
        if (std::optional<Bytes> png = pngOfBitmap(static_cast<HBITMAP>(h))) {
            const auto [width, height] = pngSize(*png);
            c.kind = Copied::Kind::Image;
            c.png = std::move(*png);
            c.width = width;
            c.height = height;
            return c;
        }
    }
    return std::nullopt;
}

std::optional<Copied> ClipboardWin::current()
{
    auto result = std::make_shared<std::optional<Copied>>();
    if (!onWindow([this, result] {
            *result = read(false, nullptr);
            return true;
        }))
        return std::nullopt;
    return *result;
}

bool ClipboardWin::write(const std::vector<std::pair<UINT, HGLOBAL>>& formats, const std::string& originDeviceId)
{
    // On the window's thread: the clipboard's owner is our window.
    bool open = false;
    for (int tries = 0; tries < 20 && !open; ++tries) {
        open = ::OpenClipboard(m_hwnd) != 0;
        if (!open)
            ::Sleep(15);
    }
    if (!open) {
        for (const auto& [format, h] : formats)
            ::GlobalFree(h);
        if (m_log)
            m_log("clipboard: busy, not written");
        return false;
    }
    ::EmptyClipboard();
    bool ok = true;
    for (const auto& [format, h] : formats) {
        if (!::SetClipboardData(format, h)) {
            ::GlobalFree(h);
            ok = false;
        }
    }
    if (const HGLOBAL origin = globalOf(originDeviceId.data(), originDeviceId.size())) {
        if (!::SetClipboardData(m_originFormat, origin))
            ::GlobalFree(origin);
    }
    ::CloseClipboard();
    m_ownSequence = ::GetClipboardSequenceNumber();
    m_lastSequence = m_ownSequence;
    return ok;
}

bool ClipboardWin::putText(const std::string& text, const std::string& originDeviceId)
{
    const std::wstring wide = windowsLineEnds(text);
    return onWindow([this, wide, originDeviceId] {
        const HGLOBAL h = globalOf(wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
        return h && write({{CF_UNICODETEXT, h}}, originDeviceId);
    });
}

bool ClipboardWin::putImage(const Bytes& png, const std::string& originDeviceId)
{
    return onWindow([this, png, originDeviceId] {
        std::vector<std::pair<UINT, HGLOBAL>> formats;
        if (const HGLOBAL h = globalOf(png.data(), png.size()))
            formats.emplace_back(m_pngFormat, h);
        if (const std::optional<Bytes> dib = dibOnWhite(png)) {
            if (const HGLOBAL h = globalOf(dib->data(), dib->size()))
                formats.emplace_back(CF_DIB, h);
        }
        return !formats.empty() && write(formats, originDeviceId);
    });
}

bool ClipboardWin::putFiles(const std::vector<std::wstring>& paths, const std::string& originDeviceId)
{
    std::wstring list;
    for (const std::wstring& path : paths) {
        list += path;
        list += L'\0';
    }
    list += L'\0';
    Bytes drop(sizeof(DROPFILES) + list.size() * sizeof(wchar_t), '\0');
    DROPFILES header {};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    std::memcpy(drop.data(), &header, sizeof header);
    std::memcpy(drop.data() + sizeof header, list.data(), list.size() * sizeof(wchar_t));
    return onWindow([this, drop, originDeviceId] {
        std::vector<std::pair<UINT, HGLOBAL>> formats;
        if (const HGLOBAL h = globalOf(drop.data(), drop.size()))
            formats.emplace_back(CF_HDROP, h);
        const DWORD effect = DROPEFFECT_COPY;
        if (const HGLOBAL h = globalOf(&effect, sizeof effect))
            formats.emplace_back(m_dropEffectFormat, h);
        if (formats.empty() || formats.front().first != CF_HDROP) {
            for (const auto& [format, h] : formats)
                ::GlobalFree(h);
            return false;
        }
        return write(formats, originDeviceId);
    });
}

bool ClipboardWin::putSecret(const std::string& text)
{
    const std::wstring wide = windowsLineEnds(text);
    return onWindow([this, wide] {
        std::vector<std::pair<UINT, HGLOBAL>> formats;
        if (const HGLOBAL h = globalOf(wide.c_str(), (wide.size() + 1) * sizeof(wchar_t)))
            formats.emplace_back(CF_UNICODETEXT, h);
        const DWORD no = 0;
        for (const UINT format : {m_historyFormat, m_cloudFormat}) {
            if (const HGLOBAL h = globalOf(&no, sizeof no))
                formats.emplace_back(format, h);
        }
        if (const HGLOBAL h = globalOf("", 0))
            formats.emplace_back(m_excludeFormat, h);
        return !formats.empty() && formats.front().first == CF_UNICODETEXT && write(formats, "self");
    });
}

} // namespace shunsync
