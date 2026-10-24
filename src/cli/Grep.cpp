// grep: the text inside files, documents too.

#include "Commands.h"
#include "Encoding.h"
#include "Format.h"
#include "Interrupt.h"
#include "Messages.h"
#include "Print.h"

#include <set>

namespace ws::cli {

namespace {

constexpr long long kMostContent = 2'000; // a newer Win顺's for the command line
constexpr std::size_t kOlderMost = 100; // search_content of a Win顺 before the command line

// path:LOCATION:snippet; LOCATION as an editor jumps to it for text files
// (the line), else p3 (page), s2 (slide), [Sheet]r12 (row), nothing (the
// document as a whole).
std::wstring placeOf(const json::Value& location)
{
    const std::string& kind = location["kind"].toString();
    if (kind == "line" || (kind.empty() && location["line"].isNumber()))
        return std::to_wstring(location["line"].toInt());
    std::wstring place;
    if (location["sheet"].isString())
        place = L"[" + wide(location["sheet"].toString()) + L"]";
    const std::wstring number = location["number"].isNumber() ? std::to_wstring(location["number"].toInt()) : std::wstring();
    if (kind == "page")
        place += L"p" + number;
    else if (kind == "slide")
        place += L"s" + number;
    else if (kind == "row")
        place += L"r" + number;
    return place;
}

} // namespace

Result<Exit> runGrep(Context& context)
{
    const Parsed& args = context.args;
    std::wstring query;
    for (const std::wstring& word : args.words) {
        if (!query.empty())
            query += L' ';
        query += word;
    }
    if (query.empty())
        return Failure {Exit::Usage, fmt(msg::kNeedsQuery, context.command.name, context.command.name)};
    Result<long long> limit = limitOf(args, kMostContent);
    if (!limit)
        return std::move(limit).failure();
    Result<long long> timeout = durationOption(args, Opt::Timeout, 0);
    if (!timeout)
        return std::move(timeout).failure();
    Result<std::vector<std::wstring>> folders = inFolders(args);
    if (!folders)
        return std::move(folders).failure();

    json::Value::Object arguments {{"query", utf8(query)}, {"limit", *limit}};
    if (*timeout > 0)
        arguments.emplace("timeout_ms", *timeout);
    if (!folders->empty()) {
        json::Value::Array list;
        for (const std::wstring& folder : *folders)
            list.emplace_back(utf8(folder));
        arguments.emplace("folders", std::move(list));
    }
    Result<std::unique_ptr<Connection>> connection = connect(context);
    if (!connection)
        return std::move(connection).failure();
    if (!folders->empty()) {
        if (Result<void> made = requireCliContract(**connection, L"--in"); !made)
            return std::move(made).failure();
    }
    Result<json::Value> result = callTool(context, **connection, "search_content", json::Value(std::move(arguments)), true,
        folders->empty() ? std::wstring_view() : std::wstring_view(folders->front()));
    if (!result)
        return std::move(result).failure();
    // Another content search had the engine for this one's whole time: nothing was searched, which
    // is not "nothing found" (and an older Win顺 does not say so: it reads as a search that ran out of time).
    if ((*result)["queued"].toBool())
        return Failure {Exit::Failed, std::wstring(tr(msg::kQueuedOut))};
    const json::Value::Array& items = (*result)["items"].toArray();

    Stream& out = context.out;
    const auto stopped = [&] { return out.broken() || interrupt::requested(); };
    if (args.has(Opt::Json)) {
        json::Value::Array list;
        list.reserve(items.size());
        for (const json::Value& item : items)
            list.push_back(jsonItem(item, true));
        out.setJsonEscapes(true);
        out.writeUtf8(json::write(json::Value(std::move(list))));
        out.newline();
        out.setJsonEscapes(false);
    } else if (args.has(Opt::Null)) {
        std::set<std::string> seen; // a file once, however many places matched
        for (const json::Value& item : items) {
            if (!seen.insert(item["path"].toString()).second)
                continue;
            out.write(wide(item["path"].toString()));
            out.nul();
            if (stopped())
                break;
        }
    } else {
        const std::vector<std::wstring> terms = queryTerms(query);
        for (const json::Value& item : items) {
            out.write(wide(item["path"].toString()), Style::GrepPath);
            out.write(L":");
            out.write(placeOf(item["location"]), Style::GrepPlace);
            out.write(L":");
            writeMarked(out, wide(item["snippet"].toString()), terms, Style::Plain, Style::Match);
            out.newline();
            if (stopped())
                break;
        }
    }
    if (interrupt::requested())
        return Failure {Exit::Interrupted, {}};
    if (!(*result)["complete"].toBool(true))
        context.note(tr(msg::kIncomplete));
    // An older Win顺 says nothing of more: a full page means there likely is.
    else if (const bool made = madeForWs(**connection); made ? (*result)["truncated"].toBool()
            : static_cast<long long>(items.size()) >= *limit || (*limit > kOlderMost && items.size() == kOlderMost))
        noteTruncated(context, items.size(), *limit, made);
    return items.empty() ? Exit::NothingFound : Exit::Found;
}

} // namespace ws::cli
