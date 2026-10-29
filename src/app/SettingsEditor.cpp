#include "SettingsEditor.h"

#include "ClipImport.h"
#include "FavoritesSettings.h"
#include "FileIconProvider.h"
#include "IndexFolder.h"
#include "McpClients.h"
#include "PathText.h"
#include "ProxyAddress.h"
#include "PluginSettings.h"
#include "Query.h"
#include "ShareLister.h"
#include "ShareReach.h"
#include "SvgIcon.h"
#include "SystemCatalog.h"
#include "WebDav.h"
#include "WebShortcut.h"
#include "Win32Util.h"
#include "platform/FileManagers.h"
#include "platform/NumberKeys.h"
#include "platform/Shell.h"
#include "platform/ShortcutCapture.h"
#include "platform/SiteIcons.h"
#include "platform/TaskbarSearch.h"
#include "platform/TaskbarSearchBox.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocale>
#include <QRegularExpression>
#include <QProcess>
#include <QTimer>
#include <QSaveFile>
#include <QSet>
#include <QStyleHints>
#include <QUrl>
#include <QVariantMap>
#include <QWinEventNotifier>

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

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
    const QString app = Settings::programFileName(name);
    if (app.isEmpty() || containsIgnoreCase(list, app))
        return false;
    list.append(app);
    return true;
}

// Off the UI thread: whether the server `host` answers, before its shares
// are listed (NetShareEnum waits long for one that is not there). The probe,
// and for SMB Windows too, as File Explorer gets there (its IPC$; GitHub
// #116: the probe can be wrong where Windows gets there).
bool serverAnswers(const std::wstring& host)
{
    share::ReachSteps steps;
    steps.probe = [host] { return share::reachable(host, std::chrono::milliseconds(1500), true); };
    if (!webdav::parseHost(host)) // WebDAV answers for itself
        steps.windows = [ipc = L"\\\\" + host + L"\\IPC$"] { return share::askWindows(ipc); };
    if (share::reach(std::move(steps), std::chrono::seconds(5)) != share::Reach::Unreachable)
        return true;
    // One that speaks only SMB 1, which Windows no longer does: there all the
    // same, and listing its shares says why not (the probe's answer, kept).
    return !webdav::parseHost(host) && share::probe(host) == share::Answer::Smb1Only;
}

} // namespace

// Changed outside Win顺 (the user pins the button, hides Windows' search in
// the Settings app): refresh the taskbar controls and status at once.
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
    , m_knownShares(unc::knownShares())
    , m_knownWsl(wsl::distributions())
    , m_knownFolders(Settings::knownFolders())
    , m_foldersAnnounced(settings.indexFolders)
    , m_sharesAnnounced(settings.shares)
    , m_taskbarWatch(std::make_unique<TaskbarWatch>())
    , m_plugins(new PluginSettings(this, this))
    , m_favorites(new FavoritesSettings(this, this))
{
    connect(this, &SettingsEditor::changed, this, &SettingsEditor::drivesChanged);
    connect(siteicons::notifier(), &siteicons::Notifier::fetched, this, [this] {
        ++m_siteIconsArrived;
        emit siteIconArrived();
    });
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
    noteFoldersEdited();
}

void SettingsEditor::noteFoldersEdited()
{
    // The cards and the folders offered follow the list (added, removed, restored).
    if (m_settings.indexFolders == m_foldersAnnounced && m_settings.shares == m_sharesAnnounced)
        return;
    m_foldersAnnounced = m_settings.indexFolders;
    m_sharesAnnounced = m_settings.shares; // a known folder on a server is offered until it is a share
    emit foldersChanged();
}

