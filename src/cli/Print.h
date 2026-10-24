#pragma once

// Lists of files and folders (find, recent, ls) as docs/design-cli.md §4 has
// them: one full path a line (on a console the folder dim, the name bold, the
// query's words marked); -l with size and time; --json one array; -0 NUL
// between paths.

#include "Context.h"

#include <string>
#include <vector>

namespace ws::cli {

struct ListStyle {
    bool longFormat = false;
    bool json = false;
    bool nul = false;
    std::vector<std::wstring> terms; // marked in names on a console
};

ListStyle listStyle(const Parsed& args, std::wstring_view query = {});

// Prints them; false when the reader went away or Ctrl+C came.
bool printItems(Context& context, const json::Value::Array& items, const ListStyle& style);

// An item as --json gives it, the same from every Win顺: without "name" (the
// path ends with it; a Win顺 before ws sends it), "size" only when kept.
json::Value jsonItem(const json::Value& item, bool keepSize);

// A path, its folder dim and its name bold with `terms` marked (console
// colours; plain text otherwise).
void writePath(Stream& out, std::wstring_view path, bool folder, const std::vector<std::wstring>& terms);
// Text with `terms` marked.
void writeMarked(Stream& out, std::wstring_view text, const std::vector<std::wstring>& terms, Style style, Style matchStyle);

// The truncation note: "only the first N…".
void noteTruncated(Context& context, std::size_t returned, long long asked, long long total = -1);

} // namespace ws::cli
