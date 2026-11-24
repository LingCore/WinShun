#pragma once

#include "SearchTypes.h"

#include <QString>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <functional>

namespace ws::dialogbar {

// Which suggestion sources appear when the dialog search bar has nothing typed
// (GitHub #438). Unchecked sources are omitted both on focus and on Down;
// normal typing/search is unaffected. All four off disables empty-query
// suggestions altogether.
struct SuggestionSources {
    bool open = true; // folders currently open in file managers
    bool clipboard = true; // a file or folder path on the clipboard
    bool favorites = true; // favorite folders (and files outside folder pickers)
    bool recent = true; // recently used folders

    [[nodiscard]] constexpr bool any() const noexcept { return open || clipboard || favorites || recent; }
    constexpr bool operator==(const SuggestionSources&) const = default;
};

struct SuggestionInputs {
    QStringList explorer;
    QStringList explorerTags;
    QString clipboard;
    bool clipboardIsDir = false;
    QString clipboardTag;
    SearchResults favorites;
    QStringList favoriteTags;
    QStringList recent;
    QString recentTag;
    bool foldersOnly = false;
    qsizetype maxExplorer = 5;
};

struct SuggestionList {
    SearchResults rows;
    QStringList tags;
};

[[nodiscard]] constexpr bool shouldSuggest(SuggestionSources sources, bool suggestOnFocus, bool asked) noexcept
{
    return sources.any() && (suggestOnFocus || asked);
}

[[nodiscard]] inline bool canAskSuggestions(
    SuggestionSources sources, bool suggestOnFocus, bool asked, QStringView query) noexcept
{
    return sources.any() && !shouldSuggest(sources, suggestOnFocus, asked) && query.trimmed().isEmpty();
}

// Assemble the empty-query suggestions according to `sources`, deduplicating
// paths case-insensitively in priority order: open file managers, clipboard,
// favorites, recent folders.
[[nodiscard]] inline SuggestionList buildSuggestions(
    const SuggestionInputs& in, SuggestionSources sources, const std::function<QString(const QString&)>& nameOf)
{
    SuggestionList out;
    if (!sources.any())
        return out;
    QStringList seen;
    const auto add = [&](const QString& path, bool isDir, const QString& tag, bool recent = false,
                         const QString& name = {}) {
        if (path.isEmpty() || seen.contains(path, Qt::CaseInsensitive))
            return;
        seen.append(path);
        SearchResult row;
        row.path = path;
        row.name = name.isEmpty() ? (nameOf ? nameOf(path) : path) : name;
        row.isDir = isDir;
        row.recent = recent;
        out.rows.append(std::move(row));
        out.tags.append(tag);
    };
    if (sources.open) {
        const qsizetype count = std::min<qsizetype>(in.explorer.size(), std::max<qsizetype>(0, in.maxExplorer));
        for (qsizetype i = 0; i < count; ++i)
            add(in.explorer[i], true, in.explorerTags.value(i));
    }
    if (sources.clipboard && !in.clipboard.isEmpty())
        add(in.clipboard, in.clipboardIsDir, in.clipboardTag);
    if (sources.favorites) {
        for (qsizetype i = 0; i < in.favorites.size(); ++i) {
            const SearchResult& f = in.favorites[i];
            if (!in.foldersOnly || f.isDir)
                add(f.path, f.isDir, in.favoriteTags.value(i), false, f.name);
        }
    }
    if (sources.recent) {
        for (const QString& folder : in.recent)
            add(folder, true, in.recentTag, true);
    }
    return out;
}

} // namespace ws::dialogbar
