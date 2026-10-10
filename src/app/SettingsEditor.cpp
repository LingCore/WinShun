#include "SettingsEditor.h"

#include "FileIconProvider.h"
#include "IndexFolder.h"
#include "Query.h"
#include "SystemCatalog.h"
#include "WebShortcut.h"
#include "Win32Util.h"
#include "platform/FileManagers.h"
#include "platform/NumberKeys.h"
#include "platform/Shell.h"
#include "platform/ShortcutCapture.h"
#include "platform/TaskbarSearch.h"
#include "platform/TaskbarSearchBox.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QStyleHints>
#include <QVariantMap>
#include <QWinEventNotifier>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

QString displayPath(const QString& path)
{
    return QString::fromStdWString(win32::expandEnvironment(QDir::toNativeSeparators(path.trimmed()).toStdWString()));
}

bool containsIgnoreCase(const QStringList& list, const QString& value)
{
    return list.contains(value, Qt::CaseInsensitive);
}

// Adds a program file to `list` ("KeePass", "keepass.exe", or a whole path
// pasted in); false when there is none or it is there already.
bool addProgram(QStringList& list, const QString& name)
{
    QString app = QFileInfo(name.trimmed()).fileName();
    if (app.isEmpty())
        return false;
    if (!app.endsWith(u".exe", Qt::CaseInsensitive))
        app += u".exe"_s;
    if (containsIgnoreCase(list, app))
        return false;
    list.append(app);
    return true;
}

} // namespace

// Changed outside Win顺 (the user pins the button, hides Windows' search in
// the Settings app): read again at once, so that the steps of 代替任务栏上的
// Windows 搜索 tick themselves off.
struct SettingsEditor::TaskbarWatch {
    QFileSystemWatcher pinned; // a shortcut there per program pinned to the taskbar
    win32::UniqueKey searchKey; // SearchboxTaskbarMode's
    win32::UniqueHandle searchChanged;
    std::unique_ptr<QWinEventNotifier> notifier;

    bool arm() // once per change
    {
        return searchKey.get() && searchChanged.valid()
            && ::RegNotifyChangeKeyValue(searchKey.get(), FALSE, REG_NOTIFY_CHANGE_LAST_SET, searchChanged.get(), TRUE)
            == ERROR_SUCCESS;
    }
};

