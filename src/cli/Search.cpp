// find, open, recent, ls: names from the index.

#include "Commands.h"
#include "Encoding.h"
#include "Format.h"
#include "Interrupt.h"
#include "Messages.h"
#include "Open.h"
#include "Print.h"
#include "Win.h"

#include <algorithm>

namespace ws::cli {

namespace {

constexpr long long kMostFiles = 50'000; // a newer Win顺 gives the command line this many (older ones 200)
constexpr long long kMostFolder = 50'000; // ls (older ones 500)
constexpr std::size_t kOlderMost = 200; // recent_files of a Win顺 before the command line

constexpr std::wstring_view kSorts[] = {L"match", L"newest", L"oldest", L"name", L"folder", L"type", L"largest", L"smallest"};

// --sort as search_files names it.
std::string serverSort(std::wstring_view sort)
{
    if (sort == L"match")
        return "relevance";
    if (sort == L"newest")
        return "modified";
    return utf8(sort);
}

json::Value::Array pathList(const std::vector<std::wstring>& paths)
{
    json::Value::Array list;
    for (const std::wstring& path : paths)
        list.emplace_back(utf8(path));
    return list;
}

std::wstring joinedWords(const Parsed& args)
{
    std::wstring joined;
    for (const std::wstring& word : args.words) {
        if (!joined.empty())
            joined += L' ';
        joined += word;
    }
    return joined;
}

// What a search_files call for find or open asks.
struct FindRequest {
    std::wstring query;
    json::Value::Object arguments;
    std::vector<std::wstring> folders;
};

Result<FindRequest> findRequest(const Parsed& args, std::wstring_view command, long long limit)
{
    FindRequest request;
    request.query = buildQuery(args);
    if (request.query.empty())
        return Failure {Exit::Usage, fmt(msg::kNeedsQuery, command, command)};
    Result<std::string> kind = kindOf(args, "all");
    if (!kind)
        return std::move(kind).failure();
    Result<std::wstring_view> sort = choiceOption(args, Opt::Sort, L"match", kSorts);
    if (!sort)
        return std::move(sort).failure();
    Result<std::vector<std::wstring>> folders = inFolders(args);
    if (!folders)
        return std::move(folders).failure();
    request.folders = std::move(*folders);
    request.arguments = {{"query", utf8(request.query)}, {"kind", *kind}, {"sort", serverSort(*sort)}, {"limit", limit}};
    // The size is read from the disk, file by file: only when it is shown or sorted by.
    if (!args.has(Opt::Long) && *sort != L"largest" && *sort != L"smallest")
        request.arguments.emplace("size", false);
    if (!request.folders.empty())
        request.arguments.emplace("folders", pathList(request.folders));
    return request;
}

} // namespace

Result<Exit> runFind(Context& context)
{
    const Parsed& args = context.args;
    Result<long long> limit = limitOf(args, kMostFiles);
    if (!limit)
        return std::move(limit).failure();
    Result<FindRequest> request = findRequest(args, context.command.name, *limit);
    if (!request)
        return std::move(request).failure();
    Result<std::unique_ptr<Connection>> connection = connect(context);
    if (!connection)
        return std::move(connection).failure();
    if (!request->folders.empty()) {
        // Only a Win顺 made for ws narrows by folder itself; an older one would answer for everything.
        if (Result<void> made = requireCliContract(**connection, L"--in"); !made)
            return std::move(made).failure();
    }
    Result<json::Value> result = callTool(context, **connection, "search_files", json::Value(request->arguments), true);
    if (!result)
        return std::move(result).failure();
    const json::Value::Array& items = (*result)["items"].toArray();
    if (!printItems(context, items, listStyle(args, request->query)))
        return interrupt::requested() ? Result<Exit>(Failure {Exit::Interrupted, {}}) : Result<Exit>(Exit::Found);
    if ((*result)["truncated"].toBool())
        noteTruncated(context, items.size(), *limit, madeForWs(**connection));
    return items.empty() ? Exit::NothingFound : Exit::Found;
}

Result<Exit> runOpen(Context& context)
{
    const Parsed& args = context.args;
    const bool reveal = args.has(Opt::Reveal);
    const bool narrowed = args.has(Opt::Ext) || args.has(Opt::In) || args.has(Opt::Files) || args.has(Opt::Folders) || args.has(Opt::Sort);
    // A path that is there: opened as it is, Win顺 not asked.
    if (const std::wstring words = joinedWords(args); !words.empty() && !narrowed) {
        if (const std::wstring path = absolutePath(words); pathExists(path)) {
            // A path the user named: opened as it is, with ws's own rights (as `start` would).
            Result<void> opened = openPath(path, reveal, false);
            if (!opened)
                return std::move(opened).failure();
            context.out.write(path);
            context.out.newline();
            return Exit::Found;
        }
    }
    Result<FindRequest> request = findRequest(args, context.command.name, 1);
    if (!request)
        return std::move(request).failure();
    request->arguments.insert_or_assign("size", false);
    Result<std::unique_ptr<Connection>> connection = connect(context);
    if (!connection)
        return std::move(connection).failure();
    // `open` acts on the answer. The pipe's name is the user's SID, which any
    // program can create first: before anything is asked of it, the process
    // behind the pipe must be Win顺. (Not with a --pipe of the tests' own,
    // unless WS_TEST_VERIFY asks for it.)
    if (!context.args.has(Opt::Pipe) || environmentSet(L"WS_TEST_VERIFY")) {
        if (Result<void> genuine = checkWinShunServes((*connection)->client); !genuine)
            return std::move(genuine).failure();
    }
    // A Win顺 that ignored --in would open a file outside the folder.
    if (!request->folders.empty()) {
        if (Result<void> made = requireCliContract(**connection, L"--in"); !made)
            return std::move(made).failure();
    }
    Result<json::Value> result = callTool(context, **connection, "search_files", json::Value(request->arguments), true);
    if (!result)
        return std::move(result).failure();
    const json::Value::Array& items = (*result)["items"].toArray();
    if (items.empty()) {
        context.note(tr(msg::kNotFound));
        return Exit::NothingFound;
    }
    const std::wstring path = wide(items.front()["path"].toString());
    // A search result: never opened with more rights than the user has (ws elevated: through Explorer).
    Result<void> opened = openPath(path, reveal, true);
    if (!opened)
        return std::move(opened).failure();
    context.out.write(path);
    context.out.newline();
    return Exit::Found;
}

Result<Exit> runRecent(Context& context)
{
    const Parsed& args = context.args;
    if (!args.words.empty())
        return Failure {Exit::Usage, std::wstring(tr(msg::kRecentNoWords))};
    Result<long long> limit = limitOf(args, kMostFiles);
    if (!limit)
        return std::move(limit).failure();
    Result<std::string> kind = kindOf(args, "files");
    if (!kind)
        return std::move(kind).failure();
    Result<long long> days = integerOption(args, Opt::Days, 0, 0, 36'500);
    if (!days)
        return std::move(days).failure();
    Result<std::vector<std::wstring>> folders = inFolders(args);
    if (!folders)
        return std::move(folders).failure();
    if (folders->size() > 1)
        return Failure {Exit::Usage, fmt(msg::kOneIn, context.command.name)};
    json::Value::Object arguments {{"kind", *kind}, {"limit", *limit}};
    if (*days > 0)
        arguments.emplace("days", *days);
    if (!folders->empty())
        arguments.emplace("folder", utf8(folders->front()));
    if (const std::vector<std::wstring> extensions = extensionList(args); !extensions.empty())
        arguments.emplace("extensions", pathList(extensions));
    if (!args.has(Opt::Long))
        arguments.emplace("size", false);
    Result<std::unique_ptr<Connection>> connection = connect(context);
    if (!connection)
        return std::move(connection).failure();
    const std::wstring_view subject = folders->empty() ? std::wstring_view() : std::wstring_view(folders->front());
    Result<json::Value> result = callTool(context, **connection, "recent_files", json::Value(arguments), true, subject);
    if (!result)
        return std::move(result).failure();
    // Win顺 leaves out what is in system, program and tool folders, and
    // hidden files: in a folder inside such a place that is all of it. The
    // user named the folder: asked again with them.
    bool hiddenPlaces = false;
    if (!folders->empty() && (*result)["items"].toArray().empty() && !interrupt::requested()) {
        arguments.insert_or_assign("include_system", true);
        arguments.insert_or_assign("include_hidden", true);
        result = callTool(context, **connection, "recent_files", json::Value(std::move(arguments)), false, subject);
        if (!result)
            return std::move(result).failure();
        hiddenPlaces = !(*result)["items"].toArray().empty();
    }
    const json::Value::Array& items = (*result)["items"].toArray();
    if (!printItems(context, items, listStyle(args)))
        return interrupt::requested() ? Result<Exit>(Failure {Exit::Interrupted, {}}) : Result<Exit>(Exit::Found);
    if (hiddenPlaces)
        context.note(tr(msg::kRecentHiddenPlaces));
    // An older Win顺 says nothing of more: a full page means there likely is,
    // and it gives at most 200, whatever was asked.
    if (const bool made = madeForWs(**connection); made ? (*result)["truncated"].toBool()
            : static_cast<long long>(items.size()) >= *limit || (*limit > kOlderMost && items.size() == kOlderMost))
        noteTruncated(context, items.size(), *limit, made);
    return items.empty() ? Exit::NothingFound : Exit::Found;
}

Result<Exit> runLs(Context& context)
{
    const Parsed& args = context.args;
    // Several words: a path with spaces, written without quotes.
    std::wstring folder = joinedWords(args);
    if (folder.empty())
        folder = L".";
    if (Result<void> clean = noQuoteIn(folder); !clean)
        return std::move(clean).failure();
    std::wstring path = absolutePath(folder);
    const PathKind kindHere = pathKind(path);
    if (args.words.size() > 1 && kindHere == PathKind::None)
        return Failure {Exit::Usage, std::wstring(tr(msg::kOneFolder))};
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    if (kindHere == PathKind::File) // ws sees it is one: no need to ask
        return Failure {Exit::Usage, fmt(msg::kIsFile, path)};
    Result<long long> limit = limitOf(args, kMostFolder);
    if (!limit)
        return std::move(limit).failure();
    Result<std::string> kind = kindOf(args, "all");
    if (!kind)
        return std::move(kind).failure();
    json::Value::Object arguments {{"path", utf8(path)}, {"kind", *kind}, {"limit", *limit}};
    if (!args.has(Opt::Long))
        arguments.emplace("size", false);
    Result<std::unique_ptr<Connection>> connection = connect(context);
    if (!connection)
        return std::move(connection).failure();
    Result<json::Value> result = callTool(context, **connection, "list_folder", json::Value(std::move(arguments)), true, path);
    if (!result)
        return std::move(result).failure();
    const json::Value::Array& items = (*result)["items"].toArray();
    if (!printItems(context, items, listStyle(args)))
        return interrupt::requested() ? Result<Exit>(Failure {Exit::Interrupted, {}}) : Result<Exit>(Exit::Found);
    if ((*result)["truncated"].toBool())
        noteTruncated(context, items.size(), *limit, madeForWs(**connection), (*result)["total"].isNumber() ? (*result)["total"].toInt() : -1);
    return items.empty() ? Exit::NothingFound : Exit::Found;
}

} // namespace ws::cli
