#include "Merge.h"

#include "Details.h"

#include <QSet>

#include <algorithm>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::plugin {

namespace {

constexpr QStringView kPathPrefix = u"winshun-plugin:";
// A prefix hint's score (0–100, the apps' scale): placeFor() puts it past
// the apps that match as well or better, before the files.
constexpr int kPrefixHintScore = 30;

// An icon spec back as the item wrote it (the model maps it to an image URL).
QString iconText(const IconSpec& spec)
{
    switch (spec.kind) {
    case IconSpec::Kind::None: return {};
    case IconSpec::Kind::File: return u"file:"_s + spec.value;
    case IconSpec::Kind::Ext: return u"ext:"_s + spec.value;
    case IconSpec::Kind::App: return u"app:"_s + spec.value;
    case IconSpec::Kind::Glyph: return u"glyph:"_s + spec.value;
    case IconSpec::Kind::Color: return u"color:"_s + spec.value;
    case IconSpec::Kind::Data: return spec.value; // the whole data: URL
    case IconSpec::Kind::PluginFile: return spec.value;
    case IconSpec::Kind::Site: return u"site:"_s + spec.value;
    case IconSpec::Kind::Phosphor: return spec.value; // the whole ph:/ph-fill: spec
    }
    return {};
}

} // namespace

QString pluginPath(const QString& pluginId, const QString& commandId, const QString& itemId)
{
    return kPathPrefix + pluginId + u'/' + commandId + u'/' + itemId;
}

bool isPluginPath(QStringView path) noexcept
{
    return path.startsWith(kPathPrefix);
}

bool isHintRow(const SearchResult& row) noexcept
{
    return row.isPlugin() && row.plugin->itemId == kHintItem;
}

SearchResult hintRow(const Match& hint, const QString& subtitle, const QString& locale)
{
    const Entry& entry = *hint.entry;
    const Command& command = *hint.command;
    auto p = std::make_shared<PluginRow>();
    p->pluginId = entry.id;
    p->commandId = command.id;
    p->itemId = kHintItem.toString();
    p->source = entry.manifest ? entry.manifest->name.pick(locale) : entry.id;
    p->subtitle = subtitle;
    p->icon = iconText(parseIcon(command.icon));
    if (p->icon.isEmpty() && entry.manifest)
        p->icon = iconText(parseIcon(entry.manifest->icon));
    if (p->icon.isEmpty())
        p->icon = u"glyph:U+EA86"_s; // Puzzle
    // The space after a keyword opens its command's own list. A keyword an
    // instant command's begins with: that keyword alone, which brings its
    // rows to the top of the search (Catalog::match).
    p->autocomplete = command.mode == Command::Mode::Instant && hint.partial ? hint.keyword : hint.keyword + u' ';
    p->setQuery = p->autocomplete;
    p->score = hint.partial ? kPrefixHintScore : 100;

    const QString title = command.title.pick(locale).isEmpty() && entry.manifest ? entry.manifest->name.pick(locale) : command.title.pick(locale);
    SearchResult r;
    r.name = title.isEmpty() ? hint.keyword : hint.keyword + u" — "_s + title;
    r.path = pluginPath(entry.id, command.id, p->itemId);
    r.app = AppKind::Plugin;
    r.score = p->score;
    r.plugin.row = std::move(p);
    return r;
}

