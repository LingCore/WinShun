#include "Sender.h"

#include "HttpLite.h"
#include "Util.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

using winshun::Json;
using namespace std::chrono_literals;

namespace localsend {

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kPrepare = "/api/localsend/v2/prepare-upload";
constexpr const char* kUpload = "/api/localsend/v2/upload";
constexpr const char* kCancel = "/api/localsend/v2/cancel";
constexpr int kMaxLevels = 32;

std::wstring nameOf(const std::wstring& path)
{
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/'))
        p.pop_back();
    const std::size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

bool isFolder(DWORD attributes)
{
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// A link's own size is 0: the size of what it points to, which is what is
// read and sent. False when that cannot be had (a link to nothing).
bool linkedSize(const std::wstring& path, std::uint64_t& size)
{
    const Handle file(::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, 0, nullptr));
    LARGE_INTEGER length {};
    if (!file.valid() || !::GetFileSizeEx(file.get(), &length))
        return false;
    size = static_cast<std::uint64_t>(length.QuadPart);
    return true;
}

// A chosen path, with one look: a folder, or a file and its size. False
// when it is gone (or a link to nothing).
bool look(const std::wstring& path, bool& folder, std::uint64_t& size)
{
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return false;
    folder = isFolder(data.dwFileAttributes);
    size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return folder || !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || linkedSize(path, size);
}

using Visit = std::function<bool(const std::wstring& path, const std::string& name, std::uint64_t size)>;

// The files under `folder`, as `prefix/…` names, until `visit` (or
// `goOn`, asked at each folder) says stop. Links to folders are not
// followed (a junction back up would never end); a cloud folder
// (OneDrive's placeholders) is a reparse point too, but no link, and is.
// A linked file is sent. False when it stopped.
bool walk(const std::wstring& folder, const std::string& prefix, int level, const Visit& visit, const std::function<bool()>& goOn = {})
{
    if (level > kMaxLevels)
        return true;
    if (goOn && !goOn())
        return false;
    WIN32_FIND_DATAW found {};
    const HANDLE find = ::FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return true;
    bool more = true;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..")
            continue;
        // A junction, a symbolic link, a mounted volume: where the name points elsewhere.
        const bool link = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(found.dwReserved0);
        const std::wstring path = folder + L"\\" + name;
        const std::string sent = prefix + "/" + narrow(name);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!link)
                more = walk(path, sent, level + 1, visit, goOn);
        } else {
            std::uint64_t size = (static_cast<std::uint64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
            if (!link || linkedSize(path, size))
                more = visit(path, sent, size);
        }
    } while (more && ::FindNextFileW(find, &found));
    ::FindClose(find);
    return more;
}

} // namespace

