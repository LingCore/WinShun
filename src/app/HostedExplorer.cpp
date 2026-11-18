#include "HostedExplorer.h"

#include "FileIndex.h"
#include "ShownFolder.h"
#include "TextUtil.h"
#include "Wtf8.h"

#include <QDir>
#include <QRegularExpression>

#include <ole2.h> // before uiautomation.h: WIN32_LEAN_AND_MEAN keeps it out of windows.h

#include <knownfolders.h>
#include <shlobj.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <iterator>

using Microsoft::WRL::ComPtr;
using namespace Qt::StringLiterals;

namespace ws::hostedexplorer {

namespace {

constexpr int kMaxNames = 16; // listed names looked at
constexpr std::size_t kMaxCandidates = 4000; // folders of that name
constexpr int kMaxChildren = 20000; // names of one candidate

// The folder view shown in the browser control: its items view (DirectUIHWND
// under the largest SHELLDLL_DefView shown).
HWND itemsView(HWND control)
{
    struct Largest {
        HWND view = nullptr;
        long long area = 0;
    } largest;
    ::EnumChildWindows(
        control,
        [](HWND window, LPARAM param) -> BOOL {
            wchar_t name[32] {};
            ::GetClassNameW(window, name, static_cast<int>(std::size(name)));
            RECT r {};
            if (std::wcscmp(name, L"SHELLDLL_DefView") != 0 || !::IsWindowVisible(window) || !::GetWindowRect(window, &r))
                return TRUE;
            auto& largest = *reinterpret_cast<Largest*>(param);
            const long long area = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
            if (area > largest.area)
                largest = {window, area};
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&largest));
    return largest.view ? ::FindWindowExW(largest.view, nullptr, L"DirectUIHWND", nullptr) : nullptr;
}

QString bstrText(BSTR text)
{
    QString s = text ? QString::fromWCharArray(text, static_cast<qsizetype>(::SysStringLen(text))) : QString();
    ::SysFreeString(text);
    return s;
}

// The names of the items listed (UI Automation's list items), in their
// groups too (File Explorer's 文件夹 / 文件 headings), the first kMaxNames.
QStringList listedNames(HWND view)
{
    QStringList names;
    ComPtr<IUIAutomation> automation;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))
        && FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
        return names;
    ComPtr<IUIAutomation2> timed;
    if (SUCCEEDED(automation.As(&timed))) {
        timed->put_ConnectionTimeout(1000);
        timed->put_TransactionTimeout(1000);
    }
    ComPtr<IUIAutomationCacheRequest> cache;
    ComPtr<IUIAutomationCondition> all;
    ComPtr<IUIAutomationElement> list;
    if (FAILED(automation->CreateCacheRequest(&cache)) || FAILED(automation->CreateTrueCondition(&all))
        || FAILED(automation->ElementFromHandle(view, &list)) || !list)
        return names;
    cache->AddProperty(UIA_NamePropertyId);
    cache->AddProperty(UIA_ControlTypePropertyId);
    const auto children = [&](IUIAutomationElement* parent) {
        ComPtr<IUIAutomationElementArray> found;
        if (FAILED(parent->FindAllBuildCache(TreeScope_Children, all.Get(), cache.Get(), &found)))
            found.Reset();
        return found;
    };
    const auto take = [&](IUIAutomationElementArray* items, int depth, auto& self) -> void {
        int count = 0;
        if (!items || FAILED(items->get_Length(&count)))
            return;
        for (int i = 0; i < count && names.size() < kMaxNames; ++i) {
            ComPtr<IUIAutomationElement> item;
            CONTROLTYPEID type = 0;
            if (FAILED(items->GetElement(i, &item)) || !item || FAILED(item->get_CachedControlType(&type)))
                continue;
            if (type == UIA_ListItemControlTypeId || type == UIA_DataItemControlTypeId) {
                BSTR name = nullptr;
                item->get_CachedName(&name);
                if (const QString text = bstrText(name); !text.isEmpty())
                    names.append(text);
            } else if (type == UIA_GroupControlTypeId && depth == 0) {
                self(children(item.Get()).Get(), depth + 1, self);
            }
        }
    };
    take(children(list.Get()).Get(), 0, take);
    return names;
}

QString displayName(PCIDLIST_ABSOLUTE pidl)
{
    PWSTR name = nullptr;
    if (!pidl || FAILED(::SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &name)))
        return {};
    const QString text = QString::fromWCharArray(name);
    ::CoTaskMemFree(name);
    return text;
}

QString knownDisplayName(REFKNOWNFOLDERID id)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(::SHGetKnownFolderIDList(id, KF_FLAG_DEFAULT, nullptr, &pidl)))
        return {};
    const QString name = displayName(pidl);
    ::ILFree(pidl);
    return name;
}

