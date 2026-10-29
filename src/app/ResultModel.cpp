#include "ResultModel.h"
#include "ResultColumns.h"

#include "AppCatalog.h"
#include "DisplayDiagnostics.h"
#include "Elide.h"
#include "FileIndex.h"
#include "FileIconProvider.h"
#include "ThumbnailProvider.h"
#include "Pinyin.h"
#include "Query.h"
#include "SwitchBackend.h"
#include "Switches.h"
#include "SystemCatalog.h"
#include "WebShortcut.h"
#include "Wtf8.h"
#include "platform/SiteIcons.h"
#include "platform/SwitchKeepAwake.h"

#include <QGuiApplication>
#include <QLocale>
#include <QStyleHints>
#include <QTime>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

using elide::highlighted;

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

// Where the terms are in a name: as they are, or read as pinyin ("bg" for
// 报告: the characters it matched).
QList<std::pair<qsizetype, qsizetype>> nameMatches(const QString& name, const QStringList& terms)
{
    QList<std::pair<qsizetype, qsizetype>> ranges;
    for (const QString& term : terms) {
        const qsizetype i = name.indexOf(term, 0, Qt::CaseInsensitive);
        if (i >= 0) {
            ranges.append({i, term.size()});
            continue;
        }
        const pinyin::Matcher matcher(term.toLower().toStdString());
        if (const auto span = matcher.findUtf16(wtf8::view(name)))
            ranges.append({static_cast<qsizetype>(span->start), static_cast<qsizetype>(span->length)});
    }
    return ranges;
}

// Whether a name has a term (see nameMatches).
bool nameHas(const QString& name, const QString& term)
{
    return name.contains(term, Qt::CaseInsensitive)
        || pinyin::Matcher(term.toLower().toStdString()).findUtf16(wtf8::view(name)).has_value();
}

// Where the terms a name lacks are in its folder, found there ("弯管机" in
// F:\模型\3轴弯管机模型): as they are, or as pinyin in a folder's name.
QList<std::pair<qsizetype, qsizetype>> folderMatches(
    const QString& name, const QString& folder, const QStringList& terms)
{
    QList<std::pair<qsizetype, qsizetype>> ranges;
    for (const QString& term : terms) {
        if (nameHas(name, term))
            continue;
        bool found = false;
        for (qsizetype i = folder.indexOf(term, 0, Qt::CaseInsensitive); i >= 0;
            i = folder.indexOf(term, i + term.size(), Qt::CaseInsensitive)) {
            ranges.append({i, term.size()});
            found = true;
        }
        const pinyin::Matcher matcher(term.toLower().toStdString());
        if (found || !matcher.valid())
            continue;
        const std::u16string_view all = wtf8::view(folder);
        for (qsizetype from = 0; from < folder.size();) {
            qsizetype to = folder.indexOf(u'\\', from);
            if (to < 0)
                to = folder.size();
            if (const auto span = matcher.findUtf16(all.substr(from, to - from)))
                ranges.append({from + static_cast<qsizetype>(span->start), static_cast<qsizetype>(span->length)});
            from = to + 1;
        }
    }
    std::sort(ranges.begin(), ranges.end());
    return ranges;
}

// A web search's row reads 在 GitHub 中搜索“WinShun”; `words`: where they are in it.
QString webSearchTitle(const SearchResult& r, std::pair<qsizetype, qsizetype>* words = nullptr)
{
    const QString title = ResultModel::tr("在 %1 中搜索“%2”").arg(r.name, r.words);
    if (words)
        *words = {title.lastIndexOf(r.words), r.words.size()};
    return title;
}

