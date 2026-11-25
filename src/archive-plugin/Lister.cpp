#include "Lister.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <string_view>
#include <vector>

#include <windows.h>

namespace wsarchive {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kBlock = 256 * 1024;
constexpr int kRetries = 8; // ARCHIVE_RETRY in a row

std::string utf8(std::wstring_view w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(n, 0)), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool isUtf8(std::string_view bytes)
{
    return bytes.empty()
        || ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0) > 0;
}

// A path Win32 opens whatever its length.
std::wstring openable(const std::wstring& file)
{
    if (file.size() < MAX_PATH - 12 || file.starts_with(L"\\\\?\\"))
        return file;
    if (file.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + file.substr(2);
    return L"\\\\?\\" + file;
}

// The file libarchive reads from: any offset (a zip's list is at its end),
// stopped from outside or when the time is up.
struct Input {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<char> block = std::vector<char>(kBlock);
    Clock::time_point until;
    const std::atomic_bool* canceled = nullptr;
    bool stopped = false; // by the time or from outside: not the archive's fault

    ~Input()
    {
        if (file != INVALID_HANDLE_VALUE)
            ::CloseHandle(file);
    }
    bool stop()
    {
        if ((canceled && canceled->load()) || Clock::now() > until)
            stopped = true;
        return stopped;
    }
};

la_ssize_t readInput(struct archive* a, void* data, const void** buffer)
{
    auto* in = static_cast<Input*>(data);
    if (in->stop()) {
        archive_set_error(a, ECANCELED, "stopped");
        return -1;
    }
    DWORD got = 0;
    if (!::ReadFile(in->file, in->block.data(), static_cast<DWORD>(in->block.size()), &got, nullptr)) {
        archive_set_error(a, EIO, "read failed (%lu)", ::GetLastError());
        return -1;
    }
    *buffer = in->block.data();
    return static_cast<la_ssize_t>(got);
}

la_int64_t seekInput(struct archive* a, void* data, la_int64_t offset, int whence)
{
    auto* in = static_cast<Input*>(data);
    LARGE_INTEGER to {};
    to.QuadPart = offset;
    LARGE_INTEGER at {};
    const DWORD from = whence == SEEK_SET ? FILE_BEGIN : whence == SEEK_CUR ? FILE_CURRENT : FILE_END;
    if (!::SetFilePointerEx(in->file, to, &at, from)) {
        archive_set_error(a, EINVAL, "seek failed (%lu)", ::GetLastError());
        return ARCHIVE_FATAL;
    }
    return at.QuadPart;
}

la_int64_t skipInput(struct archive* a, void* data, la_int64_t request)
{
    auto* in = static_cast<Input*>(data);
    LARGE_INTEGER size {};
    LARGE_INTEGER at {};
    if (request <= 0 || !::GetFileSizeEx(in->file, &size) || !::SetFilePointerEx(in->file, {}, &at, FILE_CURRENT))
        return 0;
    const la_int64_t step = std::min<la_int64_t>(request, std::max<la_int64_t>(size.QuadPart - at.QuadPart, 0));
    return step > 0 && seekInput(a, data, step, SEEK_CUR) >= 0 ? step : 0;
}

struct Closer {
    void operator()(struct archive* a) const noexcept { archive_read_free(a); }
};
using Reader = std::unique_ptr<struct archive, Closer>;

// Only what unpacks in this program: no format that takes any text for an
// archive (mtree, raw), no filter that would start another program.
Reader newReader()
{
    Reader a(archive_read_new());
    if (!a)
        return a;
    archive_read_support_format_zip(a.get());
    archive_read_support_format_7zip(a.get());
    archive_read_support_format_rar(a.get());
    archive_read_support_format_rar5(a.get());
    archive_read_support_format_tar(a.get());
    archive_read_support_format_cab(a.get());
    archive_read_support_format_iso9660(a.get());
    archive_read_support_format_lha(a.get());
    archive_read_support_format_cpio(a.get());
    archive_read_support_filter_gzip(a.get());
    archive_read_support_filter_xz(a.get());
    archive_read_support_filter_lzma(a.get());
    archive_read_support_filter_lzip(a.get());
    archive_read_support_filter_compress(a.get());
    return a;
}

enum class Opened { Yes, Away, Unreadable, NotArchive };

Opened open(Reader& a, Input& in, const std::wstring& file, std::string& problem)
{
    const std::wstring path = openable(file);
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        problem = "no such file (" + std::to_string(::GetLastError()) + ")";
        return Opened::Unreadable;
    }
    // A cloud file kept online only: opening it would download it.
    if (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) {
        problem = "online only";
        return Opened::Away;
    }
    in.file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in.file == INVALID_HANDLE_VALUE) {
        problem = "cannot open (" + std::to_string(::GetLastError()) + ")";
        return Opened::Unreadable;
    }
    a = newReader();
    if (!a) {
        problem = "no memory";
        return Opened::Unreadable;
    }
    archive_read_set_callback_data(a.get(), &in);
    archive_read_set_read_callback(a.get(), readInput);
    archive_read_set_seek_callback(a.get(), seekInput);
    archive_read_set_skip_callback(a.get(), skipInput);
    if (archive_read_open1(a.get()) != ARCHIVE_OK) {
        const char* why = archive_error_string(a.get());
        problem = why ? why : "not an archive";
        return Opened::NotArchive;
    }
    return Opened::Yes;
}

