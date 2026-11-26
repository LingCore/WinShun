#include "Download.h"

#include "Catalog.h"

#include "winshun.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace lm {

namespace {

struct InternetHandle {
    HINTERNET handle = nullptr;
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET h) : handle(h) {}
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    ~InternetHandle()
    {
        if (handle)
            ::WinHttpCloseHandle(handle);
    }
    explicit operator bool() const { return handle != nullptr; }
};

struct FileHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit FileHandle(HANDLE h) : handle(h) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() { close(); }
    void close()
    {
        if (handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(std::exchange(handle, INVALID_HANDLE_VALUE));
    }
    bool valid() const { return handle != INVALID_HANDLE_VALUE; }
};

HINTERNET session()
{
    // The system's proxy settings (WPAD and the user's), as a browser would.
    // Synchronous: each connection has a thread of its own.
    static const HINTERNET s = ::WinHttpOpen(L"WinShun-localmodel/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    return s;
}

enum class Step { Done, Again, Failed, Canceled, NoRanges };

std::string networkText(DWORD error, bool chinese)
{
    switch (error) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return chinese ? "连不上 ModelScope：检查网络" : "Cannot reach ModelScope: check the network";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return chinese ? "HTTPS 证书有问题（代理或安全软件在拦截？）" : "The HTTPS certificate is not valid (a proxy or security software in between?)";
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return chinese ? "磁盘满了" : "The disk is full";
    default:
        return chinese ? "下载断了（网络错误 " + std::to_string(error) + "）：再点“下载”会接着下"
                       : "The download broke off (network error " + std::to_string(error) + "): Download again goes on from there";
    }
}

// "bytes 100-199/200" → 100.
std::uint64_t rangeStart(const std::wstring& contentRange)
{
    const std::size_t space = contentRange.find(L' ');
    if (space == std::wstring::npos)
        return ~0ull;
    try {
        return std::stoull(contentRange.substr(space + 1));
    } catch (...) {
        return ~0ull;
    }
}

std::wstring header(HINTERNET request, DWORD which)
{
    DWORD bytes = 0;
    ::WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!::WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
        return {};
    value.resize(bytes / sizeof(wchar_t));
    return value;
}

bool writeAt(HANDLE file, std::uint64_t offset, const char* data, DWORD size)
{
    OVERLAPPED at {};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD written = 0;
    return ::WriteFile(file, data, size, &written, &at) && written == size;
}

// One request for the bytes [from, to] of the file (a Range; `whole`: the
// file without one, for a server that has no ranges), written where they
// belong. `moved` counts what came, for the progress.
Step fetch(const std::wstring& url, HANDLE file, std::uint64_t from, std::uint64_t to, bool whole, const std::atomic_bool& stop,
    std::atomic<std::uint64_t>& moved, std::string& error, bool chinese)
{
    URL_COMPONENTS parts {};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);
    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        error = chinese ? "下载地址不对" : "The download address is not valid";
        return Step::Failed;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    const auto broke = [&](DWORD code) {
        error = networkText(code, chinese);
        return code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL || code == ERROR_WINHTTP_SECURE_FAILURE ? Step::Failed : Step::Again;
    };
    if (!session())
        return broke(::GetLastError());
    InternetHandle connection(::WinHttpConnect(session(), host.c_str(), parts.nPort, 0));
    if (!connection)
        return broke(::GetLastError());
    InternetHandle request(::WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request)
        return broke(::GetLastError());
    ::WinHttpSetTimeouts(request.handle, 15000, 15000, 30000, 30000);
    const std::wstring range = whole ? std::wstring() : L"Range: bytes=" + std::to_wstring(from) + L"-" + std::to_wstring(to) + L"\r\n";
    if (!::WinHttpSendRequest(request.handle, range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(),
            range.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !::WinHttpReceiveResponse(request.handle, nullptr))
        return broke(::GetLastError());
    DWORD status = 0;
    DWORD statusSize = sizeof status;
    if (!::WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
        return broke(::GetLastError());
    if (!whole && status == 200)
        return Step::NoRanges; // the whole file came instead of the part asked for
    if (whole ? status != 200 : status != 206) {
        error = chinese ? "下载失败：服务器返回 " + std::to_string(status) + (status == 404 ? "（地址失效了，请更新插件）" : "")
                        : "Download failed: the server answered " + std::to_string(status)
                + (status == 404 ? " (the address is gone: update the plugin)" : "");
        return status >= 500 || status == 429 || status == 408 ? Step::Again : Step::Failed;
    }
    if (!whole && rangeStart(header(request.handle, WINHTTP_QUERY_CONTENT_RANGE)) != from)
        return Step::NoRanges; // not the part asked for: not to be trusted with ranges

    std::vector<char> buffer(256 << 10);
    std::uint64_t at = from;
    for (;;) {
        if (stop.load())
            return Step::Canceled;
        // What has come (a read of more would wait for all of it).
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.handle, &available))
            return broke(::GetLastError());
        if (available == 0)
            break;
        DWORD read = 0;
        if (!::WinHttpReadData(request.handle, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read))
            return broke(::GetLastError());
        if (read == 0)
            break;
        if (at + read > to + 1) {
            error = chinese ? "下载的文件比应有的大，已停下" : "The download is bigger than it should be: stopped";
            return Step::Failed;
        }
        if (!writeAt(file, at, buffer.data(), read))
            return broke(::GetLastError());
        at += read;
        moved += read;
    }
    if (at == to + 1)
        return Step::Done;
    error = chinese ? "下载断了：再点“下载”会接着下" : "The download broke off: Download again goes on from there";
    return Step::Again;
}