// The second line of an app's row: the program it starts, or where it comes
// from. A place's: where in Windows it is, or a tool's command. A web
// shortcut's: the address it opens, and for its site the keyword.
QString appOrigin(const SearchResult& r)
{
    if (r.isCommand())
        return ResultModel::tr("自定义命令 · %1").arg(r.words.isEmpty() ? r.target : r.words);
    switch (r.app) {
    case AppKind::Web:
        if (r.suggestion)
            return ResultModel::tr("%1 的搜索建议").arg(r.name);
        return r.words.isEmpty() ? ResultModel::tr("%1 · 关键词 %2").arg(displayWebUrl(r.target), r.keyword)
                                 : displayWebUrl(r.target);
    case AppKind::Store:
        return ResultModel::tr("Microsoft Store 应用");
    case AppKind::System:
        return ResultModel::tr("Windows 系统应用");
    case AppKind::Package:
        return ResultModel::tr("MSIX 应用");
    case AppKind::Setting:
        return ResultModel::tr("Windows 设置");
    case AppKind::ControlPanel:
        return ResultModel::tr("控制面板");
    case AppKind::Security:
        return ResultModel::tr("Windows 安全中心");
    case AppKind::Tool:
        return describeCommand(r.target); // "diskmgmt.msc", the folder
    case AppKind::Plugin:
        return r.plugin ? r.plugin->subtitle : QString();
    default:
        return r.target.isEmpty() ? ResultModel::tr("Windows 系统应用") : r.target; // no file: Control Panel, Run, ...
    }
}

// What Switches knows of a row's switch; any other row, and one whose switch
// has no backend yet, is an ordinary place (Unsupported).
SwitchState stateOf(const Switches* switches, const SearchResult& r)
{
    if (switches && !r.switchId.isEmpty())
        return switches->state(r.switchId);
    SwitchState none;
    none.availability = SwitchState::Availability::Unsupported;
    return none;
}

// The second line of a switch's row. Keep awake with an end time counts the
// minutes left as of now, so that the row is right whenever it is drawn.
QString switchNote(const SwitchState& state)
{
    if (state.until.isValid() && state.on == SwitchState::Value::On)
        return SwitchKeepAwake::noteOf(state.until, QDateTime::currentDateTime());
    return state.note;
}

// "1 小时", "90 分钟", "1 小时 30 分钟".
QString durationText(int minutes)
{
    const int hours = minutes / 60;
    const int rest = minutes % 60;
    if (hours == 0)
        return ResultModel::tr("%n 分钟", nullptr, rest);
    if (rest == 0)
        return ResultModel::tr("%n 小时", nullptr, hours);
    return ResultModel::tr("%1 %2").arg(ResultModel::tr("%n 小时", nullptr, hours), ResultModel::tr("%n 分钟", nullptr, rest));
}

QString clockText(int minutesAfterMidnight)
{
    return QTime(minutesAfterMidnight / 60 % 24, minutesAfterMidnight % 60).toString(u"HH:mm"_s);
}

} // namespace

ResultModel::ResultModel(QObject* parent)
    : QAbstractListModel(parent)
{
    // Store apps' logos come in a light and a dark variant; sites' icons may sit on a plate.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (!m_items.isEmpty())
            emit dataChanged(index(0), index(count() - 1), {IconRole});
    });
    // A site's icon arrived: its rows trade the browser's for it.
    connect(siteicons::notifier(), &siteicons::Notifier::fetched, this, [this](const QString& site) {
        for (int row = 0; row < count(); ++row) {
            const SearchResult* r = at(row);
            const bool shows = r
                && ((r->isWeb() && siteIconKey(r->icon) == site)
                    || (r->plugin && r->plugin->icon.startsWith(u"site:") && siteIconKey(r->plugin->icon.mid(5)) == site));
            if (shows)
                emit dataChanged(index(row), index(row), {IconRole});
        }
    });
}

void ResultModel::setSwitches(Switches* switches)
{
    if (switches == m_switches)
        return;
    m_switches = switches;
    if (!switches)
        return;
    // A switch changed (here, in another program, in Settings): its rows
    // are not new rows, only their switch and second line are.
    connect(switches, &Switches::changed, this, &ResultModel::switchChanged);
    if (!m_items.isEmpty()) // read before this was set
        emit dataChanged(index(0), index(count() - 1), {SwitchOnRole, SwitchReadyRole, FolderRole});
}

void ResultModel::switchChanged(const QString& id)
{
    for (int row = 0; row < count(); ++row) {
        if (m_items[row].switchId == id)
            emit dataChanged(index(row), index(row),
                {SwitchOnRole, SwitchLevelRole, SwitchBusyRole, SwitchNoteRole, SwitchErrorRole, SwitchReadyRole,
                    FolderRole});
    }
}

int ResultModel::defaultRow() const
{
    for (int row = 0; row < count(); ++row) {
        if (!m_items[row].statusRow)
            return row;
    }
    return 0; // nothing else listed: the status row itself
}

