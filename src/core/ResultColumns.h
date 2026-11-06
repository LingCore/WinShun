#pragma once

#include "SearchTypes.h"

#include <QCoreApplication>

namespace ws {

inline QString resultFolder(const SearchResult& row)
{
    if (row.isWindow())
        return {};
    QString path = row.isPlugin()
        ? (row.plugin->openKind == PluginRow::OpenKind::Path ? row.plugin->openTarget : QString())
        : row.path;
    path.replace(u'/', u'\\');
    const qsizetype slash = path.lastIndexOf(u'\\');
    return slash < 0 ? QString() : path.left(slash == 2 ? 3 : slash);
}

inline QString resultFolderName(const SearchResult& row)
{
    const QString folder = resultFolder(row);
    const qsizetype slash = folder.lastIndexOf(u'\\');
    return slash < 0 || slash == folder.size() - 1 ? folder : folder.mid(slash + 1);
}

inline QString resultType(const SearchResult& row)
{
    if (row.isWindow())
        return QCoreApplication::translate("ws::ResultModel", "窗口");
    if (row.isDir)
        return QCoreApplication::translate("ws::ResultModel", "文件夹");
    const qsizetype dot = row.name.lastIndexOf(u'.');
    return dot <= 0 || dot == row.name.size() - 1 ? QString() : row.name.mid(dot + 1).toUpper();
}

inline QString resultSource(const SearchResult& row)
{
    if (row.isPlugin())
        return row.plugin->source;
    const char* source = row.isWindow() ? QT_TRANSLATE_NOOP("ws::ResultModel", "已打开")
        : row.line > 0 ? QT_TRANSLATE_NOOP("ws::ResultModel", "内容")
        : row.isWeb()                 ? QT_TRANSLATE_NOOP("ws::ResultModel", "网页")
        : row.isPlace()               ? QT_TRANSLATE_NOOP("ws::ResultModel", "系统")
        : row.isApp()                 ? QT_TRANSLATE_NOOP("ws::ResultModel", "应用")
                                      : QT_TRANSLATE_NOOP("ws::ResultModel", "文件");
    return QCoreApplication::translate("ws::ResultModel", source);
}

inline bool resultColumnLess(const SearchResult& a, const SearchResult& b, SortOrder order)
{
    const auto number = [down = order.descending](auto x, auto y, auto unknown) {
        if (x == unknown || y == unknown)
            return x != unknown && y == unknown;
        return down ? x > y : x < y;
    };
    switch (order.key) {
    case SortKey::Modified:
        return number(a.modified, b.modified, std::uint32_t(0));
    case SortKey::Created:
        return number(a.created, b.created, std::uint32_t(0));
    case SortKey::Size:
        return number(a.size, b.size, std::int64_t(-1));
    case SortKey::Rank:
        return false;
    default:
        break;
    }
    const auto text = [key = order.key](const SearchResult& row) {
        switch (key) {
        case SortKey::Name:
            return row.name;
        case SortKey::Path:
            return resultFolder(row);
        case SortKey::Type:
            return resultType(row);
        case SortKey::Folder:
            return resultFolderName(row);
        case SortKey::Source:
            return resultSource(row);
        default:
            return QString();
        }
    };
    int compared = text(a).compare(text(b), Qt::CaseInsensitive);
    if (compared == 0 && order.key != SortKey::Source)
        compared = a.name.compare(b.name, Qt::CaseInsensitive);
    return order.descending ? compared > 0 : compared < 0;
}

} // namespace ws
