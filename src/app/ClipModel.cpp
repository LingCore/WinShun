#include "ClipModel.h"

#include "ColorText.h"
#include "FileIconProvider.h"

#include <QDateTime>
#include <QFileInfo>
#include <QLocale>
#include <QSet>
#include <QUrl>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr qsizetype kTitleChars = 200; // more never fits on a row
constexpr qsizetype kLeadIn = 16; // characters before a match deep in the text, when the title shows it

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

// One line of text, its runs of white space (tabs, line breaks) as one space.
QString oneLine(QStringView text)
{
    QString line;
    line.reserve(std::min(text.size(), kTitleChars));
    bool space = false;
    for (const QChar c : text) {
        if (c.isSpace()) {
            space = !line.isEmpty();
            continue;
        }
        if (space)
            line += u' ';
        space = false;
        line += c;
        if (line.size() >= kTitleChars)
            break;
    }
    return line;
}

bool isLocal(const QString& path)
{
    return path.size() >= 3 && path[1] == u':' && path[0].isLetter();
}

} // namespace

ClipModel::ClipModel(const ClipStore* store, QObject* parent)
    : QAbstractListModel(parent)
    , m_store(store)
    , m_now(QDateTime::currentMSecsSinceEpoch())
{
}

int ClipModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

const Clip* ClipModel::at(int row) const
{
    if (row < 0 || row >= count())
        return nullptr;
    const int index = m_index.value(m_ids[static_cast<std::size_t>(row)], -1);
    const auto& clips = m_store->clips();
    return index >= 0 && static_cast<std::size_t>(index) < clips.size() ? &clips[static_cast<std::size_t>(index)] : nullptr;
}

int ClipModel::rowOf(qint64 id) const
{
    const auto it = std::find(m_ids.begin(), m_ids.end(), id);
    return it == m_ids.end() ? -1 : static_cast<int>(it - m_ids.begin());
}

qint64 ClipModel::idAt(int row) const
{
    return row >= 0 && row < count() ? m_ids[static_cast<std::size_t>(row)] : 0;
}

QHash<int, QByteArray> ClipModel::roleNames() const
{
    return {
        {IdRole, "clipId"},
        {KindRole, "kind"},
        {TitleRole, "title"},
        {DetailRole, "detail"},
        {IconRole, "icon"},
        {ImageRole, "image"},
        {ColorRole, "swatch"},
        {OrderRole, "order"},
        {GroupRole, "groupName"},
        {PinnedRole, "pinned"},
        {MissingRole, "missing"},
    };
}

QVariant ClipModel::data(const QModelIndex& index, int role) const
{
    const Clip* c = at(index.row());
    if (!c)
        return {};
    switch (role) {
    case IdRole:
        return c->id;
    case KindRole:
        return static_cast<int>(c->kind);
    case TitleRole:
        return title(*c);
    case DetailRole:
        return detail(*c);
    case IconRole: {
        if (c->kind != ClipKind::Files && c->kind != ClipKind::Path)
            return QString();
        const QStringList files = c->files();
        if (files.isEmpty())
            return QString();
        const QString& first = files.constFirst();
        // A folder has no extension to tell; asking the disk is quick for local ones.
        const bool dir = isLocal(first) && !fileMissing(*c) && QFileInfo(first).isDir();
        return FileIconProvider::iconUrl(first, dir);
    }
    case ImageRole:
        return c->kind == ClipKind::Image ? QUrl::fromLocalFile(m_store->imagePath(c->id)).toString() : QString();
    case ColorRole:
        return colorOf(*c);
    case OrderRole:
        return static_cast<int>(m_selected.indexOf(c->id) + 1);
    case GroupRole:
        return groupName(c->group);
    case PinnedRole:
        return c->group == ClipStore::kPinned;
    case MissingRole:
        return fileMissing(*c);
    default:
        return {};
    }
}