QString ResultModel::switchWords(const SearchResult& row, const SwitchIntent& intent, bool title)
{
    using Kind = SwitchIntent::Kind;
    const QString& id = row.switchId;
    const QString& name = row.name;
    // A space between Chinese and a Latin name: "关闭 WLAN", "打开 WLAN 设置"
    // (simplified() takes the extra one out again where English has its own).
    const bool latin = !name.isEmpty() && name.front().script() == QChar::Script_Latin;
    const QString after = latin ? u' ' + name : name;  // after a Chinese verb
    const QString inside = latin ? after + u' ' : name; // between Chinese words
    switch (intent.kind) {
    case Kind::Toggle:
        return title ? name : tr("切换%1").arg(after).simplified();
    case Kind::On:
    case Kind::Off: {
        const bool on = intent.kind == Kind::On;
        if (id == u"theme")
            return title ? (on ? tr("设为深色") : tr("设为浅色")) : (on ? tr("切换到深色") : tr("切换到浅色"));
        if (id == u"hidden-files")
            return on ? tr("显示隐藏的文件") : tr("不显示隐藏的文件");
        if (id == u"file-extensions")
            return on ? tr("显示文件扩展名") : tr("隐藏文件扩展名");
        if (id == u"volume")
            return on ? tr("取消静音") : tr("静音");
        if (id == u"keep-awake")
            return on ? tr("保持唤醒") : tr("结束保持唤醒");
        return (on ? tr("打开%1").arg(after) : tr("关闭%1").arg(after)).simplified();
    }
    case Kind::SetLevel:
    case Kind::LevelOnly: { // a step down, as resolved: the level it reaches
        const int level = std::clamp(intent.value, 0, 100);
        return title ? tr("音量设为 %1%").arg(level) : tr("音量设为 %1").arg(level);
    }
    case Kind::StepLevel:
        return tr("音量 %1").arg(intent.value > 0 ? u"+"_s + QString::number(intent.value) : QString::number(intent.value));
    case Kind::AwakeFor:
        return title ? tr("保持唤醒 %1").arg(durationText(intent.value))
                     : tr("保持唤醒到 %1").arg(QTime::currentTime().addSecs(intent.value * 60).toString(u"HH:mm"_s));
    case Kind::AwakeUntil:
        return tr("保持唤醒到 %1").arg(clockText(intent.value));
    case Kind::OpenSettings:
        return tr("打开%1设置").arg(inside).simplified();
    }
    return name;
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
        if (r->suggestion) // the words alone: five times 在 百度 中搜索“…” would hide them
            return r->words;
        if (const QString title = switchTitle(*r); !title.isEmpty())
            return title;
        return r->isWeb() && !r->words.isEmpty() ? webSearchTitle(*r) : r->name;
    case NameHtmlRole: {
        if (r->suggestion) // what was typed stands out in it, as in a file's name
            return highlighted(r->words, nameMatches(r->words, m_highlights), m_highlightColor);
        if (const QString title = switchTitle(*r); !title.isEmpty())
            return highlighted(title, nameMatches(title, m_highlights), m_highlightColor);
        if (r->isWeb() && !r->words.isEmpty()) { // the words stand out, whatever the query's terms
            std::pair<qsizetype, qsizetype> words;
            const QString title = webSearchTitle(*r, &words);
            return highlighted(title, {words}, m_highlightColor);
        }
        return highlighted(r->name, nameMatches(r->name, m_highlights), m_highlightColor);
    }
    case NameMatchesRole: {
        QVariantList out;
        if (r->isApp()) // short names; a web search's title is its own
            return out;
        for (const auto& [start, length] : nameMatches(r->name, m_highlights))
            out << start << length;
        return out;
    }
    case FolderRole:
        if (!r->switchId.isEmpty()) { // why it failed, or what to know of it, else where it is
            const SwitchState state = stateOf(m_switches, *r);
            if (!state.error.isEmpty())
                return state.error;
            // "音量 30" while muted: Enter brings the sound back too, as the row says.
            if (state.ready() && state.on == SwitchState::Value::Off && m_switches
                && m_switches->resolve(r->switchId, r->intent).kind == SwitchIntent::Kind::SetLevel)
                return state.note.isEmpty() ? tr("已静音，会同时取消静音")
                                            : tr("%1 · 已静音，会同时取消静音").arg(state.note);
            if (const QString note = switchNote(state); !note.isEmpty())
                return note;
        }
        return r->isApp() ? appOrigin(*r) : parentFolder(r->path);
    case PathRole:
        return r->path;
    case IsDirRole:
        return r->isDir;
    case IconRole:
        if (r->isWeb() || r->isApp()) {
            const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
            if (r->isCommand())
                return FileIconProvider::commandIconUrl(r->target, r->workingDirectory, r->icon, dark);
            if (r->isWeb())
                return FileIconProvider::webIconUrl(r->icon.isEmpty() ? r->target : r->icon, dark);
            if (r->isPlugin())
                return FileIconProvider::pluginIconUrl(r->plugin->pluginId, r->plugin->icon, dark);
            if (r->isPlace() && r->icon.startsWith(u"app:")) // the logo of Settings, of Windows Security
                return FileIconProvider::appIconUrl(r->icon.mid(4), true, dark);
            if (r->isPlace())
                return FileIconProvider::placeIconUrl(r->icon);
            return FileIconProvider::appIconUrl(appIdOf(r->path), r->isPackagedApp(), dark);
        }
        return FileIconProvider::iconUrl(r->path, r->isDir);
    case ThumbnailRole:
        if (!m_thumbnails && r->clipId <= 0)
            return QString();
        [[fallthrough]];
    case GalleryThumbnailRole:
        if (r->clipId > 0) // a screenshot (jt): its picture, whatever the option says
            return r->target.isEmpty() ? QString() : ThumbnailProvider::url(r->target, r->modified);
        if (r->isApp() || r->isDir || r->offline || r->missing)
            return QString();
        return ThumbnailProvider::url(r->path, r->modified);
    case SnippetHtmlRole:
        if (r->snippet.isEmpty())
            return QString();
        return highlighted(r->snippet, r->snippetMatches, m_highlightColor);
    case LineRole:
        return r->line;
    case LocationRole:
        return location(*r);
    case RecentRole:
        return r->recent;
    case IsAppRole:
        return r->isApp();
    case PlaceRole:
        return r->isPlace();
    case WebRole:
        return r->isWeb();
    case SuggestionRole:
        return r->suggestion;
    case PackagedAppRole:
        return r->isPackagedApp();
    case ElevatableRole:
        return r->elevatable;
    case RevealableRole:
        return !r->isApp() || (!r->isPlace() && !r->target.isEmpty());
    case CopyableRole:
        return !r->isApp() || r->hasCopyableTarget();
    case SelectedRole:
        return m_selected.contains(r->path);
    case ModifiedRole:
        return r->isApp() && !r->isPlugin() && r->clipId == 0 ? QString() : modifiedText(r->modified, QDateTime::currentDateTime(), m_showModifiedTime);
    case ModifiedFullRole:
        return r->isApp() && !r->isPlugin() && r->clipId == 0 ? QString() : modifiedFullText(r->modified);
    case ModifiedDateRole:
        if ((r->isApp() && !r->isPlugin() && r->clipId == 0) || knownTime(r->modified) == 0)
            return QString();
        return QDateTime::fromSecsSinceEpoch(r->modified).toLocalTime().toString(u"yyyy-MM-dd HH:mm"_s);
    case SizeRole:
        return (r->isApp() && !r->isPlugin()) || r->isDir || r->size < 0 ? QString() : sizeText(static_cast<std::uint64_t>(r->size));
    case OfflineRole:
        return r->offline;
    case PluginRole:
        return r->isPlugin();
    case SourceRole:
        return resultSource(*r);
    case CreatedDateRole:
        return knownTime(r->created) == 0 ? QString()
            : QDateTime::fromSecsSinceEpoch(r->created).toLocalTime().toString(u"yyyy-MM-dd HH:mm"_s);
    case TypeNameRole:
        return resultType(*r);
    case FolderNameRole:
        return resultFolderName(*r);
    case FavoriteRole:
        return r->favorite;
    case FavoriteKeywordRole:
        return r->favoriteKeyword;
    case MissingRole:
        return r->missing;
    case FolderMatchesRole: {
        QVariantList out;
        if (r->isApp() || r->line > 0) // a content match has every term in the file
            return out;
        for (const auto& [start, length] : folderMatches(r->name, parentFolder(r->path), m_highlights))
            out << start << length;
        return out;
    }
    case SwitchIdRole:
        return r->switchId;
    case SwitchOnRole:
        return static_cast<int>(stateOf(m_switches, *r).on);
    case SwitchLevelRole:
        return static_cast<int>(stateOf(m_switches, *r).level);
    case SwitchBusyRole:
        return stateOf(m_switches, *r).busy;
    case SwitchNoteRole:
        return switchNote(stateOf(m_switches, *r));
    case SwitchErrorRole:
        return stateOf(m_switches, *r).error;
    case SwitchReadyRole: {
        const auto availability = stateOf(m_switches, *r).availability;
        return availability == SwitchState::Availability::Ready || availability == SwitchState::Availability::Unknown;
    }
    case StatusRowRole:
        return r->statusRow;
    case ElsewhereRole:
        return r->elsewhere;
    case PluginCopiesRole: {
        // Not a row that does something else and has nothing of its own to
        // copy (GitHub #113: 传's 接收 row offered to copy the word 接收).
        if (!r->isPlugin() || !r->plugin)
            return false;
        const PluginRow& p = *r->plugin;
        return !p.copy.isEmpty()
            || (p.valid && p.actions.isEmpty() && p.openKind == PluginRow::OpenKind::None && p.searchQuery.isEmpty() && p.setQuery.isEmpty());
    }
    case FolderAwayRole:
        return r->isPlugin() && r->plugin ? r->plugin->subtitleAway : QString();
    default:
        return {};
    }
}

