#include "DragOut.h"

#include <QDir>
#include <QImage>

#include <shellapi.h>
#include <shlobj.h>

#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ws::dragout {

namespace {

struct IdLists {
    std::vector<PIDLIST_ABSOLUTE> items;
    ~IdLists()
    {
        for (PIDLIST_ABSOLUTE item : items)
            ::CoTaskMemFree(item);
    }
};

// `bytes` as `format` in the data object, which owns the copy.
void put(IDataObject* data, CLIPFORMAT format, const void* bytes, std::size_t size)
{
    HGLOBAL global = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    if (!global)
        return;
    std::memcpy(::GlobalLock(global), bytes, size);
    ::GlobalUnlock(global);
    FORMATETC f {format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium {};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = global;
    if (FAILED(data->SetData(&f, &medium, TRUE))) // TRUE: it owns the memory now
        ::GlobalFree(global);
}

// CF_HDROP, what chats and mail programs read: the shell's object makes it
// only for items of one folder, not for these (each from the desktop).
void putFileNames(IDataObject* data, const QStringList& paths)
{
    std::vector<wchar_t> names;
    for (const QString& path : paths) {
        const auto* text = reinterpret_cast<const wchar_t*>(path.utf16());
        names.insert(names.end(), text, text + path.size());
        names.push_back(L'\0');
    }
    names.push_back(L'\0'); // the list ends with an empty name
    std::vector<char> bytes(sizeof(DROPFILES) + names.size() * sizeof(wchar_t));
    auto* header = reinterpret_cast<DROPFILES*>(bytes.data());
    header->pFiles = sizeof(DROPFILES);
    header->fWide = TRUE;
    std::memcpy(bytes.data() + sizeof(DROPFILES), names.data(), names.size() * sizeof(wchar_t));
    put(data, CF_HDROP, bytes.data(), bytes.size());
}

// Pasted or dropped, the files are copied, never moved.
void preferCopy(IDataObject* data)
{
    static const auto preferred = static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    const DWORD copy = DROPEFFECT_COPY;
    put(data, preferred, &copy, sizeof copy);
}

// The picture that follows the pointer: a window of its own, not the
// shell's drag image, which Windows shows see-through (the words under it
// showing through the icon, its shadow all but gone). Layered and
// transparent to the mouse, so the drop goes to the window under it; never
// active. In device pixels, as the picture was drawn.
class Follower {
public:
    Follower(const QImage& picture, QPoint hotspot)
        : m_hotspot(hotspot)
    {
        static const wchar_t* const name = L"WinShun.DragPicture";
        static const bool registered = [] {
            WNDCLASSEXW c {sizeof(WNDCLASSEXW)};
            c.lpfnWndProc = ::DefWindowProcW;
            c.hInstance = ::GetModuleHandleW(nullptr);
            c.lpszClassName = name;
            return ::RegisterClassExW(&c) != 0;
        }();
        if (!registered)
            return;
        const QImage image = picture.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        m_size = {image.width(), image.height()};
        m_window = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                     name, L"", WS_POPUP, 0, 0, m_size.cx, m_size.cy, nullptr, nullptr,
                                     ::GetModuleHandleW(nullptr), nullptr);
        if (!m_window)
            return;
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image.width();
        info.bmiHeader.biHeight = -image.height(); // top-down, as QImage
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        const HDC screen = ::GetDC(nullptr);
        const HDC memory = ::CreateCompatibleDC(screen);
        void* bits = nullptr;
        const HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap && bits) {
            const std::size_t row = std::size_t(image.width()) * 4;
            for (int y = 0; y < image.height(); ++y)
                std::memcpy(static_cast<uchar*>(bits) + std::size_t(y) * row, image.constScanLine(y), row);
            const HGDIOBJ old = ::SelectObject(memory, bitmap);
            POINT at = place();
            POINT origin {};
            // Nearly solid: a light hint that it is on its way, never a ghost.
            BLENDFUNCTION blend {AC_SRC_OVER, 0, 235, AC_SRC_ALPHA};
            ::UpdateLayeredWindow(m_window, screen, &at, &m_size, memory, &origin, 0, &blend, ULW_ALPHA);
            ::SelectObject(memory, old);
            ::ShowWindow(m_window, SW_SHOWNOACTIVATE);
        }
        if (bitmap)
            ::DeleteObject(bitmap);
        ::DeleteDC(memory);
        ::ReleaseDC(nullptr, screen);
    }
    ~Follower()
    {
        if (m_window)
            ::DestroyWindow(m_window);
    }
    Follower(const Follower&) = delete;
    Follower& operator=(const Follower&) = delete;