SearchResult toRow(const Entry& entry, const Command& command, const Item& item, int index, const QString& locale)
{
    auto p = std::make_shared<PluginRow>();
    p->pluginId = entry.id;
    p->commandId = command.id;
    p->itemId = item.id.isEmpty() ? u'#' + QString::number(index) : item.id;
    p->source = entry.manifest ? entry.manifest->name.pick(locale) : entry.id;
    p->subtitle = item.subtitle;
    p->subtitleAway = item.subtitleAway;
    p->icon = iconText(item.icon);
    if (p->icon.isEmpty())
        p->icon = iconText(parseIcon(command.icon));
    if (p->icon.isEmpty() && entry.manifest)
        p->icon = iconText(parseIcon(entry.manifest->icon));
    if (p->icon.isEmpty())
        p->icon = u"glyph:U+EA86"_s; // Puzzle
    p->autocomplete = item.autocomplete;
    p->copy = item.copy;
    p->preview = item.previewText;
    switch (item.open.kind) {
    case Open::Kind::None: break;
    case Open::Kind::Path: p->openKind = PluginRow::OpenKind::Path; break;
    case Open::Kind::Url: p->openKind = PluginRow::OpenKind::Url; break;
    }
    p->openTarget = item.open.target;
    p->openArgs = item.open.args;
    p->openCwd = item.open.cwd;
    p->setQuery = item.setQuery;
    p->searchQuery = item.searchQuery;
    p->searchScope = item.searchScope;
    for (const Action& a : item.actions.isEmpty() ? command.actions : item.actions)
        p->actions.push_back({a.id, a.title.pick(locale), a.key});
    p->valid = item.valid;
    p->score = item.score;
    p->item = itemToJson(item);
    // "details" (GitHub #202, asksDetails): only a resident plugin is there
    // to be asked (a once plugin's process has ended, a zero-code one has none).
    if (!entry.manifest || !entry.manifest->resident())
        p->item.remove(u"details"_s);

    SearchResult r;
    r.name = item.title;
    r.modified = item.modified;
    r.created = item.created;
    r.size = item.size;
    r.path = pluginPath(entry.id, command.id, p->itemId);
    r.app = AppKind::Plugin;
    r.score = item.score;
    r.plugin.row = std::move(p);
    return r;
}

Merge::Merge(RowSink& sink, const Catalog& catalog)
    : m_sink(sink)
    , m_catalog(catalog)
{
}

void Merge::begin(std::uint64_t queryId, const QString& text, const QString& keywordPlugin, const QString& keywordCommand, bool contentOnly, bool hints)
{
    m_queryId = queryId;
    m_text = text;
    m_contentOnly = contentOnly;
    m_hints = hints;
    m_keywordPlugin = keywordPlugin;
    m_keywordCommand = keywordCommand;
    m_namesReady = keywordMode(); // no name search to wait for
    m_alonePlugin.clear();
    m_aloneCommand.clear();
    if (!keywordMode() && !contentOnly && hints) {
        for (const Match& m : m_catalog.match(text)) {
            if (m.kind == Match::Kind::Keyword && m.alone) {
                m_alonePlugin = m.entry->id;
                m_aloneCommand = m.command->id;
            }
        }
    }
    m_assigned = false;
    m_keywordItemIndex = 0;
    m_held.clear();
    m_betterFirstRow = false;
}

void Merge::namesShown()
{
    m_namesReady = true;
    if (!keywordMode() && !m_contentOnly && m_hints) {
        placeHint();
        placeAloneHint();
    }
    for (auto it = m_held.begin(); it != m_held.end();) {
        if (it->second.complete) {
            apply(it->second);
            it = m_held.erase(it);
        } else
            ++it;
    }
}

// A content provider's complete answer with a row apply() keeps: one that
// opens a file (it may still turn out a file already listed), or a resident
// plugin's that tells what it is ("details", GitHub #202).
bool Merge::answerHeld() const
{
    return std::ranges::any_of(m_held, [this](const auto& held) {
        const Batch& batch = held.second;
        if (!batch.complete)
            return false;
        const Entry* entry = m_catalog.find(batch.pluginId);
        const Command* command = entry && entry->manifest ? entry->manifest->command(batch.commandId) : nullptr;
        const bool resident = entry && entry->manifest && entry->manifest->resident();
        return command && command->contentSearch && std::ranges::any_of(batch.items, [resident](const Item& item) {
            return item.open.kind == Open::Kind::Path || (item.details && resident);
        });
    });
}

bool Merge::contentRow(const SearchResult& row) const
{
    if (!row.isPlugin())
        return false;
    const Entry* entry = m_catalog.find(row.plugin->pluginId);
    const Command* command = entry && entry->manifest ? entry->manifest->command(row.plugin->commandId) : nullptr;
    return command && command->contentSearch;
}

int Merge::contentEnd() const
{
    int end = m_sink.count();
    while (end > 0 && contentRow(m_sink.row(end - 1)))
        --end;
    return end;
}

