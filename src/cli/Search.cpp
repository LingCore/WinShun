// find, open, recent, ls: names from the index.

#include "Commands.h"
#include "Encoding.h"
#include "Format.h"
#include "Interrupt.h"
#include "Messages.h"
#include "Print.h"
#include "Win.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

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

// An older Win顺 ignores `folders`: keep what is inside them. True when it had to.
bool keepInside(json::Value::Array& items, const std::vector<std::wstring>& folders)
{
    if (folders.empty())
        return false;
    const std::size_t before = items.size();
    std::erase_if(items, [&](const json::Value& item) {
        const std::wstring path = wide(item["path"].toString());
        return std::none_of(folders.begin(), folders.end(), [&](const std::wstring& folder) { return isUnder(path, folder); });
    });
    return items.size() != before;
}

Result<void> shellOpen(const std::wstring& path, bool reveal)
{
    // The tests': what would be opened, written to this file.
    if (const std::wstring record = environment(L"WS_TEST_OPEN"); !record.empty()) {
        const Handle file(::CreateFileW(record.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        const std::string line = utf8((reveal ? L"reveal " : L"open ") + path + L"\n");
        DWORD written = 0;
        if (!file || !::WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr))
            return Failure {Exit::Failed, fmt(msg::kOpenFailed, path, systemMessage(::GetLastError()))};
        return {};
    }
    // The shell wants COM (and no DDE) on the thread that asks it.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Result<void> result;
    if (reveal) {
        PIDLIST_ABSOLUTE item = nullptr;
        HRESULT hr = ::SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr);
        if (SUCCEEDED(hr)) {
            hr = ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            ::CoTaskMemFree(item);
        }
        if (FAILED(hr))
            result = Failure {Exit::Failed, fmt(msg::kOpenFailed, path, systemMessage(static_cast<DWORD>(hr)))};
    } else {
        SHELLEXECUTEINFOW info {};
        info.cbSize = sizeof info;
        info.fMask = SEE_MASK_NOASYNC; // ws ends right after: finish here
        info.lpFile = path.c_str();
        const std::wstring folder = path.substr(0, nameStart(path));
        info.lpDirectory = folder.empty() ? nullptr : folder.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!::ShellExecuteExW(&info))
            result = Failure {Exit::Failed, fmt(msg::kOpenFailed, path, systemMessage(::GetLastError()))};
    }
    if (SUCCEEDED(com))
        ::CoUninitialize();
    return result;
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
    Result<json::Value> result = callTool(context, **connection, "search_files", json::Value(request->arguments), true);
    if (!result)
        return std::move(result).failure();
    json::Value::Array items = (*result)["items"].toArray();
    const std::size_t returned = items.size();
    const bool filtered = keepInside(items, request->folders);
    if (!printItems(context, items, listStyle(args, request->query)))
        return interrupt::requested() ? Result<Exit>(Failure {Exit::Interrupted, {}}) : Result<Exit>(Exit::Found);
    if (filtered)
        context.note(tr(msg::kInIgnored));
    else if ((*result)["truncated"].toBool())
        noteTruncated(context, returned, *limit);
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
            Result<void> opened = shellOpen(path, reveal);
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
    Result<json::Value> result = callTool(context, **connection, "search_files", json::Value(request->arguments), true);
    if (!result)
        return std::move(result).failure();
    json::Value::Array items = (*result)["items"].toArray();
    keepInside(items, request->folders);
    if (items.empty()) {
        context.note(tr(msg::kNotFound));
        return Exit::NothingFound;
    }
    const std::wstring path = wide(items.front()["path"].toString());
    Result<void> opened = shellOpen(path, reveal);
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
    // No "truncated" here: a full page means there is likely more. An older
    // Win顺 gives at most 200, whatever was asked.
    if (static_cast<long long>(items.size()) >= *limit || (*limit > kOlderMost && items.size() == kOlderMost))
        noteTruncated(context, items.size(), *limit);
    return items.empty() ? Exit::NothingFound : Exit::Found;
}

Result<Exit> runLs(Context& context)
{
    const Parsed& args = context.args;
    // Several words: a path with spaces, written without quotes.
    std::wstring folder = joinedWords(args);
    if (folder.empty())
        folder = L".";
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
        noteTruncated(context, items.size(), *limit, (*result)["total"].isNumber() ? (*result)["total"].toInt() : -1);
    return items.empty() ? Exit::NothingFound : Exit::Found;
}

} // namespace ws::cli
