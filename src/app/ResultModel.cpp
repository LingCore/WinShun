#include "ResultModel.h"

#include "AppCatalog.h"
#include "FileIconProvider.h"
#include "Pinyin.h"
#include "Wtf8.h"

#include <QGuiApplication>
#include <QStyleHints>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// Wraps the given ranges of `text` in accent-coloured bold (Text.StyledText).
QString highlighted(const QString& text, QList<std::pair<qsizetype, qsizetype>> ranges, const QColor& color)
{
    std::sort(ranges.begin(), ranges.end());
    QString html;
    html.reserve(text.size() + 64);
    const QString open = u"<font color=\"%1\"><b>"_s.arg(color.name());
    qsizetype pos = 0;
    for (const auto& [start, length] : ranges) {
        if (start < pos || length <= 0)
            continue; // overlapping ranges: keep the first
        html += text.mid(pos, start - pos).toHtmlEscaped();
        html += open + text.mid(start, length).toHtmlEscaped() + u"</b></font>"_s;
        pos = start + length;
    }
    html += text.mid(pos).toHtmlEscaped();
    return html;
}

QString parentFolder(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(u'\\');
    if (slash < 0)
        return {};
    QString folder = path.left(slash);
    if (folder.endsWith(u':'))
        folder += u'\\';
    return folder;
}

// The second line of an app's row: the program it starts, or where it comes from.
QString appOrigin(const SearchResult& r)
{
    switch (r.app) {
    case AppKind::Store:
        return u"Microsoft Store 应用"_s;
    case AppKind::System:
        return u"Windows 系统应用"_s;
    case AppKind::Package:
        return u"MSIX 应用"_s;
    default:
        return r.target.isEmpty() ? u"Windows 系统应用"_s : r.target; // no file: Control Panel, Run, ...
    }
}

} // namespace

ResultModel::ResultModel(QObject* parent)
    : QAbstractListModel(parent)
{
    // Store apps' logos come in a light and a dark variant.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (!m_items.isEmpty())
            emit dataChanged(index(0), index(count() - 1), {IconRole});
    });
}

int ResultModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

const SearchResult* ResultModel::at(int row) const
{
    return row >= 0 && row < m_items.size() ? &m_items[row] : nullptr;
}

int ResultModel::indexOf(const QString& path) const
{
    const auto it = std::find_if(m_items.cbegin(), m_items.cend(), [&](const SearchResult& r) { return r.path == path; });
    return it == m_items.cend() ? -1 : static_cast<int>(it - m_items.cbegin());
}

QVariant ResultModel::data(const QModelIndex& index, int role) const
{
    const SearchResult* r = at(index.row());
    if (!r)
        return {};
    switch (role) {
    case NameRole:
        return r->name;
    case NameHtmlRole: {
        QList<std::pair<qsizetype, qsizetype>> ranges;
        for (const QString& term : m_highlights) {
            const qsizetype i = r->name.indexOf(term, 0, Qt::CaseInsensitive);
            if (i >= 0) {
                ranges.append({i, term.size()});
                continue;
            }
            // Found by pinyin ("bg" for 报告): highlight the characters it matched.
            const pinyin::Matcher matcher(term.toLower().toStdString());
            if (const auto span = matcher.findUtf16(wtf8::view(r->name)))
                ranges.append({static_cast<qsizetype>(span->start), static_cast<qsizetype>(span->length)});
        }
        return highlighted(r->name, ranges, m_highlightColor);
    }
    case FolderRole:
        return r->isApp() ? appOrigin(*r) : parentFolder(r->path);
    case PathRole:
        return r->path;
    case IsDirRole:
        return r->isDir;
    case IconRole:
        if (r->isApp()) {
            const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
            return FileIconProvider::appIconUrl(appIdOf(r->path), r->isPackagedApp(), dark);
        }
        return FileIconProvider::iconUrl(r->path, r->isDir);
    case SnippetHtmlRole:
        if (r->snippet.isEmpty())
            return QString();
        return highlighted(r->snippet, {{r->snippetMatchStart, r->snippetMatchLength}}, m_highlightColor);
    case LineRole:
        return r->line;
    case RecentRole:
        return r->recent;
    case IsAppRole:
        return r->isApp();
    case PackagedAppRole:
        return r->isPackagedApp();
    case ElevatableRole:
        return r->elevatable;
    case RevealableRole:
        return !r->isApp() || !r->target.isEmpty();
    case CopyableRole:
        return !r->isApp() || r->hasCopyableTarget();
    default:
        return {};
    }
}

