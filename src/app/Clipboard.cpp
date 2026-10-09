#include "Clipboard.h"

#include "ColorText.h"
#include "FileIconProvider.h"
#include "platform/Paster.h"
#include "platform/Shell.h"
#include "platform/WindowEffects.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLocale>
#include <QScreen>
#include <QStyleHints>
#include <QUrl>

#include <algorithm>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr qint64 kModifierWaitMs = 1500; // held longer than that, the paste goes ahead anyway
constexpr qint64 kFocusWaitMs = 800;
// In front is not ready yet: the program still has to put its focus back
// (on its text field), or the paste keys go to its window and are lost.
constexpr qint64 kSettleMs = 60;
constexpr qsizetype kPreviewChars = 20000; // a preview shows no more; pasting takes all of it
constexpr int kPreviewFiles = 50;

const QStringList kSeparators {u"newline"_s, u"space"_s, u"comma"_s, u"tab"_s, u"none"_s};

qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

bool isLocal(const QString& path)
{
    return path.size() >= 3 && path[1] == u':' && path[0].isLetter();
}

} // namespace

Clipboard::Clipboard(ClipStore* store, ClipboardWatcher* watcher, QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_watcher(watcher)
    , m_model(store)
{
    m_separator = m_store->value(u"separator"_s);
    if (!kSeparators.contains(m_separator))
        m_separator = kSeparators.constFirst();
    m_flashTimer.setSingleShot(true);
    m_flashTimer.setInterval(3s);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] {
        m_flash.clear();
        emit statusChanged();
    });
    m_keysTimer.setInterval(10ms);
    connect(&m_keysTimer, &QTimer::timeout, this, [this] {
        if (m_intoTarget && paste::modifiersDown() && m_pasteClock.elapsed() < kModifierWaitMs)
            return;
        m_keysTimer.stop();
        // On the clipboard first (the watcher's thread), then into the window.
        m_watcher->write(m_write, [this](bool ok) {
            QMetaObject::invokeMethod(this, [this, ok] { onWritten(ok); }, Qt::QueuedConnection);
        });
    });
    m_focusTimer.setInterval(10ms);
    connect(&m_focusTimer, &QTimer::timeout, this, [this] {
        const bool front = paste::isForeground(m_pasteWindow);
        if (front && !m_frontClock.isValid())
            m_frontClock.start();
        if (front ? m_frontClock.elapsed() < kSettleMs : m_pasteClock.elapsed() < kFocusWaitMs)
            return;
        m_focusTimer.stop();
        m_pasting = false;
        if (front)
            paste::sendPasteKeys(m_pasteWindow);
        else if (m_window && m_window->isVisible())
            flash(tr("已复制。那个窗口没有回到前面，请自己按 Ctrl+V 粘贴"));
    });
    refresh();
}

void Clipboard::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    emit activeChanged();
}

void Clipboard::setState(bool recording, bool paused, const QString& shortcut)
{
    if (m_recording == recording && m_paused == paused && m_shortcut == shortcut)
        return;
    m_recording = recording;
    m_paused = paused;
    m_shortcut = shortcut;
    refreshStatus();
    emit stateChanged();
}

void Clipboard::setQuery(const QString& query)
{
    if (m_query == query)
        return;
    m_query = query;
    emit queryChanged();
    refresh();
    emit listChanged();
}

std::vector<Clipboard::Category> Clipboard::categoryList() const
{
    using Kind = ClipFilter::Category;
    std::vector<Category> list {{Kind::All}, {Kind::Text}, {Kind::Links}, {Kind::Images}, {Kind::Files}};
    for (const ClipGroup& group : m_store->groups())
        list.push_back({Kind::Group, group.id});
    return list;
}

