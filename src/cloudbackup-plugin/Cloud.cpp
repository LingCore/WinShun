#include "Cloud.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

using winshun::Json;

namespace cb {

namespace {

constexpr const char* kPrefix = "WinShun-";
constexpr const char* kGithubFolder = "WinShun";
constexpr int kQuickMs = 30'000; // a listing, a deletion
constexpr int kTransferMs = 300'000; // a backup up or down

std::string trimmed(const std::string& s)
{
    return winshun::trim(s);
}

bool startsWith(const std::string& s, const char* prefix)
{
    return s.rfind(prefix, 0) == 0;
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void newestFirst(std::vector<Remote>& backups)
{
    std::sort(backups.begin(), backups.end(), [](const Remote& a, const Remote& b) { return a.name > b.name; });
}

std::string megabytes(std::size_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

// ---- WebDAV ---------------------------------------------------------------------

class WebDav final : public Target {
public:
    WebDav(const Settings& s, bool chinese, const std::atomic_bool* canceled)
        : m_zh(chinese), m_canceled(canceled), m_user(s.webdavUser)
    {
        m_url = encodeAddress(trimmed(s.webdavUrl));
        if (!m_url.empty() && m_url.back() != '/')
            m_url += '/';
        if (!s.webdavUser.empty() || !s.webdavPassword.empty())
            m_authorization = "Basic " + base64(s.webdavUser + ":" + s.webdavPassword);
    }

    std::string key() const override { return "webdav"; }
    std::string label() const override { return "WebDAV"; }
    std::string id() const override { return m_url + "|" + m_user; }

    Outcome list(std::vector<Remote>& out) override
    {
        out.clear();
        if (!startsWith(lower(m_url), "http://") && !startsWith(lower(m_url), "https://"))
            return {m_zh ? "WebDAV 地址要以 https:// 或 http:// 开头" : "The WebDAV address must start with https:// or http://"};
        Request q = request("PROPFIND", m_url, kQuickMs);
        q.headers["Depth"] = "1";
        q.headers["Content-Type"] = "application/xml; charset=utf-8";
        q.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:getcontentlength/></d:prop></d:propfind>";
        const Response r = send(q);
        if (r.error)
            return {networkError(r, m_zh)};
        if (r.status == 404 || r.status == 409) {
            // The folder is not there yet: made now (one level; its parent is the user's).
            const Response made = send(request("MKCOL", m_url, kQuickMs));
            if (made.error)
                return {networkError(made, m_zh)};
            if (made.status == 201 || made.status == 200 || made.status == 204 || made.status == 405)
                return {};
            if (made.status == 401 || made.status == 403)
                return failed(made);
            return {(m_zh ? "云端没有这个文件夹，也建不出来（服务器回答 " : "The folder is not there and could not be made (the server answered ")
                + std::to_string(made.status) + (m_zh ? "）：检查 WebDAV 地址" : "): check the WebDAV address")};
        }
        if (r.status != 207 && r.status != 200)
            return failed(r);
        out = webdavListing(r.body);
        return {};
    }

    Outcome upload(const std::string& name, const std::string& bytes) override
    {
        Request q = request("PUT", m_url + urlEncode(name), kTransferMs);
        q.headers["Content-Type"] = "application/zip";
        q.body = bytes;
        const Response r = send(q);
        if (r.error)
            return {networkError(r, m_zh)};
        if (r.status != 200 && r.status != 201 && r.status != 204)
            return failed(r);
        return {};
    }

    Outcome download(const Remote& backup, std::string& bytes) override
    {
        Request q = request("GET", m_url + urlEncode(backup.name), kTransferMs);
        q.maxBody = kMaxBackup;
        Response r = send(q);
        if (r.error)
            return {networkError(r, m_zh)};
        if (r.status != 200)
            return failed(r);
        bytes = std::move(r.body);
        return {};
    }

    Outcome remove(const Remote& backup) override
    {
        const Response r = send(request("DELETE", m_url + urlEncode(backup.name), kQuickMs));
        if (r.error)
            return {networkError(r, m_zh)};
        if (r.status != 200 && r.status != 204 && r.status != 404)
            return failed(r);
        return {};
    }

private:
    Request request(const char* method, const std::string& url, int budgetMs) const
    {
        Request q;
        q.method = method;
        q.url = url;
        q.budgetMs = budgetMs;
        q.canceled = m_canceled;
        if (!m_authorization.empty())
            q.headers["Authorization"] = m_authorization;
        return q;
    }

    Outcome failed(const Response& r) const
    {
        switch (r.status) {
        case 401:
        case 403:
            return {m_zh ? "WebDAV 的用户名或密码不对（坚果云等要填“应用密码”，不是登录密码）"
                         : "The WebDAV user name or password is wrong (some services want an app password, not the sign-in one)"};
        case 404:
            return {m_zh ? "云端没有这个地址：检查 WebDAV 地址" : "Nothing at that address: check the WebDAV address"};
        case 507:
            return {m_zh ? "云端空间满了" : "The cloud storage is full"};
        default:
            return {(m_zh ? "WebDAV 服务器回答 " : "The WebDAV server answered ") + std::to_string(r.status)};
        }
    }

    bool m_zh;
    const std::atomic_bool* m_canceled;
    std::string m_url;
    std::string m_user;
    std::string m_authorization;
};

// ---- GitHub ---------------------------------------------------------------------

class GitHub final : public Target {
public:
    GitHub(const Settings& s, std::optional<Repo> repo, bool chinese, const std::atomic_bool* canceled)
        : m_zh(chinese), m_canceled(canceled), m_valid(repo.has_value()), m_repo(repo ? *repo : Repo {}), m_token(trimmed(s.githubToken))
    {
        m_api = trimmed(s.githubApi);
        if (m_api.empty())
            m_api = "https://api.github.com";
        while (!m_api.empty() && m_api.back() == '/')
            m_api.pop_back();
    }

    std::string key() const override { return "github"; }
    std::string label() const override { return "GitHub"; }
    std::string id() const override { return m_api + "|" + m_repo.owner + "/" + m_repo.name; }

    Outcome list(std::vector<Remote>& out) override
    {
        out.clear();
        if (!m_valid)
            return {m_zh ? "GitHub 仓库要写成“用户名/仓库名”，或直接贴仓库的网址" : "Write the GitHub repository as owner/name, or paste its address"};
        // A backup holds the settings and the favorites: never into a
        // repository everybody can read.
        const Response about = send(request("GET", repoUrl(), kQuickMs));
        if (about.error)
            return {networkError(about, m_zh)};
        if (about.status != 200)
            return failed(about);
        const auto repo = Json::parse(about.body);
        if (!repo || !repo->isObject())
            return {m_zh ? "GitHub 的回答看不懂" : "GitHub's answer could not be read"};
        if (!(*repo)["private"].toBool(false))
            return {m_zh ? m_repo.owner + "/" + m_repo.name + " 是公开仓库：备份里有你的设置和收藏，不会传上去。请换一个私有仓库"
                         : m_repo.owner + "/" + m_repo.name + " is public: a backup holds your settings and favorites and is not uploaded there. Use a private repository"};
        const Response r = send(request("GET", folderUrl(), kQuickMs));
        if (r.error)
            return {networkError(r, m_zh)};
        if (r.status == 404)
            return {}; // nothing uploaded yet (or an empty repository)
        if (r.status != 200)
            return failed(r);
        const auto listing = Json::parse(r.body);
        if (!listing || !listing->isArray())
            return {m_zh ? "GitHub 的回答看不懂" : "GitHub's answer could not be read"};
        for (const Json& entry : listing->toArray()) {
            if (entry["type"].toString() != "file" || !parseName(entry["name"].toString()))
                continue;
            out.push_back({entry["name"].toString(), entry["size"].toInt(-1), entry["sha"].toString()});
        }
        newestFirst(out);
        return {};
    }

    Outcome upload(const std::string& name, const std::string& bytes) override
    {
        if (bytes.size() > kMaxGithub)
            return {m_zh ? "备份有 " + megabytes(bytes.size()) + "，GitHub 这样传最多 50 MB：删掉用不着的插件，或改用 WebDAV"
                         : "The backup is " + megabytes(bytes.size()) + "; GitHub takes at most 50 MB this way: remove plugins you do not use, or use WebDAV"};
        Request q = request("PUT", folderUrl() + "/" + urlEncode(name), kTransferMs);
        q.headers["Content-Type"] = "application/json";
        q.body = Json(Json::Object {{"message", "Win顺 backup " + name}, {"content", base64(bytes)}}).dump();
        const Response r = send(q);
        if (r.error)
            return {networkError(r, m_zh)};
        if (r.status != 200 && r.status != 201)
            return failed(r);
        return {};
    }

    Outcome download(const Remote& backup, std::string& bytes) override
    {
        Request q = request("GET", folderUrl() + "/" + urlEncode(backup.name), kTransferMs);
        q.headers["Accept"] = "application/vnd.github.raw+json";
        q.maxBody = kMaxBackup;
        Response r = send(q);
        if (r.error)
            return {networkError(r, m_zh)};
        if (r.status != 200)
            return failed(r);
        bytes = std::move(r.body);
        return {};
    }

    Outcome remove(const Remote& backup) override
    {
        Request q = request("DELETE", folderUrl() + "/" + urlEncode(backup.name), kQuickMs);
        q.headers["Content-Type"] = "application/json";
        q.body = Json(Json::Object {{"message", "Win顺 backup: remove " + backup.name}, {"sha", backup.sha}}).dump();
        const Response r = send(q);
        if (r.error)
            return {networkError(r, m_zh)};
        if (r.status != 200 && r.status != 404)
            return failed(r);
        return {};
    }

private:
    std::string repoUrl() const { return m_api + "/repos/" + urlEncode(m_repo.owner) + "/" + urlEncode(m_repo.name); }
    std::string folderUrl() const { return repoUrl() + "/contents/" + kGithubFolder; }

    Request request(const char* method, const std::string& url, int budgetMs) const
    {
        Request q;
        q.method = method;
        q.url = url;
        q.budgetMs = budgetMs;
        q.canceled = m_canceled;
        q.headers["Authorization"] = "Bearer " + m_token;
        q.headers["Accept"] = "application/vnd.github+json";
        q.headers["X-GitHub-Api-Version"] = "2022-11-28";
        return q;
    }

    Outcome failed(const Response& r) const
    {
        const std::string repo = m_repo.owner + "/" + m_repo.name;
        switch (r.status) {
        case 401:
            return {m_zh ? "GitHub 令牌不对，或已经过期" : "The GitHub token is wrong or has expired"};
        case 403:
        case 404:
            return {m_zh ? "找不到仓库 " + repo + "，或令牌没有它的读写权限（细粒度令牌要给这个仓库的 Contents 读写权限）"
                         : "Repository " + repo + " was not found, or the token may not read and write it (a fine-grained token needs Contents: read and write)"};
        default: {
            const auto body = Json::parse(r.body);
            std::string said = body && body->isObject() ? (*body)["message"].toString() : std::string();
            if (said.size() > 120)
                said.resize(120);
            return {(m_zh ? "GitHub 回答 " : "GitHub answered ") + std::to_string(r.status) + (said.empty() ? "" : (m_zh ? "：" : ": ") + said)};
        }
        }
    }

    bool m_zh;
    const std::atomic_bool* m_canceled;
    bool m_valid;
    Repo m_repo;
    std::string m_token;
    std::string m_api;
};

// The local name of a tag ("d:href", "D:href", "href" -> "href"), lower case.
std::string localName(const std::string& tag)
{
    const std::size_t colon = tag.find(':');
    return lower(colon == std::string::npos ? tag : tag.substr(colon + 1));
}

std::string unescaped(std::string text)
{
    static const std::pair<const char*, const char*> kEntities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& [entity, plain] : kEntities) {
        for (std::size_t at = text.find(entity); at != std::string::npos; at = text.find(entity, at + 1))
            text.replace(at, std::strlen(entity), plain);
    }
    return text;
}

} // namespace

Settings settingsFrom(const Json& json)
{
    Settings s;
    s.webdavUrl = trimmed(json["webdavUrl"].toString());
    s.webdavUser = trimmed(json["webdavUser"].toString());
    s.webdavPassword = json["webdavPassword"].toString();
    s.githubRepo = trimmed(json["githubRepo"].toString());
    s.githubToken = trimmed(json["githubToken"].toString());
    s.githubApi = trimmed(json["githubApi"].toString());
    const Json& keep = json["keep"];
    long long n = keep.isNumber() ? keep.toInt() : keep.isString() ? std::atoll(keep.toString().c_str()) : 10;
    s.keep = static_cast<int>(std::clamp<long long>(n <= 0 ? 10 : n, 1, 100));
    s.automatic = json["automatic"].toBool(true);
    return s;
}

std::string base64(const std::string& bytes)
{
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8)
            | static_cast<unsigned char>(bytes[i + 2]);
        out += kAlphabet[v >> 18];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i < bytes.size()) {
        unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        const bool two = i + 1 < bytes.size();
        if (two)
            v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += kAlphabet[v >> 18];
        out += kAlphabet[(v >> 12) & 63];
        out += two ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string urlEncode(const std::string& segment)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : segment) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            out += ch;
        else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string urlDecode(const std::string& text)
{
    const auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else
            out += text[i];
    }
    return out;
}