// Whether what libarchive last said is about a password. `opened`: a format
// took the file (before that there is none to ask about its entries).
bool aboutPassword(struct archive* a, bool opened = true)
{
    if (opened && archive_read_has_encrypted_entries(a) > 0)
        return true;
    const char* why = archive_error_string(a);
    if (!why)
        return false;
    const std::string_view text(why);
    return text.find("ncrypt") != std::string_view::npos || text.find("assphrase") != std::string_view::npos
        || text.find("assword") != std::string_view::npos;
}

// The entry's path. Its bytes as libarchive hands them over (this program
// sets no locale): UTF-8 where the archive says what its names are (a zip's
// UTF-8 flag, 7z, RAR5, pax), and the archive's own bytes where it does not
// (an old zip). Those are UTF-8 too when a Mac or Linux tool wrote them, and
// otherwise in the code page of the Windows they were made on (GBK on a
// Chinese one): not what libarchive's own Unicode form of them assumes.
std::string pathOf(struct archive_entry* entry, unsigned codePage)
{
    if (const char* raw = archive_entry_pathname(entry); raw && *raw)
        return textOf(raw, codePage);
    if (const wchar_t* wide = archive_entry_pathname_w(entry); wide && *wide)
        return utf8(wide);
    return {};
}

// The next header: ARCHIVE_OK, ARCHIVE_EOF, or what stopped it.
int nextHeader(struct archive* a, struct archive_entry** entry)
{
    for (int tries = 0;; ++tries) {
        const int r = archive_read_next_header(a, entry);
        if (r == ARCHIVE_WARN)
            return ARCHIVE_OK;
        if (r != ARCHIVE_RETRY || tries >= kRetries)
            return r;
    }
}

} // namespace

const char* outcomeName(Outcome outcome) noexcept
{
    switch (outcome) {
    case Outcome::Listed:
        return "listed";
    case Outcome::Partial:
        return "partial";
    case Outcome::Locked:
        return "locked";
    case Outcome::Away:
        return "away";
    case Outcome::Unreadable:
        return "unreadable";
    case Outcome::NotArchive:
        return "not an archive";
    }
    return "?";
}

std::string textOf(std::string_view bytes, unsigned codePage)
{
    if (isUtf8(bytes))
        return std::string(bytes);
    UINT page = codePage ? codePage : ::GetACP();
    // "Use Unicode UTF-8 for worldwide language support" is on: the bytes
    // are not UTF-8, so they are of the code page the tools here wrote before.
    if (page == CP_UTF8)
        page = ::GetOEMCP();
    if (page == CP_UTF8)
        page = PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE ? 936 : 437;
    const int n = ::MultiByteToWideChar(page, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (n <= 0)
        return std::string(bytes); // shown with U+FFFD where it does not fit (winshun::Json::quote)
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(page, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), n);
    return utf8(wide);
}

std::string cleanPath(std::string path, bool& folder)
{
    std::ranges::replace(path, '/', '\\');
    std::size_t start = 0;
    for (;;) {
        if (path.compare(start, 2, ".\\") == 0)
            start += 2;
        else if (start < path.size() && path[start] == '\\')
            ++start;
        else
            break;
    }
    if (path.size() - start == 1 && path[start] == '.')
        ++start;
    path.erase(0, start);
    while (!path.empty() && path.back() == '\\') {
        path.pop_back();
        folder = true;
    }
    // One member a line where they are kept: a name with a line end in it (tar allows any byte).
    std::ranges::replace_if(path, [](char c) { return c == '\n' || c == '\r'; }, ' ');
    return path;
}