QVariantList Clipboard::categories() const
{
    QVariantList list;
    for (const Category& c : categoryList()) {
        QString title;
        switch (c.kind) {
        case ClipFilter::Category::All:
            title = tr("全部");
            break;
        case ClipFilter::Category::Text:
            title = tr("文本");
            break;
        case ClipFilter::Category::Links:
            title = tr("链接");
            break;
        case ClipFilter::Category::Images:
            title = tr("图片");
            break;
        case ClipFilter::Category::Files:
            title = tr("文件");
            break;
        case ClipFilter::Category::Group: {
            const ClipGroup* g = m_store->group(c.group);
            title = c.group == ClipStore::kPinned ? tr("固定") : g ? g->name : QString();
            break;
        }
        }
        list.append(QVariantMap {
            {u"title"_s, title},
            {u"group"_s, c.group},
            {u"pinned"_s, c.group == ClipStore::kPinned},
            {u"size"_s, c.group != 0 ? m_store->groupSize(c.group) : 0},
        });
    }
    return list;
}

void Clipboard::setCategory(int category)
{
    const std::vector<Category> list = categoryList();
    category = std::clamp(category, 0, static_cast<int>(list.size()) - 1);
    if (category == m_category)
        return;
    m_category = category;
    m_categoryGroup = list[static_cast<std::size_t>(category)].group;
    emit categoryChanged();
    refresh();
    emit listChanged();
}

void Clipboard::cycleCategory(int delta)
{
    const int n = static_cast<int>(categoryList().size());
    setCategory(((m_category + delta) % n + n) % n);
}

void Clipboard::refresh()
{
    // A group tab stays on its group when groups before it come or go.
    const std::vector<Category> list = categoryList();
    if (m_categoryGroup != 0) {
        const auto it = std::find_if(list.begin(), list.end(), [&](const Category& c) { return c.group == m_categoryGroup; });
        const int index = it == list.end() ? 0 : static_cast<int>(it - list.begin());
        if (it == list.end())
            m_categoryGroup = 0;
        if (index != m_category) {
            m_category = index;
            emit categoryChanged();
        }
    }
    const Category& category = list[static_cast<std::size_t>(std::clamp(m_category, 0, static_cast<int>(list.size()) - 1))];
    m_model.setRows(m_store->find({category.kind, category.group, m_query.trimmed()}), m_query.trimmed());
    refreshStatus();
}

void Clipboard::historyChanged()
{
    refresh();
    emit categoriesChanged(); // the sizes of the groups
}

void Clipboard::refreshStatus()
{
    QString status;
    if (m_paused)
        status = tr("已暂停记录，在托盘菜单里可以继续");
    else if (!m_recording)
        status = tr("没有在记录剪贴板");
    else if (!m_query.trimmed().isEmpty() || m_category != 0)
        status = tr("%Ln 条", nullptr, m_model.count());
    else
        status = tr("共 %Ln 条", nullptr, total());
    if (status != m_status) {
        m_status = status;
        emit statusChanged();
    }
}

void Clipboard::flash(const QString& message)
{
    m_flash = message;
    m_flashTimer.start();
    emit statusChanged();
}

void Clipboard::handleShown()
{
    m_flash.clear();
    m_flashTimer.stop();
    dropUndo();
    m_model.clearSelection();
    if (!m_query.isEmpty()) {
        m_query.clear();
        emit queryChanged();
    }
    if (m_category != 0) {
        m_category = 0;
        m_categoryGroup = 0;
        emit categoryChanged();
    }
    m_model.refreshTimes();
    refresh();
    emit categoriesChanged();
    emit listChanged();
    emit shown();
}

void Clipboard::handleHidden()
{
    m_model.clearSelection();
    dropUndo();
}

void Clipboard::retranslate()
{
    m_model.retranslate();
    refreshStatus();
    emit categoriesChanged();
    emit separatorChanged();
}

std::vector<const Clip*> Clipboard::targets(int row) const
{
    if (row < 0 || (m_model.isSelected(row) && m_model.selectedCount() > 1))
        return m_model.selection();
    if (const Clip* c = m_model.at(row))
        return {c};
    return {};
}

QString Clipboard::separator() const
{
    if (m_separator == u"space")
        return u" "_s;
    if (m_separator == u"comma")
        return u", "_s;
    if (m_separator == u"tab")
        return u"\t"_s;
    if (m_separator == u"none")
        return {};
    return u"\r\n"_s;
}