void SettingsEditor::setSettings(const Settings& settings)
{
    if (settings == m_settings)
        return;
    const bool keys = settings.numberKeys != m_settings.numberKeys || settings.hotkey != m_settings.hotkey
        || settings.clipboardNumberKeys != m_settings.clipboardNumberKeys
        || settings.clipboardHotkey != m_settings.clipboardHotkey
        || settings.folderMenuHotkey != m_settings.folderMenuHotkey
        || settings.captureHotkey != m_settings.captureHotkey
        || settings.capturePinsHotkey != m_settings.capturePinsHotkey
        || settings.capturePinsRestoreHotkey != m_settings.capturePinsRestoreHotkey
        || settings.captureLastPinHotkey != m_settings.captureLastPinHotkey;
    // The shares' cards say whether their contents are read, too (GitHub #114).
    const bool shares = settings.shares != m_settings.shares || settings.contentSearch != m_settings.contentSearch;
    const bool drives = settings.selectedDrives != m_settings.selectedDrives;
    m_settings = settings;
    emit changed();
    if (shares)
        emit sharesChanged();
    if (drives)
        emit foldersChanged(); // drive selection is also shown on each folder's card
    noteFoldersEdited();
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

void SettingsEditor::setPreferredTypes(const QString& text)
{
    const QStringList types = Settings::fileExtensions(text);
    if (types == m_settings.preferredTypes)
        return;
    m_settings.preferredTypes = types;
    commit();
}

void SettingsEditor::resetPreferredTypes()
{
    setPreferredTypes(Settings::defaultPreferredTypes().join(u' '));
}

void SettingsEditor::setShowModified(bool on)
{
    if (m_settings.showModified == on)
        return;
    m_settings.showModified = on;
    commit();
}

void SettingsEditor::setShowModifiedTime(bool on)
{
    if (m_settings.showModifiedTime == on)
        return;
    m_settings.showModifiedTime = on;
    commit();
}

void SettingsEditor::setShowFileSize(bool on)
{
    if (m_settings.showFileSize == on)
        return;
    m_settings.showFileSize = on;
    commit();
}

void SettingsEditor::setDoubleClickToOpen(bool on)
{
    if (m_settings.doubleClickToOpen == on)
        return;
    m_settings.doubleClickToOpen = on;
    commit();
}

void SettingsEditor::setTabEntersFavoriteFolder(bool on)
{
    if (m_settings.tabEntersFavoriteFolder == on)
        return;
    m_settings.tabEntersFavoriteFolder = on;
    commit();
}

void SettingsEditor::setThumbnails(bool on)
{
    if (m_settings.thumbnails == on)
        return;
    m_settings.thumbnails = on;
    commit();
}

void SettingsEditor::setWebSuggestions(bool on)
{
    if (m_settings.webSuggestions == on)
        return;
    m_settings.webSuggestions = on;
    commit();
}

void SettingsEditor::setSort(const QString& sort)
{
    if (m_settings.sort == sort || !isSortName(sort))
        return;
    m_settings.sort = sort;
    commit();
}

void SettingsEditor::setStartList(const QString& list)
{
    if (m_settings.startList == list || (list != u"opened" && list != u"modified" && list != u"none"))
        return;
    m_settings.startList = list;
    commit();
}

void SettingsEditor::setClearOnOpen(bool on)
{
    if (m_settings.clearOnOpen == on)
        return;
    m_settings.clearOnOpen = on;
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

void SettingsEditor::setNumberHints(const QString& hints)
{
    if (m_settings.numberHints == hints || (hints != u"held" && hints != u"always" && hints != u"off"))
        return;
    m_settings.numberHints = hints;
    commit();
}

void SettingsEditor::setPreviewWith(const QString& with)
{
    if (m_settings.previewWith == with || (with != u"auto" && with != u"builtin"))
        return;
    m_settings.previewWith = with;
    commit();
}

void SettingsEditor::setClipboardNumberKeys(const QString& keys)
{
    if (m_settings.clipboardNumberKeys == keys || (keys != u"ctrl" && keys != u"alt" && keys != u"off"))
        return;
    m_settings.clipboardNumberKeys = keys;
    commit();
    refreshNumberKeys();
}

void SettingsEditor::setClipboardQuickPaste(const QString& keys)
{
    if (m_settings.clipboardQuickPaste == keys || (keys != u"ctrl" && keys != u"alt" && keys != u"off"))
        return;
    m_settings.clipboardQuickPaste = keys;
    commit();
}

void SettingsEditor::setClipboardQuickPasteError(const QString& error)
{
    if (m_clipboardQuickPasteError == error)
        return;
    m_clipboardQuickPasteError = error;
    emit hotkeyErrorChanged();
}

namespace {

// What the user should know of the number keys `keys` ("ctrl", "alt"):
// those another program holds as global hotkeys, and those that are one of
// Win顺's own shortcuts. Win顺 leaves both alone (numberkeys::heldMask,
// KeyRouter::heldAsHotkey), and their rows show the chord struck through.
QString describeNumberKeys(const QString& keys, const Settings& settings)
{
    if (keys != u"ctrl" && keys != u"alt")
        return {};
    const QString modifier = keys == u"alt" ? u"Alt"_s : u"Ctrl"_s;
    const auto chord = [&](int n) { return modifier + u'+' + QString::number(n); };
    QStringList own;
    QStringList others;
    const std::vector<int> held
        = numberkeys::heldAsHotkeys(keys == u"alt" ? numberkeys::Modifier::Alt : numberkeys::Modifier::Ctrl);
    for (int n = 1; n <= 9; ++n) {
        const QString c = chord(n);
        const bool mine = settings.hotkey.compare(c, Qt::CaseInsensitive) == 0
            || settings.clipboardHotkey.compare(c, Qt::CaseInsensitive) == 0
            || settings.folderMenuHotkey.compare(c, Qt::CaseInsensitive) == 0
            || (settings.captureEnabled
                && (settings.captureHotkey.compare(c, Qt::CaseInsensitive) == 0
                    || settings.capturePinsHotkey.compare(c, Qt::CaseInsensitive) == 0
                    || settings.capturePinsRestoreHotkey.compare(c, Qt::CaseInsensitive) == 0
                    || settings.captureLastPinHotkey.compare(c, Qt::CaseInsensitive) == 0));
        if (mine)
            own.append(c);
        else if (std::find(held.begin(), held.end(), n) != held.end())
            others.append(c);
    }
    QStringList parts;
    if (!others.isEmpty())
        parts.append(SettingsEditor::tr("%1 是别的程序的快捷键，留给它用，对应的行不标序号。")
                         .arg(others.join(SettingsEditor::tr("、"))));
    if (!own.isEmpty())
        parts.append(SettingsEditor::tr("%1 是你给 Win顺 设的快捷键，按它仍是打开或关闭窗口，对应的行不标序号。")
                         .arg(own.join(SettingsEditor::tr("、"))));
    return parts.join(u' ');
}

} // namespace

void SettingsEditor::refreshNumberKeys()
{
    const QString launcher = describeNumberKeys(m_settings.numberKeys, m_settings);
    const QString clipboard = describeNumberKeys(m_settings.clipboardNumberKeys, m_settings);
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
    refreshDoubleCtrlClash();
}

void SettingsEditor::setDoubleTapKey(const QString& key)
{
    if (m_settings.doubleTapKey == key || (key != u"ctrl" && key != u"alt" && key != u"shift"))
        return;
    m_settings.doubleTapKey = key;
    commit();
    refreshDoubleCtrlClash();
}

void SettingsEditor::setDoubleCtrlKey(const QString& key)
{
    if (m_settings.doubleCtrlKey == key || (key != u"both" && key != u"left" && key != u"right"))
        return;
    m_settings.doubleCtrlKey = key;
    commit();
    refreshDoubleCtrlClash();
}

namespace {

// The Ctrl that PowerToys' Find My Mouse is turned on to come with by a
// double tap ("left", "right"), from its settings files; empty for none
// (off, shaking the mouse, a shortcut). PowerToys' activation_method:
// 0 left Ctrl twice, 1 right Ctrl twice (FindMyMouse.h).
QString findMyMouseCtrl()
{
    const QString folder = qEnvironmentVariable("LOCALAPPDATA") + u"/Microsoft/PowerToys"_s;
    const auto read = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    };
    if (!read(folder + u"/settings.json"_s)[u"enabled"_s].toObject()[u"FindMyMouse"_s].toBool())
        return {};
    const QJsonValue method
        = read(folder + u"/FindMyMouse/settings.json"_s)[u"properties"_s].toObject()[u"activation_method"_s].toObject()[u"value"_s];
    if (!method.isDouble())
        return {};
    switch (method.toInt()) {
    case 0:
        return u"left"_s;
    case 1:
        return u"right"_s;
    default:
        return {};
    }
}

} // namespace

void SettingsEditor::refreshDoubleCtrlClash()
{
    QString clash;
    if (m_settings.doubleCtrl && m_settings.doubleTapKey == u"ctrl") {
        clash = findMyMouseCtrl();
        if (m_settings.doubleCtrlKey != u"both" && m_settings.doubleCtrlKey != clash)
            clash.clear();
    }
    if (clash == m_doubleCtrlClash)
        return;
    m_doubleCtrlClash = clash;
    emit doubleCtrlClashChanged();
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

void SettingsEditor::setShortcutAgain(const QString& what)
{
    if (m_settings.shortcutAgain == what || (what != u"close" && what != u"panel"))
        return;
    m_settings.shortcutAgain = what;
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
    emit autostartRequested(on); // App writes it: removing it may take UAC (GitHub #67)
}

void SettingsEditor::refreshAutostart()
{
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

void SettingsEditor::setIndexAccess(const QString& mode)
{
    if (mode == m_settings.indexAccess || (mode != u"auto" && mode != u"service" && mode != u"direct" && mode != u"walk"))
        return;
    if (mode == u"direct" && !m_indexAccessState.administrator)
        return; // not offered: the row leaves it out
    m_settings.indexAccess = mode;
    commit(); // the app restarts Win顺 with other rights, or switches in place (App::applyIndexAccess)
}

void SettingsEditor::setIndexAccessState(const IndexAccessState& state)
{
    emit drivesChanged();
    if (m_indexAccessState == state)
        return;
    const bool scope = state.foldersInEffect != m_indexAccessState.foldersInEffect;
    m_indexAccessState = state;
    emit indexAccessStateChanged();
    if (scope)
        emit foldersChanged(); // the cards say where each stands only while the folders are what is indexed
}

QStringList SettingsEditor::excludedPaths() const
{
    QStringList paths;
    for (const QString& p : m_settings.excludedPaths)
        paths.append(displayPath(p));
    return paths;
}

void SettingsEditor::setContentMaxSizeMB(int kind, double mb)
{
    if (kind < 0 || kind >= static_cast<int>(ContentSizeLimits::kKinds) || !std::isfinite(mb))
        return;
    mb = roundedSizeMB(mb);
    double& limit = m_settings.contentMaxSizeMB[static_cast<std::size_t>(kind)];
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

void SettingsEditor::setContentSearch(bool on)
{
    if (m_settings.contentSearch == on)
        return;
    m_settings.contentSearch = on;
    commit();
    emit sharesChanged(); // their cards say whether their contents are read (GitHub #114)
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

void SettingsEditor::setProxyMode(const QString& mode)
{
    if (m_settings.proxyMode == mode || (mode != u"system" && mode != u"none" && mode != u"manual"))
        return;
    m_settings.proxyMode = mode;
    commit();
}

void SettingsEditor::setProxyAddress(const QString& address)
{
    const QString text = address.trimmed();
    if (m_settings.proxyAddress == text)
        return;
    m_settings.proxyAddress = text;
    commit();
}

QString SettingsEditor::proxyProblem(const QString& address) const
{
    switch (proxy::parse(address).problem) {
    case proxy::Problem::None:
        return {};
    case proxy::Problem::Empty:
        return tr("还没填代理地址，例如 127.0.0.1:7890。填好之前先跟随系统");
    case proxy::Problem::NoPort:
        return tr("地址后面要写端口，例如 127.0.0.1:7890");
    case proxy::Problem::BadAddress:
        return tr("地址格式不对，应该像 127.0.0.1:7890");
    case proxy::Problem::BadPort:
        return tr("端口应该是 1 到 65535 之间的数字");
    case proxy::Problem::Socks:
        return tr("暂时不支持 SOCKS 代理。代理软件通常同一个端口也支持 HTTP，填那个端口就行");
    case proxy::Problem::OtherScheme:
        return tr("只支持 HTTP 代理，开头写 http:// 或者不写");
    case proxy::Problem::Credentials:
        return tr("暂时不支持带用户名和密码的代理");
    }
    return {};
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

void SettingsEditor::setTaskbarIcon(const QString& style)
{
    if (m_settings.taskbarIcon == style)
        return;
    m_settings.taskbarIcon = style;
    commit();
}

void SettingsEditor::setSearchLogo(bool on)
{
    if (m_settings.searchLogo == on)
        return;
    m_settings.searchLogo = on;
    commit();
}

void SettingsEditor::setUiSize(const QString& size)
{
    if (m_settings.uiSize == size || (size != u"compact" && size != u"standard" && size != u"roomy"))
        return;
    m_settings.uiSize = size;
    commit();
}

void SettingsEditor::setCorners(const QString& corners)
{
    if (m_settings.corners == corners || (corners != u"round" && corners != u"square"))
        return;
    m_settings.corners = corners;
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

void SettingsEditor::setFileManagerOpenCommand(const QString& command)
{
    const QString value = command.trimmed();
    if (m_settings.fileManagerOpenCommand == value)
        return;
    m_settings.fileManagerOpenCommand = value;
    commit();
}

void SettingsEditor::setFileManagerRevealCommand(const QString& command)
{
    const QString value = command.trimmed();
    if (m_settings.fileManagerRevealCommand == value)
        return;
    m_settings.fileManagerRevealCommand = value;
    commit();
}

QString SettingsEditor::fileManagerProblem() const
{
    if (m_settings.fileManager != u"custom")
        return {};
    const QString open = m_settings.fileManagerOpenCommand;
    const QString reveal = m_settings.fileManagerRevealCommand;
    if (open.isEmpty())
        return tr("还没填打开文件夹的命令，先用资源管理器");
    if (filemanager::customProgram(open).isEmpty())
        return tr("找不到打开文件夹的程序：%1").arg(pathtext::customCommand(open, {}).program);
    if (!reveal.isEmpty() && filemanager::customProgram(reveal).isEmpty())
        return tr("找不到打开所在位置的程序：%1").arg(pathtext::customCommand(reveal, {}).program);
    return {};
}

QString SettingsEditor::pickFileManagerProgram()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString path = shell::pickFile(owner, tr("选择文件管理器的程序"),
        {{tr("程序"), u"*.exe"_s}, {tr("所有文件"), u"*.*"_s}});
    if (path.isEmpty())
        return {};
    return pathtext::commandArgument(QDir::toNativeSeparators(path)) + u" {path}"_s;
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

QString SettingsEditor::opusFolderMenuCommand() const
{
    // {sourcepath$}: the folder shown, and nothing run for a non-folder
    // (a collection, an FTP site); |noterm: "D:" for a drive, not "D:\",
    // whose \" would read as a quote.
    const QString stub = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + u"/WinShunSearch.exe"_s);
    return u"\"%1\" --folder-menu \"{sourcepath$|noterm}\""_s.arg(stub);
}

void SettingsEditor::openOpusPreferences() const
{
    filemanager::openOpusPreferences();
}

namespace {

// Win顺's command for Directory Opus: of this copy, or of another one.
bool isFolderMenuCommand(const QString& command)
{
    return command.contains(u"WinShunSearch.exe\" --folder-menu"_s, Qt::CaseInsensitive);
}

} // namespace

void SettingsEditor::refreshOpusFolderMenu()
{
    if (m_opusFolderMenu == u"busy")
        return;
    QString state = u"manual"_s;
    QString taken;
    if (const std::wstring file = filemanager::opusPrefsFile(); !file.empty()) {
        QFile prefs(QString::fromStdWString(file));
        const pathtext::OpusBlankClick click = prefs.open(QIODevice::ReadOnly)
            ? pathtext::opusBlankClick(prefs.readAll()) : pathtext::OpusBlankClick {};
        if (!click.hasPlace) {
            state = u"manual"_s;
        } else if (click.enabled && isFolderMenuCommand(click.command)) {
            state = click.command.compare(opusFolderMenuCommand(), Qt::CaseInsensitive) == 0 ? u"on"_s : u"stale"_s;
        } else if (click.enabled && !click.command.trimmed().isEmpty()) {
            state = u"taken"_s;
            taken = click.command;
        } else {
            state = u"off"_s;
        }
    }
    const bool running = filemanager::opusRunning();
    if (state == m_opusFolderMenu && taken == m_opusFolderMenuTaken && running == m_opusRunning)
        return;
    if (!m_opusFolderMenu.isEmpty() && state != m_opusFolderMenu)
        m_opusFolderMenuProblem.clear(); // changed since (in Opus, say): the problem was the last try's
    m_opusFolderMenu = state;
    m_opusFolderMenuTaken = taken;
    m_opusRunning = running;
    emit opusFolderMenuChanged();
}

void SettingsEditor::setOpusFolderMenu(bool on)
{
    if (m_opusFolderMenu == u"busy")
        return;
    m_opusFolderMenu = u"busy"_s;
    m_opusFolderMenuProblem.clear();
    emit opusFolderMenuChanged();
    const QByteArray event = on ? pathtext::opusBlankClickEvent(opusFolderMenuCommand())
                                : m_settings.folderMenuOpusBefore.toUtf8(); // none: taken away
    const QString backup = QDir(Settings::dataDir()).filePath(u"opus-prefs.oxc.bak"_s);
    std::thread([guard = QPointer(this), on, event, backup, before = m_settings.folderMenuOpusBefore]() mutable {
        bool understood = true;
        const bool restarts = filemanager::opusRunning();
        const filemanager::PrefsEdit result = filemanager::editOpusPrefs(
            [&](const QByteArray& prefs) -> std::optional<QByteArray> {
                const pathtext::OpusBlankClick click = pathtext::opusBlankClick(prefs);
                const bool ours = isFolderMenuCommand(click.command);
                if (!on && !ours)
                    return std::nullopt; // the user's own by now: it stays
                QByteArray changed = pathtext::withOpusBlankClick(prefs, event);
                if (changed.isEmpty()) {
                    understood = false;
                    return std::nullopt;
                }
                if (on && !ours)
                    before = QString::fromUtf8(click.element);
                if (!QFile::exists(backup)) { // the file as it was before Win顺 first changed it
                    QSaveFile copy(backup);
                    if (copy.open(QIODevice::WriteOnly) && copy.write(prefs) == prefs.size())
                        copy.commit();
                }
                return changed;
            });
        QMetaObject::invokeMethod(qApp, [guard, on, result, understood, restarts, before] {
            if (!guard)
                return;
            guard->m_opusFolderMenu.clear(); // no longer busy
            if (result == filemanager::PrefsEdit::OpusStayed)
                guard->m_opusFolderMenuProblem = tr("Opus 没有关闭（也许有复制之类的任务还在进行），稍后再试");
            else if (result == filemanager::PrefsEdit::Failed)
                guard->m_opusFolderMenuProblem = tr("改不了 Opus 的设置文件，请照下面的步骤在 Opus 里设一次");
            else if (!understood)
                guard->m_opusFolderMenuProblem = tr("认不出 Opus 的设置文件，请照下面的步骤在 Opus 里设一次");
            if (result == filemanager::PrefsEdit::Done && understood
                && guard->m_settings.folderMenuOpusBefore != (on ? before : QString())) {
                guard->m_settings.folderMenuOpusBefore = on ? before : QString();
                guard->commit();
            }
            guard->refreshOpusFolderMenu();
            emit guard->opusFolderMenuChanged(); // the problem, though the state may be as it was
            if (restarts && result != filemanager::PrefsEdit::OpusStayed)
                emit guard->opusRestarted();
        });
    }).detach();
}

void SettingsEditor::setDialogAutoJump(bool on)
{
    if (m_settings.dialogAutoJump == on)
        return;
    m_settings.dialogAutoJump = on;
    m_settings.dialogAutoJumpAsked = true; // chosen here: not asked under the bar
    commit();
}

void SettingsEditor::setDialogAutoJumpWhen(const QString& when)
{
    if (m_settings.dialogAutoJumpWhen == when || (when != u"open" && when != u"back"))
        return;
    m_settings.dialogAutoJumpWhen = when;
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

void SettingsEditor::setLauncherWidthSet(bool set)
{
    if (m_launcherWidthSet == set)
        return;
    m_launcherWidthSet = set;
    emit launcherWidthSetChanged();
}

void SettingsEditor::setExplorerMenu(bool on)
{
    if (m_settings.explorerMenu == on)
        return;
    m_settings.explorerMenu = on;
    commit();
}

void SettingsEditor::setExplorerTyping(bool on)
{
    if (m_settings.explorerTyping == on)
        return;
    m_settings.explorerTyping = on;
    commit();
}

void SettingsEditor::setOpusTyping(bool on)
{
    if (m_settings.opusTyping == on)
        return;
    m_settings.opusTyping = on;
    commit();
}

void SettingsEditor::setFolderTyping(const QString& how)
{
    if (m_settings.folderTyping == how || (how != u"first" && how != u"only"))
        return;
    m_settings.folderTyping = how;
    commit();
}

void SettingsEditor::setFolderMenu(bool on)
{
    if (folderMenu() == on)
        return;
    m_settings.folderMenu = on ? QStringLiteral("auto") : QStringLiteral("off"); // "on" only by hand, in the file
    commit();
}

void SettingsEditor::setFolderMenuExplorer(bool on)
{
    if (m_settings.folderMenuExplorer == on)
        return;
    m_settings.folderMenuExplorer = on;
    commit();
}

void SettingsEditor::setFolderMenuDesktop(bool on)
{
    if (m_settings.folderMenuDesktop == on)
        return;
    m_settings.folderMenuDesktop = on;
    commit();
}

// The menu lists at least one group (GitHub #51): the last one on stays on,
// and its switch, which only follows `checked`, stays where it is.
void SettingsEditor::setFolderMenuFavorites(bool on)
{
    if (m_settings.folderMenuFavorites == on || (!on && !m_settings.folderMenuRecent && !m_settings.folderMenuWindows))
        return;
    m_settings.folderMenuFavorites = on;
    commit();
}

void SettingsEditor::setDropPie(bool on)
{
    if (m_settings.dropPie == on)
        return;
    m_settings.dropPie = on;
    commit();
}

void SettingsEditor::setFolderMenuRecent(bool on)
{
    if (m_settings.folderMenuRecent == on || (!on && !m_settings.folderMenuFavorites && !m_settings.folderMenuWindows))
        return;
    m_settings.folderMenuRecent = on;
    commit();
}

void SettingsEditor::setFolderMenuWindows(bool on)
{
    if (m_settings.folderMenuWindows == on || (!on && !m_settings.folderMenuFavorites && !m_settings.folderMenuRecent))
        return;
    m_settings.folderMenuWindows = on;
    commit();
}

void SettingsEditor::setFolderMenuHotkey(const QString& hotkey)
{
    const QString value = hotkey.trimmed();
    if (m_settings.folderMenuHotkey == value)
        return;
    m_settings.folderMenuHotkey = value;
    commit(); // the app registers it and says whether that worked (setFolderMenuHotkeyError)
}

void SettingsEditor::setFolderMenuHotkeyError(const QString& error)
{
    if (m_folderMenuHotkeyError == error)
        return;
    m_folderMenuHotkeyError = error;
    emit hotkeyErrorChanged();
}

void SettingsEditor::setCaptureLastPinHotkey(const QString& value)
{
    const auto normal = value.trimmed();
    if (m_settings.captureLastPinHotkey == normal)
        return;
    m_settings.captureLastPinHotkey = normal;
    commit();
    refreshNumberKeys();
}

void SettingsEditor::setCaptureSaveFolder(const QString& value)
{
    const auto normal = value.trimmed();
    if (m_settings.captureSaveFolder == normal)
        return;
    m_settings.captureSaveFolder = normal;
    commit();
}

void SettingsEditor::setCaptureRecordingFolder(const QString& value)
{
    const auto normal = value.trimmed();
    if (m_settings.captureRecordingFolder == normal)
        return;
    m_settings.captureRecordingFolder = normal;
    commit();
}

void SettingsEditor::setCaptureRecordingSystemAudio(bool value)
{
    const auto normal = value;
    if (m_settings.captureRecordingSystemAudio == normal)
        return;
    m_settings.captureRecordingSystemAudio = normal;
    commit();
}

void SettingsEditor::setCaptureRecordingMicrophone(bool value)
{
    const auto normal = value;
    if (m_settings.captureRecordingMicrophone == normal)
        return;
    m_settings.captureRecordingMicrophone = normal;
    commit();
}

void SettingsEditor::setCaptureRecordingQuality(int value)
{
    if (value < 0 || value > 2 || m_settings.captureRecordingQuality == value)
        return;
    m_settings.captureRecordingQuality = value;
    commit();
}

void SettingsEditor::setCaptureRecordingCursor(bool value)
{
    if (m_settings.captureRecordingCursor == value)
        return;
    m_settings.captureRecordingCursor = value;
    commit();
}

void SettingsEditor::setCaptureRecordingCountdown(bool value)
{
    if (m_settings.captureRecordingCountdown == value)
        return;
    m_settings.captureRecordingCountdown = value;
    commit();
}

void SettingsEditor::setCaptureRecordingClicks(bool value)
{
    if (m_settings.captureRecordingClicks == value) return;
    m_settings.captureRecordingClicks = value;
    commit();
}

void SettingsEditor::setCaptureLastPinHotkeyError(const QString& error)
{
    if (m_captureLastPinHotkeyError == error)
        return;
    m_captureLastPinHotkeyError = error;
    emit hotkeyErrorChanged();
}

void SettingsEditor::setCaptureEnabled(bool on)
{
    if (m_settings.captureEnabled == on)
        return;
    m_settings.captureEnabled = on;
    commit(); // the app makes or lets go of the capture (App::applyCapture)
}

void SettingsEditor::setCaptureHotkey(const QString& hotkey)
{
    const QString value = hotkey.trimmed();
    if (m_settings.captureHotkey == value)
        return;
    m_settings.captureHotkey = value;
    commit(); // the app registers it and says whether that worked (setCaptureHotkeyError)
    refreshNumberKeys(); // a Ctrl+digit of its own is struck through in the rows
}

void SettingsEditor::setCaptureHotkeyError(const QString& error)
{
    if (m_captureHotkeyError == error)
        return;
    m_captureHotkeyError = error;
    emit hotkeyErrorChanged();
}

void SettingsEditor::setCapturePrintScreen(bool on)
{
    if (m_settings.capturePrintScreen == on)
        return;
    m_settings.capturePrintScreen = on;
    commit();
}

void SettingsEditor::setCaptureCopyOnSelect(bool on)
{
    if (m_settings.captureCopyOnSelect == on)
        return;
    m_settings.captureCopyOnSelect = on;
    commit();
}

void SettingsEditor::setCaptureMagnifier(bool on)
{
    if (m_settings.captureMagnifier == on)
        return;
    m_settings.captureMagnifier = on;
    commit();
}

void SettingsEditor::setCapturePinOnCopy(bool on)
{
    if (m_settings.capturePinOnCopy == on)
        return;
    m_settings.capturePinOnCopy = on;
    commit();
}

void SettingsEditor::setCaptureMouse(QString& field, const QString& action, bool allowBack)
{
    static const QStringList known {u"none"_s, u"copy"_s, u"copypin"_s, u"save"_s, u"pin"_s, u"ocr"_s, u"ocrcopy"_s,
        u"ocrsearch"_s, u"ocrweb"_s, u"cancel"_s};
    if (field == action || !(known.contains(action) || (allowBack && action == u"back")))
        return;
    field = action;
    commit();
}

void SettingsEditor::setCaptureToolbarHidden(const QString& ids)
{
    const QString normal = Settings::normalizedToolbarHidden(ids);
    if (m_settings.captureToolbarHidden == normal)
        return;
    m_settings.captureToolbarHidden = normal;
    commit();
}

void SettingsEditor::setCaptureCursor(bool on)
{
    if (m_settings.captureCursor == on)
        return;
    m_settings.captureCursor = on;
    commit();
}

void SettingsEditor::setCaptureHdrFile(bool on)
{
    if (m_settings.captureHdrFile == on)
        return;
    m_settings.captureHdrFile = on;
    commit();
}

void SettingsEditor::setCaptureControlsUia(bool on)
{
    if (m_settings.captureControlsUia == on)
        return;
    m_settings.captureControlsUia = on;
    commit();
}

void SettingsEditor::setCaptureDelay(int seconds)
{
    if ((seconds != 3 && seconds != 5 && seconds != 10) || m_settings.captureDelay == seconds)
        return;
    m_settings.captureDelay = seconds;
    commit();
}

void SettingsEditor::setCapturePinDoubleClick(const QString& action)
{
    if ((action != u"reset" && action != u"close" && action != u"collapse" && action != u"none")
        || m_settings.capturePinDoubleClick == action)
        return;
    m_settings.capturePinDoubleClick = action;
    commit();
}

void SettingsEditor::setCapturePinsHotkey(const QString& hotkey)
{
    const QString value = hotkey.trimmed();
    if (m_settings.capturePinsHotkey == value)
        return;
    m_settings.capturePinsHotkey = value;
    commit(); // the app registers it and says whether that worked (setCapturePinsHotkeyError)
}

void SettingsEditor::setCapturePinsRestoreHotkey(const QString& hotkey)
{
    const QString value = hotkey.trimmed();
    if (m_settings.capturePinsRestoreHotkey == value)
        return;
    m_settings.capturePinsRestoreHotkey = value;
    commit();
}

void SettingsEditor::setCapturePinsHotkeyError(const QString& error)
{
    if (m_capturePinsHotkeyError == error)
        return;
    m_capturePinsHotkeyError = error;
    emit hotkeyErrorChanged();
}

void SettingsEditor::setCapturePinsRestoreHotkeyError(const QString& error)
{
    if (m_capturePinsRestoreHotkeyError == error)
        return;
    m_capturePinsRestoreHotkeyError = error;
    emit hotkeyErrorChanged();
}

void SettingsEditor::setCaptureSmartPrescan(bool on)
{
    if (m_settings.captureSmartPrescan == on)
        return;
    m_settings.captureSmartPrescan = on;
    commit();
}

void SettingsEditor::setCaptureSmartIpMac(bool on)
{
    if (m_settings.captureSmartIpMac == on)
        return;
    m_settings.captureSmartIpMac = on;
    commit();
}

void SettingsEditor::setCaptureSmartAllText(bool on)
{
    if (m_settings.captureSmartAllText == on)
        return;
    m_settings.captureSmartAllText = on;
    commit();
}

// Words separated by commas (also the Chinese ones, semicolons, line breaks),
// each trimmed, none empty, kept as one comma separated line.
void SettingsEditor::setCaptureSmartWords(const QString& words)
{
    const QString normal = Settings::normalizedSmartWords(words).join(u',');
    if (m_settings.captureSmartWords == normal)
        return;
    m_settings.captureSmartWords = normal;
    commit();
}

void SettingsEditor::setPasteFileEnabled(bool on)
{
    if (m_settings.pasteFileEnabled == on)
        return;
    m_settings.pasteFileEnabled = on;
    commit(); // the app adds or takes away the menu's verb and Ctrl+V (App::applyPasteFile)
}

// One of the two ways to it stays on (Settings::load does the same).
void SettingsEditor::setPasteFileCtrlV(bool on)
{
    if (m_settings.pasteFileCtrlV == on || (!on && !m_settings.pasteFileMenu))
        return;
    m_settings.pasteFileCtrlV = on;
    commit();
}

void SettingsEditor::setPasteFileMenu(bool on)
{
    if (m_settings.pasteFileMenu == on || (!on && !m_settings.pasteFileCtrlV))
        return;
    m_settings.pasteFileMenu = on;
    commit();
}

void SettingsEditor::setCaptureNote(const QString& note)
{
    if (m_captureNote == note)
        return;
    m_captureNote = note;
    emit hotkeyErrorChanged();
}

QString SettingsEditor::saveCaptureKeyword(const QString& keyword)
{
    const QString word = keyword.trimmed();
    if (word.isEmpty())
        return tr("请填写关键词");
    if (!isValidWebKeyword(word))
        return tr("关键词里不能有空格或引号");
    const KeywordClaim claim = keywordClaim(word, m_settings.webShortcuts, -1, {},
        [this](const QString& keyword) { return m_favorites->keywordOwner(keyword); });
    if (claim.by == KeywordClaim::By::WebShortcut)
        return tr("关键词“%1”已经给了网页搜索“%2”").arg(word, claim.owner);
    if (claim.by == KeywordClaim::By::Favorite)
        return tr("关键词“%1”已经给了收藏“%2”").arg(word, claim.owner);
    if (m_settings.captureKeyword == word)
        return {};
    m_settings.captureKeyword = word;
    commit(); // the launcher and the plugins' keywords (App::applyCapture)
    return {};
}

QString SettingsEditor::captureKeywordTaken(const QString& keyword) const
{
    if (!m_settings.captureEnabled || m_settings.captureKeyword.compare(keyword.trimmed(), Qt::CaseInsensitive) != 0)
        return {};
    return tr("关键词“%1”已经给了截图历史（截图卡片上可以改）").arg(m_settings.captureKeyword);
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

void SettingsEditor::setTaskbarSearchBoxStyle(const QString& style)
{
    if (m_settings.taskbarSearchBoxStyle == style || (style != u"box" && style != u"label" && style != u"icon"))
        return;
    m_settings.taskbarSearchBoxStyle = style;
    commit();
}

bool SettingsEditor::searchBoxSupported() const
{
    return taskbar::SearchBox::supported();
}

void SettingsEditor::setSearchBoxShown(const QString& hidden)
{
    if (m_searchBoxHidden == hidden)
        return;
    m_searchBoxHidden = hidden;
    emit searchBoxShownChanged();
}

void SettingsEditor::setClassicTaskbar(bool classic)
{
    if (m_classicTaskbar == classic)
        return;
    m_classicTaskbar = classic;
    refreshTaskbarState(); // Windows' search, as that taskbar has it
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
    if (m_settings.clipboardMaxItems == items || items < 0 || (items > 0 && items < 10))
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

void SettingsEditor::setClipboardMissingFiles(const QString& mode)
{
    if (m_settings.clipboardMissingFiles == mode || (mode != u"keep" && mode != u"hide" && mode != u"clean"))
        return;
    m_settings.clipboardMissingFiles = mode;
    commit();
}

void SettingsEditor::setClipboardClickToPaste(bool on)
{
    if (m_settings.clipboardClickToPaste == on)
        return;
    m_settings.clipboardClickToPaste = on;
    commit();
}

void SettingsEditor::setClipboardKeepPlace(bool on)
{
    if (m_settings.clipboardKeepPlace == on)
        return;
    m_settings.clipboardKeepPlace = on;
    commit();
}

void SettingsEditor::setClipboardLargePictures(bool on)
{
    if (m_settings.clipboardLargePictures == on)
        return;
    m_settings.clipboardLargePictures = on;
    commit();
}

void SettingsEditor::setClipboardFlipAbove(bool on)
{
    if (m_settings.clipboardFlipAbove == on)
        return;
    m_settings.clipboardFlipAbove = on;
    commit();
}

void SettingsEditor::setClipboardHoldToStep(bool on)
{
    if (m_settings.clipboardHoldToStep == on)
        return;
    m_settings.clipboardHoldToStep = on;
    commit();
}

void SettingsEditor::setClipboardSearchBoxSide(const QString& side)
{
    if (m_settings.clipboardSearchBoxSide == side || (side != u"near" && side != u"far"))
        return;
    m_settings.clipboardSearchBoxSide = side;
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

QString SettingsEditor::pickClipboardImportFile()
{
    if (m_clipboardImporting)
        return {};
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString path = shell::pickFile(owner, tr("导入剪贴板历史"),
        {{tr("支持的文件"), u"*.db;*.txt;*.csv"_s}, {tr("Ditto 数据库"), u"*.db"_s}, {tr("文本"), u"*.txt"_s},
            {tr("表格"), u"*.csv"_s}, {tr("所有文件"), u"*.*"_s}});
    return QDir::toNativeSeparators(path);
}

bool SettingsEditor::clipboardImportAsksSplit(const QString& path) const
{
    return clipimport::sourceOf(path) == clipimport::Source::Text;
}

QString SettingsEditor::dittoDatabase() const
{
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty())
        return {};
    const QString path = appData + u"/Ditto/Ditto.db"_s;
    return QFileInfo::exists(path) ? QDir::toNativeSeparators(path) : QString();
}

void SettingsEditor::importClipboard(const QString& path, int split)
{
    if (m_clipboardImporting || path.isEmpty())
        return;
    m_clipboardImporting = true;
    m_clipboardImportMessage = tr("正在读取…");
    emit clipboardImportChanged();
    emit clipboardImportRequested(path, split);
}

void SettingsEditor::setClipboardImportProgress(int done, int total)
{
    if (!m_clipboardImporting)
        return;
    m_clipboardImportMessage = tr("正在导入… %1 / %2").arg(done).arg(total);
    emit clipboardImportChanged();
}

void SettingsEditor::setClipboardImportDone(const clipimport::Report& report, const QString& group)
{
    using clipimport::Error;
    m_clipboardImporting = false;
    if (report.error != Error::None) {
        QString why;
        switch (report.error) {
        case Error::Missing:
            why = tr("找不到这个文件");
            break;
        case Error::TooLarge:
            why = tr("文件太大了（超过 1 GB）");
            break;
        case Error::NotDitto:
            why = tr("这不是 Ditto 的数据库，或者数据库有密码、已损坏");
            break;
        case Error::CannotCopy:
            why = tr("复制不了 Ditto 的数据库（磁盘空间不够，或被占用）");
            break;
        case Error::NoHistory:
            why = tr("剪贴板历史没有打开");
            break;
        case Error::Busy:
            why = tr("上一个导入还没做完");
            break;
        default:
            why = tr("读不了这个文件");
            break;
        }
        m_clipboardImportMessage = tr("导入失败：%1").arg(why);
    } else {
        QStringList parts;
        parts.append(report.cancelled ? tr("已取消，已导入 %n 条", "", report.added) : tr("已导入 %n 条", "", report.added));
        if (report.duplicates > 0)
            parts.append(tr("跳过 %n 条重复", "", report.duplicates));
        if (report.skipped > 0)
            parts.append(tr("%n 条用不了（空的、太大或格式不支持）", "", report.skipped));
        if (report.failed > 0)
            parts.append(tr("%n 条没存上", "", report.failed));
        m_clipboardImportMessage = parts.join(tr("，"));
        if (report.added > 0)
            m_clipboardImportMessage += report.ownGroupUsed && !group.isEmpty()
                ? tr("。放在分组“%1”里，不会被自动删除").arg(group)
                : tr("。放在分组里，不会被自动删除");
    }
    emit clipboardImportChanged();
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
            {u"homeUrl"_s, shortcut.homeUrl()},
            {u"icon"_s, shortcut.icon},
            {u"iconRef"_s, shortcut.iconRef()},
            {u"suggest"_s, shortcut.suggest},
        });
    }
    return list;
}

QVariantList SettingsEditor::fileTypes() const
{
    QVariantList list;
    for (const FileType& type : m_settings.namedFileTypes())
        list.append(QVariantMap {{u"name"_s, type.name}, {u"extensions"_s, type.extensions}, {u"keyword"_s, type.keyword}});
    return list;
}

QString SettingsEditor::setFileTypeKeyword(int index, const QString& text)
{
    if (index < -1 || index >= m_settings.fileTypes.size())
        return tr("类型已不存在");
    const auto keyword = normalizedTypeKeyword(text);
    if (!keyword)
        return tr("前缀最多 32 个字符，只能包含文字、数字、下划线和连字符");
    if (!keyword->isEmpty()) {
        if (index != -1 && *keyword == m_settings.folderKeyword)
            return tr("这个前缀已用于文件夹");
        for (qsizetype i = 0; i < m_settings.fileTypes.size(); ++i) {
            if (i != index && *keyword == m_settings.fileTypes[i].keyword)
                return tr("已有类型使用这个前缀");
        }
    }
    QString& value = index == -1 ? m_settings.folderKeyword : m_settings.fileTypes[index].keyword;
    if (value != *keyword) {
        value = *keyword;
        commit();
    }
    return {};
}

QString SettingsEditor::addFileType(const QString& name, const QString& extensions)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return tr("给类型起个名字");
    const FileTypes named = m_settings.namedFileTypes();
    if (std::ranges::any_of(named, [&](const FileType& t) { return t.name.compare(trimmed, Qt::CaseInsensitive) == 0; }))
        return tr("已有这个类型");
    const QStringList exts = Settings::fileExtensions(extensions);
    if (exts.isEmpty())
        return tr("填上扩展名，例如 md, log");
    m_settings.fileTypes.append({trimmed, exts, {}});
    commit();
    return {};
}

QString SettingsEditor::renameFileType(int index, const QString& name)
{
    if (index < 0 || index >= m_settings.fileTypes.size())
        return {};
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return tr("给类型起个名字");
    const FileTypes named = m_settings.namedFileTypes();
    if (named[index].name == trimmed)
        return {};
    for (qsizetype i = 0; i < named.size(); ++i) {
        if (i != index && named[i].name.compare(trimmed, Qt::CaseInsensitive) == 0)
            return tr("已有这个类型");
    }
    m_settings.fileTypes[index].name = trimmed;
    commit();
    return {};
}

QString SettingsEditor::setFileTypeExtensions(int index, const QString& text)
{
    if (index < 0 || index >= m_settings.fileTypes.size())
        return {};
    const QStringList exts = Settings::fileExtensions(text);
    if (exts.isEmpty())
        return tr("填上扩展名，例如 md, log");
    if (exts != m_settings.fileTypes[index].extensions) {
        m_settings.fileTypes[index].extensions = exts;
        commit();
    }
    return {};
}

void SettingsEditor::removeFileType(int index)
{
    if (index < 0 || index >= m_settings.fileTypes.size())
        return;
    m_settings.fileTypes.removeAt(index);
    commit();
}

void SettingsEditor::moveFileType(int index, int delta)
{
    const qsizetype to = index + delta;
    if (index < 0 || index >= m_settings.fileTypes.size() || to < 0 || to >= m_settings.fileTypes.size())
        return;
    m_settings.fileTypes.move(index, to);
    commit();
}

void SettingsEditor::resetFileTypes()
{
    m_settings.fileTypes = Settings::defaultFileTypes();
    m_settings.folderKeyword = Settings{}.folderKeyword;
    commit();
}

QString SettingsEditor::webIconUrl(const QString& homeUrl, bool dark) const
{
    return FileIconProvider::webIconUrl(homeUrl, dark);
}

namespace {

// The pictures no shortcut uses any more go; `picked` is one a form still has.
void pruneWebIcons(const WebShortcuts& shortcuts, const QString& picked)
{
    QStringList inUse {picked};
    for (const WebShortcut& shortcut : shortcuts)
        inUse.append(shortcut.icon);
    siteicons::pruneCustom(inUse);
}

constexpr qsizetype kMaxWebFile = 16 << 20; // a file of shortcuts, icons and all

} // namespace

QString SettingsEditor::knownSuggestUrl(const QString& url) const
{
    return ws::knownSuggestUrl(normalizeWebUrl(url));
}

QString SettingsEditor::saveWebShortcut(int index, const QString& keyword, const QString& name, const QString& url,
    const QString& home, const QString& icon, const QString& suggest)
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
    const KeywordClaim claim = keywordClaim(shortcut.keyword, shortcuts, index,
        m_settings.captureEnabled ? m_settings.captureKeyword : QString(),
        [this](const QString& keyword) { return m_favorites->keywordOwner(keyword); });
    if (claim.by == KeywordClaim::By::WebShortcut)
        return tr("关键词“%1”已经给了“%2”").arg(shortcut.keyword, claim.owner);
    if (claim.by == KeywordClaim::By::Favorite)
        return tr("关键词“%1”已经给了收藏“%2”").arg(shortcut.keyword, claim.owner);
    if (claim.by == KeywordClaim::By::Capture)
        return captureKeywordTaken(shortcut.keyword);
    shortcut.url = normalizeWebUrl(url);
    if (shortcut.url.isEmpty())
        return url.trimmed().isEmpty() ? tr("请填写网址") : tr("打不开这个网址：它应当以 https:// 开头");
    shortcut.home = normalizeWebUrl(home);
    if (shortcut.home.isEmpty() && !home.trimmed().isEmpty())
        return tr("打不开主页的网址：它应当以 https:// 开头");
    shortcut.suggest = normalizeSuggestUrl(suggest);
    if (shortcut.suggest.isEmpty() && !suggest.trimmed().isEmpty())
        return tr("搜索建议的接口地址应当以 https:// 开头，要搜的文字用 %s 代替");
    if (!shortcut.suggest.isEmpty() && !shortcut.searches())
        return tr("这个网址里没有 %s，不能搜索，也就没有搜索建议");
    if (isValidCustomIconName(icon))
        shortcut.icon = icon;
    shortcut.name = name.trimmed();
    if (shortcut.name.isEmpty()) // the site's name ("github.com"); for an app's link, the keyword
        shortcut.name = defaultWebShortcutName(shortcut);

    m_pickedWebIcon.clear(); // saved with it, or let go
    if (index < 0)
        shortcuts.append(shortcut);
    else if (shortcuts[index] != shortcut)
        shortcuts[index] = shortcut;
    else
        return {};
    commit();
    pruneWebIcons(shortcuts, m_pickedWebIcon);
    return {};
}

void SettingsEditor::removeWebShortcut(int index)
{
    if (index < 0 || index >= m_settings.webShortcuts.size())
        return;
    m_settings.webShortcuts.removeAt(index);
    commit();
    pruneWebIcons(m_settings.webShortcuts, m_pickedWebIcon);
}

void SettingsEditor::openWebShortcut(int index)
{
    if (index >= 0 && index < m_settings.webShortcuts.size())
        shell::openUrl(m_settings.webShortcuts[index].homeUrl());
}

// SVG code (pasted, or a .svg picked) drawn and kept, or why not.
QVariantMap SettingsEditor::keepSvgIcon(QByteArrayView svg)
{
    svgicon::Problem problem = svgicon::Problem::None;
    const QImage image = svgicon::render(svg, 256, &problem);
    QString error;
    switch (problem) {
    case svgicon::Problem::None:
        break;
    case svgicon::Problem::TooLarge:
        error = tr("SVG 太大了：图标的 SVG 代码请在 %1 KB 以内").arg(svgicon::kMaxBytes / 1024);
        break;
    case svgicon::Problem::NotSvg:
        error = tr("读不出这段 SVG：请复制完整的 <svg …>…</svg> 代码");
        break;
    case svgicon::Problem::External:
        error = tr("这段 SVG 引用了外部的文件或网址，Win顺 不会去读：请用只含图形本身的 SVG");
        break;
    case svgicon::Problem::Empty:
        error = tr("这段 SVG 画出来是空的：它没有大小（viewBox），或者没有可见的图形");
        break;
    }
    if (!error.isEmpty())
        return {{u"error"_s, error}};
    const QString icon = siteicons::keepCustomImage(image);
    if (icon.isEmpty())
        return {{u"error"_s, tr("保存不了这个图标")}};
    siteicons::keepCustomSvg(icon, svg); // shown again when the shortcut is edited
    m_pickedWebIcon = icon;
    return {{u"icon"_s, icon}, {u"iconRef"_s, u"custom:"_s + icon}, {u"svg"_s, QString::fromUtf8(svg)}};
}

QVariantMap SettingsEditor::pickWebIcon()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString path = shell::pickFile(owner, tr("选择图标"),
        {{tr("图片（PNG、ICO、SVG）"), u"*.png;*.ico;*.svg"_s}, {tr("所有文件"), u"*.*"_s}});
    if (path.isEmpty())
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {{u"error"_s, tr("读不了这个文件")}};
    if (file.size() > 1 << 20)
        return {{u"error"_s, tr("图片太大了：请用 1 MB 以内的 PNG")}};
    const QByteArray bytes = file.readAll();
    if (svgicon::looksLikeSvg(bytes))
        return keepSvgIcon(bytes);
    const QString icon = siteicons::keepCustom(bytes);
    if (icon.isEmpty())
        return {{u"error"_s, tr("用不了这张图片：请用 PNG、ICO 或 SVG，至少 16×16")}};
    m_pickedWebIcon = icon;
    return {{u"icon"_s, icon}, {u"iconRef"_s, u"custom:"_s + icon}};
}

QString SettingsEditor::webIconClipboardText() const
{
    const QString text = QGuiApplication::clipboard()->text().trimmed();
    if (text.size() > svgicon::kMaxBytes)
        return {};
    const QByteArray svg = text.toUtf8();
    return svg.size() <= svgicon::kMaxBytes && svgicon::looksLikeSvg(svg) && text.contains(u"<svg"_s, Qt::CaseInsensitive)
        ? text : QString();
}

QVariantMap SettingsEditor::webIconFromSvg(const QString& text)
{
    return keepSvgIcon(text.trimmed().toUtf8());
}

QString SettingsEditor::webIconSvg(const QString& icon) const
{
    return siteicons::customSvg(icon);
}

QString SettingsEditor::importWebShortcuts()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString path = shell::pickFile(owner, tr("导入网页搜索"),
        {{tr("网页搜索（JSON）"), u"*.json"_s}, {tr("所有文件"), u"*.*"_s}});
    if (path.isEmpty())
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return tr("读不了这个文件");
    if (file.size() > kMaxWebFile)
        return tr("这个文件太大了，不像是导出的网页搜索");
    bool newer = false;
    const std::optional<WebShortcutFile> read = readWebShortcutsFile(file.readAll(), &newer);
    if (!read)
        return newer ? tr("这个文件是更新版本的 Win顺 导出的，请先更新 Win顺") : tr("这不是 Win顺 导出的网页搜索文件");

    WebShortcuts imported;
    int badIcons = 0;
    for (const WebShortcutFileEntry& entry : read->entries) {
        WebShortcut shortcut = entry.shortcut;
        if (!entry.iconData.isEmpty()) {
            shortcut.icon = siteicons::keepCustom(entry.iconData);
            badIcons += shortcut.icon.isEmpty();
        }
        // One already here keeps the picture its user gave it, when the file has none.
        if (shortcut.icon.isEmpty()) {
            for (const WebShortcut& own : m_settings.webShortcuts) {
                if (own.keyword.compare(shortcut.keyword, Qt::CaseInsensitive) == 0)
                    shortcut.icon = own.icon;
            }
        }
        imported.append(shortcut);
    }
    const WebMerge merge = mergeWebShortcuts(m_settings.webShortcuts, imported);
    if (merge.added + merge.updated > 0)
        commit();
    pruneWebIcons(m_settings.webShortcuts, m_pickedWebIcon);

    if (imported.isEmpty() && read->skipped == 0)
        return tr("文件里没有网页搜索");
    QStringList parts;
    parts.append(tr("新增 %n 个", "", merge.added));
    if (merge.updated > 0)
        parts.append(tr("更新 %n 个", "", merge.updated));
    if (merge.unchanged > 0)
        parts.append(tr("%n 个本来就有", "", merge.unchanged));
    if (read->skipped > 0)
        parts.append(tr("%n 个用不了，跳过了", "", read->skipped));
    if (badIcons > 0)
        parts.append(tr("%n 个图标读不了，用网站自己的", "", badIcons));
    // Taken all the same (a web shortcut's keyword comes first), but not unsaid.
    const int favoriteKeywords = static_cast<int>(std::ranges::count_if(
        imported, [this](const WebShortcut& s) { return !m_favorites->keywordOwner(s.keyword).isEmpty(); }));
    if (favoriteKeywords > 0)
        parts.append(tr("%n 个和收藏的关键词重复，输入时先打开网页", "", favoriteKeywords));
    return tr("已导入：") + parts.join(tr("，"));
}

QString SettingsEditor::exportWebShortcuts()
{
    const WebShortcuts& shortcuts = m_settings.webShortcuts;
    if (shortcuts.isEmpty())
        return tr("还没有网页搜索可以导出");
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString path = shell::pickSaveFile(owner, tr("导出网页搜索"), tr("Win顺 网页搜索.json"),
        tr("网页搜索（JSON）"), u"json"_s);
    if (path.isEmpty())
        return {};
    const QByteArray json = writeWebShortcutsFile(shortcuts, [](const QString& icon) { return siteicons::customPng(icon); });
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit())
        return tr("写不了这个文件：%1").arg(QDir::toNativeSeparators(path));
    return tr("已导出 %n 个到 %1", "", static_cast<int>(shortcuts.size())).arg(QDir::toNativeSeparators(path));
}

namespace {

// "刚刚", "10 分钟前", "昨天 18:02", "10月3日": when a share or a chosen folder
// was last read in full (seconds since 1970).
QString sinceText(qint64 time)
{
    const QDateTime then = QDateTime::fromSecsSinceEpoch(time);
    const qint64 minutes = std::max<qint64>(0, then.secsTo(QDateTime::currentDateTime()) / 60);
    if (minutes < 1)
        return SettingsEditor::tr("刚刚");
    if (minutes < 60)
        return SettingsEditor::tr("%n 分钟前", "", static_cast<int>(minutes));
    const QDate day = then.date();
    const QDate today = QDate::currentDate();
    const QString clock = then.toString(u"HH:mm"_s);
    if (day == today)
        return SettingsEditor::tr("今天 %1").arg(clock);
    if (day == today.addDays(-1))
        return SettingsEditor::tr("昨天 %1").arg(clock);
    return SettingsEditor::tr("%1月%2日").arg(day.month()).arg(day.day());
}

// `pattern` ("%1核对过") with sinceText(time): a number and the Chinese after
// it apart, as everywhere in the UI ("10 分钟前", "3 项"): "今天 14:52 核对过",
// and still "刚刚核对过", "10月3日核对过".
QString withTime(const QString& pattern, qint64 time)
{
    const QString when = sinceText(time);
    const qsizetype at = pattern.indexOf(u"%1"_s);
    QString text = pattern.arg(when);
    const qsizetype end = at + when.size();
    if (at >= 0 && !when.isEmpty() && when.back().isDigit() && end < text.size()
        && text[end].script() == QChar::Script_Han)
        text.insert(end, u' ');
    return text;
}

QString itemsText(std::size_t items)
{
    return SettingsEditor::tr("%L1 项").arg(static_cast<qulonglong>(items));
}

// Why a share cannot be read, said for the user, with what to do about it.
QString problemText(const ShareIndexer::Status& s)
{
    using P = share::Problem;
    const QString host = QString::fromStdWString(webdav::displayHost(unc::host(s.path))); // "nas:5006"
    const QString distro = QString::fromStdWString(std::wstring(unc::wslDistribution(s.path)));
    // WebDAV goes through Windows' WebClient service: with it disabled, no address opens.
    if (webdav::isWebDavPath(s.path) && webdav::clientDisabled())
        return SettingsEditor::tr("Windows 的 WebClient 服务被禁用了，WebDAV 网址都打不开：请在“服务”里把它的启动类型改回“手动”");
    switch (s.problem) {
    case P::None:
    case P::Offline:
        if (!distro.isEmpty())
            return SettingsEditor::tr("暂时无法访问 %1 中的这个文件夹，稍后自动重试；可先在资源管理器中检查路径").arg(distro);
        return SettingsEditor::tr("离线：连不上 %1（已关机、不在同一网络，或 VPN 没连）").arg(host);
    case P::Dropped:
        return SettingsEditor::tr("连接中断了，稍后自动重试");
    case P::NotFound:
        if (!distro.isEmpty())
            return SettingsEditor::tr("找不到这个 WSL 文件夹，请检查路径是否已改名或删除");
        return SettingsEditor::tr("找不到这个共享文件夹：%1 在线，但上面没有它（改名或删除了？）").arg(host);
    case P::NeedsSignIn:
        if (webdav::httpSignInBlocked(s.path))
            return SettingsEditor::httpSignInText(false);
        return SettingsEditor::tr("需要登录：Windows 还没有 %1 的用户名和密码").arg(host);
    case P::GuestBlocked:
        return SettingsEditor::tr(
            "%1 只允许来宾（不用密码）访问，Windows 默认不允许这样连接。请在 %1 上建一个带密码的账户，再点“登录”；"
            "或者请管理员在组策略里启用“不安全的来宾登录”（会降低安全性）")
            .arg(host);
    case P::Signing:
        return SettingsEditor::tr("%1 不支持 Windows 要求的 SMB 签名，多见于老 NAS：请升级它的固件，或请管理员调整签名设置").arg(host);
    case P::Smb1Only:
        return SettingsEditor::tr("%1 只支持老旧的 SMB1，Windows 不再用它连接：请在 %1 上启用 SMB 2 或 SMB 3").arg(host);
    case P::Denied:
        if (!distro.isEmpty())
            return SettingsEditor::tr("没有权限读取这个 WSL 文件夹，请选择当前 Linux 用户有权限的文件夹");
        return SettingsEditor::tr("没有权限读取这个文件夹：换一个有权限的账户登录试试");
    case P::Other:
        switch (share::davRefusal(s.path, s.problem, s.error)) {
        case share::DavRefusal::MaybeSignIn:
            return SettingsEditor::tr("可能需要登录：%1 不让 Windows 读取，点“登录”输入用户名和密码试试").arg(host);
        case share::DavRefusal::Forbidden:
            return SettingsEditor::tr("服务器不让这个账号看这个文件夹");
        case share::DavRefusal::ServerError:
            return SettingsEditor::tr("服务器出错了，稍后自动重试");
        case share::DavRefusal::None:
            break;
        }
        break;
    case P::WslStopped:
        return SettingsEditor::tr("%1 未运行；启动后会自动继续索引").arg(distro);
    case P::WslNotRegistered:
        return SettingsEditor::tr("找不到 WSL 发行版 %1，请检查是否已卸载或改名").arg(distro);
    case P::WslUnavailable:
        return SettingsEditor::tr("暂时无法读取 WSL 状态，稍后自动重试；也可在终端检查 WSL 是否正常");
    }
    return SettingsEditor::tr("出错了（错误 %1），稍后自动重试").arg(s.error);
}

// How far a share's files' contents are read (GitHub #114), for its card.
// `share`: the share's own status; null: not known yet.
QString contentText(const remotecontent::ShareContent::Progress& p, const ShareIndexer::Status* share)
{
    using State = ShareIndexer::State;
    const bool online = share && share->state == State::Online;
    if (!p.known)
        return online ? SettingsEditor::tr("马上开始读取文件内容") : SettingsEditor::tr("连上后读取文件内容");
    if (p.metered)
        return SettingsEditor::tr("按流量计费的网络：暂停读取文件内容，文件名照常更新");
    if (p.files == 0)
        return SettingsEditor::tr("没有要读取内容的文件");
    const QLocale locale;
    const QString counts = u"%1/%2"_s.arg(locale.toString(static_cast<qulonglong>(p.current)),
        locale.toString(static_cast<qulonglong>(p.files)));
    if (share && share->state == State::Problem) // away: what was read answers searches meanwhile
        return SettingsEditor::tr("连不上：搜索时用上次读取的内容（%1）").arg(counts);
    if (p.reading)
        return SettingsEditor::tr("正在读取文件内容 %1").arg(counts);
    if (p.current >= p.files)
        return SettingsEditor::tr("已读取全部 %Ln 个文件的内容", nullptr,
            static_cast<int>(std::min<std::size_t>(p.files, std::numeric_limits<int>::max())));
    if (p.error != 0)
        return SettingsEditor::tr("已读取 %1 个文件的内容；读取时服务器没有响应，稍后再试").arg(counts);
    if (p.held)
        return SettingsEditor::tr("已读取 %1 个文件的内容，电脑空闲时继续").arg(counts);
    return SettingsEditor::tr("已读取 %1 个文件的内容").arg(counts);
}

// A share's title on its card: the name it was given, else the folder as
// users know it: "Ubuntu · /home" for WSL, the web address for WebDAV
// ("http://nas:5244/dav", GitHub #114; the card's second line has the path
// Windows opens), else its path.
QString shareTitle(const NetworkShare& share)
{
    if (!share.name.isEmpty())
        return share.name;
    const std::wstring path = share.path.toStdWString();
    if (const auto distro = unc::wslDistribution(path); !distro.empty()) {
        QString folder = QString::fromStdWString(path.substr(static_cast<std::size_t>(distro.data() - path.data()) + distro.size()));
        folder.replace(u'\\', u'/');
        return QString::fromStdWString(std::wstring(distro)) + u" · "_s + (folder.isEmpty() ? u"/"_s : folder);
    }
    if (const std::wstring url = webdav::urlOf(path); !url.empty())
        return QString::fromStdWString(url);
    return share.path;
}

} // namespace

QVariantList SettingsEditor::shares() const
{
    using State = ShareIndexer::State;
    QVariantList list;
    for (const NetworkShare& share : m_settings.shares) {
        const std::wstring path = share.path.toStdWString();
        const auto distro = unc::wslDistribution(path);
        const bool isWsl = !distro.empty();
        const auto it = std::ranges::find_if(m_shareStatuses, [&](const ShareIndexer::Status& s) {
            return unc::equal(s.path, path);
        });
        QString tone = u"idle"_s;
        QString status = tr("稍后开始索引");
        bool signIn = false;
        bool busy = false;
        if (it != m_shareStatuses.end()) {
            const ShareIndexer::Status& s = *it;
            const QString verified = s.lastVerified ? withTime(tr("%1核对过"), s.lastVerified) : QString();
            const QString connecting = isWsl
                ? tr("正在检查 %1…").arg(QString::fromStdWString(std::wstring(distro)))
                : tr("正在连接 %1…").arg(QString::fromStdWString(webdav::displayHost(unc::host(path))));
            switch (s.state) {
            case State::Waiting:
                if (s.lastVerified)
                    status = QStringList {itemsText(s.items), verified, tr("稍后连接")}.join(u" · "_s);
                break;
            case State::Building:
                tone = u"busy"_s;
                busy = true;
                status = tr("正在建立索引… 已收录 %1").arg(itemsText(s.items));
                break;
            case State::Verifying:
                tone = u"busy"_s;
                busy = true;
                status = tr("正在核对… · %1").arg(itemsText(s.items));
                break;
            case State::Online: {
                tone = u"ok"_s;
                QStringList parts {tr("在线"), itemsText(s.items), verified};
                if (isWsl)
                    parts.append(tr("自动核对变化"));
                else if (s.watching)
                    parts.append(tr("实时跟踪变化"));
                parts.removeAll(QString());
                status = parts.join(u" · "_s);
                // A line of its own, as a folder's 没有实时更新: at the usual
                // width, the end of one long line went alone onto the next.
                if (!isWsl && !s.watching) {
                    const int hours = m_settings.networkVerifyHoursUnwatched; // 0: on start (and when asked) alone
                    status += u'\n'
                        + (hours <= 0   ? tr("服务器不会通知变化，只在启动时核对")
                              : hours == 1 ? tr("服务器不会通知变化，每小时核对一次")
                                           : tr("服务器不会通知变化，每 %n 小时核对一次", "", hours));
                }
                break;
            }
            case State::Problem: {
                const bool away = s.problem == share::Problem::Offline || s.problem == share::Problem::Dropped
                    || s.problem == share::Problem::WslStopped || s.problem == share::Problem::WslUnavailable;
                tone = away ? u"idle"_s : u"error"_s;
                // Also a WebDAV root that perhaps wants signing in, or turns this
                // account away (another may do).
                const share::DavRefusal refusal = share::davRefusal(s.path, s.problem, s.error);
                signIn = !isWsl && (s.problem == share::Problem::NeedsSignIn || s.problem == share::Problem::GuestBlocked
                    || s.problem == share::Problem::Denied || refusal == share::DavRefusal::MaybeSignIn
                    || refusal == share::DavRefusal::Forbidden);
                status = s.checking ? connecting : problemText(s);
                QStringList details;
                if (s.items > 0) {
                    details.append(tr("已收录的 %1照常能搜到").arg(itemsText(s.items)));
                    if (s.lastVerified)
                        details.append(verified);
                }
                if (!details.isEmpty())
                    status += u"\n"_s + details.join(u" · "_s);
                // GitHub #116: it is tried again by itself, and says so. A line
                // of its own: at the usual width, ending the line above it
                // left its last character alone on a line.
                if (s.lastAttempt && !s.checking && s.problem != share::Problem::WslStopped)
                    status += u"\n"_s + withTime(tr("%1又试过"), s.lastAttempt);
                break;
            }
            case State::Suspect:
                tone = u"warn"_s;
                status = tr("这次核对少了 %1（原有 %2），先不删除：可能是服务器的磁盘还没准备好。半小时后再核对一次；"
                            "确实删掉了，就点“按现在的内容更新”")
                             .arg(itemsText(s.missing), itemsText(s.items));
                break;
            }
            // Asked to check (立即核对): shown at once, the button off, until
            // a pass starts or the server failed to answer (GitHub #116).
            if (s.checking) {
                tone = u"busy"_s;
                busy = true;
                if (s.state == State::Waiting || s.state == State::Online || s.state == State::Suspect)
                    status = s.lastVerified ? tr("正在核对… · %1").arg(itemsText(s.items)) : connecting;
            }
        }
        const bool starting = m_wslStarting.contains(share.path);
        if (starting) {
            tone = u"busy"_s;
            busy = true;
            status = tr("正在启动 %1…").arg(QString::fromStdWString(std::wstring(distro)));
        } else if (const auto error = m_wslStartProblems.constFind(share.path); error != m_wslStartProblems.cend()) {
            tone = u"error"_s;
            status = error.value();
        }
        QString contentStatus = tr("打开后，电脑空闲时读取文件内容；离线时也能搜到");
        if (share.content && !m_settings.contentSearch) {
            contentStatus = tr("内容搜索已关闭，不读取文件内容");
        } else if (share.content) {
            const auto c = std::ranges::find_if(m_shareContentStatuses,
                [&](const remotecontent::Indexer::Status& s) { return unc::equal(s.path, path); });
            contentStatus = contentText(c != m_shareContentStatuses.end() ? c->progress : remotecontent::ShareContent::Progress {},
                it != m_shareStatuses.end() ? &*it : nullptr);
        }
        list.append(QVariantMap {
            {u"path"_s, share.path},
            {u"name"_s, shareTitle(share)},
            {u"wsl"_s, isWsl},
            {u"startable"_s, isWsl && !starting && it != m_shareStatuses.end()
                && (it->problem == share::Problem::WslStopped || m_wslStartProblems.contains(share.path))},
            {u"tone"_s, tone},
            {u"status"_s, status},
            {u"signIn"_s, signIn},
            {u"suspect"_s, it != m_shareStatuses.end() && it->state == State::Suspect},
            {u"busy"_s, busy},
            {u"content"_s, share.content},
            {u"contentStatus"_s, contentStatus},
        });
    }
    return list;
}

QVariantList SettingsEditor::knownShares() const
{
    QVariantList list;
    for (const unc::Known& known : m_knownShares) {
        const QString path = QString::fromStdWString(known.path);
        if (std::ranges::any_of(m_settings.shares,
                [&](const NetworkShare& s) { return unc::equal(s.path.toStdWString(), known.path); }))
            continue;
        list.append(QVariantMap {{u"path"_s, path}, {u"drive"_s, QString::fromStdWString(known.drive)}});
    }
    return list;
}

QVariantList SettingsEditor::wslFolders() const
{
    QVariantList list;
    for (const auto& distro : m_knownWsl) {
        const std::wstring path = L"\\\\wsl.localhost\\" + distro.name + (distro.rootUser ? L"\\root" : L"\\home");
        if (std::ranges::any_of(m_settings.shares, [&](const NetworkShare& s) {
                return unc::contains(s.path.toStdWString(), path);
            }))
            continue;
        list.append(QVariantMap {
            {u"path"_s, QString::fromStdWString(path)},
            {u"name"_s, QString::fromStdWString(distro.name) + (distro.rootUser ? u" · /root"_s : u" · /home"_s)},
        });
    }
    return list;
}

bool SettingsEditor::isWslPath(const QString& text) const
{
    return unc::isWslHost(unc::host(QDir::toNativeSeparators(text.trimmed()).toStdWString()));
}

void SettingsEditor::startWslShare(int index)
{
    if (index < 0 || index >= m_settings.shares.size())
        return;
    const QString path = m_settings.shares[index].path;
    const std::wstring wide = path.toStdWString();
    const auto distro = unc::wslDistribution(wide);
    if (distro.empty() || m_wslStarting.contains(path))
        return;
    m_wslStarting.append(path);
    m_wslStartProblems.remove(path);
    emit sharesChanged();
    // Only this explicit user action starts WSL. No shell or interpolated command.
    auto* process = new QProcess(this);
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    const auto finish = [this, process, timeout, path](bool success) {
        if (!m_wslStarting.removeOne(path))
            return;
        timeout->stop();
        if (success)
            emit shareVerifyRequested(path);
        else
            m_wslStartProblems.insert(path, tr("没能启动 WSL，请先在终端启动这个发行版，再点“立即核对”"));
        emit sharesChanged();
        process->deleteLater();
    };
    connect(process, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit && code == 0);
    });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false);
    });
    process->start(wsl::executable(), {u"--distribution"_s, QString::fromStdWString(std::wstring(distro)),
        u"--exec"_s, u"/bin/true"_s});
    timeout->start(30'000);
}

