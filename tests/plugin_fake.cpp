// plugin_fake.exe: a plugin of every kind for tst_plugin and wsbench --plugin.
// No Qt, static C runtime (it is started by WinShunPluginHost.exe as any
// plugin is). The first argument is the role, the rest its options:
//
//   --once [--n N] [--text] [--gbk] [--fail] [--slow MS] [--long BYTES] [--echo-env] [--action] [--rerun S]
//       A once plugin: reads WS_* and prints JSON items (or plain text,
//       or N items, or a line BYTES long, or exits 3, or the argv, its own
//       path and the environment it got, or the effects of WS_ACTION, with
//       WS_TRIGGER and WS_FILES when files came); asks to be run again
//       after S seconds with --rerun.
//
// Started as python.exe is (-u -X utf8 main.py first), it skips those.
//   --serve [--partial] [--slow MS] [--crash N] [--hang] [--tool NAME] [--error] [--chatty] [--rerun S] [--toast] [--late N]
//           [--refresh MS [--refresh-burst K] [--refresh-first]]
//       A resident plugin: JSON-RPC on stdio. Answers initialize, query
//       (two query/partial first with --partial; after MS; never with
//       --hang; exits 3 on the Nth query with --crash; with an error with
//       --error; after a winshun/tool call of NAME, whose outcome goes in
//       the subtitle, with --tool; with winshun/log, notify and progress
//       before it with --chatty; with "rerun": S and the query's number in
//       data.n with --rerun; after a toast "t:<text>" with --toast (its click
//       asks for copy, two setQuery, a link, a folder, a notify); the
//       first answer followed MS later by K winshun/refresh (1 without
//       --refresh-burst), or preceded by one with --refresh-first), cancel
//       (counted), action (the files and trigger of a `files` one in its
//       notify), settingsChanged (remembered), shutdown.
//   --probe
//       A once plugin whose one item carries the facts about its process
//       (token, job, mitigations, may it start a process, read the
//       clipboard, write where) in `data`.
//   --noop
//       Exits at once: what --probe tries to start.

#include "Json.h"
#include "Lite.h"
#include "Sandbox.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ws::plugin;