QHash<int, QByteArray> ResultModel::roleNames() const
{
    return {
        {NameRole, "name"},
        {NameHtmlRole, "nameHtml"},
        {FolderRole, "folder"},
        {PathRole, "path"},
        {IsDirRole, "isDir"},
        {IconRole, "icon"},
        {SnippetHtmlRole, "snippetHtml"},
        {LineRole, "line"},
        {RecentRole, "recent"},
        {IsAppRole, "isApp"},
        {PackagedAppRole, "packagedApp"},
        {ElevatableRole, "elevatable"},
        {RevealableRole, "revealable"},
        {CopyableRole, "copyable"},
    };
}

QString ResultModel::pathAt(int row) const
{
    const SearchResult* r = at(row);
    return r ? r->path : QString();
}

void ResultModel::assign(SearchResults items, QStringList highlights)
{
    const int oldCount = count();
    const int newCount = static_cast<int>(items.size());
    const int common = std::min(oldCount, newCount);

    // Rows present before and after: overwrite in place. The name HTML of
    // every row depends on the highlights.
    int first = -1;
    int last = -1;
    if (highlights != m_highlights && common > 0) {
        first = 0;
        last = common - 1;
    }
    m_highlights = std::move(highlights);
    for (int i = 0; i < common; ++i) {
        if (m_items[i] == items[i])
            continue;
        m_items[i] = std::move(items[i]);
        if (first < 0)
            first = i;
        last = std::max(last, i);
    }
    if (first >= 0)
        emit dataChanged(index(first), index(last));

    if (newCount > oldCount) {
        beginInsertRows({}, oldCount, newCount - 1);
        for (int i = oldCount; i < newCount; ++i)
            m_items.append(std::move(items[i]));
        endInsertRows();
    } else if (newCount < oldCount) {
        beginRemoveRows({}, newCount, oldCount - 1);
        m_items.resize(newCount);
        endRemoveRows();
    }
    if (newCount != oldCount)
        emit countChanged();
}

void ResultModel::update(int row, const SearchResult& item)
{
    if (row < 0 || row >= count() || m_items[row] == item)
        return;
    m_items[row] = item;
    emit dataChanged(index(row), index(row));
}

void ResultModel::append(const SearchResults& items)
{
    if (items.isEmpty())
        return;
    beginInsertRows({}, count(), count() + static_cast<int>(items.size()) - 1);
    m_items.append(items);
    endInsertRows();
    emit countChanged();
}

bool ResultModel::remove(const QString& path)
{
    const int row = indexOf(path);
    if (row < 0)
        return false;
    beginRemoveRows({}, row, row);
    m_items.removeAt(row);
    endRemoveRows();
    emit countChanged();
    return true;
}

int ResultModel::removeAll(const QSet<QString>& paths)
{
    int removed = 0;
    // From the end, one run of adjacent rows at a time.
    for (int row = count() - 1; row >= 0;) {
        if (!paths.contains(m_items[row].path)) {
            --row;
            continue;
        }
        int start = row;
        while (start > 0 && paths.contains(m_items[start - 1].path))
            --start;
        beginRemoveRows({}, start, row);
        m_items.remove(start, row - start + 1);
        endRemoveRows();
        removed += row - start + 1;
        row = start - 1;
    }
    if (removed > 0)
        emit countChanged();
    return removed;
}

void ResultModel::clear()
{
    assign({}, {});
}

void ResultModel::setHighlightColor(const QColor& color)
{
    if (color == m_highlightColor)
        return;
    m_highlightColor = color;
    emit highlightColorChanged();
    if (!m_items.isEmpty())
        emit dataChanged(index(0), index(count() - 1), {NameHtmlRole, SnippetHtmlRole});
}

} // namespace ws