QString Clipboard::separatorName() const
{
    if (m_separator == u"space")
        return tr("空格");
    if (m_separator == u"comma")
        return tr("逗号");
    if (m_separator == u"tab")
        return tr("Tab");
    if (m_separator == u"none")
        return tr("不分隔");
    return tr("换行");
}

void Clipboard::cycleSeparator()
{
    m_separator = kSeparators[(kSeparators.indexOf(m_separator) + 1) % kSeparators.size()];
    m_store->setValue(u"separator"_s, m_separator);
    emit separatorChanged();
}

ClipWrite Clipboard::writeFor(const std::vector<const Clip*>& clips, bool plainText, QString* problem) const
{
    ClipWrite write;
    if (clips.size() == 1) {
        const Clip& c = *clips.front();
        write.id = c.id;
        switch (c.kind) {
        case ClipKind::Image:
            if (plainText)
                *problem = tr("图片没有可以粘贴的文字");
            else
                write.imagePath = m_store->imagePath(c.id);
            break;
        case ClipKind::Files: {
            QStringList files = c.files();
            if (plainText) {
                write.text = files.join(u"\r\n"_s);
                break;
            }
            files.removeIf([](const QString& f) { return isLocal(f) && !QFileInfo::exists(f); });
            if (files.isEmpty())
                *problem = tr("这些文件已经不存在了");
            write.files = files;
            break;
        }
        default: {
            ClipPayload payload = m_store->payload(c.id);
            write.text = std::move(payload.text);
            if (!plainText) {
                write.html = std::move(payload.html);
                write.rtf = std::move(payload.rtf);
            }
            break;
        }
        }
        return write;
    }
    // Several: one paste of all of them, in the order they were picked.
    const ClipBundle joined = bundle(clips, [this](const Clip& c) { return m_store->payload(c.id).text; }, separator());
    if (!joined.files.isEmpty()) {
        if (plainText)
            write.text = joined.files.join(u"\r\n"_s);
        else
            write.files = joined.files;
    } else {
        write.text = joined.text;
    }
    if (write.empty())
        *problem = tr("图片只能一张一张地粘贴");
    return write;
}

void Clipboard::startPaste(ClipWrite write, bool intoTarget, std::vector<qint64> touched, QString done)
{
    m_pasting = true;
    m_write = std::move(write);
    m_intoTarget = intoTarget;
    m_touched = std::move(touched);
    m_doneMessage = std::move(done);
    m_pasteClock.start();
    m_keysTimer.start();
}

void Clipboard::onWritten(bool ok)
{
    if (!ok) {
        m_pasting = false;
        flash(tr("剪贴板正被别的程序占用，请再试一次"));
        return;
    }
    // Joined pastes are not added to the history: their entries move up
    // instead, the first picked on top. (A single entry moves up when the
    // watcher sees it come back.)
    if (!m_touched.empty()) {
        const qint64 time = now();
        for (auto it = m_touched.rbegin(); it != m_touched.rend(); ++it)
            m_store->touch(*it, time + (it - m_touched.rbegin()) + 1);
        historyChanged();
    }
    if (!m_intoTarget) {
        m_pasting = false;
        flash(m_doneMessage);
        return;
    }
    m_pasteWindow = paste::usableTarget(m_target);
    if (!m_pasteWindow) {
        m_pasting = false;
        flash(tr("已复制。没有找到要粘贴进去的窗口，可以自己按 Ctrl+V"));
        return;
    }
    paste::activate(m_pasteWindow); // the launcher loses the focus and closes (App)
    m_pasteClock.restart();
    m_frontClock.invalidate();
    m_focusTimer.start();
}

void Clipboard::paste(int row, bool plainText)
{
    if (m_pasting)
        return;
    const std::vector<const Clip*> clips = targets(row);
    if (clips.empty())
        return;
    QString problem;
    ClipWrite write = writeFor(clips, plainText, &problem);
    if (write.empty()) {
        flash(problem);
        return;
    }
    std::vector<qint64> touched;
    if (clips.size() > 1) {
        for (const Clip* c : clips)
            touched.push_back(c->id);
    }
    startPaste(std::move(write), true, std::move(touched), {});
}

