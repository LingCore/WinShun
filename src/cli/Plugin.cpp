// plugin: Win顺's plugins (GitHub #251): the ones installed, the library's,
// installing one, its switch, removing it. Win顺 does the work and words the
// answer (its plugins_* tools, kept for ws); ws picks the tool, prints, and
// maps the answer's state to an exit code.

#include "Commands.h"
#include "Encoding.h"
#include "Format.h"
#include "Messages.h"
#include "Win.h"

#include <algorithm>
#include <array>

namespace ws::cli {

namespace {

constexpr Text kUsage {
    L"ws plugin 后面要一个动作：list、search [词]、install <插件包、文件夹或 id>、enable <id>、disable <id>、remove <id>；ws help plugin 看说明",
    L"ws plugin needs an action: list, search [words], install <package, folder or id>, enable <id>, disable <id>, remove <id>; see ws help plugin"};
constexpr Text kNoAction {L"ws plugin 没有 {} 这个动作；有 list、search、install、enable、disable、remove",
    L"ws plugin has no action {}; there are list, search, install, enable, disable and remove"};
constexpr Text kNeedsOne {L"ws plugin {} 要一个{}，比如 ws plugin {} {}", L"ws plugin {} needs one {}, such as ws plugin {} {}"};
constexpr Text kTakesNothing {L"ws plugin list 不接参数；在插件库里找用 ws plugin search", L"ws plugin list takes no words; ws plugin search looks in the library"};
constexpr Text kNoPlugins {L"没有符合的插件", L"No plugin matches"};
constexpr Text kWhatTarget {L"插件包、文件夹或插件库里的 id", L"package, folder or id of the library"};
constexpr Text kWhatId {L"插件的 id", L"plugin id"};

// How a plugin is, in a word (the answers' `state`).
constexpr std::pair<std::string_view, Text> kStates[] = {
    {"on", {L"开着", L"on"}},
    {"off", {L"关着", L"off"}},
    {"waiting", {L"等你同意", L"waiting"}},
    {"unusable", {L"用不了", L"unusable"}},
    // The library's.
    {"install", {L"没装", L"-"}},
    {"update", {L"可更新", L"update"}},
    {"installed", {L"已装", L"installed"}},
    {"bundled", {L"自带", L"bundled"}},
    {"needs-newer-winshun", {L"要更新 Win顺", L"needs a newer WinShun"}},
};
constexpr std::pair<std::string_view, Text> kReviews[] = {
    {"official", {L"官方", L"official"}},
    {"reviewed", {L"已审核", L"reviewed"}},
    {"unreviewed-native", {L"未审核", L"unreviewed"}},
};

std::wstring worded(std::span<const std::pair<std::string_view, Text>> words, const std::string& name)
{
    const auto it = std::ranges::find(words, std::string_view(name), [](const auto& word) { return word.first; });
    return it == words.end() ? std::wstring() : std::wstring(tr(it->second));
}

// Columns lined up (wide characters count two); the last one runs free.
void writeTable(Stream& out, const std::vector<std::vector<std::wstring>>& rows)
{
    std::vector<std::size_t> widths;
    for (const auto& row : rows) {
        widths.resize(std::max(widths.size(), row.size()));
        for (std::size_t i = 0; i + 1 < row.size(); ++i)
            widths[i] = std::max(widths[i], displayWidth(row[i]));
    }
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            const bool last = i + 1 == row.size();
            out.write(row[i], i == 0 ? Style::Name : last ? Style::Dim : Style::Plain);
            if (!last)
                out.write(std::wstring(widths[i] - displayWidth(row[i]) + 2, L' '));
        }
        out.newline();
    }
}

// One line however a description was written.
std::wstring oneLine(std::wstring text)
{
    std::ranges::replace_if(text, [](wchar_t c) { return c < 0x20; }, L' ');
    return text;
}

struct Asked {
    std::string_view tool;
    json::Value::Object arguments;
    bool lists = false; // the answer is a list of plugins
    bool library = false; // ... of the library's
};