std::string encodeAddress(const std::string& url)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : url) {
        const auto c = static_cast<unsigned char>(ch);
        if (c > 0x20 && c < 0x7f)
            out += ch;
        else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string backupName(const SYSTEMTIME& t, const std::string& computer)
{
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::string pc;
    for (const char ch : computer) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) && c < 0x80)
            pc += ch;
        else if ((ch == '-' || ch == '_') && !pc.empty())
            pc += ch;
        if (pc.size() == 32)
            break;
    }
    while (!pc.empty() && (pc.back() == '-' || pc.back() == '_'))
        pc.pop_back();
    return std::string(kPrefix) + stamp + (pc.empty() ? "" : "-" + pc) + ".zip";
}

std::optional<Name> parseName(const std::string& name)
{
    // WinShun-YYYY-MM-DD-HHMMSS[-computer].zip
    static const char kShape[] = "dddd-dd-dd-dddddd";
    const std::size_t prefix = std::strlen(kPrefix);
    const std::size_t shape = sizeof kShape - 1;
    if (name.size() < prefix + shape + 4 || name.compare(0, prefix, kPrefix) != 0
        || name.compare(name.size() - 4, 4, ".zip") != 0)
        return std::nullopt;
    for (std::size_t i = 0; i < shape; ++i) {
        const char c = name[prefix + i];
        if (kShape[i] == 'd' ? !(c >= '0' && c <= '9') : c != '-')
            return std::nullopt;
    }
    Name parsed;
    const std::string stamp = name.substr(prefix, shape);
    parsed.when = stamp.substr(0, 10) + " " + stamp.substr(11, 2) + ":" + stamp.substr(13, 2);
    parsed.second = stamp.substr(15, 2);
    const std::string rest = name.substr(prefix + shape, name.size() - prefix - shape - 4);
    if (!rest.empty()) {
        if (rest[0] != '-' || rest.size() < 2 || rest.size() > 33)
            return std::nullopt;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            const auto c = static_cast<unsigned char>(rest[i]);
            if (!(std::isalnum(c) && c < 0x80) && rest[i] != '-' && rest[i] != '_')
                return std::nullopt;
        }
        parsed.computer = rest.substr(1);
    }
    return parsed;
}