void SettingsEditor::setShareStatuses(std::vector<ShareIndexer::Status> statuses)
{
    // The words of each are what counts ("10 分钟前" moves on by itself).
    const QVariantList before = shares();
    for (const auto& status : statuses) {
        if (status.state == ShareIndexer::State::Online || status.state == ShareIndexer::State::Building
            || status.state == ShareIndexer::State::Verifying)
            m_wslStartProblems.remove(QString::fromStdWString(status.path));
    }
    m_shareStatuses = std::move(statuses);
    if (shares() != before)
        emit sharesChanged();
}

void SettingsEditor::setShareContentStatuses(std::vector<remotecontent::Indexer::Status> statuses)
{
    if (statuses == m_shareContentStatuses)
        return;
    const QVariantList before = shares();
    m_shareContentStatuses = std::move(statuses);
    if (shares() != before)
        emit sharesChanged();
}

void SettingsEditor::setShareContent(int index, bool on)
{
    if (index < 0 || index >= m_settings.shares.size() || m_settings.shares[index].content == on)
        return;
    m_settings.shares[index].content = on;
    commit();
    emit sharesChanged();
}

QString SettingsEditor::serverOf(const QString& text) const
{
    QString t = QDir::toNativeSeparators(text.trimmed());
    if (!t.startsWith(u"\\\\"_s))
        return {};
    t = t.mid(2);
    while (t.endsWith(u'\\'))
        t.chop(1);
    if (t.isEmpty() || t.contains(u'\\') || t.startsWith(u'?') || t.startsWith(u'.'))
        return {};
    return t;
}