void Clipboard::quickPaste(int number, bool plainText)
{
    if (number < 1 || number > m_model.count())
        return;
    m_model.clearSelection();
    paste(number - 1, plainText);
}

void Clipboard::copy(int row)
{
    if (m_pasting)
        return;
    const std::vector<const Clip*> clips = targets(row);
    if (clips.empty())
        return;
    QString problem;
    ClipWrite write = writeFor(clips, false, &problem);
    if (write.empty()) {
        flash(problem);
        return;
    }
    std::vector<qint64> touched;
    if (clips.size() > 1) {
        for (const Clip* c : clips)
            touched.push_back(c->id);
    }
    const QString done = clips.size() > 1 ? tr("已把 %Ln 条合在一起复制", nullptr, static_cast<int>(clips.size()))
                                          : tr("已复制");
    startPaste(std::move(write), false, std::move(touched), done);
}

void Clipboard::copyText(const QString& text, bool remember)
{
    if (m_pasting || text.isEmpty())
        return;
    ClipWrite write;
    write.text = text;
    if (remember && m_recording && !m_paused) { // a copy like any other: in the history, on top
        ClipCapture capture;
        capture.kind = kindOfText(text);
        capture.text = text;
        capture.source = tr("Win顺");
        capture.sourcePath = QCoreApplication::applicationFilePath();
        write.id = m_store->add(capture, now());
        historyChanged();
    }
    startPaste(std::move(write), false, {}, tr("已复制 %1").arg(text));
}

void Clipboard::togglePin(int row)
{
    const std::vector<const Clip*> clips = targets(row);
    if (clips.empty())
        return;
    const bool pinned = std::all_of(clips.begin(), clips.end(), [](const Clip* c) { return c->group == ClipStore::kPinned; });
    std::vector<qint64> ids;
    for (const Clip* c : clips)
        ids.push_back(c->id);
    m_store->setGroup(ids, pinned ? 0 : ClipStore::kPinned);
    historyChanged();
    flash(pinned ? tr("已取消固定") : tr("已固定，不会过期"));
}

void Clipboard::moveToGroup(int row, qint64 group)
{
    const std::vector<const Clip*> clips = targets(row);
    if (clips.empty() || (group != 0 && !m_store->group(group)))
        return;
    std::vector<qint64> ids;
    for (const Clip* c : clips)
        ids.push_back(c->id);
    m_store->setGroup(ids, group);
    historyChanged();
    if (group == 0)
        flash(tr("已移出分组"));
    else if (group == ClipStore::kPinned)
        flash(tr("已固定，不会过期"));
    else
        flash(tr("已移到“%1”，不会过期").arg(m_store->group(group)->name));
}

qint64 Clipboard::addGroup(const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return 0;
    for (const ClipGroup& g : m_store->groups()) {
        if (g.id != ClipStore::kPinned && g.name == trimmed)
            return g.id;
    }
    if (m_removedGroup)
        dropUndo(); // the new group may take the removed one's id
    const qint64 id = m_store->addGroup(trimmed);
    emit categoriesChanged();
    return id;
}

void Clipboard::renameGroup(qint64 group, const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return;
    m_store->renameGroup(group, trimmed);
    emit categoriesChanged();
    m_model.refreshTimes(); // the rows show the name
}

void Clipboard::remove(int row)
{
    const std::vector<const Clip*> clips = targets(row);
    if (clips.empty())
        return;
    std::vector<qint64> ids;
    for (const Clip* c : clips)
        ids.push_back(c->id);
    dropUndo();
    m_removed = m_store->remove(ids);
    historyChanged();
    emit undoChanged();
    flash(tr("已删除 %Ln 条，按 Ctrl+Z 撤销", nullptr, static_cast<int>(ids.size())));
}