QString ClipModel::title(const Clip& clip) const
{
    const ClipMatcher& matcher = m_matcher;
    switch (clip.kind) {
    case ClipKind::Image:
        return tr("图片 %1 × %2").arg(clip.width).arg(clip.height);
    case ClipKind::Files: {
        const QStringList files = clip.files();
        QStringList names;
        for (const QString& file : files) {
            const QString name = QFileInfo(file).fileName();
            names.append(name.isEmpty() ? file : name); // "D:\" has no name
        }
        QString line = names.isEmpty() ? QString() : names.constFirst();
        // The name that matched, if it is not the first.
        for (const QString& name : names) {
            if (!matcher.spans(name).isEmpty()) {
                line = name;
                break;
            }
        }
        if (names.size() > 1)
            line = tr("%1 等 %Ln 个文件", nullptr, static_cast<int>(names.size())).arg(line);
        return highlighted(line, matcher.spans(line), m_highlightColor);
    }
    default:
        break;
    }
    // The first line with something in it; or, when the query matched further
    // down, the text from a little before that match.
    QStringView text = clip.text;
    const qsizetype firstChar = [&] {
        for (qsizetype i = 0; i < text.size(); ++i) {
            if (!text[i].isSpace())
                return i;
        }
        return text.size();
    }();
    QString line = oneLine(text.mid(firstChar));
    const QList<std::pair<qsizetype, qsizetype>> spans = matcher.spans(text);
    if (!spans.isEmpty() && matcher.spans(line).size() < spans.size()) {
        qsizetype first = text.size();
        for (const auto& span : spans)
            first = std::min(first, span.first);
        const qsizetype from = std::max(firstChar, first - kLeadIn);
        line = (from > firstChar ? u"…"_s : QString()) + oneLine(text.mid(from));
    }
    return highlighted(line, matcher.spans(line), m_highlightColor);
}

QString ClipModel::detail(const Clip& clip) const
{
    QStringList parts;
    if (!clip.source.isEmpty())
        parts.append(clip.source);
    parts.append(when(clip.used, m_now));
    switch (clip.kind) {
    case ClipKind::Text:
        parts.append(tr("%Ln 字", nullptr, static_cast<int>(std::min<qint64>(clip.textLength, INT_MAX))));
        break;
    case ClipKind::Image:
        parts.append(QLocale().formattedDataSize(clip.bytes, 1, QLocale::DataSizeTraditionalFormat));
        break;
    case ClipKind::Files: {
        const auto files = static_cast<int>(clip.files().size());
        if (files > 1)
            parts.append(tr("%Ln 个文件", nullptr, files));
        break;
    }
    default:
        break;
    }
    if (fileMissing(clip))
        parts.append(tr("已不存在"));
    return parts.join(u" · "_s);
}

QString ClipModel::groupName(qint64 group) const
{
    if (group == 0)
        return {};
    if (group == ClipStore::kPinned)
        return tr("固定");
    const ClipGroup* g = m_store->group(group);
    return g ? g->name : QString();
}

bool ClipModel::fileMissing(const Clip& clip) const
{
    if (clip.kind != ClipKind::Files && clip.kind != ClipKind::Path)
        return false;
    const QStringList files = clip.files();
    if (files.isEmpty() || !isLocal(files.constFirst()))
        return false; // a network path could keep the GUI waiting
    const auto it = m_missing.constFind(files.constFirst());
    if (it != m_missing.constEnd())
        return *it;
    const bool gone = !QFileInfo::exists(files.constFirst());
    m_missing.insert(files.constFirst(), gone);
    return gone;
}

QString ClipModel::colorOf(const Clip& clip)
{
    if (clip.kind != ClipKind::Text)
        return {};
    const std::optional<QColor> color = colortext::parse(clip.text);
    if (!color)
        return {};
    return color->alpha() == 255 ? color->name() : color->name(QColor::HexArgb); // QML reads #aarrggbb
}

QString ClipModel::when(qint64 ms, qint64 now)
{
    const qint64 minutes = (now - ms) / 60000;
    if (minutes < 1)
        return tr("刚刚");
    if (minutes < 60)
        return tr("%n 分钟前", nullptr, static_cast<int>(minutes));
    const QDateTime then = QDateTime::fromMSecsSinceEpoch(ms);
    const QDate today = QDateTime::fromMSecsSinceEpoch(now).date();
    const QString time = QLocale().toString(then.time(), u"HH:mm"_s);
    if (then.date() == today)
        return time;
    if (then.date() == today.addDays(-1))
        return tr("昨天 %1").arg(time);
    if (then.date().year() == today.year())
        return QLocale().toString(then.date(), tr("M月d日"));
    return QLocale().toString(then.date(), tr("yyyy年M月d日"));
}