std::string hex(const unsigned char* bytes, std::size_t size)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += kDigits[bytes[i] >> 4];
        out += kDigits[bytes[i] & 15];
    }
    return out;
}

// Which pieces of the .part are in: "<target>.part.map", one line with the
// size and the piece size, one with a 0 or 1 per piece. Rewritten whole
// (into .tmp, then renamed) after each piece.
struct PartMap {
    std::uint64_t size = 0;
    std::uint64_t piece = 0;
    std::string done; // '0' / '1' per piece
};

std::optional<PartMap> readMap(const std::filesystem::path& file)
{
    std::ifstream in(file);
    std::string magic;
    PartMap map;
    if (!(in >> magic >> map.size >> map.piece >> map.done) || magic != "winshun-part-1" || map.piece == 0)
        return std::nullopt;
    if (map.done.size() != (map.size + map.piece - 1) / map.piece || map.done.find_first_not_of("01") != std::string::npos)
        return std::nullopt;
    return map;
}

bool writeMap(const std::filesystem::path& file, const PartMap& map)
{
    std::filesystem::path temp = file;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << "winshun-part-1 " << map.size << ' ' << map.piece << '\n' << map.done << '\n';
        if (!out.flush())
            return false;
    }
    return ::MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

} // namespace

std::string sha256Of(const std::filesystem::path& path, const std::atomic_bool& cancel, const std::function<void(std::uint64_t)>& progress)
{
    FileHandle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid())
        return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return {};
    const std::unique_ptr<void, void (*)(void*)> algorithmOwner(algorithm, [](void* a) { ::BCryptCloseAlgorithmProvider(a, 0); });
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)))
        return {};
    const std::unique_ptr<void, void (*)(void*)> hashOwner(hash, [](void* h) { ::BCryptDestroyHash(h); });
    std::vector<unsigned char> buffer(4 << 20);
    std::uint64_t done = 0;
    auto told = std::chrono::steady_clock::now();
    for (;;) {
        if (cancel.load())
            return {};
        DWORD read = 0;
        if (!::ReadFile(file.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            return {};
        if (read == 0)
            break;
        if (!BCRYPT_SUCCESS(::BCryptHashData(hash, buffer.data(), read, 0)))
            return {};
        done += read;
        if (progress && std::chrono::steady_clock::now() - told >= std::chrono::milliseconds(250)) {
            told = std::chrono::steady_clock::now();
            progress(done);
        }
    }
    std::array<unsigned char, 32> digest {};
    if (!BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0)))
        return {};
    return hex(digest.data(), digest.size());
}