void Clipboard::undo()
{
    if (m_removedGroup) {
        const QString name = m_removedGroup->group.name;
        m_store->restoreGroup(*std::exchange(m_removedGroup, std::nullopt));
        historyChanged();
        emit undoChanged();
        flash(tr("已恢复分组“%1”").arg(name));
        return;
    }
    if (m_removed.empty())
        return;
    const auto n = static_cast<int>(m_removed.size());
    m_store->restore(std::exchange(m_removed, {}));
    historyChanged();
    emit undoChanged();
    flash(tr("已恢复 %Ln 条", nullptr, n));
}

void Clipboard::dropUndo()
{
    if (m_removed.empty() && !m_removedGroup)
        return;
    std::vector<qint64> ids;
    for (const ClipRecord& r : std::exchange(m_removed, {}))
        ids.push_back(r.clip.id);
    if (m_removedGroup) {
        for (const ClipRecord& r : m_removedGroup->clips)
            ids.push_back(r.clip.id);
        m_removedGroup.reset();
    }
    m_store->discardImages(ids); // their pictures could still have come back until now
    emit undoChanged();
}

void Clipboard::toggleSelected(int row)
{
    m_model.toggle(row);
}

void Clipboard::selectRange(int from, int to, bool add)
{
    m_model.selectRange(from, to, add);
}

void Clipboard::clearSelection()
{
    m_model.clearSelection();
}

void Clipboard::dismiss()
{
    emit dismissRequested();
}

void Clipboard::turnOn()
{
    emit turnOnRequested();
}

QVariantMap Clipboard::preview(int row) const
{
    const Clip* c = m_model.at(row);
    if (!c)
        return {};
    QVariantMap map {
        {u"kind"_s, static_cast<int>(c->kind)},
        {u"source"_s, c->source},
        {u"sourceIcon"_s, c->sourcePath.isEmpty() ? QString() : FileIconProvider::iconUrl(c->sourcePath, false)},
        {u"time"_s, QLocale().toString(QDateTime::fromMSecsSinceEpoch(c->used), tr("yyyy年M月d日 HH:mm"))},
        {u"formatted"_s, c->html || c->rtf},
        {u"swatch"_s, ClipModel::colorOf(*c)},
    };
    if (c->kind == ClipKind::Text) {
        if (const std::optional<QColor> color = colortext::parse(c->text)) {
            QVariantList notations;
            for (const auto& [label, value, readsBack] : colortext::notations(*color))
                notations.append(QVariantMap {{u"label"_s, label}, {u"value"_s, value}, {u"remember"_s, readsBack}});
            map.insert(u"colors"_s, notations);
        }
    }
    QString size;
    switch (c->kind) {
    case ClipKind::Image:
        map.insert(u"image"_s, QUrl::fromLocalFile(m_store->imagePath(c->id)).toString());
        map.insert(u"width"_s, c->width);
        map.insert(u"height"_s, c->height);
        size = u"%1 × %2 · %3"_s.arg(c->width).arg(c->height).arg(
            QLocale().formattedDataSize(c->bytes, 1, QLocale::DataSizeTraditionalFormat));
        break;
    case ClipKind::Files:
    case ClipKind::Path: {
        const QStringList paths = c->files();
        QVariantList files;
        for (const QString& path : paths.mid(0, kPreviewFiles)) {
            const QFileInfo info(path);
            const bool local = isLocal(path);
            const bool exists = !local || info.exists();
            files.append(QVariantMap {
                {u"name"_s, info.fileName().isEmpty() ? path : info.fileName()},
                {u"folder"_s, QDir::toNativeSeparators(info.path())},
                {u"icon"_s, FileIconProvider::iconUrl(path, local && exists && info.isDir())},
                {u"missing"_s, !exists},
            });
        }
        map.insert(u"files"_s, files);
        map.insert(u"more"_s, std::max<qsizetype>(0, paths.size() - kPreviewFiles));
        // The first file's thumbnail, if Explorer has one (a picture, a video,
        // a PDF...); local files only: a network one could keep it waiting.
        if (!paths.isEmpty() && isLocal(paths.constFirst())) {
            const QFileInfo first(paths.constFirst());
            if (first.isFile())
                map.insert(u"thumbnail"_s, FileIconProvider::thumbnailUrl(paths.constFirst()));
        }
        if (c->kind == ClipKind::Files)
            size = tr("%Ln 个文件", nullptr, static_cast<int>(paths.size()));
        else
            map.insert(u"text"_s, c->text.left(kPreviewChars));
        break;
    }
    default: {
        const QString text = c->text.left(kPreviewChars);
        map.insert(u"text"_s, text);
        map.insert(u"more"_s, std::max<qint64>(0, c->textLength - text.size()));
        const auto lines = static_cast<int>(c->text.count(u'\n') + 1);
        size = tr("%Ln 字", nullptr, static_cast<int>(std::min<qint64>(c->textLength, INT_MAX)));
        if (lines > 1)
            size += u" · "_s + tr("%Ln 行", nullptr, lines);
        break;
    }
    }
    map.insert(u"size"_s, size);
    map.insert(u"group"_s, c->group == 0 ? QString()
            : c->group == ClipStore::kPinned ? tr("固定")
            : m_store->group(c->group) ? m_store->group(c->group)->name : QString());
    return map;
}