QString SettingsEditor::addShare(const QString& text)
{
    const QString path = Settings::sharePath(text);
    if (path.isEmpty()) {
        const QString t = text.trimmed();
        if (t.startsWith(u'/') && !t.startsWith(u"//"_s))
            return tr("请使用带发行版名的 WSL 路径，例如 %1；可以从资源管理器复制文件夹路径")
                .arg(u"\\\\wsl.localhost\\"_s
                    + (m_knownWsl.empty() ? u"Ubuntu"_s : QString::fromStdWString(m_knownWsl.front().name)) + u"\\home"_s);
        if (t.size() >= 2 && t[1] == u':')
            return tr("%1 不是映射的网络驱动器").arg(t.left(2).toUpper());
        if (const QString host = serverOf(t); !host.isEmpty())
            return tr("还要写上共享文件夹的名字，例如 \\\\%1\\资料").arg(host);
        if (t.contains(u"://"_s)) {
            switch (webdav::urlProblem(t.toStdWString())) {
            case webdav::UrlProblem::Scheme:
                return tr("WebDAV 网址要以 http:// 或 https:// 开头，例如 https://192.168.1.10:5006/资料");
            case webdav::UrlProblem::Ipv6: // WebClient's UNC form takes none
                return tr("WebDAV 网址不能用 IPv6 地址，请改用服务器的名字或 IPv4 地址");
            case webdav::UrlProblem::None:
            case webdav::UrlProblem::Address:
                break;
            }
            return tr("这个 WebDAV 网址不对，请检查服务器名和端口，例如 https://192.168.1.10:5006/资料");
        }
        return tr("请输入网络路径或 WebDAV 网址，例如 \\\\192.168.1.10\\资料、https://192.168.1.10:5006/资料");
    }
    // Both canonical (sharePath): a WebDAV server's root holds its folders,
    // which are written without it (webdav::rootHolds).
    const std::wstring wanted = path.toStdWString();
    const auto holds = [](const std::wstring& outer, const std::wstring& inner) {
        return unc::contains(outer, inner) || webdav::rootHolds(outer, inner);
    };
    for (const NetworkShare& share : m_settings.shares) {
        if (unc::equal(share.path.toStdWString(), wanted))
            return tr("已经添加过了");
        if (holds(share.path.toStdWString(), wanted))
            return tr("它在已添加的 %1 里面，已经能搜到").arg(shareTitle(share));
    }
    // A share holding one already added takes its place: no item twice. Its
    // files' contents are searched if those of one it replaces were (the
    // texts read go: the content index keeps them by the files' entries,
    // which go with the replaced share's root).
    bool content = false;
    m_settings.shares.removeIf([&](const NetworkShare& share) {
        const bool inner = holds(wanted, share.path.toStdWString());
        content = content || (inner && share.content);
        return inner;
    });
    m_settings.shares.append({path, {}, content});
    commit();
    emit sharesChanged();
    return {};
}

