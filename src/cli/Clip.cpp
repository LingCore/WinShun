// clip: Win顺's clipboard history, when the user lets AI and the command line
// read it (the one switch ws obeys besides the connection: no other program
// could read it otherwise).

#include "Commands.h"
#include "Encoding.h"
#include "Format.h"
#include "Interrupt.h"
#include "Messages.h"
#include "Print.h"

namespace ws::cli {

namespace {

constexpr long long kMostClips = 100;
constexpr long long kLineChars = 300; // of each text, one entry a line
constexpr long long kWholeChars = 20'000; // -0 and --json: the most Win顺 gives

constexpr std::wstring_view kKinds[] = {L"all", L"text", L"link", L"path", L"files", L"image"};

// One entry on one line: line breaks as \n, tabs as \t.
std::wstring oneLine(std::wstring_view text)
{
    std::wstring line;
    line.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n')
                continue;
            line += L"\\n";
        } else if (c == L'\n') {
            line += L"\\n";
        } else if (c == L'\t') {
            line += L"\\t";
        } else if (c < 0x20) {
            line += L' ';
        } else {
            line += c;
        }
    }
    return line;
}

std::wstring pictureText(const json::Value& item)
{
    return std::wstring(language() == Lang::Zh ? L"[图片 " : L"[picture ") + std::to_wstring(item["width"].toInt()) + L"×"
        + std::to_wstring(item["height"].toInt()) + L"]";
}

} // namespace

Result<Exit> runClip(Context& context)
{
    const Parsed& args = context.args;
    std::wstring query;
    for (const std::wstring& word : args.words) {
        if (!query.empty())
            query += L' ';
        query += word;
    }
    Result<long long> limit = limitOf(args, kMostClips);
    if (!limit)
        return std::move(limit).failure();
    Result<std::wstring_view> kind = choiceOption(args, Opt::Kind, L"all", kKinds);
    if (!kind)
        return std::move(kind).failure();
    const bool whole = args.has(Opt::Null) || args.has(Opt::Json);
    json::Value::Object arguments {{"kind", utf8(*kind)}, {"limit", *limit}, {"max_chars", whole ? kWholeChars : kLineChars}};
    if (!query.empty())
        arguments.emplace("query", utf8(query));

    Result<std::unique_ptr<Connection>> connection = connect(context);
    if (!connection)
        return std::move(connection).failure();
    Result<json::Value> result = callTool(context, **connection, "clipboard_history", json::Value(std::move(arguments)), false);
    if (!result)
        return std::move(result).failure();
    const json::Value::Array& items = (*result)["items"].toArray();

    Stream& out = context.out;
    if (args.has(Opt::Json)) {
        out.setJsonEscapes(true);
        out.writeUtf8(json::write((*result)["items"]));
        out.newline();
        out.setJsonEscapes(false);
    } else {
        const std::vector<std::wstring> terms = queryTerms(query);
        for (const json::Value& item : items) {
            const bool picture = item["kind"].toString() == "image";
            if (args.has(Opt::Null)) {
                if (picture)
                    continue; // no text to give
                out.write(wide(item["text"].toString()));
                out.nul();
            } else {
                if (picture) {
                    out.write(pictureText(item), Style::Dim);
                } else {
                    writeMarked(out, oneLine(wide(item["text"].toString())), terms, Style::Plain, Style::Match);
                    if (item["truncated"].toBool())
                        out.write(L"…", Style::Dim);
                }
                out.newline();
            }
            if (out.broken() || interrupt::requested())
                break;
        }
    }
    if (interrupt::requested())
        return Failure {Exit::Interrupted, {}};
    return items.empty() ? Exit::NothingFound : Exit::Found;
}

} // namespace ws::cli