void ClipModel::setRows(const std::vector<int>& rows, const QString& query)
{
    const int before = count();
    if (query != m_query) {
        m_query = query;
        m_matcher = ClipMatcher(query);
    }
    m_now = QDateTime::currentMSecsSinceEpoch();
    const auto& clips = m_store->clips();
    std::vector<qint64> ids;
    ids.reserve(rows.size());
    m_index.clear();
    for (const int row : rows) {
        const qint64 id = clips[static_cast<std::size_t>(row)].id;
        ids.push_back(id);
        m_index.insert(id, row);
    }
    // Rows that stay in the same order: remove the ones that went, insert
    // the new ones, so the view keeps its delegates and current row. Any
    // other change (an entry moved up) resets the list.
    const QSet<qint64> wanted(ids.begin(), ids.end());
    const QSet<qint64> had(m_ids.begin(), m_ids.end());
    std::vector<qint64> keptBefore;
    std::vector<qint64> keptAfter;
    std::copy_if(m_ids.begin(), m_ids.end(), std::back_inserter(keptBefore), [&](qint64 id) { return wanted.contains(id); });
    std::copy_if(ids.begin(), ids.end(), std::back_inserter(keptAfter), [&](qint64 id) { return had.contains(id); });
    if (keptBefore != keptAfter) {
        beginResetModel();
        m_ids = std::move(ids);
        m_missing.clear();
        endResetModel();
    } else {
        for (int row = count() - 1; row >= 0; --row) {
            if (wanted.contains(m_ids[static_cast<std::size_t>(row)]))
                continue;
            beginRemoveRows({}, row, row);
            m_ids.erase(m_ids.begin() + row);
            endRemoveRows();
        }
        for (std::size_t row = 0; row < ids.size(); ++row) {
            if (row < m_ids.size() && m_ids[row] == ids[row])
                continue;
            const int at = static_cast<int>(row);
            beginInsertRows({}, at, at);
            m_ids.insert(m_ids.begin() + at, ids[row]);
            endInsertRows();
        }
        if (count() > 0) // times, titles (the query), groups
            emit dataChanged(index(0), index(count() - 1));
    }
    // Picked entries stay picked while they are in the history (deleted ones
    // go), listed or not.
    const qsizetype picked = m_selected.size();
    m_selected.removeIf([this](qint64 id) { return !m_store->clip(id); });
    if (m_selected.size() != picked)
        emit selectionChanged();
    if (count() != before)
        emit countChanged();
}

void ClipModel::refreshTimes()
{
    m_now = QDateTime::currentMSecsSinceEpoch();
    m_missing.clear();
    if (count() > 0)
        emit dataChanged(index(0), index(count() - 1), {TitleRole, DetailRole, MissingRole, IconRole, GroupRole, PinnedRole});
}

bool ClipModel::isSelected(int row) const
{
    const Clip* c = at(row);
    return c && m_selected.contains(c->id);
}

void ClipModel::selectionEdited()
{
    if (count() > 0)
        emit dataChanged(index(0), index(count() - 1), {OrderRole});
    emit selectionChanged();
}

void ClipModel::toggle(int row)
{
    const Clip* c = at(row);
    if (!c)
        return;
    if (!m_selected.removeOne(c->id))
        m_selected.append(c->id);
    selectionEdited();
}

void ClipModel::selectRange(int from, int to, bool add)
{
    if (count() == 0)
        return;
    from = std::clamp(from, 0, count() - 1);
    to = std::clamp(to, 0, count() - 1);
    if (!add)
        m_selected.clear();
    const int step = from <= to ? 1 : -1;
    for (int row = from;; row += step) {
        if (const Clip* c = at(row); c && !m_selected.contains(c->id))
            m_selected.append(c->id);
        if (row == to)
            break;
    }
    selectionEdited();
}

void ClipModel::clearSelection()
{
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    selectionEdited();
}

std::vector<const Clip*> ClipModel::selection() const
{
    std::vector<const Clip*> clips;
    for (const qint64 id : m_selected) {
        if (const Clip* c = m_store->clip(id))
            clips.push_back(c);
    }
    return clips;
}

void ClipModel::setHighlightColor(const QColor& color)
{
    if (color == m_highlightColor)
        return;
    m_highlightColor = color;
    emit highlightColorChanged();
    if (count() > 0)
        emit dataChanged(index(0), index(count() - 1), {TitleRole});
}

} // namespace ws