QString SettingsEditor::httpSignInText(bool failed)
{
    // Before signing in: a server that takes another way than plain text
    // (rare on a NAS) still lets one in, so the 登录 button stays.
    return failed ? tr("登录不上：Windows 默认只在 https 地址上发送密码，请改用 https 地址")
                  : tr("需要登录，但 Windows 默认只在 https 地址上发送密码，这个 http 地址多半登录不上：请改用 https 地址");
}

void SettingsEditor::removeShare(int index)
{
    if (index < 0 || index >= m_settings.shares.size())
        return;
    m_settings.shares.removeAt(index);
    commit();
    emit sharesChanged();
}

void SettingsEditor::verifyShare(int index)
{
    if (index >= 0 && index < m_settings.shares.size()) {
        m_wslStartProblems.remove(m_settings.shares[index].path);
        emit shareVerifyRequested(m_settings.shares[index].path);
        emit sharesChanged();
    }
}

void SettingsEditor::signInToShare(int index)
{
    if (index >= 0 && index < m_settings.shares.size())
        emit signInRequested(m_settings.shares[index].path);
}

void SettingsEditor::acceptShareMissing(int index)
{
    if (index >= 0 && index < m_settings.shares.size())
        emit shareAcceptRequested(m_settings.shares[index].path);
}