Result<Asked> asked(const Parsed& args)
{
    if (args.words.empty())
        return Failure {Exit::Usage, std::wstring(tr(kUsage))};
    const std::wstring action = lowered(args.words.front());
    const std::span<const std::wstring> rest = std::span(args.words).subspan(1);
    const auto one = [&](const Text& what, std::wstring_view example) -> Result<std::wstring> {
        if (rest.size() != 1)
            return Failure {Exit::Usage, fmt(kNeedsOne, action, tr(what), action, example)};
        return rest.front();
    };
    if (action == L"list") {
        if (!rest.empty())
            return Failure {Exit::Usage, std::wstring(tr(kTakesNothing))};
        return Asked {"plugins_list", {}, true};
    }
    if (action == L"search") {
        std::wstring query;
        for (const std::wstring& word : rest)
            query += (query.empty() ? L"" : L" ") + word;
        return Asked {"plugins_search", {{"query", utf8(query)}}, true, true};
    }
    if (action == L"install") {
        Result<std::wstring> target = one(kWhatTarget, L"my-plugin.wsplugin");
        if (!target)
            return std::move(target).failure();
        if (Result<void> clean = noQuoteIn(*target); !clean)
            return std::move(clean).failure();
        // What is there as a file or a folder is that; anything else is an id of the library's.
        const std::wstring path = absolutePath(*target);
        if (pathKind(path) != PathKind::None)
            return Asked {"plugins_install", {{"path", utf8(path)}}};
        return Asked {"plugins_install", {{"id", utf8(*target)}}};
    }
    if (action == L"enable" || action == L"disable") {
        Result<std::wstring> id = one(kWhatId, L"winshun.tools");
        if (!id)
            return std::move(id).failure();
        return Asked {"plugins_switch", {{"id", utf8(*id)}, {"on", action == L"enable"}}};
    }
    if (action == L"remove") {
        Result<std::wstring> id = one(kWhatId, L"me.my-plugin");
        if (!id)
            return std::move(id).failure();
        return Asked {"plugins_remove", {{"id", utf8(*id)}}};
    }
    return Failure {Exit::Usage, fmt(kNoAction, args.words.front())};
}

std::vector<std::wstring> installedRow(const json::Value& plugin)
{
    std::wstring name = wide(plugin["name"].toString());
    if (const std::string error = plugin["error"].toString(); !error.empty())
        name += L"  " + oneLine(wide(error));
    return {wide(plugin["id"].toString()), wide(plugin["version"].toString()), worded(kStates, plugin["state"].toString()), std::move(name)};
}

std::vector<std::wstring> libraryRow(const json::Value& plugin)
{
    std::wstring state = worded(kStates, plugin["state"].toString());
    if (const std::string installed = plugin["installed"].toString(); !installed.empty() && plugin["state"].toString() == "update")
        state += L" (" + wide(installed) + L")";
    std::wstring about = wide(plugin["name"].toString());
    if (const std::string description = plugin["description"].toString(); !description.empty())
        about += (language() == Lang::Zh ? L"：" : L": ") + oneLine(wide(description));
    return {wide(plugin["id"].toString()), wide(plugin["version"].toString()), std::move(state), worded(kReviews, plugin["review"].toString()),
        std::move(about)};
}

} // namespace

Result<Exit> runPlugin(Context& context)
{
    Result<Asked> request = asked(context.args);
    if (!request)
        return std::move(request).failure();
    Result<std::unique_ptr<Connection>> connection = connect(context);
    if (!connection)
        return std::move(connection).failure();
    Result<json::Value> answer = callTool(context, **connection, request->tool, json::Value(std::move(request->arguments)), false);
    if (!answer)
        return std::move(answer).failure();

    const std::string state = (*answer)["state"].toString();
    const std::wstring message = wide((*answer)["message"].toString());
    Stream& out = context.out;
    if (context.args.has(Opt::Json)) {
        out.setJsonEscapes(true);
        out.writeUtf8(json::write(request->lists && state != "failed" ? (*answer)["plugins"] : *answer));
        out.newline();
        out.setJsonEscapes(false);
    }
    if (state == "failed")
        return Failure {(*answer)["code"].toString() == "not_found" ? Exit::NotVisible : Exit::Failed, message};
    if (request->lists) {
        const json::Value::Array& plugins = (*answer)["plugins"].toArray();
        if (!context.args.has(Opt::Json)) {
            std::vector<std::vector<std::wstring>> rows;
            for (const json::Value& plugin : plugins)
                rows.push_back(request->library ? libraryRow(plugin) : installedRow(plugin));
            writeTable(out, rows);
        }
        if (!message.empty()) // the list is the copy kept: the library could not be reached
            context.note(message);
        if (plugins.empty())
            context.note(tr(kNoPlugins));
        return plugins.empty() ? Exit::NothingFound : Exit::Found;
    }
    if (!context.args.has(Opt::Json)) {
        out.write(message);
        out.newline();
    }
    // Asking: Win顺 has a question up for the user, and nothing is done until they answer it.
    return state == "asking" ? Exit::NothingFound : Exit::Found;
}

} // namespace ws::cli