SettingsEditor::SettingsEditor(const Settings& settings, const QString& runningRenderer, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_runningRenderer(runningRenderer)
    , m_taskbarWatch(std::make_unique<TaskbarWatch>())
{
    TaskbarWatch& watch = *m_taskbarWatch;
    const QString pinned = qEnvironmentVariable("APPDATA") + u"/Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar"_s;
    if (QFileInfo(pinned).isDir())
        watch.pinned.addPath(pinned);
    connect(&watch.pinned, &QFileSystemWatcher::directoryChanged, this, &SettingsEditor::refreshTaskbarState);
    ::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", 0, KEY_NOTIFY,
        watch.searchKey.out());
    watch.searchChanged = win32::UniqueHandle(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (watch.arm()) {
        watch.notifier = std::make_unique<QWinEventNotifier>(watch.searchChanged.get());
        connect(watch.notifier.get(), &QWinEventNotifier::activated, this, [this] {
            m_taskbarWatch->arm();
            refreshTaskbarState();
        });
    }
}

SettingsEditor::~SettingsEditor() = default; // here, where ShortcutCapture and TaskbarWatch are complete

void SettingsEditor::commit()
{
    m_settings.save();
    emit changed();
    emit edited(m_settings);
}

void SettingsEditor::setSettings(const Settings& settings)
{
    if (settings == m_settings)
        return;
    const bool keys = settings.numberKeys != m_settings.numberKeys || settings.hotkey != m_settings.hotkey
        || settings.clipboardNumberKeys != m_settings.clipboardNumberKeys
        || settings.clipboardHotkey != m_settings.clipboardHotkey;
    m_settings = settings;
    emit changed();
    if (keys)
        refreshNumberKeys();
}

void SettingsEditor::setFoldersFirst(bool on)
{
    if (m_settings.foldersFirst == on)
        return;
    m_settings.foldersFirst = on;
    commit();
}

void SettingsEditor::setShowModified(bool on)
{
    if (m_settings.showModified == on)
        return;
    m_settings.showModified = on;
    commit();
}

void SettingsEditor::setSortByModified(bool on)
{
    if (m_settings.sortByModified == on)
        return;
    m_settings.sortByModified = on;
    commit();
}

void SettingsEditor::setNumberKeys(const QString& keys)
{
    if (m_settings.numberKeys == keys || (keys != u"ctrl" && keys != u"alt" && keys != u"off"))
        return;
    m_settings.numberKeys = keys;
    commit();
    refreshNumberKeys();
}

void SettingsEditor::setClipboardNumberKeys(const QString& keys)
{
    if (m_settings.clipboardNumberKeys == keys || (keys != u"ctrl" && keys != u"alt" && keys != u"off"))
        return;
    m_settings.clipboardNumberKeys = keys;
    commit();
    refreshNumberKeys();
}

namespace {

// What the user should know of the number keys `keys` ("ctrl", "alt"):
// those another program holds as global hotkeys, and those that are one of
// Win顺's own shortcuts. `whose`: whose they are when.
QString describeNumberKeys(const QString& keys, const Settings& settings, const QString& whose)
{
    if (keys != u"ctrl" && keys != u"alt")
        return {};
    const QString modifier = keys == u"alt" ? u"Alt"_s : u"Ctrl"_s;
    const auto chord = [&](int n) { return modifier + u'+' + QString::number(n); };
    QStringList own;
    QStringList others;
    const std::vector<int> held
        = NumberKeys::heldAsHotkeys(keys == u"alt" ? NumberKeys::Modifier::Alt : NumberKeys::Modifier::Ctrl);
    for (int n = 1; n <= 9; ++n) {
        const QString c = chord(n);
        const bool mine = settings.hotkey.compare(c, Qt::CaseInsensitive) == 0
            || settings.clipboardHotkey.compare(c, Qt::CaseInsensitive) == 0;
        if (mine)
            own.append(c);
        else if (std::find(held.begin(), held.end(), n) != held.end())
            others.append(c);
    }
    QStringList parts;
    if (!others.isEmpty())
        parts.append(SettingsEditor::tr("%1 也是别的程序的快捷键：%2。").arg(others.join(SettingsEditor::tr("、")), whose));
    if (!own.isEmpty())
        parts.append(SettingsEditor::tr("%1 是你给 Win顺 设的快捷键，在这里按它不再打开或关闭窗口，建议换一个。")
                         .arg(own.join(SettingsEditor::tr("、"))));
    return parts.join(u' ');
}

} // namespace

void SettingsEditor::refreshNumberKeys()
{
    const QString launcher = describeNumberKeys(
        m_settings.numberKeys, m_settings, tr("搜索框在前面时归 Win顺"));
    const QString clipboard = describeNumberKeys(m_settings.clipboardNumberKeys, m_settings,
        tr("剪贴板在别的程序上打开时归 Win顺，从 Win顺 自己的搜索框打开时归那个程序"));
    if (launcher == m_numberKeysNote && clipboard == m_clipboardNumberKeysNote)
        return;
    m_numberKeysNote = launcher;
    m_clipboardNumberKeysNote = clipboard;
    emit numberKeysNoteChanged();
}

void SettingsEditor::setDoubleCtrl(bool on)
{
    if (m_settings.doubleCtrl == on)
        return;
    m_settings.doubleCtrl = on;
    commit();
}

void SettingsEditor::setDoubleCtrlPauseInGames(bool on)
{
    if (m_settings.doubleCtrlPauseInGames == on)
        return;
    m_settings.doubleCtrlPauseInGames = on;
    commit();
}

void SettingsEditor::setDoubleCtrlPauseInFullScreen(bool on)
{
    if (m_settings.doubleCtrlPauseInFullScreen == on)
        return;
    m_settings.doubleCtrlPauseInFullScreen = on;
    commit();
}

bool SettingsEditor::addDoubleCtrlExcludedApp(const QString& name)
{
    if (!addProgram(m_settings.doubleCtrlExcludedApps, name))
        return false;
    commit();
    return true;
}

void SettingsEditor::removeDoubleCtrlExcludedApp(int index)
{
    if (index < 0 || index >= m_settings.doubleCtrlExcludedApps.size())
        return;
    m_settings.doubleCtrlExcludedApps.removeAt(index);
    commit();
}

void SettingsEditor::setHotkey(const QString& hotkey)
{
    const QString value = hotkey.trimmed();
    if (m_settings.hotkey == value)
        return;
    m_settings.hotkey = value;
    commit();
}

void SettingsEditor::setHotkeyError(const QString& error)
{
    if (m_hotkeyError == error)
        return;
    m_hotkeyError = error;
    emit hotkeyErrorChanged();
}

void SettingsEditor::setRecordingHotkey(bool recording)
{
    if (m_recordingHotkey == recording)
        return;
    m_recordingHotkey = recording;
    m_capture.reset();
    if (recording && m_window) {
        // The recorder gets its keys from the keyboard itself, ahead of Qt,
        // Windows and other programs (see ShortcutCapture), as ordinary key events.
        m_capture = std::make_unique<ShortcutCapture>(reinterpret_cast<HWND>(m_window->winId()),
            [this](const ShortcutCapture::Key& key) {
                QMetaObject::invokeMethod(this, [this, key] {
                    if (!m_recordingHotkey || !m_window)
                        return; // the recording ended meanwhile
                    QKeyEvent event(key.press ? QEvent::KeyPress : QEvent::KeyRelease, key.key, key.modifiers,
                        QString(), key.autoRepeat);
                    QCoreApplication::sendEvent(m_window, &event);
                }, Qt::QueuedConnection);
            });
        if (!m_capture->isActive())
            m_capture.reset(); // Qt's own key events then, which lack Alt+Space
    }
    emit recordingHotkeyChanged();
}

bool SettingsEditor::autostart() const
{
    return autostart::isEnabled();
}

void SettingsEditor::setAutostart(bool on)
{
    if (autostart::isEnabled() == on)
        return;
    autostart::setEnabled(on);
    emit autostartChanged();
}

void SettingsEditor::setIncludeRemovableDrives(bool on)
{
    if (m_settings.includeRemovableDrives == on)
        return;
    m_settings.includeRemovableDrives = on;
    commit();
}

void SettingsEditor::setRescanOnStartup(bool on)
{
    if (m_settings.rescanOnStartup == on)
        return;
    m_settings.rescanOnStartup = on;
    commit();
}

QStringList SettingsEditor::excludedPaths() const
{
    QStringList paths;
    for (const QString& p : m_settings.excludedPaths)
        paths.append(displayPath(p));
    return paths;
}

void SettingsEditor::setContentMaxSizeMB(int kind, int mb)
{
    if (kind < 0 || kind >= static_cast<int>(ContentSizeLimits::kKinds))
        return;
    mb = std::clamp(mb, 1, 4096);
    int& limit = m_settings.contentMaxSizeMB[static_cast<std::size_t>(kind)];
    if (limit == mb)
        return;
    limit = mb;
    commit();
}

QStringList SettingsEditor::contentKindExtensions() const
{
    constexpr qsizetype kShown = 10;
    std::array<QStringList, ContentSizeLimits::kKinds> kinds;
    for (const QString& ext : m_settings.contentExtensions)
        kinds[ContentSizeLimits::kindOf(ext.toStdString())].append(u'.' + ext);
    QStringList out;
    for (const QStringList& list : kinds)
        out.append(list.size() > kShown ? tr("%1 等").arg(list.first(kShown).join(u' ')) : list.join(u' '));
    return out;
}

void SettingsEditor::setContentIndex(bool on)
{
    if (m_settings.contentIndex == on)
        return;
    m_settings.contentIndex = on;
    commit();
}

void SettingsEditor::setContentInLowPriority(bool on)
{
    if (m_settings.contentInLowPriority == on)
        return;
    m_settings.contentInLowPriority = on;
    commit();
}

void SettingsEditor::setContentDocuments(bool on)
{
    if (m_settings.contentDocuments == on)
        return;
    m_settings.contentDocuments = on;
    commit();
}

void SettingsEditor::setContentIndexStatus(const QString& status)
{
    if (m_contentIndexStatus == status)
        return;
    m_contentIndexStatus = status;
    emit contentIndexStatusChanged();
}

void SettingsEditor::setRenderer(const QString& renderer)
{
    if (m_settings.renderer == renderer)
        return;
    m_settings.renderer = renderer;
    commit();
}

void SettingsEditor::setTheme(const QString& theme)
{
    if (m_settings.theme == theme)
        return;
    m_settings.theme = theme;
    commit();
}

void SettingsEditor::setLanguage(const QString& language)
{
    if (m_settings.language == language)
        return;
    m_settings.language = language;
    commit();
}

void SettingsEditor::setTransparency(const QString& transparency)
{
    if (m_settings.transparency == transparency)
        return;
    m_settings.transparency = transparency;
    commit();
}

void SettingsEditor::setRecordHistory(bool on)
{
    if (m_settings.recordHistory == on)
        return;
    m_settings.recordHistory = on;
    commit();
}

void SettingsEditor::setDialogBar(bool on)
{
    if (m_settings.dialogBar == on)
        return;
    m_settings.dialogBar = on;
    commit();
}

void SettingsEditor::setDialogBarPlace(const QString& place)
{
    if (m_settings.dialogBarPlace == place)
        return;
    m_settings.dialogBarPlace = place;
    commit();
}

void SettingsEditor::setDialogJump(bool on)
{
    if (m_settings.dialogJump == on)
        return;
    m_settings.dialogJump = on;
    commit();
}

void SettingsEditor::setFileManager(const QString& manager)
{
    if (m_settings.fileManager == manager)
        return;
    m_settings.fileManager = manager;
    commit();
}

void SettingsEditor::refreshFileManagers()
{
    QStringList managers;
    for (const auto& [kind, name] : filemanager::kSettingNames) {
        if (filemanager::installed(kind))
            managers.append(QString::fromLatin1(name));
    }
    if (managers == m_fileManagers)
        return;
    m_fileManagers = managers;
    emit fileManagersChanged();
}

void SettingsEditor::setDialogAutoJump(bool on)
{
    if (m_settings.dialogAutoJump == on)
        return;
    m_settings.dialogAutoJump = on;
    commit();
}

void SettingsEditor::removeDialogBarExcludedApp(int index)
{
    if (index < 0 || index >= m_settings.dialogBarExcludedApps.size())
        return;
    m_settings.dialogBarExcludedApps.removeAt(index);
    commit();
}

void SettingsEditor::setHistoryCount(int count)
{
    if (m_historyCount == count)
        return;
    m_historyCount = count;
    emit historyCountChanged();
}

void SettingsEditor::setAutoUpdate(bool on)
{
    if (m_settings.autoUpdate == on)
        return;
    m_settings.autoUpdate = on;
    commit();
}

void SettingsEditor::setClipboard(bool on)
{
    if (m_settings.clipboard == on)
        return;
    m_settings.clipboard = on;
    commit();
}

void SettingsEditor::setClipboardWinV(bool on)
{
    if (m_settings.clipboardWinV == on)
        return;
    m_settings.clipboardWinV = on;
    commit();
}

void SettingsEditor::setWinVState(const QString& state, bool canRestartExplorer)
{
    if (m_winVState == state && m_canRestartExplorer == canRestartExplorer)
        return;
    m_winVState = state;
    m_canRestartExplorer = canRestartExplorer;
    emit winVStateChanged();
}

void SettingsEditor::setTaskbarWinS(bool on)
{
    if (m_settings.taskbarWinS == on)
        return;
    m_settings.taskbarWinS = on;
    commit();
}

void SettingsEditor::setTaskbarStartTyping(bool on)
{
    if (m_settings.taskbarStartTyping == on)
        return;
    m_settings.taskbarStartTyping = on;
    commit();
}

void SettingsEditor::setTaskbarSearchBox(bool on)
{
    if (m_settings.taskbarSearchBox == on)
        return;
    m_settings.taskbarSearchBox = on;
    commit();
}

bool SettingsEditor::searchBoxSupported() const
{
    return taskbar::SearchBox::supported();
}

void SettingsEditor::setSearchBoxShown(bool shown)
{
    if (m_searchBoxShown == shown)
        return;
    m_searchBoxShown = shown;
    emit searchBoxShownChanged();
}

void SettingsEditor::setWinSState(const QString& state, bool canRestartExplorer)
{
    if (m_canRestartExplorer != canRestartExplorer) {
        m_canRestartExplorer = canRestartExplorer;
        emit winVStateChanged(); // canRestartExplorer's
    }
    if (m_winSState == state)
        return;
    m_winSState = state;
    emit winSStateChanged();
}

namespace {

std::wstring taskbarButton()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + u"/WinShunSearch.exe").toStdWString();
}

} // namespace