// ---- 索引哪些地方 (GitHub #52) ---------------------------------------------------

namespace {

// "3 秒", "2 分钟": how long a folder's last walk took.
QString tookText(std::chrono::milliseconds took)
{
    const qint64 seconds = took.count() / 1000;
    if (seconds < 1)
        return SettingsEditor::tr("不到 1 秒");
    if (seconds < 60)
        return SettingsEditor::tr("%n 秒", "", static_cast<int>(seconds));
    if (seconds < 3600)
        return SettingsEditor::tr("%n 分钟", "", static_cast<int>(seconds / 60));
    return SettingsEditor::tr("%n 小时", "", static_cast<int>(seconds / 3600));
}

// `path` is `folder` or lies inside it, as IndexService writes them ("D:"
// is that whole drive).
bool folderHolds(std::wstring_view folder, std::wstring_view path)
{
    return path.size() >= folder.size() && win32::equalsIgnoreCase(path.substr(0, folder.size()), folder)
        && (path.size() == folder.size() || path[folder.size()] == L'\\');
}

// A path typed or chosen, or a file:/// URL dropped: native separators, no
// backslash at the end but a drive's ("D:\").
QString folderText(const QString& text)
{
    QString t = text.trimmed();
    if (t.startsWith(u"file:"_s, Qt::CaseInsensitive))
        t = QUrl(t).toLocalFile();
    t = QDir::toNativeSeparators(t);
    while (t.size() > 3 && t.endsWith(u'\\'))
        t.chop(1);
    if (t.size() == 2 && t[1] == u':')
        t += u'\\';
    return t;
}

// A known folder's path as IndexService writes it, without reading the disk
// (Windows gives them in full): "D:\" -> "D:".
std::wstring knownKey(const QString& path)
{
    std::wstring key = path.toStdWString();
    while (key.size() > 2 && key.back() == L'\\')
        key.pop_back();
    return key;
}

} // namespace