QString ResultModel::switchTitle(const SearchResult& r) const
{
    if (r.switchId.isEmpty())
        return {};
    // A switch without a backend yet is a place: its name, as ever.
    if (r.intent.kind != SwitchIntent::Kind::OpenSettings
        && stateOf(m_switches, r).availability == SwitchState::Availability::Unsupported)
        return {};
    const QString title = switchWords(r, r.intent, true);
    return title == r.name ? QString() : title;
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
        {ThumbnailRole, "thumbnail"},
        {GalleryThumbnailRole, "galleryThumbnail"},
        {SnippetHtmlRole, "snippetHtml"},
        {LineRole, "line"},
        {LocationRole, "location"},
        {RecentRole, "recent"},
        {IsAppRole, "isApp"},
        {PlaceRole, "place"},
        {WebRole, "web"},
        {SuggestionRole, "suggestion"},
        {PackagedAppRole, "packagedApp"},
        {ElevatableRole, "elevatable"},
        {RevealableRole, "revealable"},
        {CopyableRole, "copyable"},
        {SelectedRole, "selected"},
        {ModifiedRole, "modified"},
        {ModifiedFullRole, "modifiedFull"},
        {ModifiedDateRole, "modifiedDate"},
        {SizeRole, "size"},
        {CreatedDateRole, "createdDate"},
        {TypeNameRole, "typeName"},
        {FolderNameRole, "folderName"},
        {OfflineRole, "offline"},
        {PluginRole, "plugin"},
        {SourceRole, "source"},
        {FavoriteRole, "favorite"},
        {FavoriteKeywordRole, "favoriteKeyword"},
        {MissingRole, "missing"},
        {FolderMatchesRole, "folderMatches"},
        {NameMatchesRole, "nameMatches"},
        {SwitchIdRole, "switchId"},
        {SwitchOnRole, "switchOn"},
        {SwitchLevelRole, "switchLevel"},
        {SwitchBusyRole, "switchBusy"},
        {SwitchNoteRole, "switchNote"},
        {SwitchErrorRole, "switchError"},
        {SwitchReadyRole, "switchReady"},
        {StatusRowRole, "statusRow"},
        {ElsewhereRole, "elsewhere"},
        {PluginCopiesRole, "pluginCopies"},
        {FolderAwayRole, "folderAway"},
    };
}