void SettingsEditor::refreshTaskbarState()
{
    const bool pinned = taskbar::buttonPinned(taskbarButton());
    const bool shown = taskbar::windowsSearchShown();
    if (pinned == m_taskbarButtonPinned && shown == m_windowsSearchShown)
        return;
    m_taskbarButtonPinned = pinned;
    m_windowsSearchShown = shown;
    emit taskbarStateChanged();
}

bool SettingsEditor::pinTaskbarButton()
{
    const std::wstring button = taskbarButton();
    if (!QFileInfo::exists(QString::fromStdWString(button)))
        return false;
    // Windows asks only for a program in the Start menu: the installer's
    // shortcut, else one made now.
    const std::wstring shortcut = taskbar::buttonShortcut(button, tr("Win顺 搜索").toStdWString());
    if (shortcut.empty())
        return false;
    // Through Explorer, with the user's rights: that program goes on the
    // taskbar, so it asks for itself (and Win顺, elevated, could not).
    shell::run(u'"' + QString::fromStdWString(button) + u"\" --pin \""_s + QString::fromStdWString(shortcut) + u'"');
    return true;
}

bool SettingsEditor::showTaskbarButton()
{
    const std::wstring button = taskbarButton();
    if (!QFileInfo::exists(QString::fromStdWString(button)))
        return false;
    const std::wstring shortcut = taskbar::buttonShortcut(button, tr("Win顺 搜索").toStdWString());
    if (shortcut.empty())
        return false;
    shell::revealInExplorer(QString::fromStdWString(shortcut)); // its menu pins it
    return true;
}

