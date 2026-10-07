#pragma once

#include <QString>
#include <QStringList>

namespace qf {

// Most-recently-opened paths, newest first. Shown for an empty query and
// boosted to the top when they match.
class History {
public:
    explicit History(QString filePath, int capacity = 200);

    void load();
    void record(const QString& path);
    void remove(const QString& path);
    const QStringList& items() const noexcept { return m_items; }

private:
    void save() const;

    QString m_filePath;
    int m_capacity;
    QStringList m_items;
};

} // namespace qf
