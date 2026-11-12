#pragma once

// A plugin's items as rows of the launcher's list, and where they go (the
// plugin design, §4.1 keyword mode, §4.2 pushing into 全部, §7.1): kept
// apart from Launcher so that tst_plugin and wsbench --launcher-sim merge
// exactly as the launcher does, over a plain list instead of ResultModel.
//
// Keyword mode: the list is the command's. Its answer replaces the rows
// (query/partial batches are appended as they come), nothing else is shown.
//
// Push mode: a command's rows (at most 3, 6 for all plugins together) take
// their place among the apps by score, once the name search's rows are
// shown. Never above a row that is recent, or an app named just what was
// typed, nor above the row the keyboard is on (below it when it is not
// the first; on the first row, a better one takes its place and the
// launcher says so with betterFirstRow). Before the files and the content
// matches. A content provider's rows are not pushed so: its first three go
// after the first five name rows, the others last.
// Answering again (a rerun, a refresh), rows with the same item id are
// updated in place; rows that went are removed, new ones inserted.
//
// An instant command's keyword typed alone ("pl", Match::alone; GitHub
// #242): the list stays the search's, and the command's rows lead it, all
// of them, in the plugin's order (below the keyboard's row when that is not
// the first). Until its answer comes a row offering the command holds the
// first place, so that Enter never opens a file the user did not mean.

#include "Catalog.h"
#include "Items.h"
#include "SearchTypes.h"

#include <QString>
#include <QStringView>

#include <cstdint>
#include <map>

namespace ws::plugin {

// "winshun-plugin:<plugin>/<command>/<item>": stands for a plugin's row
// (SearchResult::path, which keys the list).
QString pluginPath(const QString& pluginId, const QString& commandId, const QString& itemId);
bool isPluginPath(QStringView path) noexcept;
// The item id of the row offering a command whose keyword was typed alone
// ("kill — 结束进程"), or begins with what was typed ("uuid — UUID" for
// "uu"): Enter or Tab puts "kill " in the box (hintRow).
inline constexpr QStringView kHintItem = u"#hint";
bool isHintRow(const SearchResult& row) noexcept;

// An item as a row: the command's and the plugin's icon and actions fill
// in what it left out, titles in `locale`; `index`, its place in the
// answer, is its id when it has none.
SearchResult toRow(const Entry& entry, const Command& command, const Item& item, int index, const QString& locale);
// The row for a list command's keyword typed alone, or in part
// (Catalog::hints): named "<keyword> — <title>", `subtitle` under it,
// autocomplete and setQuery "<keyword> " (a keyword an instant command's
// begins with: that keyword alone). A partial one scores low: it goes among
// the rows, not first.
SearchResult hintRow(const Match& hint, const QString& subtitle, const QString& locale);

// The list the rows go into: ResultModel in the app, a QList in tests.
class RowSink {
public:
    virtual ~RowSink() = default;
    virtual int count() const = 0;
    virtual const SearchResult& row(int index) const = 0;
    virtual int currentRow() const = 0; // the keyboard's; -1 for none
    virtual void assign(SearchResults rows) = 0; // the whole list (keyword mode)
    virtual void append(const SearchResults& rows) = 0;
    virtual void insert(int index, const SearchResult& row) = 0;
    virtual void update(int index, const SearchResult& row) = 0;
    virtual void remove(int index) = 0;
};

class Merge {
public:
    static constexpr int kPerPlugin = 3; // rows one plugin may push
    static constexpr int kTotal = 6; // ... all plugins together
    // Content providers (commands[].contentSearch): this many of their rows
    // go on the first screen, after this many name rows; the rest follow
    // the native content matches.
    static constexpr int kFirstScreenRows = 3;
    static constexpr int kFirstScreenNames = 5;

    Merge(RowSink& sink, const Catalog& catalog);

