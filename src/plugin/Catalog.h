#pragma once

// The plugins installed (the plugin design, §1.2, §4.3, §4.6): every
// folder with a plugin.json under the user's plugins folder and the ones
// that came with Win顺 (the user's wins on the same id), read and checked;
// the user's folder watched for changes; the keywords and triggers of the
// enabled ones indexed, and keywords checked against each other and
// against the [WebSearch] keywords.

#include "Manifest.h"

#include <QHash>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ws {
class ChangeWatcher;
}

namespace ws::plugin {

struct Entry {
    enum class Source { User, Bundled };
    QString id; // the folder's name; the manifest's id must agree
    QString dir; // native, absolute
    Source source = Source::User;
    std::optional<Manifest> manifest; // nothing when `error` says why
    QString error;
    bool usable() const noexcept { return manifest.has_value(); }
};

// A plugin's keyword that cannot be used: what has it.
struct KeywordConflict {
    QString pluginId;
    QString commandId;
    QString keyword;
    // "builtin:<name>" (Win顺's own: the screenshot history), "web:<name>"
    // ([WebSearch]), "fav:<name>" (a favorite), "plugin:<id>/<command>"
    QString takenBy;
};

// One command triggered by a query.
struct Match {
    enum class Kind { Keyword, Regex, Push };
    const Entry* entry = nullptr;
    const Command* command = nullptr;
    Kind kind = Kind::Keyword;
    QString keyword; // Keyword: the one typed (hints(): the whole keyword)
    QString text; // Keyword: the words after it; else the whole input
    QStringList captures; // Regex: the groups
    int maxItems = 0; // Push: the trigger's cap
    bool partial = false; // hints(): the keyword only begins with what was typed
};

class Catalog : public QObject {
    Q_OBJECT

public:
    // `userDir`: %LOCALAPPDATA%\WinShun\plugins (made when missing on the
    // first scan); `bundledDir`: <install dir>\plugins, may be empty.
    Catalog(QString userDir, QString bundledDir, QObject* parent = nullptr);
    ~Catalog() override;

    void scan(); // reads everything again; changed() when anything differs
    void setWatching(bool on); // the user's folder: a change there scans again (debounced)

    const std::vector<Entry>& entries() const noexcept { return m_entries; }
    const Entry* find(QStringView id) const;
    const QString& userDir() const noexcept { return m_userDir; }

    // From the settings: which plugins are on, the keywords the user
    // changed ("<id>/<command>" -> keyword), the [WebSearch] keywords and
    // the favorites' (keyword, name). Each rebuilds the index.
    void setEnabled(const QSet<QString>& ids);
    void setEnabled(const QString& id, bool on);
    // From the settings too: the permissions the user said yes to, by
    // plugin ("<id>" -> Confirmed=). From the first call on, a plugin of
    // the user's that is switched on but whose manifest asks for more (a
    // newer version put over it) waits for the user's OK: not enabled(),
    // so not started, queried, offered to the AI or heard, and out of the
    // index, until the settings say yes to the new ones too. The bundled
    // ones come with Win顺 and are trusted as they come. Without the call
    // (tests, wsbench, the preview) nothing waits.
    // Returns who waited until now and no longer does (the user said yes):
    // the caller clears their crash state, as switching them on would.
    QStringList setConfirmed(QHash<QString, QStringList> confirmed);
    // Switched on and waiting for nothing: it may run.
    bool enabled(QStringView id) const;
    // The switch as the settings have it, whatever it waits for.
    bool switchedOn(QStringView id) const { return m_enabled.contains(id.toString()); }
    // What a plugin that is switched on asks for beyond what the user said
    // yes to, in Win顺's order of the permissions (Permissions::names()): it
    // waits for their OK on these. Empty when it waits for nothing.
    QStringList awaitingConsent(QStringView id) const { return m_awaiting.value(id.toString()); }
    void setKeywordOverrides(QHash<QString, QString> overrides);
    void setWebKeywords(QList<std::pair<QString, QString>> keywords);
    void setFavoriteKeywords(QList<std::pair<QString, QString>> keywords);
    // Win顺's own keywords (the screenshot history's jt), (keyword, name):
    // they win over every plugin's too.
    void setBuiltinKeywords(QList<std::pair<QString, QString>> keywords);