namespace {

std::vector<std::wstring> g_args;
std::vector<std::wstring> g_lead; // before the role, when it plays python.exe: -u -X utf8 main.py

bool has(const wchar_t* option)
{
    for (const std::wstring& a : g_args)
        if (a == option)
            return true;
    return false;
}

std::wstring value(const wchar_t* option, const wchar_t* fallback = L"")
{
    for (std::size_t i = 0; i + 1 < g_args.size(); ++i)
        if (g_args[i] == option)
            return g_args[i + 1];
    return fallback;
}

std::string env(const wchar_t* name)
{
    return lite::utf8(lite::environmentVariable(name));
}

void out(const std::string& line)
{
    lite::writeAll(::GetStdHandle(STD_OUTPUT_HANDLE), line + '\n');
}

void err(const std::string& line)
{
    lite::writeAll(::GetStdHandle(STD_ERROR_HANDLE), line + '\n');
}

json::Value::Object item(const std::string& id, const std::string& title, const std::string& subtitle)
{
    return {{"id", id}, {"title", title}, {"subtitle", subtitle}};
}

json::Value facts()
{
    sandbox::FactsOptions options;
    options.spawnExe = lite::ownPath();
    options.clipboard = true;
    options.parentMark = L"PLUGIN_TEST_PARENT_MARK";
    const std::wstring dataDir = lite::environmentVariable(L"WS_DATA_DIR");
    if (!dataDir.empty())
        options.writeDirs.push_back(dataDir);
    const std::wstring profile = lite::environmentVariable(L"USERPROFILE");
    if (!profile.empty())
        options.writeDirs.push_back(profile);
    return sandbox::collectFacts(options);
}

int onceMain()
{
    if (has(L"--slow"))
        ::Sleep(static_cast<DWORD>(std::wcstoul(value(L"--slow").c_str(), nullptr, 10)));
    if (has(L"--fail")) {
        err("something went wrong: 出错了");
        return 3;
    }
    if (has(L"--long")) {
        const std::size_t n = std::strtoull(lite::utf8(value(L"--long")).c_str(), nullptr, 10);
        std::string line(n, 'x');
        out(line);
        return 0;
    }
    const std::string query = env(L"WS_QUERY");
    if (has(L"--text")) {
        out("first line " + query);
        out("");
        out("第二行 " + query);
        return 0;
    }
    if (has(L"--gbk")) {
        // What a script that writes in the OEM code page produces (Windows
        // PowerShell 5.1 redirected, say): the bytes of the OEM code page.
        const std::wstring text = L"中文结果 Résumé " + lite::wide(query);
        const int n = ::WideCharToMultiByte(CP_OEMCP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string bytes(static_cast<std::size_t>(n), '\0');
        ::WideCharToMultiByte(CP_OEMCP, 0, text.data(), static_cast<int>(text.size()), bytes.data(), n, nullptr, nullptr);
        out("{\"items\":[{\"title\":" + json::quote(bytes) + "}]}");
        return 0;
    }
    if (has(L"--echo-env")) {
        json::Value::Array argv;
        for (const std::wstring& a : g_args)
            argv.push_back(lite::utf8(a));
        json::Value::Array lead;
        for (const std::wstring& a : g_lead)
            lead.push_back(lite::utf8(a));
        json::Value::Object data;
        data["argv"] = std::move(argv);
        data["lead"] = std::move(lead);
        data["self"] = lite::utf8(lite::ownPath());
        for (const wchar_t* name : {L"WS_QUERY", L"WS_TEXT", L"WS_COMMAND", L"WS_TRIGGER", L"WS_KEYWORD", L"WS_MATCH", L"WS_PLUGIN_DIR",
                 L"WS_DATA_DIR", L"WS_SETTINGS", L"WS_FILTERS", L"WS_LOCALE", L"WS_THEME", L"WS_VERSION", L"PYTHONIOENCODING", L"FAKE_EXTRA",
                 L"__PYVENV_LAUNCHER__"})
            data[lite::utf8(name)] = env(name);
        json::Value::Object one = item("env", "env", "");
        one["data"] = json::Value(std::move(data));
        out(json::write(json::Value(json::Value::Object {{"items", json::Value::Array {json::Value(std::move(one))}}})));
        return 0;
    }
    if (has(L"--action")) {
        const std::string action = env(L"WS_ACTION");
        const auto item = json::parse(env(L"WS_ITEM"));
        const std::string title = item ? (*item)["title"].toString() : "?";
        const std::string files = env(L"WS_FILES");
        const std::string said = files.empty() ? "did " + action + " on " + title : "files " + env(L"WS_TRIGGER") + ": " + files;
        out(json::write(json::Value(json::Value::Object {{"effects",
            json::Value::Array {json::Value(json::Value::Object {{"type", "notify"}, {"text", said}}),
                json::Value(json::Value::Object {{"type", "hide"}})}}})));
        return 0;
    }
    int n = 1;
    if (has(L"--n"))
        n = std::max(1, _wtoi(value(L"--n").c_str()));
    json::Value::Array items;
    for (int i = 0; i < n; ++i) {
        json::Value::Object one = item("once-" + std::to_string(i), "once: " + query + (n > 1 ? " #" + std::to_string(i) : ""),
            env(L"WS_COMMAND") + "/" + env(L"WS_TRIGGER"));
        one["score"] = 70;
        one["open"] = json::Value::Object {{"url", "https://example.com/?q=" + query}};
        items.push_back(json::Value(std::move(one)));
    }
    err("once plugin answered 已回答");
    const double rerun = has(L"--rerun") ? std::wcstod(value(L"--rerun").c_str(), nullptr) : 0.0;
    out(json::write(json::Value(json::Value::Object {{"items", std::move(items)}, {"rerun", rerun}})));
    return 0;
}

int probeMain()
{
    json::Value::Object one = item("probe", "probe", "");
    one["data"] = facts();
    out(json::write(json::Value(json::Value::Object {{"items", json::Value::Array {json::Value(std::move(one))}}})));
    return 0;
}

// ---- the resident plugin ---------------------------------------------------

struct Server {
    int queries = 0;
    int cancelled = 0;
    json::Value settings;
    long long nextId = 1;
    lite::LineReader reader {::GetStdHandle(STD_INPUT_HANDLE)};

    void send(const json::Value::Object& message)
    {
        json::Value::Object m = message;
        m["jsonrpc"] = "2.0";
        out(json::write(json::Value(std::move(m))));
    }
    void respond(const json::Value& id, json::Value result) { send({{"id", id}, {"result", std::move(result)}}); }
    void fail(const json::Value& id, int code, const std::string& text)
    {
        send({{"id", id}, {"error", json::Value::Object {{"code", code}, {"message", text}}}});
    }
    void notify(const std::string& method, json::Value::Object params)
    {
        send({{"method", method}, {"params", json::Value(std::move(params))}});
    }

    // A request of ours: waits for its response (other lines meanwhile are
    // handled, but a plugin that simple need not).
    json::Value call(const std::string& method, json::Value::Object params)
    {
        const long long id = nextId++;
        send({{"id", id}, {"method", method}, {"params", json::Value(std::move(params))}});
        std::string line;
        while (reader.next(line)) {
            const auto message = json::parse(line);
            if (!message)
                continue;
            if ((*message)["id"].isNumber() && (*message)["id"].toInt() == id && !(*message).has("method"))
                return *message;
        }
        return json::Value();
    }

    int run()
    {
        std::string line;
        while (reader.next(line)) {
            const auto message = json::parse(line);
            if (!message || !message->isObject()) {
                err("bad line: " + line);
                continue;
            }
            const json::Value& m = *message;
            const std::string method = m["method"].toString();
            const json::Value id = m["id"];
            const json::Value params = m["params"];
            if (method == "initialize") {
                settings = params["settings"];
                respond(id, json::Value::Object {{"protocolVersion", params["protocolVersion"].toString()}, {"name", "fake"}});
            } else if (method == "query") {
                ++queries;
                if (has(L"--crash") && queries == _wtoi(value(L"--crash").c_str())) {
                    err("crashing on purpose 故意崩溃");
                    return 3;
                }
                if (has(L"--hang"))
                    continue;
                if (has(L"--error")) {
                    fail(id, -32000, "cannot do that 做不到");
                    continue;
                }
                const long long queryId = params["queryId"].toInt();
                const std::string text = params["text"].toString();
                if (has(L"--chatty")) {
                    notify("winshun/log", {{"level", "info"}, {"text", "query " + text}});
                    notify("winshun/notify", {{"text", "hello from fake 你好"}});
                    notify("winshun/progress", {{"queryId", queryId}, {"text", "working 正在查"}});
                }
                if (has(L"--partial")) {
                    for (int i = 0; i < 2; ++i) {
                        json::Value::Object partial = item("p" + std::to_string(i), "partial " + std::to_string(i) + ": " + text, "");
                        notify("query/partial", {{"queryId", queryId}, {"items", json::Value::Array {json::Value(std::move(partial))}},
                                                    {"replace", false}});
                    }
                }
                if (has(L"--slow"))
                    ::Sleep(static_cast<DWORD>(std::wcstoul(value(L"--slow").c_str(), nullptr, 10)));
                if (has(L"--late") && queries == _wtoi(value(L"--late").c_str()))
                    ::Sleep(600); // the Nth answer comes after the test's timeoutMs
                std::string subtitle = params["command"].toString() + "/" + params["trigger"].toString();
                if (has(L"--tool")) {
                    const std::string tool = lite::utf8(value(L"--tool"));
                    const json::Value reply = call("winshun/tool", {{"name", tool}, {"arguments", json::Value::Object {{"query", text}}}});
                    if (reply.has("error"))
                        subtitle = "tool error " + std::to_string(reply["error"]["code"].toInt());
                    else
                        subtitle = "tool ok " + json::write(reply["result"]);
                }
                // A stable id per text, as a real plugin would give (an AI's
                // _run tool lists again and looks the id up).
                json::Value::Object one = item("q-" + text, "serve: " + text, subtitle);
                one["score"] = 80;
                one["open"] = json::Value::Object {{"url", "https://example.com/?q=" + text}};
                if (!text.empty() && text[0] == '#') // a colour (a regex trigger's push): shown as a swatch
                    one["icon"] = "color:" + text;
                one["data"] = json::Value::Object {{"cancelled", cancelled}, {"settings", settings}, {"filters", params["filters"]},
                    {"pid", static_cast<unsigned long>(::GetCurrentProcessId())}};
                if (has(L"--toast")) {
                    // A click asking for everything: Win顺 keeps only its own
                    // list ("fk "), the local folder and (with clipboard:write) the copy.
                    json::Value::Array click {json::Value(json::Value::Object {{"type", "copy"}, {"text", text}}),
                        json::Value(json::Value::Object {{"type", "setQuery"}, {"text", "fk " + text}}),
                        json::Value(json::Value::Object {{"type", "setQuery"}, {"text", "zz " + text}}),
                        json::Value(json::Value::Object {{"type", "open"}, {"url", "https://example.com/"}}),
                        json::Value(json::Value::Object {{"type", "open"}, {"path", "C:\\Windows"}}),
                        json::Value(json::Value::Object {{"type", "notify"}, {"text", "n"}})};
                    notify("winshun/notify", {{"toast", true}, {"title", "t:" + text}, {"text", "body " + text}, {"click", std::move(click)}});
                }
                json::Value::Object result;
                if (has(L"--rerun")) {
                    one["data"] = json::Value::Object {{"n", queries}};
                    result["rerun"] = std::wcstod(value(L"--rerun").c_str(), nullptr);
                }
                result["items"] = json::Value::Array {json::Value(std::move(one))};
                const bool refreshing = has(L"--refresh") && queries == 1;
                if (refreshing && has(L"--refresh-first"))
                    notify("winshun/refresh", {}); // while this query is still being answered
                respond(id, json::Value(std::move(result)));
                if (refreshing && !has(L"--refresh-first")) {
                    ::Sleep(static_cast<DWORD>(std::wcstoul(value(L"--refresh").c_str(), nullptr, 10)));
                    const int burst = has(L"--refresh-burst") ? std::max(1, _wtoi(value(L"--refresh-burst").c_str())) : 1;
                    for (int i = 0; i < burst; ++i)
                        notify("winshun/refresh", {});
                }
            } else if (method == "cancel") {
                ++cancelled;
            } else if (method == "action") {
                const std::string action = params["action"].toString();
                const std::string title = params["item"]["title"].toString();
                std::string said = "did " + action + " on " + title;
                if (params.has("files")) {
                    said = "files " + params["trigger"].toString() + ":";
                    for (const json::Value& file : params["files"].toArray())
                        said += " " + file.toString();
                }
                respond(id, json::Value::Object {{"effects",
                    json::Value::Array {json::Value(json::Value::Object {{"type", "notify"}, {"text", said}}),
                        json::Value(json::Value::Object {{"type", "hide"}})}}});
            } else if (method == "settingsChanged") {
                settings = params["settings"];
            } else if (method == "shutdown") {
                respond(id, json::Value::Object {});
                return 0;
            } else if (!id.isNull()) {
                fail(id, -32601, "Method not found: " + method);
            }
        }
        return 0; // stdin closed
    }
};

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // Playing python.exe (tst_plugin's Python cases), it gets python's
    // options and the script before the role: -u -X utf8 main.py --once ...
    int first = 1;
    while (first < argc && argv[first][0] == L'-' && argv[first][1] != L'-') {
        g_lead.emplace_back(argv[first]);
        if (std::wcscmp(argv[first], L"-X") == 0 && first + 1 < argc)
            g_lead.emplace_back(argv[++first]);
        ++first;
    }
    if (first < argc) {
        const std::wstring script = argv[first];
        if (script.size() > 3 && script.compare(script.size() - 3, 3, L".py") == 0) {
            g_lead.push_back(script);
            ++first;
        }
    }
    for (int i = first + 1; i < argc; ++i)
        g_args.emplace_back(argv[i]);
    const std::wstring role = first < argc ? argv[first] : L"";
    if (role == L"--once")
        return onceMain();
    if (role == L"--probe")
        return probeMain();
    if (role == L"--serve")
        return Server().run();
    if (role == L"--noop")
        return 0;
    err("usage: plugin_fake --once | --serve | --probe | --noop [options]");
    return 1;
}