std::unique_ptr<Target> webdavTarget(const Settings& settings, bool chinese, const std::atomic_bool* canceled)
{
    if (!settings.webdav())
        return nullptr;
    return std::make_unique<WebDav>(settings, chinese, canceled);
}

std::unique_ptr<Target> githubTarget(const Settings& settings, bool chinese, const std::atomic_bool* canceled)
{
    if (!settings.github())
        return nullptr;
    // What is not a repository's name: list() says how to write one.
    return std::make_unique<GitHub>(settings, parseRepo(settings.githubRepo), chinese, canceled);
}

std::vector<Remote> webdavListing(const std::string& xml)
{
    std::vector<Remote> found;
    Remote current;
    bool inside = false;
    for (std::size_t at = xml.find('<'); at != std::string::npos; at = xml.find('<', at + 1)) {
        const std::size_t end = xml.find('>', at);
        if (end == std::string::npos)
            break;
        std::string tag = xml.substr(at + 1, end - at - 1);
        const bool closing = !tag.empty() && tag[0] == '/';
        if (closing)
            tag.erase(0, 1);
        if (const std::size_t space = tag.find_first_of(" \t\r\n/"); space != std::string::npos)
            tag.resize(space);
        const std::string name = localName(tag);
        if (name == "response") {
            if (!closing) {
                current = {};
                inside = true;
            } else if (inside) {
                inside = false;
                if (parseName(current.name))
                    found.push_back(current);
            }
            continue;
        }
        if (closing || !inside || (name != "href" && name != "getcontentlength"))
            continue;
        const std::size_t close = xml.find('<', end + 1);
        if (close == std::string::npos)
            break;
        const std::string text = trimmed(unescaped(xml.substr(end + 1, close - end - 1)));
        if (name == "href") {
            std::string path = text;
            if (const std::size_t query = path.find_first_of("?#"); query != std::string::npos)
                path.resize(query);
            while (!path.empty() && path.back() == '/')
                path.pop_back();
            const std::size_t slash = path.rfind('/');
            current.name = urlDecode(slash == std::string::npos ? path : path.substr(slash + 1));
        } else if (!text.empty() && text.find_first_not_of("0123456789") == std::string::npos)
            current.size = std::atoll(text.c_str());
    }
    newestFirst(found);
    return found;
}