// A list command's keyword typed alone: a row offering it, first and
// current (its words say Enter and Tab open the command, so they must;
// the names found for the word, the recent ones too, are a ↓ away). A
// word that keywords begin with ("uu" for uuid): a row for each, placed
// as a pushed row of its score would be, after the recent rows and the
// apps, before the files (Catalog::hints). Once per list: the sink's rows
// are replaced by every name search, and the rows go in again with them.
void Merge::placeHint()
{
    for (const Match& hint : m_catalog.hints(m_text)) {
        const SearchResult row = hintRow(hint, hint.partial ? m_prefixHintText : m_hintText, m_locale);
        bool there = false;
        for (int i = 0; i < m_sink.count() && !there; ++i)
            there = m_sink.row(i).path == row.path;
        if (there)
            continue;
        const int at = hint.partial ? placeFor(row.score) : 0;
        // Put in above the keyboard's row, the list moves that row down:
        // the launcher brings the keyboard back to the first (betterFirstRow)
        // for the keyword typed in full. A partial one is a suggestion: the
        // keyboard stays on the real first result (Tab finds the row anyway,
        // Launcher::completionRow).
        m_betterFirstRow = m_betterFirstRow || (!hint.partial && at == 0 && m_sink.currentRow() == 0);
        m_sink.insert(at, row);
    }
}

// An instant command's keyword typed alone, its answer still to come (a
// plugin starting): a row offering the command, first and current, so that
// Enter takes the user into its list instead of opening the first file
// found. The answer takes its place (applyAlone).
void Merge::placeAloneHint()
{
    if (m_alonePlugin.isEmpty())
        return;
    if (const auto held = m_held.find(m_alonePlugin + u'/' + m_aloneCommand); held != m_held.end() && held->second.complete)
        return; // namesShown puts the answer itself in
    for (int i = 0; i < m_sink.count(); ++i) {
        if (keywordAlone(m_sink.row(i)))
            return; // the row, or the answer, is there
    }
    for (const Match& m : m_catalog.match(m_text)) {
        if (m.kind != Match::Kind::Keyword || !m.alone)
            continue;
        m_betterFirstRow = m_betterFirstRow || m_sink.currentRow() == 0;
        m_sink.insert(0, hintRow(m, m_hintText, m_locale));
        return;
    }
}

bool Merge::keywordAlone(const SearchResult& row) const
{
    return !m_alonePlugin.isEmpty() && row.isPlugin() && row.plugin->pluginId == m_alonePlugin
        && row.plugin->commandId == m_aloneCommand;
}

int Merge::pluginRows() const
{
    int n = 0;
    for (int i = 0; i < m_sink.count(); ++i)
        n += m_sink.row(i).isPlugin();
    return n;
}