void SettingsEditor::setClipboardHotkey(const QString& hotkey)
{
    if (m_settings.clipboardHotkey == hotkey)
        return;
    m_settings.clipboardHotkey = hotkey;
    commit();
}

void SettingsEditor::setClipboardHotkeyError(const QString& error)
{
    if (m_clipboardHotkeyError == error)
        return;
    m_clipboardHotkeyError = error;
    emit hotkeyErrorChanged();
}

void SettingsEditor::setClipboardMaxItems(int items)
{
    if (m_settings.clipboardMaxItems == items || items < 10)
        return;
    m_settings.clipboardMaxItems = items;
    commit();
}

void SettingsEditor::setClipboardMaxDays(int days)
{
    if (m_settings.clipboardMaxDays == days || days < 0)
        return;
    m_settings.clipboardMaxDays = days;
    commit();
}

void SettingsEditor::setClipboardImages(bool on)
{
    if (m_settings.clipboardImages == on)
        return;
    m_settings.clipboardImages = on;
    commit();
}

void SettingsEditor::setClipboardCount(int count)
{
    if (m_clipboardCount == count)
        return;
    m_clipboardCount = count;
    emit clipboardCountChanged();
}

bool SettingsEditor::addClipboardExcludedApp(const QString& name)
{
    if (!addProgram(m_settings.clipboardExcludedApps, name))
        return false;
    commit();
    return true;
}

