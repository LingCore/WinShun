#include "QueryHistory.h"

#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>

namespace ws {
namespace {
QString clean(QString query)
{
    query.replace(u'\r', u' ').replace(u'\n', u' ').replace(u'\t', u' ');
    return query.trimmed();
}
bool usable(const QString& query) { return !query.isEmpty() && query.size() <= 2048 && !query.contains(QChar::Null); }
}

QueryHistory::QueryHistory(QString fileName, int capacity)
    : m_fileName(std::move(fileName)), m_capacity(std::clamp(capacity, 1, 100)) {}

void QueryHistory::load()
{
    m_items.clear();
    resetNavigation();
    QFile file(m_fileName);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return;
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray()) return;
    for (const auto& value : document.array()) {
        const QString query = clean(value.toString());
        if (usable(query) && !m_items.contains(query)) m_items.append(query);
        if (m_items.size() == m_capacity) break;
    }
}

bool QueryHistory::record(const QString& text)
{
    const QString query = clean(text);
    if (!usable(query) || (!m_items.isEmpty() && m_items.first() == query)) return false;
    m_items.removeAll(query);
    m_items.prepend(query);
    if (m_items.size() > m_capacity) m_items.removeLast();
    resetNavigation();
    if (!save()) qWarning("Could not save query history");
    return true;
}

bool QueryHistory::remove(int index)
{
    if (index < 0 || index >= m_items.size()) return false;
    m_items.removeAt(index);
    resetNavigation();
    if (!save()) qWarning("Could not save query history");
    return true;
}

bool QueryHistory::clear()
{
    if (m_items.isEmpty()) return false;
    m_items.clear();
    resetNavigation();
    if (!save()) qWarning("Could not clear query history");
    return true;
}

void QueryHistory::resetNavigation() { m_position = -1; m_draft.clear(); }

std::optional<QString> QueryHistory::step(const QString& draft, int direction)
{
    if (m_items.isEmpty() || direction == 0 || (m_position < 0 && direction < 0)) return std::nullopt;
    if (m_position < 0) m_draft = draft;
    m_position = std::clamp(m_position + (direction > 0 ? 1 : -1), -1, int(m_items.size()) - 1);
    return m_position < 0 ? m_draft : m_items[m_position];
}

bool QueryHistory::save() const
{
    if (!QDir().mkpath(QFileInfo(m_fileName).absolutePath())) return false;
    QSaveFile file(m_fileName);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const auto bytes = QJsonDocument(QJsonArray::fromStringList(m_items)).toJson(QJsonDocument::Compact);
    return file.write(bytes) == bytes.size() && file.commit();
}
}
