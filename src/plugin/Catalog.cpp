#include "Catalog.h"

#include "ChangeWatcher.h"
#include "Install.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::plugin {

Catalog::Catalog(QString userDir, QString bundledDir, QObject* parent)
    : QObject(parent)
    , m_userDir(QDir::toNativeSeparators(std::move(userDir)))
    , m_bundledDir(QDir::toNativeSeparators(std::move(bundledDir)))
{
    // Editors write a file in several steps: one scan after the last.
    m_rescan.setSingleShot(true);
    m_rescan.setInterval(300);
    connect(&m_rescan, &QTimer::timeout, this, &Catalog::scan);
    m_scriptTimer.setSingleShot(true);
    m_scriptTimer.setInterval(300);
    connect(&m_scriptTimer, &QTimer::timeout, this, [this] {
        QStringList ids(m_scriptHits.begin(), m_scriptHits.end());
        m_scriptHits.clear();
        ids.sort();
        emit scriptsChanged(ids);
    });
}

Catalog::~Catalog() = default;

void Catalog::scanFolder(const QString& folder, Entry::Source source, std::vector<Entry>& out)
{
    if (folder.isEmpty())
        return;
    const QDir dir(folder);
    for (const QFileInfo& info : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString manifestPath = info.filePath() + u"/plugin.json"_s;
        if (!QFileInfo::exists(manifestPath))
            continue;
        Entry entry;
        entry.id = info.fileName();
        entry.dir = QDir::toNativeSeparators(info.absoluteFilePath());
        entry.source = source;
        if (std::any_of(out.begin(), out.end(), [&](const Entry& e) { return e.id == entry.id; }))
            continue; // the user's folder was read first and wins
        entry.manifest = Manifest::load(manifestPath, &entry.error);
        if (entry.manifest && entry.manifest->id != entry.id) {
            entry.error = u"文件夹叫 %1，manifest 里的 id 是 %2：两者要一样"_s.arg(entry.id, entry.manifest->id);
            entry.manifest.reset();
        }
        out.push_back(std::move(entry));
    }
}

void Catalog::scan()
{
    if (!m_userDir.isEmpty())
        QDir().mkpath(m_userDir);
    std::vector<Entry> fresh;
    scanFolder(m_userDir, Entry::Source::User, fresh);
    scanFolder(m_bundledDir, Entry::Source::Bundled, fresh);
    const bool same = fresh.size() == m_entries.size()
        && std::equal(fresh.begin(), fresh.end(), m_entries.begin(), [](const Entry& a, const Entry& b) {
               return a.id == b.id && a.dir == b.dir && a.source == b.source && a.error == b.error && a.manifest.has_value() == b.manifest.has_value()
                   && (!a.manifest || *a.manifest == *b.manifest);
           });
    if (same)
        return;
    m_entries = std::move(fresh);
    rebuildIndex();
    emit changed();
}

void Catalog::setWatching(bool on)
{
    if (!on) {
        m_watcher.reset();
        return;
    }
    if (m_watcher || m_userDir.isEmpty())
        return;
    QDir().mkpath(m_userDir);
    // The watcher's thread: over to this one with the paths, which say
    // whether a manifest changed (a scan) or some other file of a plugin's
    // (its scripts: scriptsChanged, for developer mode).
    m_watcher = std::make_unique<ChangeWatcher>([this](std::vector<FsChange>&& changes) {
        std::vector<QString> paths;
        paths.reserve(changes.size());
        for (const FsChange& c : changes)
            paths.push_back(QString::fromStdWString(c.path));
        QMetaObject::invokeMethod(this, [this, paths = std::move(paths)] { onFsChanges(paths); }, Qt::QueuedConnection);
    });
    m_watcher->setRoots({m_userDir.toStdWString()});
}

void Catalog::onFsChanges(const std::vector<QString>& paths)
{
    bool manifest = false;
    for (const QString& path : paths) {
        const QString native = QDir::toNativeSeparators(path);
        if (!native.startsWith(m_userDir, Qt::CaseInsensitive)) {
            manifest = true; // an overflow, or something else: look at everything
            continue;
        }
        const QString relative = native.mid(m_userDir.size() + 1);
        const qsizetype slash = relative.indexOf(u'\\');
        const QString id = slash < 0 ? relative : relative.left(slash);
        const QString file = slash < 0 ? QString() : relative.mid(slash + 1);
        // A folder appearing or going, or a manifest: the entries change.
        if (file.isEmpty() || file.compare(u"plugin.json", Qt::CaseInsensitive) == 0)
            manifest = true;
        else if (find(id))
            m_scriptHits.insert(id);
    }
    if (manifest)
        m_rescan.start();
    if (!m_scriptHits.isEmpty())
        m_scriptTimer.start();
}