QVariantList Clipboard::menuItems(int row) const
{
    const std::vector<const Clip*> clips = targets(row);
    if (clips.empty())
        return {};
    const auto entry = [](int action, const QString& text, const QString& shortcut, const QString& glyph) {
        return QVariantMap {
            {u"action"_s, action},
            {u"text"_s, text},
            {u"shortcut"_s, shortcut},
            {u"glyph"_s, glyph},
        };
    };
    const QVariantMap separator {{u"separator"_s, true}};
    const auto n = static_cast<int>(clips.size());
    const bool many = n > 1;
    const bool onlyImages = std::all_of(clips.begin(), clips.end(), [](const Clip* c) { return c->kind == ClipKind::Image; });
    // Glyphs: Segoe Fluent Icons / MDL2 Assets.
    QVariantList items;
    items.append(entry(Paste, many ? tr("粘贴 %Ln 条", nullptr, n) : tr("粘贴"), u"Enter"_s, u""_s));
    if (!onlyImages)
        items.append(entry(PastePlain, tr("粘贴为纯文本"), u"Shift+Enter"_s, u""_s));
    items.append(entry(Copy, many ? tr("复制 %Ln 条", nullptr, n) : tr("复制"), u"Ctrl+C"_s, u""_s));
    if (!many) {
        const Clip& c = *clips.front();
        if (c.kind == ClipKind::Link)
            items.append(entry(OpenLink, tr("在浏览器中打开"), QString(), u""_s));
        else if (c.kind == ClipKind::Files || c.kind == ClipKind::Path)
            items.append(entry(Reveal, tr("打开所在位置"), QString(), u""_s));
        else if (c.kind == ClipKind::Image)
            items.append(entry(OpenImage, tr("打开图片"), QString(), u""_s));
    }
    items.append(separator);
    const bool pinned = std::all_of(clips.begin(), clips.end(), [](const Clip* c) { return c->group == ClipStore::kPinned; });
    items.append(entry(Pin, pinned ? tr("取消固定") : tr("固定"), u"Ctrl+P"_s, pinned ? u""_s : u""_s));
    for (const ClipGroup& g : m_store->groups()) {
        if (g.id == ClipStore::kPinned)
            continue;
        if (std::all_of(clips.begin(), clips.end(), [&](const Clip* c) { return c->group == g.id; }))
            continue;
        items.append(entry(MoveToGroup + static_cast<int>(g.id), tr("移到“%1”").arg(g.name), QString(), u""_s));
    }
    items.append(entry(NewGroup, tr("移到新分组…"), QString(), u""_s));
    if (std::any_of(clips.begin(), clips.end(), [](const Clip* c) { return c->group > ClipStore::kPinned; }))
        items.append(entry(LeaveGroup, tr("移出分组"), QString(), u""_s));
    items.append(separator);
    items.append(entry(Remove, many ? tr("删除 %Ln 条", nullptr, n) : tr("删除"), u"Delete"_s, u""_s));
    return items;
}