// The rows of an answer, each item once (the first of an id wins), by score
// when asked (push mode; keyword mode only when some item gave one).
SearchResults Merge::rowsOf(const Entry& entry, const Command& command, const Items& items, bool sortByScore, int offset) const
{
    SearchResults rows;
    QSet<QString> ids;
    bool scored = false;
    for (int i = 0; i < items.size() && i < kMaxItems; ++i) {
        SearchResult row = toRow(entry, command, items[i], offset + i, m_locale);
        if (ids.contains(row.path))
            continue;
        ids.insert(row.path);
        scored = scored || items[i].hasScore;
        rows.push_back(std::move(row));
    }
    if (sortByScore || scored)
        std::stable_sort(rows.begin(), rows.end(), [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
    return rows;
}

void Merge::results(std::uint64_t queryId, const QString& pluginId, const QString& commandId, const Items& items, bool partial,
    bool replace)
{
    if (queryId != m_queryId)
        return; // an answer to a query since typed over
    if (keywordMode()) {
        if (pluginId != m_keywordPlugin || commandId != m_keywordCommand)
            return; // the list is the keyword's command's alone
        const Entry* entry = m_catalog.find(pluginId);
        const Command* command = entry && entry->manifest ? entry->manifest->command(commandId) : nullptr;
        if (!command)
            return;
        if (replace || !m_assigned)
            m_keywordItemIndex = 0;
        SearchResults rows = rowsOf(*entry, *command, items, false, m_keywordItemIndex);
        m_keywordItemIndex += static_cast<int>(items.size());
        if (replace || !m_assigned) {
            m_sink.assign(std::move(rows));
            m_assigned = true;
        } else {
            QSet<QString> ids;
            for (int i = 0; i < m_sink.count(); ++i)
                ids.insert(m_sink.row(i).path);
            rows.removeIf([&ids](const SearchResult& row) { return ids.contains(row.path); });
            if (rows.size() > std::max(0, kMaxItems - m_sink.count()))
                rows.resize(std::max(0, kMaxItems - m_sink.count()));
            m_sink.append(rows);
        }
        return;
    }
    // Push mode: a command's batches add up until its final answer, which
    // goes in once the name search's rows are there (positions depend on
    // scores, so partial batches are not placed one by one).
    Batch& batch = m_held[pluginId + u'/' + commandId];
    batch.pluginId = pluginId;
    batch.commandId = commandId;
    if (replace)
        batch.items = items;
    else {
        for (const Item& item : items) {
            if (batch.items.size() >= kMaxItems)
                break;
            batch.items.append(item);
        }
    }
    batch.complete = !partial;
    if (batch.complete && m_namesReady) {
        const Batch done = std::move(batch);
        m_held.erase(pluginId + u'/' + commandId);
        apply(done);
    }
}

void Merge::replace(const QString& pluginId, const QString& commandId, const Items& items)
{
    const Entry* entry = m_catalog.find(pluginId);
    const Command* command = entry && entry->manifest ? entry->manifest->command(commandId) : nullptr;
    if (!command)
        return;
    m_sink.assign(rowsOf(*entry, *command, items, false));
    m_assigned = true;
    m_keywordItemIndex = static_cast<int>(items.size());
}

// The first row a pushed row may take: after the last row that is recent, a
// favorite, or an app named just what was typed, and below the keyboard's row
// unless that is the first.
int Merge::floorRow() const
{
    const int current = m_sink.currentRow();
    int floor = current > 0 ? current + 1 : 0;
    for (int i = 0; i < m_sink.count(); ++i) {
        const SearchResult& r = m_sink.row(i);
        if (r.isWindow() || r.recent || r.favorite || (r.exact && r.isApp()))
            floor = std::max(floor, i + 1);
    }
    return std::min(floor, m_sink.count());
}

// Where a content provider's first rows go (kFirstScreenRows): past the rows
// placeFor() keeps clear and the first kFirstScreenNames name rows, and past
// the provider rows already there, so they stay in the provider's order.
int Merge::firstScreenRow() const
{
    const int count = m_sink.count();
    int at = floorRow();
    for (int names = 0; at < count && names < kFirstScreenNames; ++at)
        names += !m_sink.row(at).isPlugin();
    while (at < count && contentRow(m_sink.row(at)))
        ++at;
    return at;
}

// Where a pushed row of `score` goes: after the last row that is recent
// (an app or a file opened lately), a favorite, or an app named just what was typed,
// below the keyboard's row unless that is the first, then past the apps
// that match as well or better, before the files. Files named just what
// was typed do not hold it down: "report" finds dozens of report.md, and a
// row below them all would be out of sight (the design's "完全匹配" read as
// the apps'; a decision for the user to confirm).
int Merge::placeFor(int score) const
{
    const int count = m_sink.count();
    int at = floorRow();
    while (at < count && m_sink.row(at).isApp() && m_sink.row(at).score >= score)
        ++at;
    return at;
}

void Merge::apply(const Batch& batch)
{
    const Entry* entry = m_catalog.find(batch.pluginId);
    const Command* command = entry && entry->manifest ? entry->manifest->command(batch.commandId) : nullptr;
    if (!command)
        return;
    if (batch.pluginId == m_alonePlugin && batch.commandId == m_aloneCommand) {
        applyAlone(batch);
        return;
    }
    SearchResults rows = rowsOf(*entry, *command, batch.items, true);
    const bool content = command->contentSearch;
    if (content) {
        rows.removeIf([&](const SearchResult& row) {
            if (!row.plugin)
                return true;
            // A row that is no file stays when the plugin tells what it is
            // (a mail: "details", GitHub #202); other such rows (setup hints) go.
            if (row.plugin->openKind != PluginRow::OpenKind::Path)
                return !asksDetails(*row.plugin);
            for (int i = 0; i < m_sink.count(); ++i) {
                const SearchResult& other = m_sink.row(i);
                if (!other.isPlugin() && other.path.compare(row.plugin->openTarget, Qt::CaseInsensitive) == 0)
                    return true;
            }
            return false;
        });
    }

    // The quota: this plugin's other commands and the other plugins count.
    int samePlugin = 0;
    int others = 0;
    std::map<QString, int> existing; // item path -> row, of this command
    for (int i = 0; i < m_sink.count(); ++i) {
        const SearchResult& r = m_sink.row(i);
        if (!r.isPlugin() || isHintRow(r))
            continue; // the hint row is nobody's answer: outside the quota
        if (!content && contentRow(r))
            continue;
        if (r.plugin->pluginId != batch.pluginId)
            ++others;
        else if (r.plugin->commandId != batch.commandId)
            ++samePlugin;
        else
            existing.emplace(r.path, i);
    }
    const int cap = content ? kMaxItems : std::max(0, std::min(kPerPlugin - samePlugin, kTotal - others - samePlugin));
    const bool cut = rows.size() > cap;
    rows.resize(std::min<qsizetype>(rows.size(), cap));
    if (cut) {
        const QStringList keywords = m_catalog.keywordsOf(batch.pluginId, batch.commandId);
        if (!keywords.isEmpty() && m_moreText.contains(u"%1")) {
            for (SearchResult& r : rows) {
                auto p = std::make_shared<PluginRow>(*r.plugin);
                p->subtitle += m_moreText.arg(keywords.first());
                r.plugin.row = std::move(p);
            }
        }
    }

    // Rows still there are rewritten where they are; then the ones gone are
    // removed (from the end: the rows before keep their numbers); then the
    // new ones go in, each placed against the list as it is by then.
    QSet<QString> kept;
    for (const SearchResult& r : rows) {
        if (const auto it = existing.find(r.path); it != existing.end()) {
            m_sink.update(it->second, r);
            kept.insert(r.path);
        }
    }
    std::vector<int> gone;
    for (const auto& [path, index] : existing) {
        if (!kept.contains(path))
            gone.push_back(index);
    }
    std::sort(gone.rbegin(), gone.rend());
    for (const int index : gone)
        m_sink.remove(index);
    // A content provider's first rows are on the first screen (they would
    // be out of sight past the names, up to a page of them); the rest
    // follow the native matches (contentEnd). Content only has no names.
    int onList = 0;
    if (content) {
        for (int i = 0; i < m_sink.count(); ++i)
            onList += contentRow(m_sink.row(i));
    }
    for (const SearchResult& r : rows) {
        if (kept.contains(r.path))
            continue;
        const int at = !content ? placeFor(r.score)
            : onList < kFirstScreenRows && !m_contentOnly ? firstScreenRow()
                                                          : m_sink.count();
        ++onList;
        m_betterFirstRow = m_betterFirstRow || (at == 0 && m_sink.currentRow() == 0);
        m_sink.insert(at, r);
    }
}

// The answer of the instant command whose keyword was typed alone: its rows
// as its own list would have them (every one, the plugin's order), at the
// top of the search's rows; below the keyboard's row once the user has moved
// it. Answering again, the rows still there are rewritten where they are.
void Merge::applyAlone(const Batch& batch)
{
    const Entry* entry = m_catalog.find(batch.pluginId);
    const Command* command = entry && entry->manifest ? entry->manifest->command(batch.commandId) : nullptr;
    if (!command)
        return;
    const SearchResults rows = rowsOf(*entry, *command, batch.items, false);
    QSet<QString> wanted;
    for (const SearchResult& r : rows)
        wanted.insert(r.path);
    // The keyboard's row by what it is: the rows taken out below move it.
    const int current = m_sink.currentRow();
    const QString below = current > 0 && current < m_sink.count() ? m_sink.row(current).path : QString();
    // What is not in the answer goes (the row that stood in for it too),
    // from the end: the rows before keep their numbers.
    for (int i = m_sink.count() - 1; i >= 0; --i) {
        if (keywordAlone(m_sink.row(i)) && !wanted.contains(m_sink.row(i).path))
            m_sink.remove(i);
    }
    int at = 0;
    for (int i = 0; i < m_sink.count() && !below.isEmpty(); ++i) {
        if (m_sink.row(i).path == below) {
            at = i + 1;
            break;
        }
    }
    for (const SearchResult& r : rows) {
        int was = -1;
        for (int i = 0; i < m_sink.count() && was < 0; ++i) {
            if (m_sink.row(i).path == r.path)
                was = i;
        }
        if (was >= 0) {
            m_sink.update(was, r);
            at = was + 1;
            continue;
        }
        m_betterFirstRow = m_betterFirstRow || (at == 0 && current == 0);
        m_sink.insert(at++, r);
    }
}

} // namespace ws::plugin
