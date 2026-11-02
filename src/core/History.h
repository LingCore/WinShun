#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace ws {

// Most-recently-opened paths, newest first. Shown for an empty query and
// boosted to the top when they match. Each also counts how often it was
// opened and when last (the frequent apps over the taskbar).
//
// One line per path: "path\tcount\tlast ms\tweight" and, for each text typed
// before opening it, "\ttext\tweight\tlast ms". Older files have fewer of
// these: the path with the count and time since counts were kept, the path
// alone before (read as opened once, at an unknown time). Older builds read
// the first three and leave the rest. No path or text holds a tab: Windows
// does not allow one in a name, and a text has its tabs made spaces.
//
// A weight is how much something was opened, lately counting more
// (frecency): each opening adds 1 to what the weight has decayed to, which
// halves every 14 days. Kept for the path, and for each of up to 8 texts
// typed to reach it.
class History {
public:
    struct Use {
        int count = 0;
        qint64 last = 0; // ms since the epoch; 0: unknown (kept before counts were)
    };

    struct Affinity {
        int group = 3; // exact text, containing text, contained text, entity only
        double weight = 0;
        qint64 last = 0;

        // One recent opening is enough, including one found with another
        // query. Search still decides whether the entity actually matches.
        bool learned() const noexcept { return weight >= 0.5; }
        int bonus() const noexcept;
        double preferenceWeight() const noexcept;
    };
    using Affinities = QHash<QString, Affinity>; // by pathKey(), a value snapshot for search workers

    explicit History(QString filePath, int capacity = 200);

    void load();
    // `query`: what was typed to find it (empty: not found by typing, which
    // only counts the path). Kept as normalize() makes it.
    void record(const QString& path, const QString& query, qint64 now);
    void record(const QString& path, qint64 now) { record(path, QString(), now); }
    void remove(const QString& path);
    void clear();
    const QStringList& items() const noexcept { return m_items; }
    Use use(const QString& path) const; // count 0 when not there

    // The paths for what is being typed, best first: those opened for just
    // this text, then for a text containing this one, then for a text
    // contained in this one, then the rest; each group by its weight
    // for that text (the rest by the path's own), a tie to the newer. The
    // items() again, newest first, when nothing is typed.
    QStringList ranked(const QString& query, qint64 now) const;
    Affinities affinities(const QString& query, qint64 now) const;
    static Affinity affinity(const Affinities& affinities, const QString& path);
    // A text as it is kept and compared: no case, no filters (size:, dm:,
    // ext:) and no folder to look in (abc/), spaces as one, up to 64 characters.
    static QString normalize(const QString& query);

private:
    struct Typed {
        QString text; // normalize()d, never empty
        double weight = 0; // as of `last`
        qint64 last = 0;
    };
    struct Entry {
        Use use;
        double weight = 0; // as of use.last (that 0: unknown, taken as old)
        QList<Typed> typed;
    };

    void save() const;
    static QString key(const QString& path);

    QString m_filePath;
    int m_capacity;
    QStringList m_items;
    QHash<QString, Entry> m_uses; // by key()
};

} // namespace ws