QString ResultModel::pathAt(int row) const
{
    const SearchResult* r = at(row);
    return r ? r->path : QString();
}

QStringList ResultModel::copyTexts() const
{
    constexpr int kRows = 64;
    QStringList texts;
    const int rows = std::min<int>(rowCount(), kRows);
    for (int i = 0; i < rows; ++i) {
        const SearchResult* r = at(i);
        const PluginRow* p = r && r->isPlugin() ? r->plugin.operator->() : nullptr;
        const bool copiesTitle = p && p->valid && p->openKind == PluginRow::OpenKind::None && p->searchQuery.isEmpty()
                                 && p->setQuery.isEmpty() && !p->copy.isEmpty();
        texts.push_back(copiesTitle ? r->name : QString());
    }
    return texts;
}

QString ResultModel::elidedHtml(
    const QString& text, const QVariantList& matches, qreal width, const QFont& font, const QColor& color)
{
    elide::Ranges ranges;
    for (qsizetype i = 0; i + 1 < matches.size(); i += 2) {
        const qsizetype start = matches[i].toLongLong();
        const qsizetype length = matches[i + 1].toLongLong();
        if (start >= 0 && length > 0 && start + length <= text.size())
            ranges.append({start, length});
    }
    if (!ranges.isEmpty())
        diagnostics::watchHighlightFont(font); // GitHub #70: the highlights drawn as other characters
    return elide::shortenedHtml(text, ranges, width, font, color);
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
    keepSelectionInList();
    findElsewhere();
}