    void setLocale(const QString& locale) { m_locale = locale; }
    // The words added to pushed rows' subtitles when more were cut: "%1" is
    // the keyword that shows them all.
    void setMoreText(const QString& format) { m_moreText = format; }
    // The subtitle of the hint row (a keyword typed alone, hintRow()), and
    // of the rows for keywords that begin with what was typed.
    void setHintText(const QString& text) { m_hintText = text; }
    void setPrefixHintText(const QString& text) { m_prefixHintText = text; }

    // A query begins: earlier answers are dropped. With `keywordPlugin`
    // and `keywordCommand`, keyword mode for that command. `text` as typed
    // (a trailing space tells a keyword from a name, Catalog::match).
    void begin(std::uint64_t queryId, const QString& text, const QString& keywordPlugin = {}, const QString& keywordCommand = {}, bool contentOnly = false,
        bool hints = true);
    std::uint64_t queryId() const noexcept { return m_queryId; }
    bool keywordMode() const noexcept { return !m_keywordPlugin.isEmpty(); }
    // Push mode: the name search's rows are in the sink now; what was held
    // for them goes in, and the hint rows: a keyword typed alone first,
    // keywords it begins among the rows (placeHint).
    void namesShown();
    bool namesReady() const noexcept { return m_namesReady; }
    bool contentOnly() const noexcept { return m_contentOnly; }
    // Content only (内容): no name search to wait for, only for the list to
    // be this query's. True when a provider's answer with a file is held
    // for that: the launcher clears the last query's rows (or, asked again,
    // has its own) and calls namesShown at once, not when the native scan
    // shows a row.
    bool answerHeld() const;
    // A content provider's row (commands[].contentSearch).
    bool contentRow(const SearchResult& row) const;
    // Where the native content matches end: the first of the content
    // providers' rows after them, which stay last (count() with none).
    int contentEnd() const;

    // What Host::results said (dropped unless for the current query).
    void results(std::uint64_t queryId, const QString& pluginId, const QString& commandId, const Items& items, bool partial,
        bool replace);
    // An action's `replace` effect: these rows instead of the list.
    void replace(const QString& pluginId, const QString& commandId, const Items& items);

    // The words an action or a details request of this row is about: none
    // for a row of the command whose keyword was typed alone (as in its own
    // list), else what was typed.
    bool keywordAlone(const SearchResult& row) const;

    // A row went in at the top while the keyboard was on the first row,
    // since last asked.
    bool takeBetterFirstRow() noexcept { return std::exchange(m_betterFirstRow, false); }
    int pluginRows() const; // in the sink now

private:
    struct Batch {
        QString pluginId;
        QString commandId;
        Items items;
        bool complete = false;
    };
    SearchResults rowsOf(const Entry& entry, const Command& command, const Items& items, bool sortByScore, int offset = 0) const;
    void apply(const Batch& batch);
    void applyAlone(const Batch& batch);
    void placeHint();
    void placeAloneHint();
    int floorRow() const;
    int firstScreenRow() const;
    int placeFor(int score) const;

    RowSink& m_sink;
    const Catalog& m_catalog;
    QString m_locale = QStringLiteral("zh");
    QString m_moreText = QStringLiteral("（%1 查看全部）");
    QString m_hintText = QStringLiteral("按空格进入；Enter 或 Tab 也行");
    QString m_prefixHintText = QStringLiteral("按 Tab 补全；Enter 直接进入");
    std::uint64_t m_queryId = 0;
    QString m_text;
    QString m_keywordPlugin;
    QString m_keywordCommand;
    bool m_namesReady = false;
    bool m_contentOnly = false;
    bool m_hints = true;
    // The instant command whose keyword was typed alone (empty: none).
    QString m_alonePlugin;
    QString m_aloneCommand;
    bool m_assigned = false; // keyword mode: an answer has replaced the rows
    int m_keywordItemIndex = 0; // anonymous ids span every appended batch
    std::map<QString, Batch> m_held; // "<plugin>/<command>" -> its answer so far
    bool m_betterFirstRow = false;
};

} // namespace ws::plugin
