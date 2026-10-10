#include "SettingsSearch.h"

using namespace Qt::StringLiterals;

namespace ws {

namespace {

QStringList stringsOf(const QVariant& value)
{
    QStringList strings;
    for (const QVariant& item : value.toList()) {
        const QString s = item.toString();
        if (!s.isEmpty())
            strings.append(s);
    }
    return strings;
}

} // namespace

SettingsSearch::SettingsSearch(QObject* parent)
    : QObject(parent)
{
}

void SettingsSearch::setQuery(const QString& query)
{
    if (query == m_query)
        return;
    const bool wasActive = active();
    m_query = query;
    m_parsed = settingsmatch::parse(query);
    ++m_revision;
    emit queryChanged();
    emit revisionChanged();
    if (active() != wasActive)
        emit activeChanged();
    emit queryApplied(wasActive);
}

void SettingsSearch::setHighlightColor(const QColor& color)
{
    if (color == m_color)
        return;
    m_color = color;
    ++m_revision;
    emit highlightColorChanged();
    emit revisionChanged();
}

void SettingsSearch::setCurrent(QObject* row)
{
    if (row == m_current)
        return;
    m_current = row;
    emit currentChanged();
}

QVariant SettingsSearch::match(int /*revision*/, const QVariantMap& fields) const
{
    if (m_parsed.isEmpty())
        return {};
    settingsmatch::Fields f;
    f.title = fields.value(u"title"_s).toString();
    f.description = fields.value(u"description"_s).toString();
    f.keywords = stringsOf(fields.value(u"keywords"_s));
    f.options = stringsOf(fields.value(u"options"_s));
    f.context = stringsOf(fields.value(u"context"_s));
    f.values = stringsOf(fields.value(u"values"_s));

    const settingsmatch::Match found = settingsmatch::match(m_parsed, f);
    if (found.score < 0)
        return {};
    const QString color = m_color.name();
    QStringList values;
    for (const int i : found.values)
        values.append(f.values.value(i));
    return QVariantMap {
        {u"score"_s, found.score},
        {u"title"_s, settingsmatch::highlight(f.title, found.title, color)},
        {u"description"_s, settingsmatch::highlight(f.description, found.description, color)},
        {u"values"_s, values},
    };
}

void SettingsSearch::report(QObject* row, QObject* section, int page, int score)
{
    if (!row)
        return;
    if (!m_watched.contains(row)) {
        m_watched.insert(row);
        // Rows go away with their window, without a word.
        connect(row, &QObject::destroyed, this, [this, row] {
            m_watched.remove(row);
            if (m_shown.remove(row))
                recount();
        });
    }
    if (score < 0) {
        if (!m_shown.remove(row))
            return;
    } else {
        auto it = m_shown.find(row);
        if (it == m_shown.end())
            m_shown.insert(row, {section, page, score});
        else if (it->section == section && it->page == page && it->score == score)
            return;
        else
            *it = {section, page, score};
    }
    recount();
}

int SettingsSearch::sectionCount(QObject* section) const
{
    return m_sectionCounts.value(section);
}

QVariantList SettingsSearch::results() const
{
    QVariantList list;
    for (auto it = m_shown.cbegin(); it != m_shown.cend(); ++it)
        list.append(QVariantMap {{u"row"_s, QVariant::fromValue(it.key())}, {u"score"_s, it->score}});
    return list;
}

void SettingsSearch::recount()
{
    QList<int> counts;
    QHash<QObject*, int> sectionCounts;
    for (auto it = m_shown.cbegin(); it != m_shown.cend(); ++it) {
        if (it->page < 0)
            continue;
        if (counts.size() <= it->page)
            counts.resize(it->page + 1);
        ++counts[it->page];
        if (it->section)
            ++sectionCounts[it->section.data()];
    }
    const int total = static_cast<int>(m_shown.size());
    if (counts == m_counts && sectionCounts == m_sectionCounts && total == m_total)
        return;
    m_counts = counts;
    m_sectionCounts = sectionCounts;
    m_total = total;
    emit countsChanged();
}

} // namespace ws