void ResultModel::update(int row, const SearchResult& item)
{
    if (row < 0 || row >= count() || m_items[row] == item)
        return;
    m_items[row] = item;
    emit dataChanged(index(row), index(row));
    findElsewhere();
}

void ResultModel::append(const SearchResults& items)
{
    if (items.isEmpty())
        return;
    beginInsertRows({}, count(), count() + static_cast<int>(items.size()) - 1);
    m_items.append(items);
    endInsertRows();
    emit countChanged();
    findElsewhere();
}

void ResultModel::insert(int row, const SearchResult& item)
{
    row = std::clamp(row, 0, count());
    beginInsertRows({}, row, row);
    m_items.insert(row, item);
    endInsertRows();
    emit countChanged();
    findElsewhere();
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
    keepSelectionInList();
    findElsewhere();
    return true;
}

void ResultModel::removeRange(int row, int rows)
{
    rows = std::min(rows, count() - row);
    if (row < 0 || rows <= 0)
        return;
    beginRemoveRows({}, row, row + rows - 1);
    m_items.remove(row, rows);
    endRemoveRows();
    emit countChanged();
    keepSelectionInList();
    findElsewhere();
}

// The rows not in the folder typed in come last (SearchEngine::Request::
// currentFolder): the first of them is where they start. A few hundred rows
// at most, looked through after each change.
void ResultModel::findElsewhere()
{
    const SearchResults& items = std::as_const(m_items);
    const auto it = std::ranges::find_if(items, [](const SearchResult& r) { return r.elsewhere; });
    const int row = it == items.cend() ? -1 : static_cast<int>(it - items.cbegin());
    if (row == m_elsewhereRow)
        return;
    m_elsewhereRow = row;
    emit elsewhereRowChanged();
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
    if (removed > 0) {
        emit countChanged();
        keepSelectionInList();
        findElsewhere();
    }
    return removed;
}

void ResultModel::clear()
{
    assign({}, {});
}

void ResultModel::retranslate()
{
    if (!m_items.isEmpty()) // a web search's title and a content match's place ("第 2 页") too
        emit dataChanged(index(0), index(count() - 1),
            {FolderRole, NameRole, NameHtmlRole, LocationRole, ModifiedRole, ModifiedFullRole});
}

// Briefly, as a mail list dates its mail: the time today and yesterday, the
// day of the week within the last week, the day this year, else the date.
void ResultModel::setShowModifiedTime(bool on)
{
    if (m_showModifiedTime == on)
        return;
    m_showModifiedTime = on;
    if (count() > 0)
        emit dataChanged(index(0), index(count() - 1), {ModifiedRole});
}

