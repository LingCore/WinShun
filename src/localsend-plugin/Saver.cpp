#include "Saver.h"

#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <vector>

namespace localsend {

namespace {

bool reservedDeviceName(std::wstring_view part)
{
    // The name before the first dot: "CON.txt" and "com1.tar.gz" are devices too.
    std::wstring stem(part.substr(0, part.find(L'.')));
    while (!stem.empty() && stem.back() == L' ')
        stem.pop_back();
    for (wchar_t& c : stem)
        c = static_cast<wchar_t>(::towupper(c));
    static constexpr const wchar_t* kNames[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    for (const wchar_t* n : kNames) {
        if (stem == n)
            return true;
    }
    if (stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0)) {
        const wchar_t d = stem[3];
        return (d >= L'0' && d <= L'9') || d == L'¹' || d == L'²' || d == L'³';
    }
    return false;
}

// At most `limit` characters, the extension kept when it is short.
std::wstring shortened(const std::wstring& part, std::size_t limit)
{
    if (part.size() <= limit)
        return part;
    const std::size_t dot = part.rfind(L'.');
    const std::size_t extension = dot != std::wstring::npos && dot > 0 ? part.size() - dot : 0;
    if (extension > 0 && extension <= 16 && limit > extension) {
        std::wstring stem = part.substr(0, std::min(dot, limit - extension));
        if (!stem.empty() && IS_HIGH_SURROGATE(stem.back()))
            stem.pop_back();
        return stem + part.substr(dot);
    }
    std::wstring cut = part.substr(0, limit);
    if (!cut.empty() && IS_HIGH_SURROGATE(cut.back()))
        cut.pop_back();
    return cut;
}

std::wstring withNumber(const std::wstring& name, int n, bool isFolder)
{
    const std::wstring tag = L" (" + std::to_wstring(n) + L")";
    const std::size_t dot = isFolder ? std::wstring::npos : name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0)
        return name + tag;
    return name.substr(0, dot) + tag + name.substr(dot);
}

bool exists(const std::wstring& path)
{
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring joined(const std::wstring& folder, const std::wstring& name)
{
    return !folder.empty() && folder.back() == L'\\' ? folder + name : folder + L'\\' + name;
}

// The mark could not be written: said once per disk (one without named
// streams, FAT32 or exFAT, never keeps one; every file would say it again).
void markFailed(const std::wstring& path, DWORD error)
{
    wchar_t root[MAX_PATH + 1] {};
    if (!::GetVolumePathNameW(path.c_str(), root, MAX_PATH))
        root[0] = L'\0';
    static std::mutex mutex;
    static std::set<std::wstring, NoCaseLess> told;
    {
        const std::lock_guard lock(mutex);
        if (!told.insert(root).second)
            return;
    }
    DWORD flags = 0;
    const bool streams = root[0] && ::GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0)
        && (flags & FILE_NAMED_STREAMS);
    logWarning(streams ? "received files could not be marked as from the internet (" + std::to_string(error) + ")"
                       : std::string("received files on this disk are not marked as from the internet: it has no named streams"));
}

void removeEmpty(const std::wstring& folder, int level)
{
    const DWORD attributes = ::GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return; // gone, or a link put there since: not ours to remove
    if (level < kMaxDepth) {
        WIN32_FIND_DATAW found {};
        const HANDLE find = ::FindFirstFileExW(joined(folder, L"*").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = found.cFileName;
                if (name != L"." && name != L".." && (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    removeEmpty(joined(folder, name), level + 1);
            } while (::FindNextFileW(find, &found));
            ::FindClose(find);
        }
    }
    ::RemoveDirectoryW(folder.c_str()); // only when it is empty
}

} // namespace

std::optional<std::wstring> safeRelativePath(std::string_view name, std::string* why)
{
    const auto refuse = [&](const char* text) -> std::optional<std::wstring> {
        if (why)
            *why = text;
        return std::nullopt;
    };
    const std::wstring text = wide(name);
    if (!text.empty() && (text[0] == L'/' || text[0] == L'\\'))
        return refuse("a root or a share");
    if (text.size() >= 2 && text[1] == L':')
        return refuse("a drive");
    std::vector<std::wstring> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find_first_of(L"/\\", start);
        if (end == std::wstring::npos)
            end = text.size();
        std::wstring part = text.substr(start, end - start);
        start = end + 1;
        if (part == L"..")
            return refuse("a step up (..)");
        // " .." and ".. " are no steps up once trimmed, but read like ones:
        // refused too ("." and ". ." are dropped).
        std::wstring trimmedPart = part;
        while (!trimmedPart.empty() && (trimmedPart.back() == L'.' || trimmedPart.back() == L' '))
            trimmedPart.pop_back();
        while (!trimmedPart.empty() && trimmedPart.front() == L' ')
            trimmedPart.erase(0, 1);
        if (trimmedPart.empty()) {
            if (part.find(L"..") != std::wstring::npos)
                return refuse("a step up (..)");
            continue; // "", ".", " "
        }
        for (wchar_t& c : trimmedPart) {
            if (c < 0x20 || c == 0x7F || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'|' || c == L'?' || c == L'*'
                || directionMark(c))
                c = L'_';
        }
        if (reservedDeviceName(trimmedPart))
            trimmedPart = L"_" + trimmedPart;
        parts.push_back(shortened(trimmedPart, kMaxSegmentChars));
    }
    if (parts.size() > static_cast<std::size_t>(kMaxDepth))
        return refuse("too many folders");
    if (parts.empty())
        return std::wstring(L"文件");
    std::wstring out;
    for (const std::wstring& part : parts) {
        if (!out.empty())
            out += L'\\';
        out += part;
    }
    return out;
}

