#include "Session.h"

#include "Encoding.h"
#include "Interrupt.h"
#include "Messages.h"

namespace ws::cli {

namespace {

// The protocol revision asked for; Win顺 answers with it (McpServer.h).
constexpr const char* kProtocolVersion = "2025-11-25";

std::string requestLine(long long id, std::string_view method, json::Value params)
{
    json::Value::Object message {{"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}, {"params", std::move(params)}};
    return json::write(json::Value(std::move(message))) + '\n';
}

std::string notificationLine(std::string_view method, json::Value params)
{
    json::Value::Object message {{"jsonrpc", "2.0"}, {"method", std::string(method)}};
    if (!params.isNull())
        message.emplace("params", std::move(params));
    return json::write(json::Value(std::move(message))) + '\n';
}

std::string cancelLine(long long id)
{
    return notificationLine("notifications/cancelled", json::Value::Object {{"requestId", id}, {"reason", "Interrupted (Ctrl+C)"}});
}

bool contains(std::wstring_view text, std::wstring_view part)
{
    return text.find(part) != std::wstring_view::npos;
}

} // namespace

Session::Session(pipe::LineClient& client)
    : m_client(client)
{
    m_initializeId = m_nextId++;
    json::Value::Object params {{"protocolVersion", kProtocolVersion}, {"capabilities", json::Value::Object {}},
        {"clientInfo", json::Value::Object {{"name", "ws"}, {"version", WINSHUN_VERSION}}}};
    m_out = requestLine(m_initializeId, "initialize", std::move(params));
    m_out += notificationLine("notifications/initialized", nullptr);
    interrupt::setAction([this] { onInterrupt(); });
}

Session::~Session()
{
    interrupt::clearAction();
}

void Session::onInterrupt()
{
    // On Ctrl+C's thread, perhaps more than once: the cancellations go once,
    // a read the main thread waits in ends every time.
    bool send = false;
    std::string lines;
    {
        const std::lock_guard lock(m_mutex);
        if (!m_cancelSent) {
            m_cancelSent = true;
            send = true;
            for (const long long id : m_open)
                lines += cancelLine(id);
        }
    }
    if (send && !lines.empty())
        m_client.write(lines);
    m_client.interrupt();
}

long long Session::queue(std::string_view tool, json::Value arguments)
{
    const long long id = m_nextId++;
    m_out += requestLine(id, "tools/call", json::Value::Object {{"name", std::string(tool)}, {"arguments", std::move(arguments)}});
    const std::lock_guard lock(m_mutex);
    m_open.insert(id);
    return id;
}

bool Session::flush()
{
    if (m_out.empty())
        return true;
    const bool written = m_client.write(m_out);
    m_out.clear();
    return written;
}

void Session::cancel(long long id)
{
    {
        const std::lock_guard lock(m_mutex);
        if (!m_open.erase(id))
            return;
    }
    m_dropped.insert(id);
    m_early.erase(id);
    m_out += notificationLine("notifications/cancelled", json::Value::Object {{"requestId", id}, {"reason", "Not needed"}});
    flush();
}

Result<json::Value> Session::answer(long long id)
{
    if (!flush())
        return Failure {interrupt::requested() ? Exit::Interrupted : Exit::Failed, std::wstring(tr(msg::kBroken))};
    if (const auto it = m_early.find(id); it != m_early.end()) {
        json::Value found = std::move(it->second);
        m_early.erase(it);
        return found;
    }
    std::string line;
    for (;;) {
        if (interrupt::requested())
            return Failure {Exit::Interrupted, {}};
        if (!m_client.readLine(line)) {
            if (interrupt::requested())
                return Failure {Exit::Interrupted, {}};
            return Failure {Exit::Failed, std::wstring(tr(msg::kBroken))};
        }
        std::optional<json::Value> message = json::parse(line);
        if (!message || !message->isObject() || !message->has("id") || !(*message)["id"].isNumber())
            continue; // a notification (tools/list_changed…), or not ours
        const long long got = (*message)["id"].toInt();
        {
            const std::lock_guard lock(m_mutex);
            m_open.erase(got);
        }
        if (got == id)
            return std::move(*message);
        if (m_dropped.count(got))
            continue;
        m_early.emplace(got, std::move(*message));
    }
}

Result<CallResult> Session::wait(long long id)
{
    // initialize's answer comes first; a refusal there is the session's.
    if (m_initializeId != 0) {
        Result<std::wstring> version = serverVersion();
        if (!version)
            return std::move(version).failure();
    }
    Result<json::Value> response = answer(id);
    if (!response)
        return std::move(response).failure();
    const json::Value& message = *response;
    CallResult out;
    if (message.has("error")) { // the request itself refused: "Unknown tool: …"
        out.isError = true;
        out.errorText = wide(message["error"]["message"].toString());
        return out;
    }
    const json::Value& result = message["result"];
    out.isError = result["isError"].toBool();
    if (out.isError) {
        out.errorCode = result["structuredContent"]["error"].toString();
        for (const json::Value& part : result["content"].toArray()) {
            if (part["type"].toString() == "text") {
                out.errorText = wide(part["text"].toString());
                break;
            }
        }
        return out;
    }
    if (!result["structuredContent"].isObject())
        return Failure {Exit::Failed, std::wstring(tr(msg::kNoResult))};
    out.structured = result["structuredContent"];
    return out;
}

Result<CallResult> Session::call(std::string_view tool, json::Value arguments)
{
    const long long id = queue(tool, std::move(arguments));
    return wait(id);
}

Result<std::wstring> Session::serverVersion()
{
    if (m_initializeId == 0)
        return m_version;
    const long long id = m_initializeId;
    Result<json::Value> response = answer(id);
    m_initializeId = 0;
    if (!response)
        return std::move(response).failure();
    if ((*response).has("error"))
        return Failure {Exit::Failed, fmt(msg::kServerError, wide((*response)["error"]["message"].toString()))};
    m_version = wide((*response)["result"]["serverInfo"]["version"].toString());
    return m_version;
}

namespace {

// The path Win顺's words end with ("…: D:\x"), else nothing.
std::wstring pathAtEnd(std::wstring_view text)
{
    const std::size_t colon = text.rfind(L": ");
    if (colon == std::wstring_view::npos)
        return {};
    std::wstring_view path = text.substr(colon + 2);
    while (!path.empty() && (path.back() == L'.' || path.back() == L' '))
        path.remove_suffix(1);
    const bool drive = path.size() >= 2 && path[1] == L':' && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'));
    const bool share = path.size() > 2 && path[0] == L'\\' && path[1] == L'\\';
    return drive || share ? std::wstring(path) : std::wstring();
}

bool clipboardTool(std::string_view tool)
{
    return tool == "clipboard_history" || tool == "clipboard_get";
}

} // namespace

Failure toolFailure(std::string_view tool, std::string_view errorCode, const std::wstring& text, std::wstring_view subject)
{
    const std::wstring about = subject.empty() ? pathAtEnd(text) : std::wstring(subject);
    // A Win顺 made for ws says what kind of refusal it is.
    if (errorCode == "not_found")
        return {Exit::NotVisible, fmt(contains(text, L"not in the index") ? msg::kNotIndexed : msg::kNotVisible, about)};
    if (errorCode == "off")
        return {Exit::Off, clipboardTool(tool) ? std::wstring(tr(msg::kClipOff)) : fmt(msg::kTurnedOff, wide(tool))};
    if (errorCode == "too_large")
        return {Exit::Failed, std::wstring(tr(msg::kAnswerTooLarge))};
    // An older one only in words.
    if (contains(text, L"Unknown tool")) {
        // A tool not offered: the clipboard history while Win顺 keeps none.
        if (tool == "clipboard_history")
            return {Exit::Off, std::wstring(tr(msg::kClipUnavailable))};
        return {Exit::Failed, fmt(msg::kUnknownTool, wide(tool))};
    }
    if (contains(text, L"There is no such file") || contains(text, L"not visible to AI"))
        return {Exit::NotVisible, fmt(msg::kNotVisible, about)};
    if (contains(text, L"is not in the index"))
        return {Exit::NotVisible, fmt(msg::kNotIndexed, about)};
    if (contains(text, L"is a file, not a folder"))
        return {Exit::Usage, fmt(msg::kIsFile, about)};
    if (contains(text, L"This is a folder"))
        return {Exit::Usage, fmt(msg::kIsFolder, about)};
    if (contains(text, L"content search is turned off"))
        return {Exit::Failed, std::wstring(tr(msg::kContentOff))};
    if (contains(text, L"turned off") || contains(text, L"is off:")) {
        if (clipboardTool(tool))
            return {Exit::Off, std::wstring(tr(msg::kClipOff))};
        return {Exit::Off, fmt(msg::kTurnedOff, wide(tool))};
    }
    if (contains(text, L"not a text file or a document"))
        return {Exit::Failed, fmt(msg::kNotText, about)};
    if (contains(text, L"larger than"))
        return {Exit::Failed, fmt(msg::kTooLarge, about)};
    if (contains(text, L"cloud file"))
        return {Exit::Failed, fmt(msg::kCloudOnly, about)};
    if (contains(text, L"could not be read"))
        return {Exit::Failed, fmt(msg::kUnreadable, about)};
    return {Exit::Failed, fmt(msg::kServerError, text)};
}

} // namespace ws::cli