const std::vector<std::wstring>& SettingsEditor::folderKeys() const
{
    if (m_folderKeysOf != m_settings.indexFolders || m_folderKeys.size() != static_cast<std::size_t>(m_settings.indexFolders.size())) {
        m_folderKeys.clear();
        for (const QString& folder : m_settings.indexFolders)
            m_folderKeys.push_back(Settings::indexFolderPath(folder));
        m_folderKeysOf = m_settings.indexFolders;
    }
    return m_folderKeys;
}

void SettingsEditor::setIndexScope(const QString& scope)
{
    if (scope == m_settings.indexScope || (scope != u"auto" && scope != u"drives" && scope != u"folders"))
        return;
    m_settings.indexScope = scope;
    commit(); // the app prunes the index to the folders, or reads the disks whole again (IndexService::setScope)
}

QVariantList SettingsEditor::drives() const
{
    QVariantList list;
    QSet<QString> present;
    const DWORD mask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i)))
            continue;
        const QString root = QString(QChar(char16_t(u'A' + i))) + u':';
        const UINT type = ::GetDriveTypeW((root + u'\\').toStdWString().c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
            continue;
        present.insert(root);
        const bool eligible = type == DRIVE_FIXED || m_settings.includeRemovableDrives;
        list.append(QVariantMap{{u"root"_s, root}, {u"selected"_s, allDrives() ? eligible : m_settings.selectedDrives->contains(root)},
            {u"enabled"_s, eligible}, {u"status"_s, eligible ? QString() : tr("先打开“包括 U 盘和移动硬盘”")}});
    }
    if (m_settings.selectedDrives) {
        for (const QString& root : *m_settings.selectedDrives) {
            if (!present.contains(root))
                list.append(QVariantMap{{u"root"_s, root}, {u"selected"_s, true}, {u"enabled"_s, true}, {u"status"_s, tr("未连接，选择会保留")}});
        }
    }
    return list;
}

void SettingsEditor::setAllDrives(bool all)
{
    if (all == allDrives())
        return;
    if (all) {
        m_settings.selectedDrives.reset();
    } else {
        QStringList selected;
        for (const QVariant& value : drives()) {
            const QVariantMap drive = value.toMap();
            if (drive.value(u"enabled"_s).toBool())
                selected.append(drive.value(u"root"_s).toString());
        }
        m_settings.selectedDrives = selected;
    }
    commit();
    emit foldersChanged();
}

void SettingsEditor::setDriveSelected(const QString& givenRoot, bool selected)
{
    const QString root = givenRoot.trimmed().toUpper();
    if (allDrives() || root.size() != 2 || root[0] < u'A' || root[0] > u'Z' || root[1] != u':')
        return;
    QStringList& chosen = *m_settings.selectedDrives;
    if (chosen.contains(root) == selected)
        return;
    if (selected)
        chosen.append(root);
    else
        chosen.removeAll(root);
    commit();
    emit foldersChanged();
}

void SettingsEditor::setFolderVerifyHours(int hours)
{
    hours = std::clamp(hours, 0, 24 * 7);
    if (hours == m_settings.folderVerifyHours)
        return;
    m_settings.folderVerifyHours = hours;
    commit();
}

QVariantList SettingsEditor::folders() const
{
    using State = IndexService::FolderStatus::State;
    const std::vector<std::wstring>& keys = folderKeys();
    const auto statusOf = [this](std::wstring_view key) {
        return std::ranges::find_if(
            m_folderStatuses, [&](const IndexService::FolderStatus& s) { return win32::equalsIgnoreCase(s.path, key); });
    };
    QVariantList list;
    for (qsizetype i = 0; i < m_settings.indexFolders.size(); ++i) {
        const QString path = displayPath(m_settings.indexFolders[i]);
        const std::wstring& key = keys[static_cast<std::size_t>(i)];
        // 桌面, 文档 …: as Windows calls its own folders; else the folder's name.
        QString name = path.size() > 3 ? QDir(QDir::fromNativeSeparators(path)).dirName() : path;
        if (const auto known = std::ranges::find_if(
                m_knownFolders, [&](const Settings::KnownFolder& k) { return k.path.compare(path, Qt::CaseInsensitive) == 0; });
            known != m_knownFolders.end())
            name = known->name;
        QString tone = u"idle"_s;
        QString status;
        bool busy = false;
        bool verifiable = false;
        const bool linked = hasDirectoryLink(key);
        const bool network = key.empty() && Settings::isNetworkPath(path);
        if (key.empty()) {
            tone = u"warn"_s;
            status = network ? tr("这是网络或 WSL 位置，请在“网络与 WSL”里添加") : tr("不是完整的文件夹路径，不会索引");
        } else if (m_settings.selectedDrives && !m_settings.selectedDrives->contains(QString::fromStdWString(key.substr(0, 2)))) {
            tone = u"warn"_s;
            status = tr("所在的磁盘未勾选，不会索引");
        } else if (m_indexAccessState.foldersInEffect || linked || statusOf(key) != m_folderStatuses.end()) {
            if (const auto it = statusOf(key); it == m_folderStatuses.end()) {
                // Inside another one: that one has it.
                const auto outer = std::ranges::find_if(m_folderStatuses, [&](const IndexService::FolderStatus& s) {
                    return !win32::equalsIgnoreCase(s.path, key) && folderHolds(s.path, key);
                });
                status = outer != m_folderStatuses.end() ? tr("已经包含在 %1 里").arg(QString::fromStdWString(outer->path))
                                                         : tr("稍后开始索引");
            } else {
                const IndexService::FolderStatus& s = *it;
                const QString walked
                    = s.lastVerified ? tr("上次核对 %1（用时 %2）").arg(sinceText(s.lastVerified), tookText(s.took)) : QString();
                verifiable = true;
                switch (s.state) {
                case State::Waiting:
                    status = s.lastVerified ? QStringList {itemsText(s.items), walked}.join(u" · "_s) : tr("稍后开始索引");
                    break;
                case State::Verifying:
                    tone = u"busy"_s;
                    busy = true;
                    status = s.lastVerified ? tr("正在核对… · %1").arg(itemsText(s.items)) : tr("正在建立索引…");
                    break;
                case State::Ready: {
                    tone = u"ok"_s;
                    QStringList parts {itemsText(s.items), walked};
                    if (s.failed > 0)
                        parts.push_back(tr("其中 %n 个文件夹读不了", "", static_cast<int>(s.failed)));
                    status = parts.join(u" · "_s);
                    break;
                }
                case State::NotFound:
                    tone = u"error"_s;
                    status = tr("找不到这个文件夹");
                    break;
                case State::Unreadable:
                    tone = u"error"_s;
                    status = tr("读不了（没有权限）");
                    break;
                case State::Excluded:
                    tone = u"warn"_s;
                    verifiable = false;
                    status = tr("在“不搜索的文件夹”里，不会索引");
                    break;
                case State::Offline:
                    verifiable = false;
                    status = tr("所在的磁盘现在不在，接上后再索引");
                    break;
                case State::WholeDrive:
                    tone = u"ok"_s;
                    verifiable = false;
                    status = tr("整个磁盘都索引");
                    break;
                }
                if (!s.watching && (s.state == State::Ready || s.state == State::Waiting))
                    status += u'\n' + tr("没有实时更新：改动等下次核对时补上");
            }
        } else {
            tone = u"ok"_s;
            status = tr("已包含在整盘索引中，无需重复扫描");
        }
        list.append(QVariantMap {
            {u"path"_s, path},
            {u"name"_s, name},
            {u"tone"_s, tone},
            {u"status"_s, status},
            {u"busy"_s, busy},
            {u"verifiable"_s, verifiable && !busy},
            {u"network"_s, network},
            {u"linked"_s, linked},
            {u"signIn"_s, false},
            {u"suspect"_s, false},
        });
    }
    return list;
}

QVariantList SettingsEditor::commonFolders() const
{
    const std::vector<std::wstring>& keys = folderKeys();
    QVariantList list;
    for (const Settings::KnownFolder& k : m_knownFolders) {
        if (k.network) {
            // On a server: offered for 网络共享, unless a share added has it.
            const QString share = Settings::sharePath(k.path);
            if (share.isEmpty() || std::ranges::any_of(m_settings.shares, [&](const NetworkShare& s) {
                    return unc::contains(s.path.toStdWString(), share.toStdWString())
                        || webdav::rootHolds(s.path.toStdWString(), share.toStdWString());
                }))
                continue;
        } else if (const std::wstring key = knownKey(k.path);
            std::ranges::any_of(keys, [&](const std::wstring& chosen) { return !chosen.empty() && folderHolds(chosen, key); })) {
            continue; // chosen, or in one that is
        }
        list.append(QVariantMap {{u"name"_s, k.name}, {u"path"_s, k.path}, {u"network"_s, k.network}});
    }
    return list;
}

