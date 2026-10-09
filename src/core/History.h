#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace ws {

// Most-recently-opened paths, newest first. Shown for an empty query and
// boosted to the top when they match. Each also counts how often it was
// opened and when last (the frequent apps over the taskbar).
//
// One line per path; "path\tcount\tlast ms" since counts were kept, the path
// alone before (read as opened once, at an unknown time). No path holds a
// tab: Windows does not allow one in a name.
class History {
public:
    struct Use {
        int count = 0;
        qint64 last = 0; // ms since the epoch; 0: unknown (kept before counts were)
    };

    explicit History(QString filePath, int capacity = 200);

    void load();
    void record(const QString& path, qint64 now);
    void remove(const QString& path);
    void clear();
    const QStringList& items() const noexcept { return m_items; }
    Use use(const QString& path) const; // count 0 when not there

private:
    void save() const;
    static QString key(const QString& path) { return path.toCaseFolded(); }

    QString m_filePath;
    int m_capacity;
    QStringList m_items;
    QHash<QString, Use> m_uses; // by key()
};

} // namespace ws