std::string mimeType(const std::wstring& path)
{
    const std::size_t dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? std::wstring() : path.substr(dot + 1);
    for (wchar_t& c : ext)
        c = static_cast<wchar_t>(std::towlower(c));
    static const std::pair<const wchar_t*, const char*> kTypes[] = {
        {L"jpg", "image/jpeg"}, {L"jpeg", "image/jpeg"}, {L"png", "image/png"}, {L"gif", "image/gif"},
        {L"webp", "image/webp"}, {L"heic", "image/heic"}, {L"bmp", "image/bmp"}, {L"svg", "image/svg+xml"},
        {L"mp4", "video/mp4"}, {L"mov", "video/quicktime"}, {L"mkv", "video/x-matroska"}, {L"avi", "video/x-msvideo"},
        {L"mp3", "audio/mpeg"}, {L"m4a", "audio/mp4"}, {L"wav", "audio/wav"}, {L"flac", "audio/flac"},
        {L"txt", "text/plain"}, {L"md", "text/markdown"}, {L"csv", "text/csv"}, {L"html", "text/html"},
        {L"pdf", "application/pdf"}, {L"zip", "application/zip"}, {L"apk", "application/vnd.android.package-archive"},
        {L"doc", "application/msword"}, {L"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {L"xls", "application/vnd.ms-excel"}, {L"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {L"ppt", "application/vnd.ms-powerpoint"},
        {L"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    };
    for (const auto& [e, type] : kTypes) {
        if (ext == e)
            return type;
    }
    return "application/octet-stream";
}

bool onNetwork(const std::wstring& path)
{
    std::wstring p = path;
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0)
        return true;
    if (p.rfind(L"\\\\?\\", 0) == 0)
        p = p.substr(4); // "\\?\C:\…": a drive's
    if (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/'))
        return true; // \\server\share
    if (p.size() >= 2 && p[1] == L':') {
        const wchar_t root[] = {p[0], L':', L'\\', L'\0'};
        return ::GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    return false;
}

Summary summarize(const std::vector<std::wstring>& paths, std::chrono::milliseconds budget)
{
    Summary s;
    const Clock::time_point until = Clock::now() + budget;
    const auto inTime = [&] {
        if (Clock::now() < until)
            return true;
        s.partial = true;
        return false;
    };
    std::size_t seen = 0;
    for (const std::wstring& p : paths) {
        if (s.names.size() < 3)
            s.names.push_back(narrow(nameOf(p)));
        // Each one counts against the time: thousands chosen on a slow share add up.
        if (s.partial || s.files >= kMaxFiles || !inTime()) {
            s.partial = true;
            if (s.names.size() >= 3)
                break;
            continue;
        }
        if (onNetwork(p)) {
            // At least a file; what it holds is seen when it is sent.
            ++s.files;
            ++s.remote;
            continue;
        }
        bool folder = false;
        std::uint64_t size = 0;
        if (!look(p, folder, size))
            continue;
        if (!folder) {
            ++s.files;
            s.bytes += size;
            continue;
        }
        walk(p, std::string(), 0, [&](const std::wstring&, const std::string&, std::uint64_t bytes) {
            ++s.files;
            s.bytes += bytes;
            if (s.files >= kMaxFiles || (++seen % 64 == 0 && !inTime())) {
                s.partial = true;
                return false;
            }
            return true;
        }, inTime);
    }
    if (s.remote)
        s.partial = true; // "at least": what the shares hold is not known
    return s;
}

bool collectFiles(const std::vector<std::wstring>& paths, std::vector<OutgoingFile>& out, std::string* why)
{
    out.clear();
    bool tooMany = false;
    for (const std::wstring& p : paths) {
        bool folder = false;
        std::uint64_t size = 0;
        if (!look(p, folder, size))
            continue; // gone since it was copied
        if (!folder) {
            out.push_back({p, narrow(nameOf(p)), size});
        } else {
            walk(p, narrow(nameOf(p)), 0, [&](const std::wstring& path, const std::string& name, std::uint64_t size) {
                out.push_back({path, name, size});
                return !(tooMany = out.size() > kMaxFiles);
            });
        }
        if (tooMany || out.size() > kMaxFiles) {
            *why = "more than 10,000 files";
            return false;
        }
    }
    if (out.empty()) {
        *why = "no files (empty folders, or gone)";
        return false;
    }
    return true;
}

SendJob::SendJob(Client& client, Target target, std::string deviceName, DeviceInfo self, Outgoing what,
    std::function<void(const SendOutcome&)> done)
    : m_client(client)
    , m_target(std::move(target))
    , m_deviceName(std::move(deviceName))
    , m_self(std::move(self))
    , m_what(std::move(what))
    , m_done(std::move(done))
{
    m_view.device = m_deviceName;
    m_view.text = m_what.kind == Outgoing::Kind::Text;
    m_thread = std::thread([this] { run(); });
}

SendJob::~SendJob()
{
    cancel();
    if (m_thread.joinable())
        m_thread.join();
}

std::optional<SendView> SendJob::view() const
{
    if (m_finished)
        return std::nullopt;
    const std::lock_guard lock(m_mutex);
    SendView v = m_view;
    v.sent = m_sent;
    return v;
}

void SendJob::cancel()
{
    {
        // Under the lock: a wait for the PIN that has just looked cannot miss it.
        const std::lock_guard lock(m_mutex);
        m_cancelled = true;
    }
    m_cutter.cut();
    m_wake.notify_all();
}

void SendJob::shutDown(std::chrono::milliseconds within)
{
    {
        const std::lock_guard lock(m_mutex);
        m_cancelled = true;
        m_sayWithin = within;
    }
    m_cutter.cut();
    m_wake.notify_all();
    std::unique_lock lock(m_mutex);
    if (!m_wake.wait_for(lock, within, [&] { return m_finished.load(); })) {
        lock.unlock();
        m_sayCutter.cut(); // the receiver was not told in time: it gives up on its own
    }
}

bool SendJob::givePin(const std::string& pin)
{
    const std::lock_guard lock(m_mutex);
    if (m_finished || m_view.state != SendView::State::NeedsPin || pin.empty())
        return false;
    m_pin = pin;
    m_view.state = SendView::State::Waiting; // asked again with it: the row says so at once, one PIN at a time
    m_wake.notify_all();
    return true;
}

void SendJob::sample()
{
    const std::lock_guard lock(m_mutex);
    const Clock::time_point now = Clock::now();
    m_samples.emplace_back(now, m_sent.load());
    while (m_samples.size() > 2 && now - m_samples.front().first > 3s)
        m_samples.erase(m_samples.begin());
    if (m_samples.size() >= 2) {
        const double seconds = std::chrono::duration<double>(m_samples.back().first - m_samples.front().first).count();
        if (seconds > 0.3)
            m_view.bytesPerSecond = static_cast<double>(m_samples.back().second - m_samples.front().second) / seconds;
    }
}

SendOutcome::Kind SendJob::prepare(const PrepareUpload& request, std::string& sessionId, std::map<std::string, std::string>& tokens,
    std::string& detail)
{
    const std::string body = toJson(request).dump();
    std::string pin;
    for (;;) {
        {
            const std::lock_guard lock(m_mutex);
            m_view.state = SendView::State::Waiting;
        }
        const std::string target = pin.empty() ? std::string(kPrepare) : std::string(kPrepare) + "?pin=" + http::percentEncode(pin);
        const Clock::time_point asked = Clock::now();
        // The answer: a token for each file, at most (a token LocalSend makes is far shorter).
        constexpr std::size_t kMaxAnswer = kMaxFiles * 256 + Client::kMaxReply;
        const Reply r = m_client.request(m_target, "POST", target, body,
            std::chrono::duration_cast<std::chrono::milliseconds>(kAnswerTime), &m_cutter, Client::kConnectTime, kMaxAnswer);
        if (m_cancelled)
            return SendOutcome::Kind::Cancelled;
        switch (r.status) {
        case 200: {
            const std::optional<Json> json = parsePeerJson(r.body, kMaxValues);
            if (!json || !(*json)["sessionId"].isString() || !(*json)["files"].isObject()) {
                detail = "prepare-upload answered 200 without a session";
                return SendOutcome::Kind::Unreachable;
            }
            sessionId = (*json)["sessionId"].toString();
            for (const auto& [id, token] : (*json)["files"].toObject()) {
                if (token.isString())
                    tokens[id] = token.toString();
            }
            return SendOutcome::Kind::Sent;
        }
        case 204:
            return SendOutcome::Kind::TextSent; // nothing to upload: a text message, or all files declined
        case 401: {
            // A PIN: wait for the one the user types in the 传 list.
            std::unique_lock lock(m_mutex);
            m_view.state = SendView::State::NeedsPin;
            m_view.wrongPin = !pin.empty();
            const bool given = m_wake.wait_for(lock, kPinTime, [&] { return m_cancelled || m_pin.has_value(); });
            if (m_cancelled)
                return SendOutcome::Kind::Cancelled;
            if (!given || !m_pin)
                return SendOutcome::Kind::PinRefused;
            pin = *m_pin;
            m_pin.reset();
            continue;
        }
        case 403:
            // Win顺's receiver gives up after its two minutes with "No answer",
            // and says when it has no room before anyone is asked.
            if (r.body.find("No answer") != std::string::npos)
                return SendOutcome::Kind::NoAnswer;
            if (r.body.find("Not enough space") != std::string::npos || r.body.find("Save location unavailable") != std::string::npos)
                return SendOutcome::Kind::NoRoom;
            return SendOutcome::Kind::Declined;
        case 409:
            return SendOutcome::Kind::Busy;
        case 413:
            return SendOutcome::Kind::TooMany; // Win顺: more than 10,000 files, or 4 MB of file list
        case 429:
            // Win顺's receiver: one text message per sender in five seconds, and
            // a while before a sender just declined may ask again. Else a PIN lockout.
            return r.body.find("Too many messages") != std::string::npos || r.body.find("Too many requests") != std::string::npos
                ? SendOutcome::Kind::Paused
                : SendOutcome::Kind::TooManyPins;
        case 0:
            detail = r.error;
            if (r.wrongCertificate)
                return SendOutcome::Kind::NotTheDevice;
            // Connected and waited the whole time: nobody answered. Otherwise it could not be reached.
            return Clock::now() - asked >= kAnswerTime - 5s ? SendOutcome::Kind::NoAnswer : SendOutcome::Kind::Unreachable;
        default:
            detail = "prepare-upload answered " + std::to_string(r.status);
            return SendOutcome::Kind::Unreachable;
        }
    }
}

void SendJob::sayCancel(const std::string& sessionId)
{
    // Not through the job's cutter: it is cut. Best effort; the connections are gone anyway.
    std::chrono::milliseconds within {};
    {
        const std::lock_guard lock(m_mutex);
        within = m_sayWithin;
    }
    const std::string target = sessionId.empty() ? std::string(kCancel) : std::string(kCancel) + "?sessionId=" + http::percentEncode(sessionId);
    m_client.request(m_target, "POST", target, std::string(), within, &m_sayCutter, within);
}

void SendJob::run()
{
    SendOutcome outcome;
    outcome.device = m_deviceName;
    outcome.text = m_what.kind == Outgoing::Kind::Text;
    outcome.kind = SendOutcome::Kind::Interrupted; // what an exception leaves
    guarded("sending", [&] { outcome.kind = work(outcome); });
    {
        const std::lock_guard lock(m_mutex);
        m_finished = true;
    }
    m_wake.notify_all(); // shutDown() waits for it
    guarded("telling a send's end", [&] {
        if (m_done)
            m_done(outcome);
    });
}

SendOutcome::Kind SendJob::work(SendOutcome& outcome)
{
    PrepareUpload request;
    request.info = m_self;
    std::vector<OutgoingFile> files;
    if (m_what.kind == Outgoing::Kind::Text) {
        FileMeta text;
        text.id = "t" + randomToken(6);
        text.fileName = text.id + ".txt";
        text.size = m_what.text.size();
        text.fileType = "text/plain";
        text.preview = m_what.text;
        request.files.push_back(std::move(text));
    } else {
        std::string why;
        if (!collectFiles(m_what.paths, files, &why)) {
            outcome.detail = why;
            return SendOutcome::Kind::Nothing;
        }
        for (std::size_t i = 0; i < files.size(); ++i) {
            FileMeta f;
            f.id = "f" + std::to_string(i + 1);
            f.fileName = files[i].name;
            f.size = files[i].size;
            f.fileType = mimeType(files[i].path);
            request.files.push_back(std::move(f));
        }
    }
    {
        const std::lock_guard lock(m_mutex);
        m_view.files = request.files.size();
        m_view.total = request.totalBytes();
    }
    outcome.files = request.files.size();
    outcome.bytes = request.totalBytes();
    if (m_cancelled)
        return SendOutcome::Kind::Cancelled;

    std::string sessionId;
    std::map<std::string, std::string> tokens;
    const SendOutcome::Kind answer = prepare(request, sessionId, tokens, outcome.detail);
    if (answer == SendOutcome::Kind::Cancelled)
        sayCancel(sessionId);
    if (answer != SendOutcome::Kind::Sent)
        return answer == SendOutcome::Kind::TextSent && m_what.kind == Outgoing::Kind::Files ? SendOutcome::Kind::Declined : answer;

    {
        const std::lock_guard lock(m_mutex);
        m_view.state = SendView::State::Sending;
    }
    const std::string base = std::string(kUpload) + "?sessionId=" + http::percentEncode(sessionId);
    std::size_t sentFiles = 0;
    std::uint64_t before = 0; // bytes of the files done
    for (std::size_t i = 0; i < request.files.size(); ++i) {
        const FileMeta& f = request.files[i];
        const auto token = tokens.find(f.id);
        if (token == tokens.end()) {
            before += f.size; // the receiver did not want this one
            m_sent = before;
            continue;
        }
        const std::string target = base + "&fileId=" + http::percentEncode(f.id) + "&token=" + http::percentEncode(token->second);
        Reply r;
        if (m_what.kind == Outgoing::Kind::Text) {
            r = m_client.request(m_target, "POST", target, m_what.text, 30s, &m_cutter);
        } else {
            auto last = Clock::now();
            r = m_client.upload(m_target, target, files[i].path, f.size, [&](std::uint64_t sent) {
                m_sent = before + sent;
                if (Clock::now() - last >= 500ms) {
                    last = Clock::now();
                    sample();
                }
                return !m_cancelled.load();
            }, &m_cutter);
        }
        if (m_cancelled) {
            sayCancel(sessionId);
            return SendOutcome::Kind::Cancelled;
        }
        if (r.status != 200) {
            outcome.detail = r.status ? "upload answered " + std::to_string(r.status) : r.error;
            outcome.files = sentFiles;
            sayCancel(sessionId);
            return SendOutcome::Kind::Interrupted;
        }
        ++sentFiles;
        before += f.size;
        m_sent = before;
    }
    outcome.files = sentFiles;
    return m_what.kind == Outgoing::Kind::Text ? SendOutcome::Kind::TextSent : SendOutcome::Kind::Sent;
}

} // namespace localsend
