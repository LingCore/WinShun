#include "Protocol.h"

#include "Util.h"

#include <algorithm>
#include <cmath>
#include <set>

using winshun::Json;

namespace localsend {

namespace {

// A peer's text as UTF-8 (its bad bytes U+FFFD), at most `bytes` of it,
// not cutting a character. Only what can end up within `bytes` is looked
// at: a byte never becomes less than a byte.
std::string peerText(std::string_view text, std::size_t bytes)
{
    return cutUtf8(validUtf8(text.substr(0, bytes + 4)), bytes);
}

// A peer's name for itself, as rows and notifications show it: one line,
// nothing in it that turns the text around; UTF-8, cut on a character (a
// mark of three bytes becomes one: three times what is kept is looked at).
std::string shown(std::string_view text, std::size_t bytes)
{
    std::wstring w = wide(text.substr(0, bytes * 3 + 4));
    for (wchar_t& c : w) {
        if (c < 0x20 || c == 0x7F)
            c = L' ';
        else if (directionMark(c))
            c = L'_';
    }
    return cutUtf8(narrow(w), bytes);
}

// SHA-256 hex (HTTPS) or LocalSend's random string (HTTP): nothing else is
// one. It names a row and a device; it is never cut.
bool fingerprintOk(const std::string& text)
{
    if (text.empty() || text.size() > kMaxFingerprintBytes)
        return false;
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_';
    });
}

} // namespace

bool fewValues(std::string_view json, std::size_t maxValues)
{
    std::size_t values = 1;
    bool inString = false;
    for (std::size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (inString) {
            if (c == '\\')
                ++i; // the escaped character is no quote
            else if (c == '"')
                inString = false;
        } else if (c == '"') {
            inString = true;
        } else if (c == ',' || c == '[' || c == '{') {
            if (++values > maxValues)
                return false;
        }
    }
    return true;
}

std::optional<Json> parsePeerJson(const std::string& text, std::size_t maxValues)
{
    if (!fewValues(text, maxValues))
        return std::nullopt;
    return Json::parse(text);
}

std::string normalizedDeviceType(std::string_view type)
{
    for (const char* known : {"mobile", "desktop", "web", "headless", "server"}) {
        if (type == known)
            return known;
    }
    return "desktop";
}

Json toJson(const DeviceInfo& info, bool withPort)
{
    Json j;
    j.set("alias", info.alias);
    j.set("version", info.version);
    j.set("deviceModel", info.deviceModel);
    j.set("deviceType", info.deviceType);
    j.set("fingerprint", info.fingerprint);
    if (withPort) {
        j.set("port", info.port);
        j.set("protocol", info.protocol);
    }
    j.set("download", info.download);
    return j;
}

std::optional<DeviceInfo> deviceFromJson(const Json& json)
{
    if (!json.isObject())
        return std::nullopt;
    DeviceInfo d;
    d.alias = shown(json["alias"].toString(), 128);
    d.fingerprint = json["fingerprint"].toString();
    if (d.alias.empty() || !fingerprintOk(d.fingerprint))
        return std::nullopt;
    d.version = peerText(json["version"].toString(), 16);
    d.deviceModel = shown(json["deviceModel"].toString(), 64);
    d.deviceType = normalizedDeviceType(json["deviceType"].toString());
    const double port = json["port"].toNumber(kPort);
    d.port = port >= 1 && port <= 65535 && std::floor(port) == port ? static_cast<int>(port) : kPort;
    d.protocol = json["protocol"].toString() == "http" ? "http" : "https";
    d.download = json["download"].toBool(false);
    return d;
}

std::uint64_t PrepareUpload::totalBytes() const
{
    std::uint64_t total = 0;
    for (const FileMeta& f : files) {
        if (total + f.size < total)
            return UINT64_MAX; // 10,000 sizes near 2^53 could wrap: "more than any disk"
        total += f.size;
    }
    return total;
}

std::optional<PrepareUpload> parsePrepareUpload(const Json& json, std::string* why)
{
    const auto fail = [&](const char* text) {
        if (why)
            *why = text;
        return std::nullopt;
    };
    if (!json.isObject())
        return fail("not an object");
    PrepareUpload request;
    const std::optional<DeviceInfo> info = deviceFromJson(json["info"]);
    if (!info)
        return fail("no info");
    request.info = *info;
    const Json& files = json["files"];
    if (!files.isObject() || files.toObject().empty())
        return fail("no files");
    if (files.toObject().size() > kMaxFiles)
        return fail("too many files");
    std::set<std::string> ids;
    for (const auto& [key, value] : files.toObject()) {
        if (!value.isObject())
            return fail("a file is not an object");
        FileMeta f;
        f.id = value["id"].isString() ? value["id"].toString() : key;
        if (f.id.empty() || f.id.size() > 128 || !ids.insert(f.id).second)
            return fail("a file id is empty, too long or repeated");
        f.fileName = value["fileName"].toString();
        if (f.fileName.empty() || f.fileName.size() > kMaxNameBytes)
            return fail("a file name is empty or too long");
        const double size = value["size"].toNumber(-1);
        if (!(size >= 0) || size > 9.0e15 || std::floor(size) != size)
            return fail("a file size is not a whole number of bytes");
        f.size = static_cast<std::uint64_t>(size);
        f.fileType = peerText(value["fileType"].toString(), 128);
        f.sha256 = peerText(value["sha256"].toString(), 128);
        // A text message's words go to rows, toasts and the clipboard: UTF-8 here, once.
        f.preview = peerText(value["preview"].toString(), kMaxTextBytes);
        request.files.push_back(std::move(f));
    }
    return request;
}

Json toJson(const PrepareUpload& request)
{
    Json files;
    for (const FileMeta& f : request.files) {
        Json file;
        file.set("id", f.id);
        file.set("fileName", f.fileName);
        file.set("size", static_cast<double>(f.size));
        file.set("fileType", f.fileType.empty() ? std::string("application/octet-stream") : f.fileType);
        if (!f.sha256.empty())
            file.set("sha256", f.sha256);
        if (!f.preview.empty())
            file.set("preview", f.preview);
        files.set(f.id, file);
    }
    Json j;
    j.set("info", toJson(request.info, true));
    j.set("files", files);
    return j;
}

bool isTextMessage(const PrepareUpload& request)
{
    if (request.files.size() != 1)
        return false;
    const FileMeta& f = request.files.front();
    return (f.fileType == "text" || f.fileType.rfind("text/", 0) == 0) && !f.preview.empty();
}

} // namespace localsend