void SettingsEditor::removeClipboardExcludedApp(int index)
{
    if (index < 0 || index >= m_settings.clipboardExcludedApps.size())
        return;
    m_settings.clipboardExcludedApps.removeAt(index);
    commit();
}

QVariantList SettingsEditor::webShortcuts() const
{
    QVariantList list;
    for (const WebShortcut& shortcut : m_settings.webShortcuts) {
        list.append(QVariantMap {
            {u"keyword"_s, shortcut.keyword},
            {u"name"_s, shortcut.name},
            {u"url"_s, shortcut.url},
            {u"home"_s, shortcut.home},
            {u"searches"_s, shortcut.searches()},
            {u"shownUrl"_s, displayWebUrl(shortcut.url)},
            {u"shownHome"_s, displayWebUrl(shortcut.homeUrl())},
        });
    }
    return list;
}

QString SettingsEditor::saveWebShortcut(
    int index, const QString& keyword, const QString& name, const QString& url, const QString& home)
{
    WebShortcuts& shortcuts = m_settings.webShortcuts;
    if (index >= shortcuts.size())
        return tr("这一项已经不在了"); // the file was changed meanwhile

    WebShortcut shortcut;
    shortcut.keyword = keyword.trimmed();
    if (shortcut.keyword.isEmpty())
        return tr("请填写关键词");
    if (!isValidWebKeyword(shortcut.keyword))
        return tr("关键词里不能有空格或引号");
    for (qsizetype i = 0; i < shortcuts.size(); ++i) {
        if (i != index && shortcuts[i].keyword.compare(shortcut.keyword, Qt::CaseInsensitive) == 0)
            return tr("关键词“%1”已经给了“%2”").arg(shortcuts[i].keyword, shortcuts[i].name);
    }
    shortcut.url = normalizeWebUrl(url);
    if (shortcut.url.isEmpty())
        return url.trimmed().isEmpty() ? tr("请填写网址") : tr("打不开这个网址：它应当以 https:// 开头");
    shortcut.home = normalizeWebUrl(home);
    if (shortcut.home.isEmpty() && !home.trimmed().isEmpty())
        return tr("打不开主页的网址：它应当以 https:// 开头");
    shortcut.name = name.trimmed();
    if (shortcut.name.isEmpty()) { // the site's name ("github.com"); for an app's link, the keyword
        const QString shown = displayWebUrl(shortcut.homeUrl());
        const QString site = shown.left(shown.indexOf(u'/'));
        shortcut.name = site.isEmpty() || site.contains(u':') ? shortcut.keyword : site;
    }

    if (index < 0)
        shortcuts.append(shortcut);
    else if (shortcuts[index] != shortcut)
        shortcuts[index] = shortcut;
    else
        return {};
    commit();
    return {};
}