    void follow()
    {
        if (!m_window)
            return;
        const POINT at = place();
        ::SetWindowPos(m_window, HWND_TOPMOST, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

private:
    POINT place() const
    {
        POINT cursor {};
        ::GetCursorPos(&cursor);
        return {cursor.x - m_hotspot.x(), cursor.y - m_hotspot.y()};
    }

    HWND m_window = nullptr;
    SIZE m_size {};
    QPoint m_hotspot;
};

// The left button's drag, as OLE's own: Esc cancels, letting go drops; each
// move takes the picture along, with the system's cursors (copy, link, no).
class Source final : public IDropSource {
public:
    explicit Source(Follower& follower)
        : m_follower(follower)
    {
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (!out)
            return E_POINTER;
        if (id == IID_IUnknown || id == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --m_refs; } // on the caller's stack
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override
    {
        if (escape)
            return DRAGDROP_S_CANCEL;
        return keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
    {
        m_follower.follow();
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    Follower& m_follower;
    ULONG m_refs = 1;
};

} // namespace

ComPtr<IDataObject> dataObject(const QStringList& paths)
{
    IdLists lists;
    QStringList found;
    for (const QString& path : paths) {
        // Qt paths use '/', which the shell rejects with E_INVALIDARG.
        const QString nativePath = QDir::toNativeSeparators(path);
        PIDLIST_ABSOLUTE item = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(reinterpret_cast<const wchar_t*>(nativePath.utf16()), nullptr, &item, 0, nullptr))) {
            lists.items.push_back(item);
            found.append(nativePath);
        }
    }
    if (lists.items.empty())
        return {};
    // Relative to the desktop, each item's whole ID list: items of several
    // folders in one object, as Explorer's search results hand them out.
    std::vector<PCUITEMID_CHILD> children;
    for (PIDLIST_ABSOLUTE item : lists.items)
        children.push_back(reinterpret_cast<PCUITEMID_CHILD>(item));
    PIDLIST_ABSOLUTE desktop = nullptr; // a null folder would leave the items out
    if (FAILED(::SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &desktop)))
        return {};
    ComPtr<IDataObject> data;
    const HRESULT made = ::SHCreateDataObject(
        desktop, static_cast<UINT>(children.size()), children.data(), nullptr, IID_PPV_ARGS(&data));
    ::CoTaskMemFree(desktop);
    if (FAILED(made))
        return {};
    putFileNames(data.Get(), found);
    preferCopy(data.Get());
    return data;
}

ComPtr<IDataObject> fileNamesObject(const QStringList& paths)
{
    if (paths.isEmpty())
        return {};
    // The shell's object with no items holds whatever is put in it.
    ComPtr<IDataObject> data;
    if (FAILED(::SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&data))))
        return {};
    QStringList native;
    for (const QString& path : paths)
        native.append(QDir::toNativeSeparators(path));
    putFileNames(data.Get(), native);
    preferCopy(data.Get());
    return data;
}

Outcome drag(const QStringList& paths, HWND source, bool allowLink, const QImage& picture, QPoint hotspot)
{
    const ComPtr<IDataObject> data = dataObject(paths);
    if (!data)
        return Outcome::Failed;
    DWORD effect = DROPEFFECT_NONE;
    const DWORD allowed = DROPEFFECT_COPY | (allowLink ? DROPEFFECT_LINK : 0);
    HRESULT hr = E_FAIL;
    if (picture.isNull()) {
        hr = ::SHDoDragDrop(source, data.Get(), nullptr, allowed, &effect); // with the shell's stand-in picture
    } else {
        // OLE's own loop: the shell's would add its see-through stand-in.
        Follower follower(picture, hotspot);
        Source dropSource(follower);
        hr = ::DoDragDrop(data.Get(), &dropSource, allowed, &effect);
    }
    if (hr == DRAGDROP_S_DROP)
        return effect != DROPEFFECT_NONE ? Outcome::Dropped : Outcome::Cancelled;
    return hr == DRAGDROP_S_CANCEL ? Outcome::Cancelled : Outcome::Failed;
}

} // namespace ws::dragout
