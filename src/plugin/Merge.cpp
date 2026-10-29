#include "Merge.h"

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
    // The space after a list command's keyword opens it; an instant
    // command's keyword works alone (Catalog::match).
    p->autocomplete = command.mode == Command::Mode::Instant ? hint.keyword : hint.keyword + u' ';
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

void Merge::begin(std::uint64_t queryId, const QString& text, const QString& keywordPlugin, const QString& keywordCommand)
{
    m_queryId = queryId;
    m_text = text;
    m_keywordPlugin = keywordPlugin;
    m_keywordCommand = keywordCommand;
    m_namesReady = keywordMode(); // no name search to wait for
    m_assigned = false;
    m_held.clear();
    m_betterFirstRow = false;
}

void Merge::namesShown()
{
    m_namesReady = true;
    if (!keywordMode())
        placeHint();
    for (auto it = m_held.begin(); it != m_held.end();) {
        if (it->second.complete) {
            apply(it->second);
            it = m_held.erase(it);
        } else
            ++it;
    }
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

int Merge::pluginRows() const
{
    int n = 0;
    for (int i = 0; i < m_sink.count(); ++i)
        n += m_sink.row(i).isPlugin();
    return n;
}

// The rows of an answer, each item once (the first of an id wins), by score
// when asked (push mode; keyword mode only when some item gave one).
SearchResults Merge::rowsOf(const Entry& entry, const Command& command, const Items& items, bool sortByScore) const
{
    SearchResults rows;
    QSet<QString> ids;
    bool scored = false;
    for (int i = 0; i < items.size(); ++i) {
        SearchResult row = toRow(entry, command, items[i], i, m_locale);
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
        SearchResults rows = rowsOf(*entry, *command, items, false);
        if (replace || !m_assigned) {
            m_sink.assign(std::move(rows));
            m_assigned = true;
        } else
            m_sink.append(rows);
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
    else
        batch.items.append(items);
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
    const int current = m_sink.currentRow();
    int floor = current > 0 ? current + 1 : 0;
    for (int i = 0; i < count; ++i) {
        const SearchResult& r = m_sink.row(i);
        if (r.recent || r.favorite || (r.exact && r.isApp()))
            floor = std::max(floor, i + 1);
    }
    int at = std::min(floor, count);
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
    SearchResults rows = rowsOf(*entry, *command, batch.items, true);

    // The quota: this plugin's other commands and the other plugins count.
    int samePlugin = 0;
    int others = 0;
    std::map<QString, int> existing; // item path -> row, of this command
    for (int i = 0; i < m_sink.count(); ++i) {
        const SearchResult& r = m_sink.row(i);
        if (!r.isPlugin() || isHintRow(r))
            continue; // the hint row is nobody's answer: outside the quota
        if (r.plugin->pluginId != batch.pluginId)
            ++others;
        else if (r.plugin->commandId != batch.commandId)
            ++samePlugin;
        else
            existing.emplace(r.path, i);
    }
    const int cap = std::max(0, std::min(kPerPlugin - samePlugin, kTotal - others - samePlugin));
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
    for (const SearchResult& r : rows) {
        if (kept.contains(r.path))
            continue;
        const int at = placeFor(r.score);
        m_betterFirstRow = m_betterFirstRow || (at == 0 && m_sink.currentRow() == 0);
        m_sink.insert(at, r);
    }
}

} // namespace ws::plugin