std::optional<Repo> parseRepo(const std::string& text)
{
    std::string s = trimmed(text);
    if (const std::size_t scheme = s.find("://"); scheme != std::string::npos) {
        s.erase(0, scheme + 3);
        const std::size_t slash = s.find('/');
        if (slash == std::string::npos)
            return std::nullopt;
        s.erase(0, slash + 1); // the host
    } else if (startsWith(lower(s), "github.com/"))
        s.erase(0, 11);
    if (const std::size_t query = s.find_first_of("?#"); query != std::string::npos)
        s.resize(query);
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    const std::size_t slash = s.find('/');
    if (slash == std::string::npos || slash == 0)
        return std::nullopt;
    Repo repo {s.substr(0, slash), s.substr(slash + 1)};
    if (const std::size_t more = repo.name.find('/'); more != std::string::npos)
        repo.name.resize(more); // …/tree/main
    if (repo.name.size() > 4 && repo.name.compare(repo.name.size() - 4, 4, ".git") == 0)
        repo.name.resize(repo.name.size() - 4);
    const auto fine = [](const std::string& part) {
        return !part.empty() && part.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.") == std::string::npos;
    };
    if (!fine(repo.owner) || !fine(repo.name))
        return std::nullopt;
    return repo;
}

} // namespace cb
