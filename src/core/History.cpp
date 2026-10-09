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
    m_uses.clear();
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    while (!file.atEnd() && m_items.size() < m_capacity) {
        const QStringList fields = QString::fromUtf8(file.readLine()).trimmed().split(u'\t');
        const QString& path = fields.first();
        if (path.isEmpty() || m_uses.contains(key(path)))
            continue;
        Use use {1, 0};
        if (fields.size() >= 3) {
            use.count = std::max(1, fields[1].toInt());
            use.last = std::max<qint64>(0, fields[2].toLongLong());
        }
        m_items.append(path);
        m_uses.insert(key(path), use);
    }
}

void History::record(const QString& path, qint64 now)
{
    if (path.isEmpty())
        return;
    m_items.removeIf([&](const QString& p) { return p.compare(path, Qt::CaseInsensitive) == 0; });
    m_items.prepend(path);
    Use& use = m_uses[key(path)];
    ++use.count;
    use.last = now;
    while (m_items.size() > m_capacity)
        m_uses.remove(key(m_items.takeLast()));
    save();
}

void History::remove(const QString& path)
{
    if (m_items.removeIf([&](const QString& p) { return p.compare(path, Qt::CaseInsensitive) == 0; }) > 0) {
        m_uses.remove(key(path));
        save();
    }
}

void History::clear()
{
    if (m_items.isEmpty())
        return;
    m_items.clear();
    m_uses.clear();
    save();
}

History::Use History::use(const QString& path) const
{
    return m_uses.value(key(path));
}

void History::save() const
{
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    for (const QString& item : m_items) {
        const Use use = m_uses.value(key(item));
        file.write(item.toUtf8());
        file.write("\t" + QByteArray::number(use.count) + "\t" + QByteArray::number(use.last) + "\n");
    }
    file.commit();
}

} // namespace ws
