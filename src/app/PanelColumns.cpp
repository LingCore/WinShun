#include "PanelColumns.h"

#include <QSet>
#include <QSettings>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace ws {
namespace {
struct Definition {
    const char* id;
    const char* title;
    int width;
    int minimum;
    int sort;
};
constexpr std::array kColumns {
    Definition {"name", QT_TRANSLATE_NOOP("ws::PanelColumns", "名称"), 280, 120, 1},
    Definition {"path", QT_TRANSLATE_NOOP("ws::PanelColumns", "路径"), 288, 120, 2},
    Definition {"size", QT_TRANSLATE_NOOP("ws::PanelColumns", "大小"), 96, 72, 3},
    Definition {"modified", QT_TRANSLATE_NOOP("ws::PanelColumns", "修改时间"), 168, 128, 4},
    Definition {"type", QT_TRANSLATE_NOOP("ws::PanelColumns", "类型"), 100, 72, 5},
    Definition {"created", QT_TRANSLATE_NOOP("ws::PanelColumns", "创建时间"), 168, 128, 6},
    Definition {"folder", QT_TRANSLATE_NOOP("ws::PanelColumns", "所在文件夹"), 160, 100, 7},
    Definition {"source", QT_TRANSLATE_NOOP("ws::PanelColumns", "来源"), 140, 100, 8},
};

const Definition* definition(const QString& id)
{
    const auto found = std::find_if(
        kColumns.begin(), kColumns.end(), [&](const auto& column) { return id == QLatin1StringView(column.id); });
    return found == kColumns.end() ? nullptr : &*found;
}
} // namespace

PanelColumns::PanelColumns(QObject* parent)
    : QObject(parent)
{
    reset();
}

void PanelColumns::load(const QString& settingsFile)
{
    m_settingsFile = settingsFile;
    const QString saved = QSettings(settingsFile, QSettings::IniFormat).value(u"Panel/Columns"_s).toString();
    if (saved.isEmpty())
        return;
    QList<Column> columns;
    QSet<QString> seen;
    for (const QString& part : saved.split(u',')) {
        const QString id = part.section(u':', 0, 0).trimmed();
        const auto* spec = definition(id);
        if (!spec || seen.contains(id))
            continue;
        bool ok = false;
        const int width = part.section(u':', 1, 1).toInt(&ok);
        columns.append({id, ok ? std::clamp(width, spec->minimum, 2000) : spec->width});
        seen.insert(id);
    }
    if (columns.isEmpty())
        return;
    if (!seen.contains(u"name"_s))
        columns.prepend({u"name"_s, kColumns[0].width});
    m_columns = std::move(columns);
    emit changed();
}

QVariantList PanelColumns::visibleColumns() const
{
    QVariantList columns;
    for (const auto& column : m_columns) {
        const auto& spec = *definition(column.id);
        columns.append(QVariantMap {{u"id"_s, column.id}, {u"title"_s, tr(spec.title)}, {u"width"_s, column.width},
            {u"minimum"_s, spec.minimum}, {u"sort"_s, spec.sort}});
    }
    return columns;
}

QVariantList PanelColumns::availableColumns() const
{
    QVariantList columns;
    for (const auto& spec : kColumns) {
        const QString id = QString::fromLatin1(spec.id);
        const bool visible = std::any_of(m_columns.begin(), m_columns.end(), [&](const auto& c) { return c.id == id; });
        columns.append(QVariantMap {{u"id"_s, id}, {u"title"_s, tr(spec.title)}, {u"visible"_s, visible}});
    }
    return columns;
}

void PanelColumns::toggle(const QString& id)
{
    const auto* spec = definition(id);
    if (!spec || id == u"name")
        return;
    const auto found = std::find_if(m_columns.begin(), m_columns.end(), [&](const auto& c) { return c.id == id; });
    if (found == m_columns.end())
        m_columns.append({id, spec->width});
    else
        m_columns.erase(found);
    save();
}

void PanelColumns::move(int from, int to)
{
    if (from < 0 || to < 0 || from >= m_columns.size() || to >= m_columns.size() || from == to)
        return;
    m_columns.move(from, to);
    save();
}

void PanelColumns::resize(const QString& id, int width)
{
    const auto* spec = definition(id);
    if (!spec)
        return;
    for (auto& column : m_columns) {
        if (column.id == id) {
            column.width = std::clamp(width, spec->minimum, 2000);
            save();
            return;
        }
    }
}

void PanelColumns::reset()
{
    m_columns.clear();
    for (std::size_t i = 0; i < 4; ++i)
        m_columns.append({QString::fromLatin1(kColumns[i].id), kColumns[i].width});
    save();
}

void PanelColumns::save()
{
    if (!m_settingsFile.isEmpty()) {
        QStringList parts;
        for (const auto& column : m_columns)
            parts.append(column.id + u':' + QString::number(column.width));
        QSettings(m_settingsFile, QSettings::IniFormat).setValue(u"Panel/Columns"_s, parts.join(u','));
    }
    emit changed();
}

} // namespace ws