void SettingsEditor::removeWebShortcut(int index)
{
    if (index < 0 || index >= m_settings.webShortcuts.size())
        return;
    m_settings.webShortcuts.removeAt(index);
    commit();
}

void SettingsEditor::openWebShortcut(int index)
{
    if (index >= 0 && index < m_settings.webShortcuts.size())
        shell::openUrl(m_settings.webShortcuts[index].homeUrl());
}

bool SettingsEditor::isDefault() const
{
    return m_settings == Settings::defaults();
}

void SettingsEditor::addExcludedFolder()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString folder = shell::pickFolder(owner, tr("选择不需要搜索的文件夹"));
    if (folder.isEmpty() || containsIgnoreCase(excludedPaths(), displayPath(folder)))
        return;
    m_settings.excludedPaths.append(folder);
    commit();
}

void SettingsEditor::removeExcludedPath(int index)
{
    if (index < 0 || index >= m_settings.excludedPaths.size())
        return;
    m_settings.excludedPaths.removeAt(index);
    commit();
}

bool SettingsEditor::addExcludedName(const QString& name)
{
    QString value = QDir::fromNativeSeparators(name.trimmed());
    while (value.endsWith(u'/'))
        value.chop(1);
    if (value.isEmpty() || containsIgnoreCase(m_settings.excludedNames, value))
        return false;
    m_settings.excludedNames.append(value);
    commit();
    return true;
}