Listing list(const std::wstring& file, const Limits& limits, const std::function<void(Member&&)>& member)
{
    Listing out;
    Input in;
    in.until = Clock::now() + limits.time;
    in.canceled = limits.canceled;
    Reader a;
    switch (open(a, in, file, out.problem)) {
    case Opened::Away:
        out.outcome = Outcome::Away;
        return out;
    case Opened::Unreadable:
        out.outcome = Outcome::Unreadable;
        return out;
    case Opened::NotArchive:
        // The file's start is the names' when they are locked (7z, RAR with -hp).
        out.outcome = in.stopped ? Outcome::Partial : a && aboutPassword(a.get(), false) ? Outcome::Locked : Outcome::NotArchive;
        return out;
    case Opened::Yes:
        break;
    }

    std::size_t count = 0;
    std::size_t bytes = 0;
    for (;;) {
        struct archive_entry* entry = nullptr;
        const int r = nextHeader(a.get(), &entry);
        if (r == ARCHIVE_EOF) {
            out.outcome = Outcome::Listed;
            break;
        }
        if (r != ARCHIVE_OK) {
            const char* why = archive_error_string(a.get());
            out.problem = in.stopped ? "stopped" : why ? why : "damaged";
            out.outcome = in.stopped || count > 0 ? Outcome::Partial : aboutPassword(a.get()) ? Outcome::Locked : Outcome::NotArchive;
            break;
        }
        if (out.format.empty()) {
            if (const char* name = archive_format_name(a.get()))
                out.format = name;
        }
        Member m;
        m.folder = archive_entry_filetype(entry) == AE_IFDIR;
        m.path = cleanPath(pathOf(entry, limits.codePage), m.folder);
        if (m.path.empty())
            continue;
        if (!m.folder && archive_entry_size_is_set(entry))
            m.size = static_cast<std::uint64_t>(std::max<la_int64_t>(archive_entry_size(entry), 0));
        if (archive_entry_mtime_is_set(entry))
            m.modified = std::max<std::int64_t>(archive_entry_mtime(entry), 0);
        m.locked = archive_entry_is_data_encrypted(entry) > 0;
        bytes += m.path.size() + 1;
        if (count >= limits.maxMembers || bytes > limits.maxNameBytes) {
            out.problem = "too many members";
            out.outcome = Outcome::Partial;
            break;
        }
        ++count;
        member(std::move(m));
    }
    return out;
}

Taken take(const std::wstring& file, const std::string& path, std::size_t index, const std::wstring& target, const TakeLimits& limits,
    std::string* problem)
{
    std::string why;
    const auto fail = [&](Taken taken, std::string text) {
        if (problem)
            *problem = std::move(text);
        return taken;
    };
    // Two passes at most: the member at `index` when it has this path (the
    // usual case), else the first of this path.
    for (const bool byIndex : {true, false}) {
        Input in;
        in.until = Clock::now() + limits.time;
        in.canceled = limits.canceled;
        Reader a;
        if (open(a, in, file, why) != Opened::Yes)
            return fail(Taken::NotFound, why);
        std::size_t at = 0;
        for (;;) {
            struct archive_entry* entry = nullptr;
            const int r = nextHeader(a.get(), &entry);
            if (r == ARCHIVE_EOF)
                break;
            if (r != ARCHIVE_OK) {
                if (in.stopped)
                    return fail(Taken::TimedOut, "stopped");
                const char* text = archive_error_string(a.get());
                return fail(aboutPassword(a.get()) ? Taken::Locked : Taken::Failed, text ? text : "damaged");
            }
            bool folder = archive_entry_filetype(entry) == AE_IFDIR;
            const std::string name = cleanPath(pathOf(entry, limits.codePage), folder);
            if (name.empty())
                continue;
            const std::size_t mine = at++;
            if (byIndex && mine > index)
                break; // not where it was: by its path alone
            if (name != path || folder || (byIndex && mine != index))
                continue;
            if (archive_entry_is_data_encrypted(entry) > 0)
                return fail(Taken::Locked, "encrypted");
            if (archive_entry_size_is_set(entry) && static_cast<std::uint64_t>(archive_entry_size(entry)) > limits.maxBytes)
                return fail(Taken::TooLarge, "larger than the limit");

            const std::wstring partial = target + L".part";
            const HANDLE to = ::CreateFileW(openable(partial).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (to == INVALID_HANDLE_VALUE)
                return fail(Taken::Failed, "cannot write (" + std::to_string(::GetLastError()) + ")");
            Taken taken = Taken::Done;
            std::uint64_t written = 0;
            std::vector<char> block(kBlock);
            for (;;) {
                const la_ssize_t got = archive_read_data(a.get(), block.data(), block.size());
                if (got == 0)
                    break;
                if (got < 0) {
                    const char* text = archive_error_string(a.get());
                    why = text ? text : "damaged";
                    taken = in.stopped ? Taken::TimedOut : aboutPassword(a.get()) ? Taken::Locked : Taken::Failed;
                    break;
                }
                written += static_cast<std::uint64_t>(got);
                DWORD put = 0;
                if (written > limits.maxBytes) {
                    why = "larger than the limit";
                    taken = Taken::TooLarge;
                    break;
                }
                if (!::WriteFile(to, block.data(), static_cast<DWORD>(got), &put, nullptr) || put != static_cast<DWORD>(got)) {
                    why = "cannot write (" + std::to_string(::GetLastError()) + ")";
                    taken = Taken::Failed;
                    break;
                }
            }
            ::CloseHandle(to);
            if (taken == Taken::Done
                && !::MoveFileExW(openable(partial).c_str(), openable(target).c_str(), MOVEFILE_REPLACE_EXISTING)) {
                why = "cannot write (" + std::to_string(::GetLastError()) + ")";
                taken = Taken::Failed;
            }
            if (taken != Taken::Done)
                ::DeleteFileW(openable(partial).c_str());
            return fail(taken, why);
        }
    }
    return fail(Taken::NotFound, "no such member");
}

} // namespace wsarchive