QString ResultModel::modifiedText(std::uint32_t modified, const QDateTime& now, bool withTime)
{
    if (knownTime(modified) == 0)
        return {}; // 1970-01-01 and the like: not a time anyone wrote it
    const QLocale locale;
    const QDateTime when = QDateTime::fromSecsSinceEpoch(modified).toLocalTime();
    const QDate day = when.date();
    const qint64 daysAgo = day.daysTo(now.date());
    const QString time = locale.toString(when.time(), QLocale::ShortFormat);
    if (daysAgo < 0)
        return locale.toString(when, QLocale::ShortFormat); // ahead of the clock: no guessing
    if (daysAgo == 0)
        return tr("今天 %1").arg(time);
    if (daysAgo == 1)
        return tr("昨天 %1").arg(time);
    if (daysAgo < 7)
        return locale.dayName(day.dayOfWeek(), QLocale::ShortFormat) + u' ' + time;
    //: A date this year, as QLocale::toString formats it: "Oct 3"
    if (day.year() == now.date().year())
        return locale.toString(day, tr("M月d日")) + (withTime ? u' ' + time : QString());
    //: An earlier date, as QLocale::toString formats it: "Mar 5, 2024"
    return locale.toString(day, tr("yyyy年M月d日")) + (withTime ? u' ' + time : QString());
}

QString ResultModel::modifiedFullText(std::uint32_t modified)
{
    if (knownTime(modified) == 0)
        return {};
    const QDateTime when = QDateTime::fromSecsSinceEpoch(modified).toLocalTime();
    //: When a file was last written, in full, as QLocale::toString formats it: "Oct 3, 2026, 14:32"
    return tr("修改于 %1").arg(QLocale().toString(when, tr("yyyy年M月d日 HH:mm")));
}

QString ResultModel::location(const SearchResult& r)
{
    using Where = SearchResult::Where;
    switch (r.where) {
    case Where::Line:
        return r.line > 0 ? tr("第 %1 行").arg(r.line) : QString();
    case Where::Document:
        return {};
    case Where::Page:
        return tr("第 %1 页").arg(r.placeNumber);
    case Where::Slide:
        return tr("第 %1 张幻灯片").arg(r.placeNumber);
    case Where::Row: {
        constexpr qsizetype kSheetChars = 12; // a long sheet name would squeeze the snippet
        const QString sheet = r.sheet.size() > kSheetChars ? r.sheet.left(kSheetChars - 1) + u'…' : r.sheet;
        return sheet.isEmpty() ? tr("第 %1 行").arg(r.placeNumber) : tr("%1 第 %2 行").arg(sheet).arg(r.placeNumber);
    }
    }
    return {};
}

bool ResultModel::isSelected(int row) const
{
    const SearchResult* r = at(row);
    return r && m_selected.contains(r->path);
}

void ResultModel::setSelection(QSet<QString> paths)
{
    QSet<QString> inList;
    for (const SearchResult& r : std::as_const(m_items)) {
        if (paths.contains(r.path))
            inList.insert(r.path);
    }
    if (inList == m_selected)
        return;
    std::swap(m_selected, inList); // inList: the old selection now
    for (int row = 0; row < count(); ++row) {
        const QString& path = m_items[row].path;
        if (m_selected.contains(path) != inList.contains(path))
            emit dataChanged(index(row), index(row), {SelectedRole});
    }
    emit selectionChanged();
}

SearchResults ResultModel::selection() const
{
    SearchResults items;
    QSet<QString> seen;
    for (const SearchResult& r : m_items) {
        if (m_selected.contains(r.path) && !seen.contains(r.path)) {
            seen.insert(r.path);
            items.append(r);
        }
    }
    return items;
}

void ResultModel::keepSelectionInList()
{
    if (m_selected.isEmpty())
        return;
    QSet<QString> inList;
    for (const SearchResult& r : std::as_const(m_items)) {
        if (m_selected.contains(r.path))
            inList.insert(r.path);
    }
    if (inList.size() == m_selected.size())
        return;
    m_selected = std::move(inList); // rows that went away need no dataChanged
    emit selectionChanged();
}

void ResultModel::setHighlightColor(const QColor& color)
{
    if (color == m_highlightColor)
        return;
    m_highlightColor = color;
    emit highlightColorChanged();
    if (!m_items.isEmpty()) // a web search's title too, and where content matches are
        emit dataChanged(index(0), index(count() - 1), {NameHtmlRole, SnippetHtmlRole});
}

void ResultModel::setThumbnails(bool on)
{
    if (on == m_thumbnails)
        return;
    m_thumbnails = on;
    if (!m_items.isEmpty())
        emit dataChanged(index(0), index(count() - 1), {ThumbnailRole});
}

} // namespace ws
