#include "History.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace ws {

History::History(QString filePath, int capacity)
    : m_filePath(std::move(filePath))
    , m_capacity(capacity)
{
}

void History::load()
{
    m_items.clear();
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    while (!file.atEnd() && m_items.size() < m_capacity) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (!line.isEmpty() && !m_items.contains(line, Qt::CaseInsensitive))
            m_items.append(line);
    }
}

void History::record(const QString& path)
{
    if (path.isEmpty())
        return;
    m_items.removeIf([&](const QString& p) { return p.compare(path, Qt::CaseInsensitive) == 0; });
    m_items.prepend(path);
    while (m_items.size() > m_capacity)
        m_items.removeLast();
    save();
}

void History::remove(const QString& path)
{
    if (m_items.removeIf([&](const QString& p) { return p.compare(path, Qt::CaseInsensitive) == 0; }) > 0)
        save();
}

void History::save() const
{
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    for (const QString& item : m_items) {
        file.write(item.toUtf8());
        file.write("\n");
    }
    file.commit();
}

} // namespace ws