void SettingsEditor::setFolderStatuses(std::vector<IndexService::FolderStatus> statuses)
{
    // The words of each are what counts ("10 分钟前" moves on by itself).
    const QVariantList before = folders();
    m_folderStatuses = std::move(statuses);
    if (folders() != before)
        emit foldersChanged();
}

void SettingsEditor::retranslateFolders()
{
    m_knownFolders = Settings::knownFolders(); // 桌面 or Desktop: translated as they are read
    emit foldersChanged();
}

QString SettingsEditor::addFolder(const QString& text)
{
    bool added = false;
    const QString problem = addFolderTo(m_settings, text, added);
    if (added)
        commit();
    return problem;
}

QString SettingsEditor::addFolderTo(Settings& settings, const QString& text, bool& added)
{
    added = false;
    const QString folder = folderText(text);
    if (folder.isEmpty())
        return {};
    if (Settings::isNetworkPath(folder))
        return tr("这是网络或 WSL 位置，请在“网络与 WSL”里添加");
    const std::wstring key = Settings::indexFolderPath(folder);
    if (key.empty())
        return tr("请选一个完整的文件夹路径");
    if (const QFileInfo info(folder); !info.isDir())
        return info.exists() ? tr("只能添加文件夹") : tr("找不到这个文件夹");
    std::vector<std::wstring> keys;
    for (const QString& chosen : settings.indexFolders)
        keys.push_back(Settings::indexFolderPath(chosen));
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].empty())
            continue;
        if (win32::equalsIgnoreCase(keys[i], key))
            return tr("已经添加过了");
        if (folderHolds(keys[i], key) && !hasDirectoryLink(key, keys[i]))
            return tr("已经包含在 %1 里").arg(displayPath(settings.indexFolders[static_cast<qsizetype>(i)]));
    }
    // One that holds folders added takes their place: none twice.
    QStringList kept;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].empty() || !folderHolds(key, keys[i]) || hasDirectoryLink(keys[i], key))
            kept.push_back(settings.indexFolders[static_cast<qsizetype>(i)]);
    }
    kept.push_back(folder);
    settings.indexFolders = kept;
    settings.indexFoldersChosen = true; // set now, as the file will have it (Settings::load)
    added = true;
    return {};
}

void SettingsEditor::removeFolder(int index)
{
    if (index < 0 || index >= m_settings.indexFolders.size())
        return;
    m_settings.indexFolders.removeAt(index);
    m_settings.indexFoldersChosen = true; // set now, as the file will have it (Settings::load)
    commit();
}

void SettingsEditor::verifyFolder(int index)
{
    if (index < 0 || index >= m_settings.indexFolders.size())
        return;
    const std::wstring& key = folderKeys()[static_cast<std::size_t>(index)];
    const auto it = std::ranges::find_if(
        m_folderStatuses, [&](const IndexService::FolderStatus& s) { return !key.empty() && win32::equalsIgnoreCase(s.path, key); });
    if (it != m_folderStatuses.end())
        emit folderVerifyRequested(QString::fromStdWString(it->path));
}

bool SettingsEditor::isNetworkFolder(const QString& text) const
{
    return Settings::isNetworkPath(folderText(text));
}

QString SettingsEditor::localPath(const QString& text) const
{
    return folderText(text);
}

QString SettingsEditor::pickFolder(const QString& title)
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    return shell::pickFolder(owner, title);
}

void SettingsEditor::listServerShares(const QString& server)
{
    const QString host = serverOf(u"\\\\"_s + server);
    if (host.isEmpty())
        return;
    // NetShareEnum waits long for a server that is not there: first a quick look.
    std::thread([guard = QPointer(this), host] {
        QStringList found;
        QString problem;
        bool signIn = false;
        if (unc::isWslHost(host.toStdWString())) {
            for (const auto& d : wsl::distributions())
                found.append(QString::fromStdWString(d.name));
            if (found.isEmpty())
                problem = tr("这台电脑还没有安装 WSL 发行版");
        } else if (!serverAnswers(host.toStdWString())) {
            problem = tr("连不上 %1（已关机、不在同一网络，或 VPN 没连）")
                          .arg(QString::fromStdWString(webdav::displayHost(host.toStdWString())));
        } else if (webdav::parseHost(host.toStdWString())) {
            // WebDAV has no list of shares (NetShareEnum is SMB's): the folder's own address, then.
            problem = tr("WebDAV 服务器列不出共享的文件夹，请直接粘贴文件夹的网址（http:// 或 https:// 开头）");
        } else {
            unsigned long error = 0;
            for (const std::wstring& name : share::listShares(host.toStdWString(), &error))
                found.append(QString::fromStdWString(name));
            if (found.isEmpty()) {
                const share::Problem p = share::classify(error, true);
                signIn = p == share::Problem::NeedsSignIn || p == share::Problem::Denied
                    || p == share::Problem::GuestBlocked;
                if (p == share::Problem::GuestBlocked || p == share::Problem::Signing || p == share::Problem::Smb1Only)
                    problem = problemText({.path = L"\\\\" + host.toStdWString(), .problem = p, .error = error});
                else
                    problem = signIn ? tr("%1 要先登录才能列出共享").arg(host)
                        : error  ? tr("%1 没有列出共享（错误 %2）；可以直接输入共享的路径").arg(host).arg(error)
                                 : tr("%1 上没有共享文件夹").arg(host);
            }
        }
        QMetaObject::invokeMethod(qApp, [guard, host, found, problem, signIn] {
            if (guard)
                emit guard->serverSharesListed(host, found, problem, signIn);
        });
    }).detach();
}

void SettingsEditor::signInToServer(const QString& server)
{
    const QString host = serverOf(u"\\\\"_s + server);
    if (!host.isEmpty())
        emit signInRequested(u"\\\\"_s + host);
}

void SettingsEditor::setSignedIn(const QString& path, const QString& problem)
{
    emit signedIn(path, problem);
}

bool SettingsEditor::isDefault() const
{
    Settings defaults = Settings::defaults();
    defaults.backupFolder = m_settings.backupFolder; // kept by restoreDefaults()
    // The known folders taken for good (Settings::chooseDefaultIndexFolders) are still the defaults.
    defaults.indexFoldersChosen = m_settings.indexFoldersChosen;
    return m_settings == defaults;
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
    const QString value = Settings::excludedFolderName(name);
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

bool SettingsEditor::addExcludedTypes(const QString& text)
{
    bool added = false;
    for (const QString& ext : Settings::fileExtensions(text)) {
        if (m_settings.excludedTypes.contains(ext))
            continue;
        m_settings.excludedTypes.append(ext);
        added = true;
    }
    if (added)
        commit();
    return added;
}

void SettingsEditor::removeExcludedType(int index)
{
    if (index < 0 || index >= m_settings.excludedTypes.size())
        return;
    m_settings.excludedTypes.removeAt(index);
    commit();
}

bool SettingsEditor::addExcludedFileNames(const QString& text)
{
    bool added = false;
    for (const QString& pattern : Settings::fileNamePatterns(text)) {
        if (m_settings.excludedFileNames.contains(pattern, Qt::CaseInsensitive))
            continue;
        m_settings.excludedFileNames.append(pattern);
        added = true;
    }
    if (added)
        commit();
    return added;
}

void SettingsEditor::removeExcludedFileName(int index)
{
    if (index < 0 || index >= m_settings.excludedFileNames.size())
        return;
    m_settings.excludedFileNames.removeAt(index);
    commit();
}

bool SettingsEditor::addContentExtensions(const QString& text)
{
    bool added = false;
    for (const QString& ext : Settings::contentExtensionList(text)) {
        if (containsIgnoreCase(m_settings.contentExtensions, ext))
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
    // Backups go on: the settings before this are in them.
    const QString backupFolder = m_settings.backupFolder;
    m_settings = Settings::defaults();
    m_settings.backupFolder = backupFolder;
    commit();
}

void SettingsEditor::openDataFolder()
{
    shell::open(Settings::dataDir());
}

void SettingsEditor::exportDiagnostics()
{
    if (m_diagnosticsBusy)
        return;
    m_diagnosticsBusy = true;
    m_diagnosticsStatus.clear();
    emit diagnosticsChanged();
    emit diagnosticsExportRequested();
}

void SettingsEditor::finishDiagnosticsExport(const QString& error)
{
    m_diagnosticsBusy = false;
    m_diagnosticsStatus = error.isEmpty() ? tr("诊断包已生成，把 WinShun-diagnostics.zip 发给开发者即可") : error;
    emit diagnosticsChanged();
}

bool SettingsEditor::chooseBackupFolder()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString folder = shell::pickFolder(owner, tr("选择备份文件夹（可以选同步盘里的文件夹）"));
    if (!folder.isEmpty() && folder != m_settings.backupFolder) {
        m_settings.backupFolder = folder;
        commit();
    }
    return !m_settings.backupFolder.isEmpty();
}

void SettingsEditor::stopBackups()
{
    if (m_settings.backupFolder.isEmpty())
        return;
    m_settings.backupFolder.clear();
    commit();
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

// ---- AI 助手 -----------------------------------------------------------------

void SettingsEditor::setMcp(bool on)
{
    if (m_settings.mcp == on)
        return;
    m_settings.mcp = on;
    commit();
}

void SettingsEditor::setMcpAllowOpen(bool on)
{
    if (m_settings.mcpAllowOpen == on)
        return;
    m_settings.mcpAllowOpen = on;
    commit();
}

void SettingsEditor::setMcpClipboard(bool on)
{
    if (m_settings.mcpClipboard == on)
        return;
    m_settings.mcpClipboard = on;
    commit();
}

void SettingsEditor::setMcpScreenshot(bool on)
{
    if (m_settings.mcpScreenshot == on)
        return;
    m_settings.mcpScreenshot = on;
    commit();
}

void SettingsEditor::setMcpPlugins(bool on)
{
    if (m_settings.mcpPlugins == on)
        return;
    m_settings.mcpPlugins = on;
    commit();
}

void SettingsEditor::setMcpSettings(const QString& mode)
{
    if (m_settings.mcpSettings == mode || (mode != u"ask" && mode != u"auto" && mode != u"off"))
        return;
    m_settings.mcpSettings = mode;
    commit();
}

void SettingsEditor::setMcpPluginActions(bool on)
{
    if (m_settings.mcpPluginActions == on)
        return;
    m_settings.mcpPluginActions = on;
    commit();
}

QStringList SettingsEditor::mcpExcludedPaths() const
{
    QStringList out;
    for (const QString& path : m_settings.mcpExcludedPaths)
        out.push_back(displayPath(path));
    return out;
}

void SettingsEditor::addMcpExcludedFolder()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString folder = shell::pickFolder(owner, tr("选择不让 AI 看到的文件夹"));
    if (folder.isEmpty() || containsIgnoreCase(mcpExcludedPaths(), displayPath(folder)))
        return;
    m_settings.mcpExcludedPaths.append(folder);
    commit();
}

void SettingsEditor::removeMcpExcludedPath(int index)
{
    if (index < 0 || index >= m_settings.mcpExcludedPaths.size())
        return;
    m_settings.mcpExcludedPaths.removeAt(index);
    commit();
}

void SettingsEditor::setMcpStatus(const QString& status)
{
    if (m_mcpStatus == status)
        return;
    m_mcpStatus = status;
    emit mcpStatusChanged();
}

void SettingsEditor::refreshMcpClients()
{
    m_mcpClients = mcpclients::toVariant(mcpclients::detect());
    m_mcpRelayPath = mcpclients::relayPath();
    emit mcpClientsChanged();
}

void SettingsEditor::connectMcpClient(const QString& id)
{
    // Connecting a client is wanting the connection: the switch goes on with it.
    if (!m_settings.mcp) {
        m_settings.mcp = true;
        commit();
    }
    setMcpMessage(tr("正在连接…"));
    mcpclients::connect(id, this, [this](const mcpclients::Result& result) {
        setMcpMessage(result.message);
        refreshMcpClients();
    });
}

void SettingsEditor::copyMcpSnippet(const QString& id)
{
    shell::copyText(mcpclients::snippet(id));
    setMcpMessage(id == u"claude-code" ? tr("已复制命令，在终端里运行它就行") : tr("已复制，粘贴到那个客户端的 MCP 配置文件里"));
}

void SettingsEditor::setMcpMessage(const QString& message)
{
    if (m_mcpMessage == message)
        return;
    m_mcpMessage = message;
    emit mcpMessageChanged();
}

void SettingsEditor::openMcpLog()
{
    const QString log = Settings::dataDir() + u"\\mcp.log"_s;
    shell::open(QFileInfo::exists(log) ? log : Settings::dataDir());
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
