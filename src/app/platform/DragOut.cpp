#include "DragOut.h"

#include <QDir>

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

Outcome drag(const QStringList& paths, HWND source, bool allowLink)
{
    const ComPtr<IDataObject> data = dataObject(paths);
    if (!data)
        return Outcome::Failed;
    DWORD effect = DROPEFFECT_NONE;
    const DWORD allowed = DROPEFFECT_COPY | (allowLink ? DROPEFFECT_LINK : 0);
    const HRESULT hr = ::SHDoDragDrop(source, data.Get(), nullptr, allowed, &effect);
    if (hr == DRAGDROP_S_DROP)
        return effect != DROPEFFECT_NONE ? Outcome::Dropped : Outcome::Cancelled;
    return hr == DRAGDROP_S_CANCEL ? Outcome::Cancelled : Outcome::Failed;
}

} // namespace ws::dragout