bool NoCaseLess::operator()(const std::wstring& a, const std::wstring& b) const
{
    return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}

bool Placement::taken(const std::wstring& path) const
{
    return m_given.count(path) > 0 || exists(path);
}

std::wstring Placement::place(const std::wstring& relative)
{
    std::wstring rest = relative;
    std::wstring base = m_folder;
    if (!base.empty() && base.back() != L'\\')
        base += L'\\';
    if (const std::size_t slash = rest.find(L'\\'); slash != std::wstring::npos) {
        // A folder sent along: its first level renamed once if it was there.
        const std::wstring top = rest.substr(0, slash);
        auto it = m_tops.find(top);
        if (it == m_tops.end()) {
            std::wstring chosen = top;
            for (int n = 1; taken(base + chosen) && n < 10'000; ++n)
                chosen = withNumber(top, n, true);
            m_given.insert(base + chosen);
            it = m_tops.emplace(top, chosen).first;
        }
        rest = it->second + rest.substr(slash);
    }
    std::wstring path = base + rest;
    if (path.size() > kMaxPathChars) {
        // Too deep or too long: the file's own name, straight in the folder.
        std::wstring name = rest.substr(rest.rfind(L'\\') == std::wstring::npos ? 0 : rest.rfind(L'\\') + 1);
        name = shortened(name, kMaxPathChars > base.size() + 16 ? kMaxPathChars - base.size() - 8 : 16);
        path = base + name;
    }
    const std::size_t slash = path.rfind(L'\\');
    const std::wstring dir = path.substr(0, slash + 1);
    const std::wstring name = path.substr(slash + 1);
    std::wstring chosen = path;
    for (int n = 1; taken(chosen) && n < 10'000; ++n)
        chosen = dir + withNumber(name, n, false);
    m_given.insert(chosen);
    return chosen;
}

std::vector<std::wstring> Placement::folders() const
{
    std::vector<std::wstring> out;
    for (const auto& [sent, placed] : m_tops)
        out.push_back(joined(m_folder, placed));
    return out;
}

std::unique_ptr<IncomingFile> IncomingFile::create(std::wstring finalPath, std::uint64_t declaredSize, std::string sha256,
    std::string* error, const std::wstring& partialFolder)
{
    std::unique_ptr<IncomingFile> f(new IncomingFile);
    f->m_final = std::move(finalPath);
    f->m_declared = declaredSize;
    f->m_expectedSha = std::move(sha256);
    if (!f->m_expectedSha.empty())
        f->m_hash = std::make_unique<Sha256>(); // no checksum declared: none worked out either
    const std::size_t slash = f->m_final.rfind(L'\\');
    if (slash != std::wstring::npos && !ensureFolder(f->m_final.substr(0, slash))) {
        *error = "the folder could not be made";
        f->m_done = true;
        return nullptr;
    }
    const std::wstring stem = partialFolder.empty() || slash == std::wstring::npos ? f->m_final
                                                                                    : joined(partialFolder, f->m_final.substr(slash + 1));
    // Not shared for deleting: while it is written, nothing deletes it (the
    // sweep of another Win顺 starting meanwhile included).
    for (int n = 0; n < 100 && !f->m_file.valid(); ++n) {
        f->m_temp = stem + (n == 0 ? L"" : L"." + std::to_wstring(n)) + L".wspart";
        f->m_file = Handle(::CreateFileW(f->m_temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!f->m_file.valid() && ::GetLastError() != ERROR_FILE_EXISTS)
            break;
    }
    if (!f->m_file.valid()) {
        *error = "the file could not be made (" + std::to_string(::GetLastError()) + ")";
        f->m_done = true; // nothing to delete
        return nullptr;
    }
    return f;
}

IncomingFile::~IncomingFile()
{
    if (!m_done)
        abort();
}

bool IncomingFile::write(const char* data, std::size_t size)
{
    if (m_done || !m_file.valid() || size > m_declared - m_written)
        return false;
    if (m_hash)
        m_hash->add(data, size);
    while (size > 0) {
        DWORD wrote = 0;
        const DWORD n = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 24));
        if (!::WriteFile(m_file.get(), data, n, &wrote, nullptr) || wrote == 0)
            return false;
        data += wrote;
        size -= wrote;
        m_written += wrote;
    }
    return true;
}

std::optional<std::wstring> IncomingFile::finish(const std::string& hostUrl, std::string* why, Fault* fault)
{
    const auto fail = [&](std::string text, Fault f) -> std::optional<std::wstring> {
        *why = std::move(text);
        if (fault)
            *fault = f;
        abort();
        return std::nullopt;
    };
    if (m_done)
        return std::nullopt;
    if (m_written != m_declared)
        return fail("it came short", Fault::Sender);
    if (m_hash && !equalsNoCase(m_hash->finish(), m_expectedSha))
        return fail("its checksum does not agree", Fault::Checksum);
    m_file.reset();
    // Shown, then marked, then given its name: the mark is a stream of the
    // file and moves with it, so the file is never there unmarked.
    const bool unhidden = ::SetFileAttributesW(m_temp.c_str(), FILE_ATTRIBUTE_NORMAL) != 0;
    DWORD error = 0;
    if (!writeMarkOfTheWeb(m_temp, hostUrl, &error))
        markFailed(m_temp, error);
    std::wstring target = m_final;
    const std::size_t slash = target.rfind(L'\\');
    bool moved = false;
    for (int n = 1; n < 10'000 && !moved; ++n) {
        moved = ::MoveFileExW(m_temp.c_str(), target.c_str(), 0) != 0;
        if (moved)
            break;
        error = ::GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
            return fail("it could not be renamed (" + std::to_string(error) + ")", Fault::Disk);
        target = m_final.substr(0, slash + 1) + withNumber(m_final.substr(slash + 1), n, false); // taken meanwhile
    }
    if (!moved)
        return fail("no free name for it", Fault::Disk);
    m_done = true;
    if (!unhidden && !::SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL))
        logWarning("a received file stays hidden: its attributes could not be set (" + std::to_string(::GetLastError()) + ")");
    return target;
}