QString knownPath(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)))
        return {};
    const QString text = QString::fromWCharArray(path);
    ::CoTaskMemFree(path);
    return text;
}

bool isFolder(const QString& path)
{
    const DWORD attributes = ::GetFileAttributesW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// What is in a folder, as on disk: for Windows' own folders, which the
// index may not have under their display names.
QStringList childrenOnDisk(const QString& folder)
{
    QStringList names;
    std::wstring pattern = QDir::toNativeSeparators(folder).toStdWString();
    if (pattern.empty())
        return names;
    if (pattern.back() != L'\\')
        pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW data;
    const HANDLE find = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return names;
    do {
        const std::wstring_view name = data.cFileName;
        if (name != L"." && name != L"..")
            names.append(QString::fromWCharArray(name.data(), static_cast<qsizetype>(name.size())));
    } while (names.size() < kMaxChildren && ::FindNextFileW(find, &data));
    ::FindClose(find);
    return names;
}

// The folders the index has by the name `title`, with what is in each.
std::vector<shownfolder::Candidate> indexed(const FileIndex& index, const QString& title)
{
    std::vector<shownfolder::Candidate> candidates;
    const std::string name = wtf8::fromUtf16(wtf8::view(title));
    const auto lock = index.readLock();
    std::vector<EntryId> found;
    for (std::size_t c = 0; c < index.chunkCount() && found.size() < kMaxCandidates; ++c) {
        const std::span<const EntryScan> entries = index.scanChunk(c);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const EntryScan& e = entries[i];
            if (!e.isDir() || e.isDeleted())
                continue;
            const std::string_view entryName = index.name(e);
            if (entryName.size() == name.size() && text::equalsIgnoreAsciiCase(entryName, name))
                found.push_back(static_cast<EntryId>((c << FileIndex::kChunkBits) + i));
        }
    }
    candidates.reserve(found.size());
    for (const EntryId id : found) {
        shownfolder::Candidate candidate {index.path(id), {}};
        int count = 0;
        for (EntryId child = index.entry(id).firstChild(); child != kNoEntry && count < kMaxChildren;
            child = index.entry(child).nextSibling, ++count) {
            if (!index.scan(child).isDeleted())
                candidate.children.append(wtf8::toQString(index.name(child)));
        }
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

} // namespace

Shown shown(HWND frame, HWND control)
{
    Shown s;
    wchar_t title[512] {};
    ::GetWindowTextW(frame, title, static_cast<int>(std::size(title)));
    s.title = QString::fromWCharArray(title).trimmed();
    if (const HWND view = control ? itemsView(control) : nullptr)
        s.names = listedNames(view);
    return s;
}

std::optional<QString> place(const Shown& shown, const FileIndex* index)
{
    const QString& title = shown.title;
    if (title.isEmpty())
        return std::nullopt;
    // A drive: "Windows (C:)", "本地磁盘 (D:)".
    static const QRegularExpression drive(uR"(\(([A-Za-z]):\)$)"_s);
    if (const QRegularExpressionMatch m = drive.match(title); m.hasMatch()) {
        const QString root = m.captured(1).toUpper() + u":\\"_s;
        return isFolder(root) ? std::optional(root) : std::nullopt;
    }
    if (title == knownDisplayName(FOLDERID_ComputerFolder))
        return QString(); // 此电脑: the whole PC
    std::vector<shownfolder::Candidate> candidates;
    // Windows' own folders go by names of their own (桌面, 文档…).
    static constexpr std::array known {&FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads, &FOLDERID_Pictures,
        &FOLDERID_Music, &FOLDERID_Videos, &FOLDERID_Profile, &FOLDERID_Objects3D, &FOLDERID_PublicDesktop,
        &FOLDERID_PublicDocuments, &FOLDERID_PublicDownloads, &FOLDERID_PublicPictures, &FOLDERID_PublicMusic,
        &FOLDERID_PublicVideos};
    for (const KNOWNFOLDERID* id : known) {
        if (knownDisplayName(*id) != title)
            continue;
        if (const QString path = knownPath(*id); !path.isEmpty())
            candidates.push_back({path, childrenOnDisk(path)});
    }
    if (index) {
        for (shownfolder::Candidate& candidate : indexed(*index, title)) {
            if (std::ranges::none_of(candidates, [&](const shownfolder::Candidate& c) { return c.path.compare(candidate.path, Qt::CaseInsensitive) == 0; }))
                candidates.push_back(std::move(candidate));
        }
    }
    const std::optional<QString> picked = shownfolder::pick(candidates, shown.names);
    if (!picked || !isFolder(*picked))
        return std::nullopt;
    return QDir::toNativeSeparators(*picked);
}

} // namespace ws::hostedexplorer
