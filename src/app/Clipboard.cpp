#include "Clipboard.h"

#include "ClipFiles.h"
#include "ColorText.h"
#include "FileIconProvider.h"
#include "HotkeySteps.h"
#include "TextField.h"
#include "platform/ComWorker.h"
#include "platform/DragOut.h"
#include "platform/FileDialog.h"
#include "platform/FileManagers.h"
#include "platform/Integrity.h"
#include "platform/Paster.h"
#include "platform/Shell.h"
#include "platform/WindowEffects.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLocale>
#include <QQuickWindow>
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
constexpr qint64 kClipboardWaitMs = 300; // held open longer than that, the paste goes ahead anyway
// The search box by a file dialog shows again only once the dialog is back
// in front.
constexpr qint64 kFieldWaitMs = 1000;
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
    , m_dragFolder(store->folder() + u"/dragged"_s)
{
    QDir(m_dragFolder).removeRecursively();
    loadHistoryState();
    m_store->setListener({
        .lost = [this] { historyChanged(); },
        .pictureSaved = [this](qint64 id) {
            if (!m_recording || m_paused || !m_store->isOpen())
                return;
            m_model.pictureSaved(id); // the preview follows (dataChanged)
            m_recognizer.read(id, m_store->imagePath(id), true);
        },
    });
    // The words in pictures: kept with them, found by the search.
    connect(&m_recognizer, &TextRecognizer::done, this, [this](qint64 id, const QString& words, bool ok) {
        if (!m_recording || m_paused || !m_store->isOpen() || !ok)
            return; // no recognizer, or it failed: read when the app starts again
        m_store->setPictureText(id, words);
        m_model.pictureRead(id);
        if (m_rereading.remove(id) && m_rereading.isEmpty())
            m_store->setValue(u"recognizer"_s, m_recognizer.engine());
    });
    // Those copied before, or before there was this: a little after the start,
    // out of its way. All of them when they were read by another recognizer
    // (Windows' before 0.7.4), those not read yet first.
    m_backlogTimer.setSingleShot(true);
    m_backlogTimer.setInterval(20s);
    connect(&m_backlogTimer, &QTimer::timeout, this, [this] {
        if (!m_recording || m_paused || !m_store->isOpen())
            return;
        const bool again = m_store->value(u"recognizer"_s) != m_recognizer.engine();
        const std::vector<qint64> ids = m_store->unreadPictures(again);
        if (again && ids.empty())
            m_store->setValue(u"recognizer"_s, m_recognizer.engine());
        for (const qint64 id : ids) {
            if (again)
                m_rereading.insert(id);
            m_recognizer.read(id, m_store->imagePath(id));
        }
    });
    if (m_store->isOpen())
        m_backlogTimer.start();
    m_flashTimer.setSingleShot(true);
    m_flashTimer.setInterval(3s);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] {
        m_flash.clear();
        emit statusChanged();
    });
    m_keysTimer.setInterval(10ms);
    connect(&m_keysTimer, &QTimer::timeout, this, [this] {
        if (m_into == Into::CurrentProgram) {
            if (!paste::isForeground(m_pasteWindow)) {
                failPaste(tr("已取消快捷粘贴：当前窗口已切换"));
                return;
            }
            if (paste::modifiersDown()) {
                if (m_pasteClock.elapsed() >= kModifierWaitMs)
                    failPaste(tr("已取消快捷粘贴，请松开修饰键后重试"));
                return;
            }
        }
        if (m_into == Into::Program && paste::modifiersDown() && m_pasteClock.elapsed() < kModifierWaitMs)
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
        if (m_into == Into::CurrentProgram) {
            if (!front) {
                failPaste(tr("内容已复制；窗口已切换，未自动粘贴"));
                return;
            }
            if (paste::modifiersDown()) {
                if (m_pasteClock.elapsed() >= kModifierWaitMs)
                    failPaste(tr("内容已复制，请松开修饰键后按 Ctrl+V 粘贴"));
                return;
            }
        }
        if (front && !m_frontClock.isValid())
            m_frontClock.start();
        // In front: a moment more, and while some program has the clipboard
        // open (clipboard history, other managers read each new copy): the
        // target's Ctrl+V finds it held and pastes nothing (pitfalls.md).
        if (front ? m_frontClock.elapsed() < kSettleMs
                        || (::GetOpenClipboardWindow() && m_frontClock.elapsed() < kClipboardWaitMs)
                  : m_pasteClock.elapsed() < kFocusWaitMs)
            return;
        m_focusTimer.stop();
        const std::unique_ptr<paste::Guard> guard = std::move(m_pasteGuard); // done with either way
        // File lists go into an upload dialog's name field, even if its folder
        // list held focus. Never send Ctrl+V there: it could copy files into
        // that folder. Leave Open to the user, as a normal paste would (#57).
        const auto dialogKind
            = front && !m_write.files.isEmpty() ? filedialog::kind(m_pasteWindow) : filedialog::Kind::None;
        if (dialogKind == filedialog::Kind::Open || dialogKind == filedialog::Kind::Legacy) {
            const HWND target = m_pasteWindow;
            const QString names = clipfiles::fileNames(m_write.files);
            if (!m_filePasteWorker)
                m_filePasteWorker = std::make_unique<ComWorker>();
            m_filePasteWorker->post([this, target, names] {
                const bool ok = !names.isEmpty() && paste::isForeground(target)
                    && filedialog::setFileName(target, names.toStdWString());
                QMetaObject::invokeMethod(
                    this,
                    [this, ok] {
                        m_pasting = false;
                        if (!ok)
                            flash(tr("已复制文件，但未能填写上传窗口的文件名，请重试"));
                    },
                    Qt::QueuedConnection);
            });
            return;
        }
        m_pasting = false;
        if (front) {
            if (!paste::sendPasteKeys(m_pasteWindow, guard.get()) && m_into == Into::CurrentProgram)
                failPaste(tr("内容已复制，自动粘贴失败，请按 Ctrl+V 粘贴"));
        } else if (m_window && m_window->isVisible())
            flash(tr("已复制。那个窗口没有回到前面，请自己按 Ctrl+V 粘贴"));
    });
    m_returnTimer.setInterval(10ms);
    connect(&m_returnTimer, &QTimer::timeout, this, [this] {
        if (m_returning.item && !m_returning.item->hasActiveFocus() && m_returnClock.elapsed() < kFieldWaitMs)
            return;
        fillField();
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

void Clipboard::setKeysRouted(bool routed)
{
    if (m_keysRouted == routed)
        return;
    m_keysRouted = routed;
    refreshStatus();
    emit keysRoutedChanged();
}

void Clipboard::setPinned(bool pinned)
{
    if (m_pinned == pinned)
        return;
    m_pinned = pinned;
    refreshStatus();
    emit pinnedChanged();
}

void Clipboard::setPreviewOpen(bool open)
{
    if (m_previewOpen == open)
        return;
    m_previewOpen = open;
    m_store->setValue(u"preview"_s, open ? u"open"_s : u"closed"_s);
    emit previewOpenChanged();
}

void Clipboard::setPreviewWidth(int width)
{
    width = std::max(0, width);
    if (m_previewWidth == width)
        return;
    m_previewWidth = width;
    m_store->setValue(u"previewWidth"_s, QString::number(width));
    emit previewWidthChanged();
}

void Clipboard::setListWidth(int width)
{
    width = std::clamp(width, 360, 1600);
    if (m_listWidth == width)
        return;
    m_listWidth = width;
    m_store->setValue(u"listWidth"_s, QString::number(width));
    emit listWidthChanged();
}

void Clipboard::setPinnedFirst(bool on)
{
    if (m_pinnedFirst == on)
        return;
    m_pinnedFirst = on;
    m_store->setValue(u"order"_s, on ? u"pinnedFirst"_s : u"newest"_s);
    emit pinnedFirstChanged();
    refresh();
    emit listChanged();
    flash(on ? tr("固定的内容排在最前") : tr("全部按时间排列，最新的在前"));
}

void Clipboard::setField(QQuickItem* field, const QString& name)
{
    const bool had = m_field.item;
    m_field = {};
    if (field) {
        // Read through the input method queries, as a text field answers
        // them: block-relative positions, which is what textfield::select
        // takes back (the whole text, in a TextInput).
        m_field.item = field;
        m_field.name = name;
        m_field.cursor = field->inputMethodQuery(Qt::ImCursorPosition).toInt();
        m_field.anchor = field->inputMethodQuery(Qt::ImAnchorPosition).toInt();
        m_field.oneLine = textfield::isOneLine(field);
    }
    if (had || field)
        emit fieldChanged();
}

void Clipboard::setNumberKeys(const QString& keys)
{
    const QString held = keys == u"off" ? QString() : keys;
    if (held == m_numberKeys)
        return;
    m_numberKeys = held;
    emit numberKeysChanged();
}

void Clipboard::setRowOptions(bool clickToPaste, bool largePictures)
{
    if (clickToPaste == m_clickToPaste && largePictures == m_largePictures)
        return;
    m_clickToPaste = clickToPaste;
    m_largePictures = largePictures;
    emit rowOptionsChanged();
    refreshStatus(); // pinned, it says how to paste
}

void Clipboard::setLayoutOptions(bool flipAbove, bool searchFar)
{
    if (flipAbove == m_flipAbove && searchFar == m_searchFar)
        return;
    m_flipAbove = flipAbove;
    m_searchFar = searchFar;
    emit layoutOptionsChanged();
}

void Clipboard::setHoldToStep(bool on)
{
    if (m_holdToStep == on)
        return;
    m_holdToStep = on;
    emit holdChanged();
}

void Clipboard::beginSteps(const QString& keys)
{
    m_holding = true;
    m_heldId = 0;
    m_holdHint = false;
    m_holdKeys = keys;
    emit holdChanged();
}

void Clipboard::step()
{
    if (!m_holding)
        return;
    // One older than the entry it is on (the first as it opened, until a
    // step; the first row if the list was empty then).
    const int row = hotkeysteps::stepRow(m_heldId ? m_model.rowOf(m_heldId) : 0, m_model.count());
    if (row >= 0) {
        m_heldId = m_model.idAt(row);
        emit stepped(row);
    }
    if (!m_holdHint) {
        m_holdHint = true;
        emit holdChanged();
    }
}

void Clipboard::holdFirst()
{
    if (!m_holding || m_holdHint)
        return;
    if (!m_heldId && m_model.count() > 0) {
        m_heldId = m_model.idAt(0);
        emit stepped(0);
    }
    m_holdHint = true;
    emit holdChanged();
}

bool Clipboard::finishSteps(std::unique_ptr<paste::Guard> guard)
{
    if (!m_holding)
        return false;
    // On no entry: the list was empty as it opened; the first row, if one
    // came in since.
    const qint64 held = m_heldId;
    const int row = held ? m_model.rowOf(held) : (m_model.count() > 0 ? 0 : -1);
    endSteps();
    if (row < 0) {
        if (held) // nothing to say for an empty list
            flash(tr("那一条已不在列表里，没有粘贴"));
        return false;
    }
    m_nextGuard = std::move(guard);
    paste(row, false);
    m_nextGuard.reset(); // not taken: no paste keys (a field of Win顺's own, one in progress)
    return true;
}

void Clipboard::endSteps()
{
    if (!m_holding)
        return;
    m_holding = false;
    m_heldId = 0;
    m_holdHint = false;
    emit holdChanged();
}

int Clipboard::heldRow() const
{
    return m_holding && m_heldId ? m_model.rowOf(m_heldId) : -1;
}

void Clipboard::setNumberHints(const QString& hints)
{
    if (m_numberHints == hints)
        return;
    m_numberHints = hints;
    emit numberKeysChanged();
}

void Clipboard::setNumberKeysLeft(unsigned digits)
{
    if (digits == m_numberKeysLeft)
        return;
    m_numberKeysLeft = digits;
    emit numberKeysChanged();
}

void Clipboard::setState(bool recording, bool paused, const QString& shortcut)
{
    if (m_recording == recording && m_paused == paused && m_shortcut == shortcut)
        return;
    m_recording = recording;
    m_paused = paused;
    m_shortcut = shortcut;
    if (recording && !paused && m_store->isOpen()) {
        m_backlogTimer.start();
    } else {
        m_backlogTimer.stop();
        m_recognizer.cancel();
        m_rereading.clear();
    }
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
    const QString query = m_query.trimmed();
    // A group's own order is set by moving its rows, but not in a search:
    // where a row would go among the others is not to be seen there.
    const bool group = category.kind == ClipFilter::Category::Group;
    m_model.setOrdering(m_pinnedFirst && !group,
        !query.isEmpty() ? ClipModel::Movable::None
            : group      ? ClipModel::Movable::All
            : m_pinnedFirst ? ClipModel::Movable::Pinned
                            : ClipModel::Movable::None);
    m_model.setRows(m_store->find({category.kind, category.group, query, m_pinnedFirst}), query);
    refreshStatus();
}

void Clipboard::historyChanged()
{
    refresh();
    emit categoriesChanged(); // the sizes of the groups
}

void Clipboard::loadHistoryState()
{
    m_separator = m_store->value(u"separator"_s);
    if (!kSeparators.contains(m_separator))
        m_separator = kSeparators.constFirst();
    m_previewOpen = m_store->value(u"preview"_s) == u"open"_s;
    m_previewWidth = std::max(0, m_store->value(u"previewWidth"_s).toInt());
    const int savedWidth = m_store->value(u"listWidth"_s).toInt();
    m_listWidth = savedWidth >= 360 && savedWidth <= 1600 ? savedWidth : 480;
    m_pinnedFirst = m_store->value(u"order"_s) == u"pinnedFirst"_s;
}

void Clipboard::resumeHistory()
{
    loadHistoryState();
    emit separatorChanged();
    emit previewOpenChanged();
    emit previewWidthChanged();
    emit listWidthChanged();
    emit pinnedFirstChanged();
    historyChanged();
    if (m_recording && !m_paused)
        m_backlogTimer.start();
}

void Clipboard::suspendHistory()
{
    m_backlogTimer.stop();
    m_recognizer.cancel();
    m_rereading.clear();
    dropUndo();
    m_model.setRows({}, {});
    m_model.clearSelection();
    emit listChanged();
}

void Clipboard::refreshStatus()
{
    QString status;
    if (m_paused)
        status = tr("已暂停记录，在托盘菜单里可以继续");
    else if (!m_recording)
        status = tr("没有在记录剪贴板");
    else {
        status = !m_query.trimmed().isEmpty() || m_category != 0 ? tr("%Ln 条", nullptr, m_model.count()) : tr("共 %Ln 条", nullptr, total());
        // Pinned and the keys not coming here: a click is how an entry gets pasted.
        if (m_pinned && !m_keysRouted)
            status = (m_clickToPaste ? tr("%1 · 单击粘贴") : tr("%1 · 双击粘贴")).arg(status);
    }
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

void Clipboard::failPaste(const QString& message)
{
    m_keysTimer.stop();
    m_focusTimer.stop();
    m_pasteGuard.reset();
    m_pasting = false;
    flash(message);
    if (m_into == Into::CurrentProgram)
        emit quickPasteFailed(message);
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
    // Its hotkey held (beginSteps, before this): on the first entry from
    // now on, not the first row, which a copy just coming in would take.
    if (m_holding && !m_heldId && m_model.count() > 0)
        m_heldId = m_model.idAt(0);
    emit categoriesChanged();
    emit listChanged();
    emit shown();
}

void Clipboard::handleHidden()
{
    setPinned(false);
    m_model.clearSelection();
    dropUndo();
    setField(nullptr, {}); // a paste on its way back to it has its own
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

ClipWrite Clipboard::writeFor(
    const std::vector<const Clip*>& clips, bool plainText, QString* problem, HWND target) const
{
    ClipWrite write;
    if (!clips.empty() && !plainText && filemanager::kindOf(target) == filemanager::Kind::Explorer
        && std::ranges::all_of(clips, [](const Clip* clip) { return clip->kind == ClipKind::Image; })) {
        const auto exported = clipfiles::images(*m_store, clips, m_dragFolder, tr("剪贴板图片"));
        write.files = exported.paths;
        if (clips.size() == 1)
            write.id = clips.front()->id;
        if (write.empty())
            *problem = tr("无法准备要粘贴的图片文件");
        return write;
    }
    if (clips.size() == 1) {
        const Clip& c = *clips.front();
        write.id = c.id;
        switch (c.kind) {
        case ClipKind::Image: {
            ClipPayload payload = m_store->payload(c.id); // its file written, if it was just copied
            if (!plainText) {
                write.imagePath = std::move(payload.imagePath);
                write.formats = std::move(payload.formats); // an equation: pasted from these
                // Chat and upload targets commonly accept animations as files.
                if (c.animated) {
                    const auto exported = clipfiles::images(*m_store, clips, m_dragFolder, tr("剪贴板图片"));
                    if (exported.error != clipfiles::Error::None) {
                        *problem = tr("无法准备要粘贴的图片文件");
                        return {};
                    }
                    write.files = exported.paths;
                }
                break;
            }
            // An equation has its text (Word's linear form); a picture none.
            for (const ClipFormat& format : unpackClipFormats(payload.formats)) {
                if (format.name != u"CF_UNICODETEXT"_s)
                    continue;
                write.text = QString::fromUtf16(reinterpret_cast<const char16_t*>(format.data.constData()),
                    format.data.size() / 2);
                if (const qsizetype end = write.text.indexOf(QChar()); end >= 0)
                    write.text.truncate(end);
            }
            if (write.text.trimmed().isEmpty()) {
                write.text.clear();
                *problem = tr("图片没有可以粘贴的文字");
            }
            break;
        }
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

void Clipboard::startPaste(ClipWrite write, Into into, std::vector<qint64> touched, QString done)
{
    m_pasting = true;
    m_write = std::move(write);
    m_into = into;
    m_pasteGuard = into == Into::Program ? std::move(m_nextGuard) : nullptr;
    if (into == Into::CurrentProgram)
        m_pasteWindow = m_target;
    m_touched = std::move(touched);
    m_doneMessage = std::move(done);
    m_pasteClock.start();
    m_keysTimer.start();
}

void Clipboard::onWritten(bool ok)
{
    // A field of Win顺's own gets its text from here, not through the
    // clipboard (which is written only to hold it after, as any paste
    // leaves it): no reason to stop.
    if (!ok && m_into != Into::Field) {
        failPaste(tr("剪贴板正被别的程序占用，请再试一次"));
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
    if (m_into == Into::Clipboard) {
        m_pasting = false;
        flash(m_doneMessage);
        return;
    }
    if (m_into == Into::Field) {
        returnToField(std::exchange(m_fieldText, {})); // m_pasting until it is in
        return;
    }
    if (m_into != Into::CurrentProgram)
        m_pasteWindow = paste::usableTarget(m_target);
    if (!m_pasteWindow) {
        m_pasting = false;
        m_pasteGuard.reset();
        flash(tr("已复制。没有找到要粘贴进去的窗口，可以自己按 Ctrl+V"));
        return;
    }
    // An elevated program while Win顺 is not: Windows drops the keys Win顺
    // would press there (UIPI), SendInput says nothing. The user presses them.
    if (integrity::higherThanOurs(m_pasteWindow)) {
        failPaste(tr("已复制。那个程序以管理员身份运行，Win顺 没法替它粘贴：回到它那里按 Ctrl+V"));
        return;
    }
    if (m_into == Into::CurrentProgram) {
        if (!paste::isForeground(m_pasteWindow)) {
            failPaste(tr("内容已复制；窗口已切换，未自动粘贴"));
            return;
        }
        m_pasteClock.restart();
        m_frontClock.start();
        m_focusTimer.start();
        return;
    }
    // Pinned, it stays up: a joined paste's picks go, the next click is one
    // entry again.
    if (m_pinned)
        m_model.clearSelection();
    m_pasteClock.restart();
    // Never left the front, its text field never lost the keyboard: the
    // window goes (and with it KeyRouter, which would take the keys), then
    // the paste keys once the clipboard settles (m_focusTimer). Pinned, it
    // stays and only gives the keys back (App).
    if ((m_keysRouted || m_pinned) && paste::isForeground(m_pasteWindow)) {
        emit dismissRequested();
        m_frontClock.start();
        m_focusTimer.start();
        return;
    }
    paste::activate(m_pasteWindow); // the clipboard window loses the focus and goes (App)
    m_frontClock.invalidate();
    m_focusTimer.start();
}

void Clipboard::returnToField(QString text)
{
    // m_field stays as it is until the window goes or opens again: the
    // footer goes on saying where Enter pastes until it is gone.
    m_returning = m_field;
    m_returnText = std::move(text);
    if (!m_returning.item) { // gone with its window (the settings, closed)
        m_pasting = false;
        emit dismissRequested();
        return;
    }
    m_returnClock.start();
    emit fieldRequested(m_returning.item);
    // Its window takes the keyboard back (App): the text goes in once the
    // field has it.
    if (m_returning.item && m_returning.item->hasActiveFocus())
        fillField();
    else
        m_returnTimer.start();
}

void Clipboard::fillField()
{
    m_returnTimer.stop();
    m_pasting = false;
    const Field field = std::exchange(m_returning, {});
    const QString text = std::exchange(m_returnText, {});
    QQuickItem* item = field.item;
    // Not back in front (its file dialog is gone): nothing goes into a field
    // nobody sees. The clipboard has it.
    const bool shown = item && item->isVisible() && item->window() && item->window()->isVisible();
    if (!item || (!item->hasActiveFocus() && !shown)) {
        if (!m_window || !m_window->isVisible())
            return;
        if (text.isEmpty())
            emit dismissRequested();
        else
            flash(tr("已复制。没有找到要粘贴进去的窗口，可以自己按 Ctrl+V"));
        return;
    }
    // Its caret and selection back first (the window took the focus, and with
    // it the selection), then the text over the selection.
    textfield::select(item, field.anchor, field.cursor);
    textfield::type(item, text);
}

void Clipboard::paste(int row, bool plainText)
{
    if (m_pasting)
        return;
    // Pinned, clicked without the keys: into the program in front now, which
    // kept the focus. (Its own window in front: where it was before.)
    if (m_pinned && !m_keysRouted) {
        DWORD process = 0;
        const HWND foreground = ::GetForegroundWindow();
        ::GetWindowThreadProcessId(foreground, &process);
        if (foreground && process != ::GetCurrentProcessId())
            m_target = foreground;
    }
    const std::vector<const Clip*> clips = targets(row);
    if (clips.empty())
        return;
    std::vector<qint64> touched;
    if (clips.size() > 1) {
        for (const Clip* c : clips)
            touched.push_back(c->id);
    }
    QString problem;
    if (m_field.item) {
        // A field of Win顺's own takes text, a file list as its paths (gone
        // or not); the clipboard holds the entry after, as any paste leaves it.
        ClipWrite write = writeFor(clips, true, &problem);
        m_fieldText = m_field.oneLine ? textfield::oneLine(write.text) : write.text;
        if (m_fieldText.isEmpty()) {
            flash(tr("%1里只能粘贴文字").arg(m_field.name));
            return;
        }
        if (!plainText) {
            if (ClipWrite formatted = writeFor(clips, false, &problem); !formatted.empty())
                write = std::move(formatted);
        }
        startPaste(std::move(write), Into::Field, std::move(touched), {});
        return;
    }
    ClipWrite write = writeFor(clips, plainText, &problem, m_target);
    if (write.empty()) {
        flash(problem);
        return;
    }
    startPaste(std::move(write), Into::Program, std::move(touched), {});
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
    startPaste(std::move(write), Into::Clipboard, std::move(touched), done);
}

Clipboard::~Clipboard()
{
    m_filePasteWorker.reset();
    QDir(m_dragFolder).removeRecursively();
}

void Clipboard::dragOut(int row)
{
    if (m_pasting || m_dragging || !m_window)
        return;
    const Clip* clip = m_model.at(row);
    if (!clip || clip->kind != ClipKind::Image)
        return;
    const auto exported = clipfiles::images(*m_store, targets(row), m_dragFolder, tr("剪贴板图片"));
    if (exported.error != clipfiles::Error::None) {
        flash(exported.error == clipfiles::Error::NotImages ? tr("请只选择图片后再拖出") : tr("无法准备要拖出的图片"));
        return;
    }
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    m_dragging = true;
    emit focusNeeded(); // stop key routing so OLE receives Escape and Ctrl
    const auto outcome = dragout::drag(exported.paths, hwnd, false);
    // OLE consumes the release; clear Qt's pressed state without pasting.
    POINT p {};
    ::GetCursorPos(&p);
    ::ScreenToClient(hwnd, &p);
    ::PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<short>(p.x), static_cast<short>(p.y)));
    m_dragging = false;
    if (outcome == dragout::Outcome::Failed)
        flash(tr("无法拖出图片"));
    else if (outcome == dragout::Outcome::Dropped)
        emit dismissRequested();
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
    // Named in the status line: its first line, cut short (a picture's words run to many).
    QString shown = text.trimmed().section(u'\n', 0, 0).trimmed();
    if (shown.size() > 40 || shown.size() < text.trimmed().size())
        shown = shown.left(40) + u'…';
    startPaste(std::move(write), Into::Clipboard, {}, tr("已复制 %1").arg(shown));
}

void Clipboard::copyImage(const QImage& image, bool remember, bool screenshot, QByteArray project)
{
    encodeImage(image, remember, screenshot, std::move(project), true);
}

void Clipboard::recordImage(const QImage& image, QByteArray project)
{
    if (!m_recording || m_paused)
        return; // nothing to keep: no need to encode it either
    encodeImage(image, true, true, std::move(project), false);
}

void Clipboard::encodeImage(const QImage& image, bool remember, bool screenshot, QByteArray project, bool toClipboard)
{
    if (image.isNull())
        return;
    if (m_imageEncoder.joinable())
        m_imageEncoder.join(); // the one before, done by now
    // A whole screen takes a moment to encode: not on the GUI thread.
    m_imageEncoder = std::jthread([this, image, remember, screenshot, project = std::move(project), toClipboard] {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        const QByteArray hash = imagePixelHash(image);
        QMetaObject::invokeMethod(this,
            [this, png, hash, size = image.size(), remember, screenshot, project, toClipboard] {
                storeImage(png, hash, size, remember, screenshot, project, toClipboard);
            },
            Qt::QueuedConnection);
    });
}

void Clipboard::storeImage(const QByteArray& png, const QByteArray& hash, QSize size, bool remember, bool screenshot,
    const QByteArray& project, bool toClipboard)
{
    if (png.isEmpty())
        return;
    if (toClipboard && m_pasting) { // a paste from the window still under way: right after it
        QTimer::singleShot(50ms, this, [this, png, hash, size, remember, screenshot, project] {
            storeImage(png, hash, size, remember, screenshot, project, true);
        });
        return;
    }
    ClipWrite write;
    write.imageData = png;
    if (remember && m_recording && !m_paused) { // a copy like any other: in the history, on top
        ClipCapture capture;
        capture.kind = ClipKind::Image;
        capture.imageData = png;
        capture.hash = hash;
        capture.width = size.width();
        capture.height = size.height();
        capture.source = screenshot ? tr("Win顺 截图") : tr("Win顺");
        capture.sourcePath = QCoreApplication::applicationFilePath();
        capture.screenshot = screenshot;
        capture.project = project;
        write.id = m_store->add(capture, now());
        historyChanged();
    }
    if (!toClipboard)
        return; // history only: the clipboard and the status line stay as they are
    startPaste(std::move(write), Into::Clipboard, {}, screenshot ? tr("已复制截图") : tr("已复制图片"));
}

void Clipboard::pasteDirect(qint64 id, HWND target)
{
    if (m_pasting)
        return;
    const Clip* clip = m_store->clip(id);
    if (!clip) {
        emit quickPasteFailed(tr("这一条还没有内容，可以先复制内容，或打开剪贴板面板查看"));
        return;
    }
    const HWND window = paste::usableTarget(target);
    if (!window || !paste::isForeground(window))
        return;
    QString problem;
    ClipWrite write = writeFor({clip}, false, &problem, window);
    if (write.empty()) {
        emit quickPasteFailed(problem);
        return;
    }
    m_target = window;
    startPaste(std::move(write), Into::CurrentProgram, {}, {});
}

bool Clipboard::pasteEntry(qint64 id, HWND target)
{
    const Clip* clip = m_store->clip(id);
    if (!clip)
        return false;
    if (m_pasting)
        return true; // one under way: this key is late, not wrong
    QString problem;
    ClipWrite write = writeFor({clip}, false, &problem, paste::usableTarget(target));
    if (write.empty()) {
        flash(problem);
        return true;
    }
    if (!paste::usableTarget(target)) { // nothing to paste into: on the clipboard, as 复制
        startPaste(std::move(write), Into::Clipboard, {}, tr("已复制"));
        return true;
    }
    m_target = target;
    startPaste(std::move(write), Into::Program, {}, {});
    return true;
}

void Clipboard::removeEntries(const std::vector<qint64>& ids)
{
    if (ids.empty())
        return;
    dropUndo();
    m_removed = m_store->remove(ids);
    historyChanged();
    emit undoChanged();
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

bool Clipboard::canMove(int from, int to) const
{
    const Clip* a = m_model.at(from);
    const Clip* b = m_model.at(to);
    return a && b && from != to && m_model.movable(from) && m_model.movable(to) && a->group == b->group;
}

void Clipboard::moveRow(int from, int to)
{
    if (!canMove(from, to))
        return;
    m_store->moveInGroup(m_model.idAt(from), m_model.idAt(to));
    refresh(); // the row slides there (ClipModel), still the current one
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
    removeEntries(ids);
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

int Clipboard::selectAll()
{
    return m_model.selectListed(m_categoryGroup != 0);
}

void Clipboard::removeListed()
{
    const std::vector<qint64> ids = m_model.listedIds(m_categoryGroup != 0);
    if (ids.empty())
        return;
    removeEntries(ids);
    flash(tr("已删除 %Ln 条，按 Ctrl+Z 撤销", nullptr, static_cast<int>(ids.size())));
}

void Clipboard::dismiss()
{
    if (m_pinned) { // it stays up
        emit keysReleased();
        return;
    }
    if (m_field.item && !m_pasting && !m_returnTimer.isActive())
        returnToField({});
    else
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
        {u"id"_s, QString::number(c->id)},
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
        // None while its file is being written: asked again once it is there (ClipModel::pictureSaved).
        map.insert(u"image"_s, m_store->imagePending(c->id) ? QString()
                                                            : QUrl::fromLocalFile(m_store->imagePath(c->id)).toString());
        map.insert(u"width"_s, c->width);
        map.insert(u"height"_s, c->height);
        map.insert(u"animated"_s, c->animated);
        map.insert(u"words"_s, c->searchText()); // what the search finds it by (an equation: its text)
        size = u"%1 · %2 × %3 · %4"_s.arg(clipImageExtension(c->imageFormat).toUpper()).arg(c->width).arg(c->height).arg(
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
    // Into a field of Win顺's own it is text either way: no plain-text entry.
    const QString field = fieldName();
    items.append(entry(Paste, many ? tr("粘贴 %Ln 条", nullptr, n) : field.isEmpty() ? tr("粘贴") : tr("粘贴到%1").arg(field),
        u"Enter"_s, u""_s));
    if (!onlyImages && field.isEmpty())
        items.append(entry(PastePlain, tr("粘贴为纯文本"), u"Shift+Enter"_s, u""_s));
    items.append(entry(Copy, many ? tr("复制 %Ln 条", nullptr, n) : tr("复制"), u"Ctrl+C"_s, u""_s));
    if (!many) {
        const Clip& c = *clips.front();
        if (c.kind == ClipKind::Link)
            items.append(entry(OpenLink, tr("在浏览器中打开"), QString(), u""_s));
        else if (c.kind == ClipKind::Files || c.kind == ClipKind::Path)
            items.append(entry(Reveal, tr("打开所在位置"), QString(), u""_s));
        else if (c.kind == ClipKind::Image)
            items.append(entry(PinImage, c.animated ? tr("贴首帧到屏幕") : tr("贴到屏幕"), QString(), u"\uE718"_s));
        if (c.kind == ClipKind::Image)
            items.append(entry(EditImage, c.animated ? tr("编辑首帧") : tr("编辑"), QString(), u"\uE70F"_s));
        if (c.kind == ClipKind::Image)
            items.append(entry(OpenImage, tr("打开图片"), QString(), u""_s));
    }
    items.append(separator);
    const bool pinned = std::all_of(clips.begin(), clips.end(), [](const Clip* c) { return c->group == ClipStore::kPinned; });
    items.append(entry(Pin, pinned ? tr("取消固定") : tr("固定"), u"Ctrl+P"_s, pinned ? u""_s : u""_s));
    if (!many && canMove(row, row - 1))
        items.append(entry(MoveUp, tr("上移"), u"Ctrl+↑"_s, u""_s));
    if (!many && canMove(row, row + 1))
        items.append(entry(MoveDown, tr("下移"), u"Ctrl+↓"_s, u""_s));
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
    // The whole list: in a group's tab that group, else what is in no group.
    const bool inGroup = m_categoryGroup != 0;
    const auto all = static_cast<int>(m_model.listedIds(inGroup).size());
    if (all > 1) {
        QString text = inGroup ? tr("删除本组全部 %Ln 条", nullptr, all)
            : !m_query.trimmed().isEmpty() ? tr("删除搜到的 %Ln 条", nullptr, all)
                                           : tr("删除全部 %Ln 条", nullptr, all);
        if (all < m_model.count())
            text += tr("（固定和分组的保留）");
        items.append(entry(RemoveAll, text, QString(), u""_s));
    }
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
    case MoveUp:
        moveRow(row, row - 1);
        break;
    case MoveDown:
        moveRow(row, row + 1);
        break;
    case Remove:
        remove(row);
        break;
    case RemoveAll:
        removeListed();
        break;
    case PinImage: {
        const Clip* c = m_model.at(row);
        if (!c || c->kind != ClipKind::Image)
            break;
        if (m_store->imagePending(c->id)) { // just copied, its file not written yet
            flash(tr("图片还在保存，请稍后再试"));
            break;
        }
        emit pinRequested(m_store->imagePath(c->id));
        break;
    }
    case EditImage: {
        const Clip* c = m_model.at(row);
        if (!c || c->kind != ClipKind::Image)
            break;
        if (m_store->imagePending(c->id)) { // just copied, its file not written yet
            flash(tr("图片还在保存，请稍后再试"));
            break;
        }
        // The window goes first (its keys are given back), then the editing window takes the front.
        const qint64 id = c->id;
        emit dismissRequested();
        emit editRequested(id);
        break;
    }
    case OpenLink:
    case Reveal:
    case OpenImage: {
        const Clip* c = m_model.at(row);
        if (!c)
            break;
        // What opens may come to the front only from the program in front.
        if (action == OpenLink) {
            const QString url = c->text.trimmed();
            emit focusNeeded();
            shell::openUrl(url.startsWith(u"www.", Qt::CaseInsensitive) ? u"https://"_s + url : url);
        } else if (action == Reveal) {
            QStringList files = c->files();
            files.removeIf([](const QString& f) { return isLocal(f) && !QFileInfo::exists(f); });
            if (files.isEmpty()) {
                flash(tr("这些文件已经不存在了"));
                break;
            }
            emit focusNeeded();
            shell::reveal(files);
        } else {
            emit focusNeeded();
            shell::open(m_store->payload(c->id).imagePath);
        }
        emit dismissRequested(); // what opened it is in front now, not the field the window came from
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
    // Showing it must not take the focus from the clipboard window (see Launcher::prepareMenuWindow).
    menu->setProperty("_q_showWithoutActivating", true);
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    win::styleFramelessWindow(menu);
    win::setDarkFrame(menu, dark);
}

} // namespace ws