DownloadResult download(const std::string& url, const std::filesystem::path& target, std::uint64_t size, std::string_view sha256,
    const std::atomic_bool& cancel, const DownloadProgress& progress, bool chinese, const DownloadTuning& tuning)
{
    DownloadResult result;
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::path part = target;
    part += L".part";
    std::filesystem::path mapFile = part;
    mapFile += L".map";
    const std::uint64_t piece = std::max<std::uint64_t>(tuning.pieceBytes, 64 << 10);
    const std::size_t pieces = static_cast<std::size_t>((size + piece - 1) / piece);
    const auto pieceStart = [&](std::size_t i) { return static_cast<std::uint64_t>(i) * piece; };
    const auto pieceEnd = [&](std::size_t i) { return std::min<std::uint64_t>(size, pieceStart(i) + piece) - 1; };

    // What is there already: the map's pieces, or (a .part of one stream
    // from the start, without a map) the whole pieces it holds.
    PartMap map {size, piece, std::string(pieces, '0')};
    const std::uint64_t partSize = std::filesystem::is_regular_file(part, ec) ? std::filesystem::file_size(part, ec) : 0;
    const bool hasMap = std::filesystem::exists(mapFile, ec);
    if (const auto saved = readMap(mapFile); saved && saved->size == size && saved->piece == piece && partSize == size)
        map = *saved;
    else if (!hasMap && partSize > 0 && partSize < size)
        std::fill_n(map.done.begin(), static_cast<std::size_t>(partSize / piece), '1');
    else if (partSize > 0)
        std::filesystem::remove(part, ec); // not one to go on with
    std::uint64_t have = 0;
    for (std::size_t i = 0; i < pieces; ++i)
        have += map.done[i] == '1' ? pieceEnd(i) - pieceStart(i) + 1 : 0;

    // Room for the rest, and a little more (the .part is sparse: only what came takes room).
    ULARGE_INTEGER free {};
    if (::GetDiskFreeSpaceExW(target.parent_path().c_str(), &free, nullptr, nullptr) && free.QuadPart < size - have + (64ull << 20)) {
        result.error = chinese ? "磁盘空间不够：还要 " + sizeText(size - have) + "，只剩 " + sizeText(free.QuadPart)
                               : "Not enough disk space: " + sizeText(size - have) + " needed, " + sizeText(free.QuadPart) + " free";
        return result;
    }
    FileHandle file(::CreateFileW(part.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        result.error = chinese ? "下载的文件写不进去（" + std::to_string(::GetLastError()) + "）"
                               : "Cannot write the download (" + std::to_string(::GetLastError()) + ")";
        return result;
    }
    // Sparse, then its full size: a piece far into the file is written
    // without the disk first filling all before it with zeros.
    DWORD returned = 0;
    ::DeviceIoControl(file.handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
    FILE_END_OF_FILE_INFO end {};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!::SetFileInformationByHandle(file.handle, FileEndOfFileInfo, &end, sizeof end)) {
        result.error = networkText(::GetLastError(), chinese);
        return result;
    }
    if (!writeMap(mapFile, map)) {
        result.error = chinese ? "下载的记录写不进去" : "Cannot write the download's record";
        return result;
    }

    // The pieces over several connections (the server slows each one down,
    // not all of them together); a piece whose connection breaks is tried
    // again; the map is written as each piece is in.
    const std::wstring wideUrl = winshun::wide(url);
    const int connections = std::max(1, tuning.connections);
    std::mutex mutex; // map, taken, have, failures, the errors
    std::vector<bool> taken(pieces, false);
    std::vector<std::atomic<std::uint64_t>> flight(static_cast<std::size_t>(connections)); // bytes of each one's piece so far
    std::atomic_bool stop = false;
    bool noRanges = false;
    int failures = 0; // in a row, across the connections
    std::string lastError;
    std::string failedError;
    const auto worker = [&](std::size_t c) {
        std::unique_lock lock(mutex);
        for (;;) {
            std::size_t i = 0;
            while (i < pieces && (map.done[i] == '1' || taken[i]))
                ++i;
            if (i == pieces || stop)
                return;
            taken[i] = true;
            flight[c] = 0;
            lock.unlock();
            std::string error;
            const Step step = fetch(wideUrl, file.handle, pieceStart(i), pieceEnd(i), false, stop, flight[c], error, chinese);
            lock.lock();
            taken[i] = false;
            flight[c] = 0;
            switch (step) {
            case Step::Done:
                map.done[i] = '1';
                have += pieceEnd(i) - pieceStart(i) + 1;
                failures = 0;
                writeMap(mapFile, map);
                continue;
            case Step::Canceled:
                return;
            case Step::NoRanges:
                noRanges = true;
                stop = true;
                return;
            case Step::Failed:
                failedError = error;
                stop = true;
                return;
            case Step::Again:
                break;
            }
            lastError = error;
            if (++failures > 3 * connections) { // nothing has come through for a good while
                failedError = error;
                stop = true;
                return;
            }
            const auto wait = std::chrono::seconds(std::min(failures, 5));
            lock.unlock();
            for (auto waited = std::chrono::milliseconds(0); waited < wait && !stop; waited += std::chrono::milliseconds(100))
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            lock.lock();
        }
    };
    const auto bytesNow = [&] {
        std::uint64_t moving = 0;
        for (const auto& f : flight)
            moving += f.load();
        return std::min(size, have + moving);
    };
    std::vector<std::thread> threads;
    for (int c = 0; c < connections; ++c)
        threads.emplace_back(worker, static_cast<std::size_t>(c));
    // The progress, until they are through; a cancel stops them.
    for (;;) {
        bool through = false;
        {
            const std::lock_guard lock(mutex);
            if (cancel)
                stop = true;
            through = stop || map.done.find('0') == std::string::npos;
            if (progress)
                progress(bytesNow(), size, false);
        }
        if (through)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    for (std::thread& t : threads)
        t.join();
    if (cancel) {
        result.canceled = true;
        return result;
    }
    if (noRanges) {
        // A server without ranges: the whole file in one stream, from the start.
        std::fill(map.done.begin(), map.done.end(), '0');
        writeMap(mapFile, map);
        have = 0;
        for (int attempt = 1;; ++attempt) {
            std::string error;
            Step step = Step::Again;
            std::thread whole([&] { step = fetch(wideUrl, file.handle, 0, size - 1, true, cancel, flight[0], error, chinese); });
            while (::WaitForSingleObject(whole.native_handle(), 250) == WAIT_TIMEOUT) {
                if (progress)
                    progress(std::min(size, flight[0].load()), size, false);
            }
            whole.join();
            flight[0] = 0;
            if (step == Step::Done)
                break;
            if (step == Step::Canceled || cancel) {
                result.canceled = true;
                return result;
            }
            if (step == Step::Failed || step == Step::NoRanges || attempt >= 4) {
                result.error = error.empty() ? (chinese ? "下载没有完成" : "The download did not finish") : error;
                return result;
            }
            for (int waited = 0; waited < 2000 * attempt && !cancel; waited += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } else if (!failedError.empty()) {
        result.error = failedError;
        return result;
    } else if (map.done.find('0') != std::string::npos) {
        result.error = lastError.empty() ? (chinese ? "下载没有完成" : "The download did not finish") : lastError;
        return result;
    }
    file.close();

    if (progress)
        progress(0, size, true);
    const std::string actual = sha256Of(part, cancel, [&](std::uint64_t done) {
        if (progress)
            progress(done, size, true);
    });
    if (cancel.load()) {
        result.canceled = true;
        return result;
    }
    if (progress)
        progress(size, size, true); // read through (the last step may have come within 250 ms)
    std::filesystem::remove(mapFile, ec);
    if (actual != sha256) {
        std::filesystem::remove(part, ec);
        result.error = chinese ? "下载的文件校验不对，已删掉，请重新下载" : "The download did not pass the check and was deleted: download it again";
        return result;
    }
    if (!::MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        result.error = chinese ? "下好的模型放不进模型文件夹（" + std::to_string(::GetLastError()) + "）"
                               : "The model could not be moved into place (" + std::to_string(::GetLastError()) + ")";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace lm
