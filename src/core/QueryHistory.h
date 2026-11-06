#pragma once

#include <QStringList>
#include <optional>

namespace ws {
// Exact, completed queries; separate from the normalized ranking history.
class QueryHistory {
public:
    explicit QueryHistory(QString fileName, int capacity = 100);
    void load();
    bool record(const QString& query);
    bool remove(int index);
    bool clear();
    const QStringList& items() const { return m_items; }
    std::optional<QString> step(const QString& draft, int direction);
    void resetNavigation();

private:
    bool save() const;
    QString m_fileName;
    QStringList m_items;
    QString m_draft;
    int m_capacity;
    int m_position = -1;
};
}
