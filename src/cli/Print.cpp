#include "Print.h"

#include "Encoding.h"
#include "Format.h"
#include "Interrupt.h"
#include "Messages.h"

#include <algorithm>

namespace ws::cli {

namespace {

bool stopped(const Stream& out)
{
    return out.broken() || interrupt::requested();
}

std::wstring itemPath(const json::Value& item)
{
    return wide(item["path"].toString());
}

bool isFolderItem(const json::Value& item)
{
    return item["type"].toString() == "folder";
}

} // namespace

ListStyle listStyle(const Parsed& args, std::wstring_view query)
{
    ListStyle style;
    style.longFormat = args.has(Opt::Long);
    style.json = args.has(Opt::Json);
    style.nul = args.has(Opt::Null);
    if (!query.empty())
        style.terms = queryTerms(query);
    return style;
}

void writeMarked(Stream& out, std::wstring_view text, const std::vector<std::wstring>& terms, Style style, Style matchStyle)
{
    if (!out.colored() || terms.empty()) {
        out.write(text, style);
        return;
    }
    std::size_t at = 0;
    for (const auto& [from, to] : findTerms(text, terms)) {
        if (from > at)
            out.write(text.substr(at, from - at), style);
        out.write(text.substr(from, to - from), matchStyle);
        at = to;
    }
    if (at < text.size())
        out.write(text.substr(at), style);
}

void writePath(Stream& out, std::wstring_view path, bool folder, const std::vector<std::wstring>& terms)
{
    if (!out.colored()) {
        out.write(path);
        return;
    }
    // "C:\" alone, or a folder given with a backslash at the end: all of it is the name.
    std::wstring_view trimmed = path;
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
        trimmed.remove_suffix(1);
    const std::size_t start = trimmed.size() <= 3 ? 0 : nameStart(trimmed);
    out.write(path.substr(0, start), Style::Dim);
    writeMarked(out, path.substr(start), terms, folder ? Style::FolderName : Style::Name, Style::Match);
}

bool printItems(Context& context, const json::Value::Array& items, const ListStyle& style)
{
    Stream& out = context.out;
    if (style.json) {
        // The size only with -l: Win顺 reads it from the disk only when asked.
        json::Value::Array list;
        list.reserve(items.size());
        for (const json::Value& item : items)
            list.push_back(jsonItem(item, style.longFormat));
        out.setJsonEscapes(true);
        out.writeUtf8(json::write(json::Value(std::move(list))));
        out.newline();
        out.setJsonEscapes(false);
        return !stopped(out);
    }
    if (style.nul) {
        for (const json::Value& item : items) {
            out.write(itemPath(item));
            out.nul();
            if (stopped(out))
                return false;
        }
        return true;
    }
    if (style.longFormat) {
        std::vector<std::wstring> sizes;
        sizes.reserve(items.size());
        std::size_t width = 1;
        for (const json::Value& item : items) {
            sizes.push_back(item["size"].isNumber() ? humanSize(item["size"].toInt()) : std::wstring(L"-"));
            width = std::max(width, sizes.back().size());
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            const json::Value& item = items[i];
            out.write(std::wstring(width - sizes[i].size(), L' ') + sizes[i]);
            out.write(L"  ");
            std::wstring time = shortTime(item["modified"].toString());
            if (time.size() < 16)
                time.append(16 - time.size(), L' ');
            out.write(time, Style::Dim);
            out.write(L"  ");
            writePath(out, itemPath(item), isFolderItem(item), style.terms);
            out.newline();
            if (stopped(out))
                return false;
        }
        return true;
    }
    for (const json::Value& item : items) {
        writePath(out, itemPath(item), isFolderItem(item), style.terms);
        out.newline();
        if (stopped(out))
            return false;
    }
    return true;
}

json::Value jsonItem(const json::Value& item, bool keepSize)
{
    const bool dropSize = !keepSize && item.has("size");
    if (!item.has("name") && !dropSize)
        return item; // shares the object: nothing copied
    json::Value::Object copy = item.toObject();
    copy.erase("name");
    if (dropSize)
        copy.erase("size");
    return copy;
}

void noteTruncated(Context& context, std::size_t returned, long long asked, bool madeForWs, long long total)
{
    // More matched, yet fewer came than asked: a Win顺 made for ws stopped at its
    // answer's byte budget; an older one gives no more than its own limit.
    if (asked > static_cast<long long>(returned))
        context.note(fmt(madeForWs ? msg::kAnswerBudget : msg::kServerCapped, returned));
    else if (context.args.value(Opt::Limit) == L"0") // -n 0 already: as many as ws takes
        context.note(fmt(msg::kTruncatedMax, returned));
    else if (total >= 0)
        context.note(fmt(msg::kTruncatedOf, returned, total));
    else
        context.note(fmt(msg::kTruncated, returned));
}

} // namespace ws::cli