    // The keywords in use: effective after overrides and conflicts.
    QStringList keywordsOf(QStringView id, QStringView command) const;
    std::vector<KeywordConflict> conflicts() const { return m_conflicts; }

    // What `text` triggers among the enabled plugins: at most one keyword
    // match (first), then regex and push matches. Only "全部" asks. A list
    // command's keyword counts only with a space after it ("kill ": typed
    // alone, "kill" is a name to search for; see hint()); an instant
    // command's counts alone too. Leading spaces are ignored, trailing ones
    // are not: give it the text as typed.
    std::vector<Match> match(const QString& text) const;
    // The list command whose keyword `text` is, with nothing after it: the
    // launcher offers it as a row ("kill — 结束进程（按空格进入）"). Nothing
    // when no enabled command has that keyword in use.
    std::optional<Match> hint(const QString& text) const;
    // The same, and then (`partial`) the commands whose keywords begin with
    // the word typed, once it has two characters: at most kPrefixHints,
    // the shortest keywords first, none when more than kPrefixCandidates
    // begin so (the word is anybody's). Each keyword once; an instant
    // command's too (typing it in full runs it).
    static constexpr int kPrefixHints = 2;
    static constexpr int kPrefixCandidates = 4;
    std::vector<Match> hints(const QString& text) const;

    // The enabled commands whose `files` trigger takes these files and
    // folders (`folders[i]`: paths[i] is one), for the result menu: each
    // trigger's kind, extensions and count must fit the whole selection.
    // In the catalog's order.
    struct FileCommand {
        const Entry* entry = nullptr;
        const Command* command = nullptr;
        const Trigger* trigger = nullptr;
    };
    std::vector<FileCommand> fileCommands(const QStringList& paths, const QList<bool>& folders) const;
    // `pluginId`'s `commandId` is one of `commands`: a menu's choice, looked
    // up again at the click by who it was, never by its place (GitHub #113).
    static bool hasFileCommand(const std::vector<FileCommand>& commands, QStringView pluginId, QStringView commandId);

signals:
    void changed(); // entries or the index
    // Files other than plugin.json changed under these plugins' folders (the
    // user's): their scripts, as far as the catalog can tell. For the host
    // in developer mode, which restarts them.
    void scriptsChanged(const QStringList& ids);

private:
    struct KeywordUse {
        QString pluginId;
        QString commandId;
        bool user = false; // the user set it
    };
    struct Compiled {
        const Entry* entry;
        const Command* command;
        const Trigger* trigger;
        QRegularExpression regex;
    };
    void scanFolder(const QString& folder, Entry::Source source, std::vector<Entry>& out);
    void rebuildIndex();
    void onFsChanges(const std::vector<QString>& paths); // the watcher's, on this thread
    const Command* commandOf(const KeywordUse& use, const Entry** entry) const;

    QString m_userDir;
    QString m_bundledDir;
    std::vector<Entry> m_entries;
    QSet<QString> m_enabled;
    bool m_holdToConfirmed = false; // setConfirmed() was called: the user's plugins wait for their OK on more
    QHash<QString, QStringList> m_confirmed;
    QHash<QString, QStringList> m_awaiting; // id -> what it waits for the user's OK on (rebuildIndex)
    QHash<QString, QString> m_overrides;
    QList<std::pair<QString, QString>> m_web;
    QList<std::pair<QString, QString>> m_favorites;
    QList<std::pair<QString, QString>> m_builtin;
    QHash<QString, KeywordUse> m_keywords; // lower-cased keyword -> the command it opens
    QHash<QString, QStringList> m_effective; // "<id>/<command>" -> its keywords in use
    std::vector<KeywordConflict> m_conflicts;
    std::vector<Compiled> m_regexes;
    std::vector<Compiled> m_pushes;
    std::unique_ptr<ChangeWatcher> m_watcher;
    QTimer m_rescan;
    QSet<QString> m_scriptHits; // plugins whose files changed, until m_scriptTimer tells
    QTimer m_scriptTimer;
};

} // namespace ws::plugin
