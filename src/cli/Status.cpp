// status, --version, and ws alone on a console: what is there, never
// starting Win顺 and never waiting for it.

#include "Commands.h"
#include "Encoding.h"
#include "Format.h"
#include "Messages.h"

#include <cmath>

namespace ws::cli {

namespace {

constexpr Text kRunning {L"Win顺 {}：在运行", L"WinShun {}: running"};
constexpr Text kIndexReady {L"索引：就绪，{} 项", L"Index: ready, {} items"};
constexpr Text kIndexLoading {L"索引：正在加载", L"Index: loading"};
constexpr Text kIndexLoadingPercent {L"索引：正在加载，{}%", L"Index: loading, {}%"};
constexpr Text kIndexRefreshing {L"（正在追最近的改动）", L" (catching up with recent changes)"};
constexpr Text kContentOn {L"内容索引：开，{} 个文件", L"Content index: on, {} files"};
constexpr Text kContentPending {L"（还有 {} 个待读）", L" ({} waiting)"};
constexpr Text kContentOff {L"内容索引：关", L"Content index: off"};
constexpr Text kClipboardAllowed {L"剪贴板历史：命令行和 AI 可以读", L"Clipboard history: the command line and AI may read it"};
constexpr Text kClipboardKept {L"剪贴板历史：不给命令行和 AI 读", L"Clipboard history: kept from the command line and AI"};
constexpr Text kNotRunning {L"Win顺 没在运行（用 ws 搜索时会启动它）", L"WinShun is not running (ws starts it when you search)"};
constexpr Text kWinShunVersion {L"Win顺 {}", L"WinShun {}"};
constexpr Text kBareReady {L"Win顺 {} 在运行，索引就绪（{} 项）。", L"WinShun {} is running; its index is ready ({} items)."};
constexpr Text kBareLoading {L"Win顺 {} 在运行，索引还在加载。", L"WinShun {} is running; its index is loading."};

int loadingPercent(const json::Value& index)
{
    double sum = 0;
    int counted = 0;
    for (const json::Value& volume : index["volumes"].toArray()) {
        if (volume["reading"].toBool() && volume["percent"].isNumber()) {
            sum += volume["percent"].toNumber();
            ++counted;
        }
    }
    return counted > 0 ? static_cast<int>(std::lround(sum / counted)) : -1;
}

// Why the pipe is not there (it was tried): the switch, or Win顺 not running.
Failure whyNoPipe(const Context& context)
{
    if (!context.config.connectionOn)
        return {Exit::Off, std::wstring(tr(msg::kSwitchOff))};
    if (context.launcher.running())
        return {Exit::Off, std::wstring(tr(msg::kRunningNoPipe))};
    return {Exit::NotRunning, std::wstring(tr(kNotRunning))};
}

void line(Stream& out, std::wstring_view text)
{
    out.write(text);
    out.newline();
}

} // namespace

Result<Exit> runStatus(Context& context)
{
    const bool json = context.args.has(Opt::Json);
    pipe::LineClient client;
    // Waits for a Win顺 that is on its way up (another ws or the logon autostart
    // has just started it), never starts one: ws status only says what is there.
    const Result<bool> there = connectIfThere(client, connectOptions(context), context.launcher);
    if (!there)
        return there.failure();
    if (!*there) {
        const Failure why = whyNoPipe(context);
        if (json) {
            const bool running = context.launcher.running();
            context.out.writeUtf8(json::write(json::Value::Object {{"running", running}, {"connected", false}}));
            context.out.newline();
        }
        return why;
    }
    Session session(client);
    session.setIdleLimit(Session::kShortIdleMs);
    Result<CallResult> status = session.call("index_status", json::Value::Object {});
    if (!status)
        return std::move(status).failure();
    if (status->isError)
        return toolFailure("index_status", status->errorCode, status->errorText, {});
    const json::Value& s = status->structured;
    const json::Value& index = s["index"];
    const bool ready = index["state"].toString() == "ready";
    Stream& out = context.out;
    if (json) {
        out.setJsonEscapes(true);
        out.writeUtf8(json::write(s));
        out.newline();
        out.setJsonEscapes(false);
        return ready ? Exit::Found : Exit::NotReady;
    }
    line(out, fmt(kRunning, wide(s["version"].toString())));
    std::wstring indexLine;
    if (ready) {
        indexLine = fmt(kIndexReady, thousands(index["items"].toInt()));
    } else {
        const int percent = loadingPercent(index);
        indexLine = percent >= 0 ? fmt(kIndexLoadingPercent, percent) : std::wstring(tr(kIndexLoading));
    }
    if (index["refreshing"].toBool())
        indexLine += tr(kIndexRefreshing);
    line(out, indexLine);
    const json::Value& content = s["content_index"];
    if (content["enabled"].toBool()) {
        std::wstring contentLine = fmt(kContentOn, thousands(content["files"].toInt()));
        if (content["pending"].toInt() > 0)
            contentLine += fmt(kContentPending, thousands(content["pending"].toInt()));
        line(out, contentLine);
    } else {
        line(out, tr(kContentOff));
    }
    line(out, tr(s["ai"]["clipboard_allowed"].toBool() ? kClipboardAllowed : kClipboardKept));
    return ready ? Exit::Found : Exit::NotReady;
}

Result<Exit> runVersion(Context& context)
{
    Stream& out = context.out;
    out.write(L"ws " WINSHUN_VERSION);
    out.newline();
    pipe::LineClient client;
    if (connectIfThere(client, context.pipe)) {
        Session session(client);
        session.setIdleLimit(Session::kShortIdleMs); // a Win顺 that does not answer is not worth waiting for here
        if (Result<std::wstring> version = session.serverVersion(); version && !version->empty()) {
            out.write(fmt(kWinShunVersion, *version));
            out.newline();
        }
    }
    return Exit::Found;
}

Result<Exit> runBare(Context& context)
{
    Stream& out = context.out;
    out.write(shortHelp());
    out.newline();
    pipe::LineClient client;
    if (!connectIfThere(client, context.pipe)) {
        const Failure why = whyNoPipe(context);
        out.write(why.message, Style::Dim);
        out.newline();
        return Exit::Found;
    }
    Session session(client);
    session.setIdleLimit(Session::kShortIdleMs);
    Result<CallResult> status = session.call("index_status", json::Value::Object {});
    if (status && !status->isError) {
        const json::Value& s = status->structured;
        const std::wstring version = wide(s["version"].toString());
        const bool ready = s["index"]["state"].toString() == "ready";
        out.write(ready ? fmt(kBareReady, version, thousands(s["index"]["items"].toInt())) : fmt(kBareLoading, version), Style::Dim);
        out.newline();
    }
    return Exit::Found;
}

} // namespace ws::cli