QVariantList Clipboard::categoryMenuItems(int category) const
{
    const std::vector<Category> list = categoryList();
    if (category < 0 || category >= static_cast<int>(list.size()))
        return {};
    const qint64 group = list[static_cast<std::size_t>(category)].group;
    if (group == 0 || group == ClipStore::kPinned)
        return {};
    const auto entry = [](int action, const QString& text, const QString& glyph) {
        return QVariantMap {{u"action"_s, action}, {u"text"_s, text}, {u"glyph"_s, glyph}};
    };
    const int size = m_store->groupSize(group);
    return {
        entry(RenameGroup, tr("重命名…"), u""_s),
        entry(RemoveGroup, size > 0 ? tr("删除分组和里面的 %Ln 条", nullptr, size) : tr("删除分组"), u""_s),
    };
}

void Clipboard::trigger(int row, int action)
{
    if (action >= MoveToGroup) {
        moveToGroup(row, action - MoveToGroup);
        return;
    }
    switch (static_cast<Action>(action)) {
    case Paste:
        paste(row, false);
        break;
    case PastePlain:
        paste(row, true);
        break;
    case Copy:
        copy(row);
        break;
    case Pin:
        togglePin(row);
        break;
    case NewGroup:
        emit groupNameRequested(row, 0);
        break;
    case LeaveGroup:
        moveToGroup(row, 0);
        break;
    case Remove:
        remove(row);
        break;
    case OpenLink:
    case Reveal:
    case OpenImage: {
        const Clip* c = m_model.at(row);
        if (!c)
            break;
        if (action == OpenLink) {
            const QString url = c->text.trimmed();
            shell::openUrl(url.startsWith(u"www.", Qt::CaseInsensitive) ? u"https://"_s + url : url);
        } else if (action == Reveal) {
            QStringList files = c->files();
            files.removeIf([](const QString& f) { return isLocal(f) && !QFileInfo::exists(f); });
            if (files.isEmpty()) {
                flash(tr("这些文件已经不存在了"));
                break;
            }
            shell::reveal(files);
        } else {
            shell::open(m_store->imagePath(c->id));
        }
        dismiss();
        break;
    }
    default:
        break;
    }
}

void Clipboard::triggerCategory(int category, int action)
{
    const std::vector<Category> list = categoryList();
    if (category < 0 || category >= static_cast<int>(list.size()))
        return;
    const qint64 group = list[static_cast<std::size_t>(category)].group;
    if (action == RenameGroup) {
        emit groupNameRequested(-1, group);
    } else if (action == RemoveGroup && group > ClipStore::kPinned) {
        dropUndo();
        ClipGroupRecord removed = m_store->removeGroup(group);
        if (removed.group.id == 0)
            return;
        const QString name = removed.group.name;
        const auto n = static_cast<int>(removed.clips.size());
        m_removedGroup = std::move(removed);
        historyChanged();
        emit undoChanged();
        flash(n > 0 ? tr("已删除分组“%1”和里面的 %Ln 条，按 Ctrl+Z 撤销", nullptr, n).arg(name)
                    : tr("已删除分组“%1”，按 Ctrl+Z 撤销").arg(name));
    }
}

QRectF Clipboard::screenArea(QPointF globalPos) const
{
    QScreen* screen = QGuiApplication::screenAt(globalPos.toPoint());
    if (!screen && m_window)
        screen = m_window->screen();
    return screen ? QRectF(screen->availableGeometry()) : QRectF();
}

void Clipboard::prepareMenuWindow(QWindow* menu) const
{
    if (!menu)
        return;
    // Showing it must not take the focus from the launcher (see Launcher::prepareMenuWindow).
    menu->setProperty("_q_showWithoutActivating", true);
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    win::styleFramelessWindow(menu);
    win::setDarkFrame(menu, dark);
}

} // namespace ws