const Entry* Catalog::find(QStringView id) const
{
    for (const Entry& e : m_entries) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

void Catalog::setEnabled(const QSet<QString>& ids)
{
    if (m_enabled == ids)
        return;
    m_enabled = ids;
    rebuildIndex();
    emit changed();
}

void Catalog::setEnabled(const QString& id, bool on)
{
    if (on == m_enabled.contains(id))
        return;
    if (on)
        m_enabled.insert(id);
    else
        m_enabled.remove(id);
    rebuildIndex();
    emit changed();
}

QStringList Catalog::setConfirmed(QHash<QString, QStringList> confirmed)
{
    if (m_holdToConfirmed && m_confirmed == confirmed)
        return {};
    const QList<QString> waiting = m_awaiting.keys();
    m_holdToConfirmed = true;
    m_confirmed = std::move(confirmed);
    rebuildIndex();
    QStringList allowed; // waited before, waits for nothing now
    for (const QString& id : waiting) {
        if (!m_awaiting.contains(id))
            allowed.push_back(id);
    }
    emit changed();
    return allowed;
}

bool Catalog::enabled(QStringView id) const
{
    const QString key = id.toString();
    return m_enabled.contains(key) && !m_awaiting.contains(key);
}

void Catalog::setKeywordOverrides(QHash<QString, QString> overrides)
{
    if (m_overrides == overrides)
        return;
    m_overrides = std::move(overrides);
    rebuildIndex();
    emit changed();
}

void Catalog::setWebKeywords(QList<std::pair<QString, QString>> keywords)
{
    if (m_web == keywords)
        return;
    m_web = std::move(keywords);
    rebuildIndex();
    emit changed();
}

void Catalog::setBuiltinKeywords(QList<std::pair<QString, QString>> keywords)
{
    if (m_builtin == keywords)
        return;
    m_builtin = std::move(keywords);
    rebuildIndex();
    emit changed();
}

void Catalog::setFavoriteKeywords(QList<std::pair<QString, QString>> keywords)
{
    if (m_favorites == keywords)
        return;
    m_favorites = std::move(keywords);
    rebuildIndex();
    emit changed();
}

QStringList Catalog::keywordsOf(QStringView id, QStringView command) const
{
    return m_effective.value(id.toString() + u'/' + command.toString());
}

void Catalog::rebuildIndex()
{
    // Who waits for the user's OK first (see setConfirmed): the index
    // leaves them out, keywords and triggers alike.
    m_awaiting.clear();
    if (m_holdToConfirmed) {
        for (const Entry& entry : m_entries) {
            if (entry.source != Entry::Source::User || !entry.manifest || !m_enabled.contains(entry.id))
                continue;
            QStringList more = install::unconfirmed(*entry.manifest, m_confirmed.value(entry.id));
            if (!more.isEmpty())
                m_awaiting.insert(entry.id, std::move(more));
        }
    }

    m_keywords.clear();
    m_effective.clear();
    m_conflicts.clear();
    m_regexes.clear();
    m_pushes.clear();

    // Who wants which keyword. The [WebSearch] entries and the favorites'
    // win over every plugin's; keywords the user set win over plugins'
    // defaults; two defaults alike stop each other (nobody wins until the
    // user picks).
    struct Claim {
        const Entry* entry;
        const Command* command;
        QString keyword; // as written
        bool user;
    };
    QHash<QString, QString> web; // lower-cased keyword -> name
    for (const auto& [keyword, name] : m_web)
        web.insert(keyword.toLower(), name.isEmpty() ? keyword : name);
    QHash<QString, QString> favorites; // the same
    QHash<QString, QString> builtin; // the same
    for (const auto& [keyword, name] : m_builtin)
        builtin.insert(keyword.toLower(), name.isEmpty() ? keyword : name);
    for (const auto& [keyword, name] : m_favorites)
        favorites.insert(keyword.toLower(), name.isEmpty() ? keyword : name);
    std::vector<Claim> claims;
    for (const Entry& entry : m_entries) {
        if (!entry.manifest || !enabled(entry.id))
            continue;
        for (const Command& command : entry.manifest->commands) {
            const QString key = entry.id + u'/' + command.id;
            if (const auto it = m_overrides.find(key); it != m_overrides.end()) {
                if (!it->isEmpty())
                    claims.push_back({&entry, &command, *it, true});
                continue; // the user's choice replaces the defaults, even with nothing
            }
            for (const QString& keyword : command.keywords)
                claims.push_back({&entry, &command, keyword, false});
        }
    }
    for (const Claim& claim : claims) {
        const QString lower = claim.keyword.toLower();
        QString takenBy;
        if (const auto own = builtin.find(lower); own != builtin.end())
            takenBy = u"builtin:"_s + *own;
        else if (const auto it = web.find(lower); it != web.end())
            takenBy = u"web:"_s + *it;
        else if (const auto fav = favorites.find(lower); fav != favorites.end())
            takenBy = u"fav:"_s + *fav;
        else {
            for (const Claim& other : claims) {
                if (&other == &claim || other.keyword.toLower() != lower)
                    continue;
                if (other.user && !claim.user) {
                    takenBy = u"plugin:"_s + other.entry->id + u'/' + other.command->id;
                    break;
                }
                if (!other.user && !claim.user) {
                    takenBy = u"plugin:"_s + other.entry->id + u'/' + other.command->id;
                    break;
                }
                // Both the user's: the first in the list keeps it.
                if (other.user && claim.user && &other < &claim) {
                    takenBy = u"plugin:"_s + other.entry->id + u'/' + other.command->id;
                    break;
                }
            }
        }
        if (!takenBy.isEmpty()) {
            m_conflicts.push_back({claim.entry->id, claim.command->id, claim.keyword, takenBy});
            continue;
        }
        m_keywords.insert(lower, {claim.entry->id, claim.command->id, claim.user});
        m_effective[claim.entry->id + u'/' + claim.command->id].push_back(claim.keyword);
    }

    for (const Entry& entry : m_entries) {
        if (!entry.manifest || !enabled(entry.id))
            continue;
        for (const Command& command : entry.manifest->commands) {
            for (const Trigger& trigger : command.triggers) {
                if (trigger.kind == Trigger::Kind::Regex) {
                    // Compiled once here, not per keystroke (pitfalls: 循环里现建正则).
                    QRegularExpression regex(trigger.regex);
                    regex.optimize();
                    m_regexes.push_back({&entry, &command, &trigger, std::move(regex)});
                } else if (trigger.kind == Trigger::Kind::Push)
                    m_pushes.push_back({&entry, &command, &trigger, {}});
            }
        }
    }
}

const Command* Catalog::commandOf(const KeywordUse& use, const Entry** entry) const
{
    const Entry* e = find(use.pluginId);
    *entry = e;
    return e && e->manifest ? e->manifest->command(use.commandId) : nullptr;
}

std::vector<Match> Catalog::match(const QString& typed) const
{
    std::vector<Match> out;
    // Leading spaces mean nothing; a trailing one is what opens a list
    // command's keyword, so the text keeps it (see hint()).
    qsizetype start = 0;
    while (start < typed.size() && typed[start].isSpace())
        ++start;
    const QString text = typed.mid(start);
    if (text.isEmpty())
        return out;
    // A keyword: the first word, followed by a space and the words (an
    // instant command's: alone as well).
    const qsizetype space = text.indexOf(u' ');
    const QString word = (space < 0 ? text : text.left(space)).toLower();
    bool keywordMode = false;
    if (!word.isEmpty()) {
        if (const auto it = m_keywords.find(word); it != m_keywords.end()) {
            const Entry* entry = nullptr;
            const Command* command = commandOf(*it, &entry);
            if (command && (space >= 0 || command->mode == Command::Mode::Instant)) {
                Match m;
                m.entry = entry;
                m.command = command;
                m.kind = Match::Kind::Keyword;
                m.keyword = space < 0 ? text : text.left(space);
                m.text = space < 0 ? QString() : text.mid(space + 1).trimmed();
                out.push_back(std::move(m));
                keywordMode = true;
            }
        }
    }
    // The triggers see the words as the name search does: without the
    // spaces around them.
    const QString plain = text.trimmed();
    for (const Compiled& c : m_regexes) {
        if (plain.size() < c.trigger->minLength || plain.size() > c.trigger->maxLength)
            continue;
        const QRegularExpressionMatch found = c.regex.match(plain);
        if (!found.hasMatch())
            continue;
        Match m;
        m.entry = c.entry;
        m.command = c.command;
        m.kind = Match::Kind::Regex;
        m.text = plain;
        for (int i = 1; i <= found.lastCapturedIndex(); ++i)
            m.captures.push_back(found.captured(i));
        out.push_back(std::move(m));
    }
    if (!keywordMode && !plain.isEmpty()) {
        for (const Compiled& c : m_pushes) {
            if (plain.size() < c.trigger->minLength || plain.size() > c.trigger->maxLength)
                continue;
            Match m;
            m.entry = c.entry;
            m.command = c.command;
            m.kind = Match::Kind::Push;
            m.text = plain;
            m.maxItems = c.trigger->maxItems;
            out.push_back(std::move(m));
        }
    }
    return out;
}

std::optional<Match> Catalog::hint(const QString& typed) const
{
    const QString word = typed.trimmed();
    if (word.isEmpty() || word.contains(u' ') || typed.endsWith(u' '))
        return std::nullopt;
    const auto it = m_keywords.find(word.toLower());
    if (it == m_keywords.end())
        return std::nullopt;
    const Entry* entry = nullptr;
    const Command* command = commandOf(*it, &entry);
    if (!command || command->mode != Command::Mode::List)
        return std::nullopt;
    Match m;
    m.entry = entry;
    m.command = command;
    m.kind = Match::Kind::Keyword;
    m.keyword = word;
    return m;
}

std::vector<Match> Catalog::hints(const QString& typed) const
{
    std::vector<Match> out;
    if (const std::optional<Match> exact = hint(typed))
        out.push_back(*exact);
    const QString word = typed.trimmed().toLower();
    if (word.size() < 2 || word.contains(u' ') || typed.endsWith(u' '))
        return out;
    // The keywords in use that begin so, as written (m_effective keeps the case).
    QStringList candidates;
    for (const QStringList& keywords : m_effective) {
        for (const QString& keyword : keywords) {
            if (keyword.size() > word.size() && keyword.startsWith(word, Qt::CaseInsensitive))
                candidates.push_back(keyword);
        }
    }
    if (candidates.isEmpty() || candidates.size() > kPrefixCandidates)
        return out;
    std::stable_sort(candidates.begin(), candidates.end(), [](const QString& a, const QString& b) {
        return a.size() != b.size() ? a.size() < b.size() : a.compare(b, Qt::CaseInsensitive) < 0;
    });
    int partial = 0;
    for (const QString& keyword : candidates) {
        if (partial >= kPrefixHints)
            break;
        const auto use = m_keywords.constFind(keyword.toLower());
        if (use == m_keywords.constEnd())
            continue;
        const Entry* entry = nullptr;
        const Command* command = commandOf(*use, &entry);
        if (!command)
            continue;
        if (std::any_of(out.begin(), out.end(), [&](const Match& m) { return m.command == command; }))
            continue; // the exact one, or another keyword of the same command
        Match m;
        m.entry = entry;
        m.command = command;
        m.kind = Match::Kind::Keyword;
        m.keyword = keyword;
        m.partial = true;
        out.push_back(std::move(m));
        ++partial;
    }
    return out;
}

std::vector<Catalog::FileCommand> Catalog::fileCommands(const QStringList& paths, const QList<bool>& folders) const
{
    std::vector<FileCommand> out;
    if (paths.isEmpty() || folders.size() != paths.size())
        return out;
    const bool anyFolder = folders.contains(true);
    const bool anyFile = folders.contains(false);
    for (const Entry& e : m_entries) {
        if (!e.usable() || !enabled(e.id))
            continue;
        for (const Command& c : e.manifest->commands) {
            for (const Trigger& t : c.triggers) {
                if (t.kind != Trigger::Kind::Files || paths.size() > t.maxFiles)
                    continue;
                if ((t.fileKind == u"file" && anyFolder) || (t.fileKind == u"folder" && anyFile))
                    continue;
                bool extensionsFit = true;
                for (qsizetype i = 0; i < paths.size() && extensionsFit && !t.extensions.isEmpty(); ++i)
                    extensionsFit = folders[i] || t.extensions.contains(QFileInfo(paths[i]).suffix().toLower());
                if (!extensionsFit)
                    continue;
                out.push_back({&e, &c, &t});
                break; // one entry per command
            }
        }
    }
    return out;
}

bool Catalog::hasFileCommand(const std::vector<FileCommand>& commands, QStringView pluginId, QStringView commandId)
{
    return std::any_of(commands.begin(), commands.end(),
        [&](const FileCommand& c) { return c.entry && c.command && c.entry->id == pluginId && c.command->id == commandId; });
}

} // namespace ws::plugin