void IncomingFile::abort()
{
    m_file.reset();
    if (!m_done && !m_temp.empty())
        ::DeleteFileW(m_temp.c_str());
    m_done = true;
}

bool writeMarkOfTheWeb(const std::wstring& path, const std::string& hostUrl, DWORD* error)
{
    const std::string text = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=" + hostUrl + "\r\n";
    const Handle stream(::CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD wrote = 0;
    if (stream.valid() && ::WriteFile(stream.get(), text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size())
        return true;
    if (error)
        *error = ::GetLastError();
    return false;
}

std::uint64_t freeBytes(const std::wstring& folder)
{
    ULARGE_INTEGER free {};
    return ::GetDiskFreeSpaceExW(folder.c_str(), &free, nullptr, nullptr) ? free.QuadPart : 0;
}

std::wstring downloadsFolder()
{
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &path)) && path)
        out = path;
    ::CoTaskMemFree(path);
    return out;
}

bool ensureFolder(const std::wstring& folder)
{
    const DWORD attributes = ::GetFileAttributesW(folder.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const int result = ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS;
}

bool folderUsable(const std::wstring& folder)
{
    std::wstring f = folder;
    while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/'))
        f.pop_back();
    while (!f.empty()) {
        const DWORD attributes = ::GetFileAttributesW(f.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            return false; // the drive not ready, the share not there, no access
        // Not there yet: the folder above it.
        const std::size_t slash = f.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return false;
        if (slash == 2 && f[1] == L':') {
            if (f.size() == 3)
                return false; // "Q:\": no such drive
            f.resize(3); // "D:\", never "D:" (that is the drive's current folder)
            continue;
        }
        if (slash < 2)
            return false; // above a share's name: the share was not there
        f.resize(slash);
    }
    return false;
}

std::size_t sweepPartials(const std::wstring& folder)
{
    if (folder.empty())
        return 0;
    WIN32_FIND_DATAW found {};
    const HANDLE find = ::FindFirstFileExW(joined(folder, L"*.wspart").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch,
        nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    static constexpr std::wstring_view kSuffix = L".wspart";
    std::size_t gone = 0;
    do {
        // The pattern meets short (8.3) names too: the long name is looked at.
        const std::wstring_view name = found.cFileName;
        const bool partial = name.size() > kSuffix.size()
            && ::CompareStringOrdinal(name.data() + name.size() - kSuffix.size(), static_cast<int>(kSuffix.size()), kSuffix.data(),
                   static_cast<int>(kSuffix.size()), TRUE)
                == CSTR_EQUAL;
        const DWORD a = found.dwFileAttributes;
        if (partial && (a & FILE_ATTRIBUTE_HIDDEN) && !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            && ::DeleteFileW(joined(folder, found.cFileName).c_str()))
            ++gone;
    } while (::FindNextFileW(find, &found));
    ::FindClose(find);
    return gone;
}

void removeEmptyFolders(const std::wstring& folder)
{
    removeEmpty(folder, 0);
}

} // namespace localsend