void SettingsEditor::removeExcludedName(int index)
{
    if (index < 0 || index >= m_settings.excludedNames.size())
        return;
    m_settings.excludedNames.removeAt(index);
    commit();
}

bool SettingsEditor::addContentExtensions(const QString& text)
{
    static const QRegularExpression separators(u"[\\s,;，；、]+"_s);
    bool added = false;
    for (QString ext : text.split(separators, Qt::SkipEmptyParts)) {
        while (ext.startsWith(u'*') || ext.startsWith(u'.'))
            ext.remove(0, 1);
        ext = ext.toLower();
        if (ext.isEmpty() || containsIgnoreCase(m_settings.contentExtensions, ext))
            continue;
        m_settings.contentExtensions.append(ext);
        added = true;
    }
    if (added)
        commit();
    return added;
}

void SettingsEditor::removeContentExtension(int index)
{
    if (index < 0 || index >= m_settings.contentExtensions.size())
        return;
    m_settings.contentExtensions.removeAt(index);
    commit();
}

void SettingsEditor::restoreDefaults()
{
    if (isDefault())
        return;
    m_settings = Settings::defaults();
    commit();
}

void SettingsEditor::openDataFolder()
{
    shell::open(Settings::dataDir());
}

void SettingsEditor::setIndexFolderState(const IndexFolderState& state)
{
    if (m_indexFolderState == state)
        return;
    m_indexFolderState = state;
    emit indexFolderChanged();
}

void SettingsEditor::chooseIndexFolder()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString picked = shell::pickFolder(owner, tr("选择存放索引的文件夹"));
    if (picked.isEmpty())
        return;
    // An empty folder, or one with the index in it, is used as it is. In any
    // other the files get a WinShun folder of their own: they never mix with
    // other files, and leaving it out of searches hides nothing else.
    const QString folder = QDir::cleanPath(QDir::fromNativeSeparators(picked));
    const auto same = [&](const QString& other) {
        return folder.compare(QDir::fromNativeSeparators(other), Qt::CaseInsensitive) == 0;
    };
    const bool asItIs
        = same(Settings::dataDir()) || same(m_indexFolderState.folder) || indexfolder::onlyIndexFiles(folder);
    setIndexFolder(asItIs ? folder : QDir(folder).filePath(u"WinShun"_s));
}

void SettingsEditor::setIndexFolder(const QString& folder)
{
    // The default place is kept as nothing: it follows %LOCALAPPDATA%.
    QString value = QDir::cleanPath(QDir::fromNativeSeparators(folder.trimmed()));
    if (value.compare(QDir::fromNativeSeparators(Settings::dataDir()), Qt::CaseInsensitive) == 0)
        value.clear();
    if (m_settings.indexFolder == value)
        return;
    m_settings.indexFolder = value;
    commit();
}

void SettingsEditor::openIndexFolder()
{
    shell::open(m_indexFolderState.folder);
}

void SettingsEditor::openUrl(const QString& url)
{
    shell::openUrl(url);
}

void SettingsEditor::copyText(const QString& text)
{
    shell::copyText(text);
}

QVariantList SettingsEditor::findWindowsSettings(const QString& query, int max) const
{
    QVariantList found;
    const auto places = m_places ? m_places->places() : nullptr;
    const ParsedQuery parsed = parseQuery(query);
    if (!places || parsed.isEmpty())
        return found;
    const NameMatcher matcher(parsed);
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    for (const PlaceHit& hit : searchPlaces(*places, parsed, matcher, {})) {
        if (found.size() >= max)
            break;
        const PlaceInfo& place = (*places)[hit.index];
        // As the launcher shows them (ResultModel): the logo of Settings, or the place's own icon.
        const QString icon = place.icon.startsWith(u"app:") ? FileIconProvider::appIconUrl(place.icon.mid(4), true, dark)
                                                            : FileIconProvider::placeIconUrl(place.icon);
        found.append(QVariantMap {{u"name"_s, place.name}, {u"icon"_s, icon}, {u"command"_s, place.command}});
    }
    return found;
}

void SettingsEditor::openWindowsSetting(const QString& command)
{
    shell::run(command);
}

} // namespace ws
