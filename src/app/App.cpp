#include "App.h"
#include "AdminTapHint.h"
#include "WebSearchProvider.h"
#include "platform/OcrFiles.h"
#include "platform/OcrPackage.h"

#include "AiBridge.h"
#include "AppCatalog.h"
#include "Backup.h"
#include "Grabber.h" // take_screenshot (MCP)
#include "CliSetup.h"
#include "ScoopInstall.h"
#include "ClipImportJob.h"
#include "ClipStore.h"
#include "Clipboard.h"
#include "ClipboardDisplay.h"
#include "DialogBar.h"
#include "Diagnostics.h"
#include "DisplayDiagnostics.h"
#include "Favorites.h"
#include "FavoritesSettings.h"
#include "FileIconProvider.h"
#include "FilePreview.h"
#include "ThumbnailProvider.h"
#include "ClipThumbnailProvider.h"
#include "DropPie.h"
#include "DropPieHelper.h"
#include "FolderMenu.h"
#include "History.h"
#include "Host.h"
#include "IndexFolder.h"
#include "IndexService.h"
#include "Launcher.h"
#include "McpServer.h"
#include "McpTools.h"
#include "MissingFiles.h"
#include "Onboarding.h"
#include "MovedPlugins.h"
#include "Pipe.h"
#include "Placement.h"
#include "PluginSettings.h"
#include "PluginUpdates.h"
#include "ProxyAddress.h"
#include "SavedFile.h"
#include "SearchEngine.h"
#include "Secrets.h"
#include "ServiceVolumeAccess.h"
#include "ShortcutAgain.h"
#include "SettingsCommand.h"
#include "SettingsEditor.h"
#include "Switches.h"
#include "SystemCatalog.h"
#include "SystemTheme.h"
#include "TextField.h"
#include "Tools.h"
#include "TrayMenu.h"
#include "TriggerJournal.h"
#include "Unc.h"
#include "Updater.h"
#include "WebDav.h"
#include "WindowFrame.h"
#include "platform/ClipboardWatcher.h"
#include "platform/ComWorker.h"
#include "platform/Credentials.h"
#include "platform/DialogJump.h"
#include "platform/ExternalPreview.h"
#include "platform/FileManagers.h"
#include "platform/Foreground.h"
#include "platform/Http.h"
#include "platform/IdleTrim.h"
#include "platform/MemoryReport.h"
#include "BackgroundStatus.h"
#include "HealthMonitor.h"
#include "ActivityText.h"
#include "platform/Integrity.h"
#include "platform/KeyListener.h"
#include "platform/MessageWindow.h"
#include "platform/PluginHotkeys.h"
#include "platform/NetworkNotifier.h"
#include "platform/Paster.h"
#include "platform/PointerWatch.h"
#include "platform/ServiceDialogNav.h"
#include "platform/ServiceKeyListener.h"
#include "platform/Shell.h"
#include "platform/ShellContextMenu.h"
#include "platform/ShellMenu.h"
#include "platform/SiteIcons.h"
#include "SwitchPlugin.h"
#include "platform/TypeToSearch.h"
#include "platform/FolderViewHit.h"
#include "platform/TaskbarSearch.h"
#include "platform/TextCaret.h"
#include "platform/VolumeNotifier.h"
#include "platform/WinV.h"
#include "platform/WindowEffects.h"
#include "PrintScreen.h" // restorePrintScreen
#include "platform/HotkeyText.h"

#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QOperatingSystemVersion>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QVersionNumber>
#include <QGuiApplication>
#include <QInputMethod>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QStorageInfo>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <QUrl>
#include <QWinEventNotifier>

#include <windows.h>
#include <dwmapi.h>

#include <cmath>
#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

bool localSendNeedsUpdate(const plugin::Catalog& catalog)
{
    const auto* entry = catalog.find(u"winshun.localsend"_s);
    return entry && entry->manifest && QVersionNumber::fromString(entry->manifest->version) < QVersionNumber(1, 0, 1);
}

constexpr int kHotkeyId = 1;
constexpr int kDialogJumpHotkeyId = 2; // Ctrl+G, while a file dialog is in front
constexpr int kWinVHotkeyId = 3; // Win+V: the clipboard history, once Explorer gave it up (winv::)
constexpr int kClipboardHotkeyId = 4; // another shortcut for the clipboard history
constexpr int kWinSHotkeyId = 5; // Win+S: the launcher over the taskbar, once Explorer gave it up (winv::)
constexpr int kScreenClipHotkeyId = 6; // Win+Shift+S, which Explorer gives up with Win+S: the screen capture
constexpr int kFolderMenuHotkeyId = 7; // the folder menu in the window in front (GitHub #25)
constexpr int kQuickPasteHotkeyId = 20; // + 1…9: paste without opening the clipboard
// For another program to tell where its text caret is: longer, and the
// clipboard would open late; it opens by the mouse pointer then.
constexpr auto kCaretWait = 100ms;
constexpr DWORD kSlowMenuMs = 100; // a tray menu slower than this to appear is logged

// The screen a monitor is, as Qt knows it; the primary when none is.
QScreen* screenOf(HMONITOR monitor)
{
    for (QScreen* screen : QGuiApplication::screens()) {
        const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor)
            return screen;
    }
    return QGuiApplication::primaryScreen();
}

// The favorites' keywords (keyword, name), which the plugins' give way to.
QList<std::pair<QString, QString>> favoriteKeywords(const Favorites& favorites)
{
    QList<std::pair<QString, QString>> keywords;
    for (const Favorite& f : favorites.items()) {
        if (!f.keyword.isEmpty())
            keywords.append({f.keyword, f.name});
    }
    return keywords;
}

enum TrayCommand {
    ShowCommand = 1,
    IndexInfo,
    AccessInfo, // the index service wanted and not reached: walking meanwhile
    ContentInfo,
    RebuildCommand,
    AutostartCommand,
    SettingsCommand,
    UpdateCommand,
    CheckUpdateCommand,
    RestartCommand,
    QuitCommand,
    ClipboardCommand,
    PauseClipboardCommand,
    KeepAwakeCommand, // 保持唤醒（到 16:42）: only while it is on; ends it
    PluginUpdatesCommand, // N plugins have a newer version
    PluginMenuCommand = 1000 // and up: the entries of the plugins' submenus (trayMenu)
};

// `folders`: only the chosen ones are indexed (App::foldersInEffect, GitHub #52).
IndexService::Options indexOptions(const Settings& settings, bool folders)
{
    IndexService::Options options;
    options.rules = settings.crawlRules();
    options.includeRemovable = settings.includeRemovableDrives;
    if (settings.selectedDrives) {
        options.selectedDrives.emplace();
        for (const QString& root : *settings.selectedDrives)
            options.selectedDrives->push_back(root.toStdWString());
    }
    options.rescanOnStartup = settings.rescanOnStartup;
    options.shares = settings.shareOptions();
    // The local disks' contents ([Content] Index), and each share's on its own
    // (GitHub #114). Neither: the index is cleared.
    const bool shareContent = std::ranges::any_of(options.shares.shares, &ShareConfig::content);
    options.content.enabled = settings.contentSearch && (settings.contentIndex || shareContent);
    options.content.local = settings.contentIndex;
    options.content.extensions = settings.contentExtensions;
    options.content.sizeLimits = settings.contentSizeLimits();
    options.content.includeLowPriority = settings.contentInLowPriority;
    options.content.documents = settings.contentDocuments;
    if (folders)
        options.folders = settings.indexFolderPaths();
    else
        options.extraFolders = settings.indexFolderPaths();
    options.folderVerifyEvery = std::chrono::hours(settings.folderVerifyHours);
    return options;
}

// The index service (GitHub #21) comes up a moment after the user's Win顺 at
// login: the index waits this long for it (IndexService::Options::accessWaitMs)
// rather than walk a volume whose journal position it has, looking every
// kServiceRetry meanwhile, and every kServiceRecheck after that (the service
// installed later, stopped and started again).
constexpr auto kServiceWait = 30s;
constexpr auto kServiceRetry = 2s;
constexpr auto kServiceRecheck = 5min;
// A program run as administrator stays in front this long before it is told
// of (AdminTapHint): Alt+Tab past Task Manager says nothing.
constexpr auto kAdminTapDwell = 1500ms;
// The service's UIAccess listener answers within this of the key listener's
// start; until then a service that runs is waited for, not told of as missing.
constexpr qint64 kAdminTapSettleMs = 30000;

// Where the index reads NTFS volumes from (Settings::indexAccess, GitHub
// #21): the volumes themselves when running as administrator (unless told
// service or walk), else the index service (WinShunService.exe) when it
// answers, else nowhere: the volumes are walked and watched. `wantsService`:
// the service would be used, so it is looked for again while it is not
// (App::startServiceRetry); a portable copy too, for one installed from its
// settings. WINSHUN_PIPE names another pipe, for tests with
// `WinShunService.exe --console --pipe NAME`.
struct AccessChoice {
    std::shared_ptr<ntfs::VolumeAccess> access;
    bool wantsService = false;
    QString why; // the service wanted and not used: why (English, from the pipe client)
    ntfs::ServiceState state = ntfs::ServiceState::Unknown; // the SCM's word, when the service is wanted
};

// Not installed, disabled or stopped: nothing listens on the pipe, so it is
// not asked (WINSHUN_SERVICE pretends one of these on a computer that has it).
bool serviceAbsent(ntfs::ServiceState state)
{
    return state == ntfs::ServiceState::NotInstalled || state == ntfs::ServiceState::Disabled
        || state == ntfs::ServiceState::Stopped;
}

// Never there (or switched off by someone): no waiting for it at start, and
// looked for again only now and then.
bool serviceLongAbsent(ntfs::ServiceState state)
{
    return state == ntfs::ServiceState::NotInstalled || state == ntfs::ServiceState::Disabled;
}

const QString kServiceNotRunning = QStringLiteral("the index service is not running");

AccessChoice chooseVolumeAccess(const QString& mode)
{
    AccessChoice choice;
    const bool elevated = integrity::processElevated();
    if (elevated && mode != u"service" && mode != u"walk") {
        choice.access = std::make_shared<ntfs::DirectVolumeAccess>();
        return choice;
    }
    if (mode == u"walk") {
        choice.access = std::make_shared<ntfs::NoVolumeAccess>();
        return choice;
    }
    // auto and service; direct too while not running as administrator (the
    // settings offer the restart), rather than nothing.
    choice.wantsService = true;
    choice.state = ntfs::ServiceVolumeAccess::state();
    auto service = std::make_shared<ntfs::ServiceVolumeAccess>();
    if (serviceAbsent(choice.state)) {
        choice.why = kServiceNotRunning;
        choice.access = std::make_shared<ntfs::NoVolumeAccess>();
        return choice;
    }
    if (service->probe()) {
        choice.access = std::move(service);
        return choice;
    }
    choice.why = QString::fromStdWString(service->lastError());
    qWarning("WinShun: the index service is not used: %ls (pipe %ls)", service->lastError().c_str(),
        service->pipeName().c_str());
    choice.access = std::make_shared<ntfs::NoVolumeAccess>();
    return choice;
}

// '/' separated and cleaned, as IndexService keeps it.
QString folderPath(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool sameFolder(const QString& a, const QString& b)
{
    return folderPath(a).compare(folderPath(b), Qt::CaseInsensitive) == 0;
}

// "D:\" for "D:/...", else nothing.
QString driveRoot(const QString& folder)
{
    if (folder.size() < 3 || !folder[0].isLetter() || folder[1] != u':' || folder[2] != u'/')
        return {};
    return QDir::toNativeSeparators(folder.left(3));
}

UINT driveType(const QString& folder)
{
    const QString root = driveRoot(folder);
    return root.isEmpty() ? DRIVE_UNKNOWN : ::GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
}

bool isDarkMode()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

// Before create(): see-through where the window's background is (Theme.qml),
// for the Mica behind it. And it stays so: the window's color turns opaque
// while it is inactive, QQuickWindow::setColor() then takes the alpha out of
// the format, though the swap chain made with it keeps its alpha, and Qt's
// D3D11 backend logged a warning about that on every resize after.
void prepareBackdrop(QQuickWindow* window)
{
    if (!SystemTheme::backdropAvailable()) // the setting can turn it on later
        return;
    const auto keepAlpha = [window] {
        QSurfaceFormat format = window->requestedFormat();
        if (format.alphaBufferSize() < 8) {
            format.setAlphaBufferSize(8);
            window->setFormat(format);
        }
    };
    keepAlpha();
    QObject::connect(window, &QQuickWindow::colorChanged, window, keepAlpha);
}

// What is remembered between runs that is not a setting: the windows'
// spots and sizes, the launcher's last choices.
QString statePath()
{
    return Settings::dataDir() + u"\\state.ini"_s;
}

// Rounded corners and the shadow, for our windows without a system title bar
// (the launcher, the clipboard, the bar by file dialogs, the settings
// window). Windows 11 draws their borders; WindowEdge.qml is the Windows 10 fallback.
void styleWindow(QWindow* window, bool backdrop = false)
{
    win::styleFramelessWindow(window, backdrop);
}

App* g_launcherApp = nullptr; // for foregroundChanged, a hook's callback

} // namespace

App::App()
{
    m_settingsReload.setSingleShot(true);
    m_settingsReload.setInterval(400ms); // editors often write a file in several steps
    connect(&m_settingsReload, &QTimer::timeout, this, &App::reloadSettings);
    m_indexOptionsApply.setSingleShot(true);
    m_indexOptionsApply.setInterval(1s);
    connect(&m_indexOptionsApply, &QTimer::timeout, this, [this] {
        // 索引哪些地方 and the folders are settings too: no telling the user what they chose.
        m_foldersInEffect = foldersInEffect(m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None);
        if (m_foldersInEffect)
            takeDefaultIndexFolders();
        m_index->setOptions(indexOptions(m_settings, m_foldersInEffect));
        tellLauncherFolders();
        refreshIndexAccess();
        refreshFolderStatuses();
    });
    m_darkFrameGuard.setInterval(10ms);
    connect(&m_darkFrameGuard, &QTimer::timeout, this, [this] {
        if (m_darkFrameWatch.elapsed() < 50) // Qt's own, after some 5 ms, first
            return;
        // Qt sets every window's, so every native window here: the notices,
        // panels and menus too, shown or not.
        for (QWindow* window : QGuiApplication::topLevelWindows()) {
            if (window->handle() && win::isDarkFrame(window) != m_darkFrame)
                win::setDarkFrame(window, m_darkFrame);
        }
        if (m_darkFrameWatch.elapsed() > 1000)
            m_darkFrameGuard.stop();
    });
    m_holdTimer.setInterval(std::chrono::milliseconds(hotkeysteps::kPollMs));
    connect(&m_holdTimer, &QTimer::timeout, this, &App::checkClipboardHold);
    m_cliTallyTimer.setSingleShot(true);
    m_cliTallyTimer.setInterval(500ms);
    connect(&m_cliTallyTimer, &QTimer::timeout, this, &App::refreshCliTally);
    m_contentStatusTimer.setInterval(2s);
    connect(&m_contentStatusTimer, &QTimer::timeout, this, &App::refreshContentIndexStatus);
    connect(&m_contentStatusTimer, &QTimer::timeout, this, &App::refreshShareStatuses); // counts while reading
    connect(&m_contentStatusTimer, &QTimer::timeout, this, &App::refreshFolderStatuses); // "10 分钟前" moves on
    m_serviceRetry.setSingleShot(true); // serviceProbed() starts the next
    connect(&m_serviceRetry, &QTimer::timeout, this, &App::probeService);
}

App::~App()
{
    // Hidden first, while everything it reaches is still there: its reset
    // (SettingsWindow.resetForReopen) then, never from inside its destructor.
    if (m_settingsWindow)
        m_settingsWindow->hide();
    m_clipDisplay.reset();
    delete m_onboardingWindow.data();
    m_onboardingQml.reset();
    m_onboarding.reset();
    m_onboardingEditor.reset();
    m_onboardingFrame.reset();
    if (m_launcher)
        m_launcher->setSwitches(nullptr, {});
    m_switches.reset();
    m_switchPlugin.reset();
    cancelTypingSelection();
    m_typingWorker.reset();
    if (m_launcherFrontHook)
        ::UnhookWinEvent(m_launcherFrontHook);
    g_launcherApp = nullptr; // a change of the front queued already finds no one
    m_clipImport.reset(); // its thread joined before the history it fills goes
    siteicons::shutdown(); // an icon arriving now would be told to no one
    m_keyRouter.reset(); // no keys taken from here on
    m_launcherRouting.router.reset();
    m_numberKeyHook.reset();
    m_drainingNumberKeyHook.reset();
    m_drainingRouter.reset();
    m_launcherRouting.draining.reset();
    m_holdWatch.reset();
    m_drainingWatch.reset();
    m_keyListenerMode = -1; // recorder teardown must not recreate input listeners
    m_serviceKeyListener.reset();
    m_keyListener.reset(); // no double click told to the folder menu
    // DialogJump's worker may be asking m_serviceNav about an elevated dialog,
    // and its hooks reach the bar: it goes while both are here (joined), the
    // bar and then the navigator at the end, as applyDialogs does.
    m_dialogJump.reset();
    if (m_screenshotAsk && m_screenshotAsk->window)
        delete m_screenshotAsk->window.data(); // before the QML engine, and before its frame
    m_screenshotAsk.reset();
    if (m_settingsAsk && m_settingsAsk->window)
        delete m_settingsAsk->window.data(); // likewise
    m_settingsAsk.reset();
    // The AI's host asks the app (to close that card) while it goes: it goes
    // before the members the app's own order would take first.
    m_aiBridge.reset();
    m_mcpServer.reset();
    m_mcpHost.reset();
    m_mcpEngine.reset();
    m_mcpContentEngine.reset();
    delete m_noticeWindow.data();
    m_noticeFrame.reset();
    delete m_folderMenuWindow; // before its view-model and the QML engine
    m_folderMenu.reset();
    delete m_dropPieWindow; // likewise
    m_dropPie.reset();
    delete m_settingsWindow; // before the QML engine it was created with
    delete m_barWindow;
    delete m_clipWindow;
    m_dialogBar.reset(); // after its window
    m_serviceNav.reset(); // after the two that use it (members would go the other way round)
}

bool App::start(const StartOptions& options)
{
    m_clipDisplayProbe = options.clipboardDisplayProbe;
    m_settingsProbe = options.settingsProbe;
    ocr::setComponentsRoot(Settings::dataDir() + u"/components"_s);
    const bool firstRun = !Settings::exists();
    // Updated from a version without the clipboard history: tell about it once.
    const bool clipboardIsNew = !firstRun && !Settings::hasClipboardSettings();
    m_settings.load();
    applyAutostartLevel(); // may set the mode (an old elevated task): before the index is made
    applyAppearance(); // before any window: the first frame is already in the chosen theme and language
    m_ocrPackage = std::make_unique<OcrPackage>();
    m_renderer = Settings::resolveRenderer(m_settings.renderer);
    if (m_renderer == u"software")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    else if (m_renderer == u"d3d11")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    // The software renderer draws a window with an alpha channel as a layered
    // window, which DWM puts no backdrop behind.
    SystemTheme::setBackdropAvailable(m_renderer == u"d3d11" && win::backdropSupported());
    if (SystemTheme::backdropAvailable()) {
        // Grey-scale text, not ClearType: a window see-through to Mica has
        // alpha 0 under its text, and subpixel glyphs blended there come out
        // with colour fringes, blue and orange the wrong way round (GitHub
        // #12). Qt draws them only where the ClearType tuner was run
        // (HKLM Avalon.Graphics\DISPLAY1 PixelStructure). For every window
        // and from the start: the transparency can be turned on at any
        // time, and the text items copy the application font when made.
        QFont font = QGuiApplication::font();
        font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::NoSubpixelAntialias));
        QGuiApplication::setFont(font);
    }

    if (!m_settings.onboardingComplete)
        return showOnboarding(options);
    return startRuntime(options, firstRun, clipboardIsNew);
}

bool App::showOnboarding(const StartOptions& options)
{
    m_onboarding = std::make_unique<Onboarding>(m_settings);
    m_onboardingFrame = std::make_unique<WindowFrame>(true);
    m_onboardingQml = std::make_unique<QQmlApplicationEngine>();
    m_onboardingQml->addImageProvider(u"fileicon"_s, new FileIconProvider);
    m_onboardingQml->setInitialProperties({
        {u"setup"_s, QVariant::fromValue(m_onboarding.get())},
        {u"frame"_s, QVariant::fromValue(m_onboardingFrame.get())},
    });
    m_onboardingQml->loadFromModule(u"WinShun"_s, u"OnboardingWindow"_s);
    const auto roots = m_onboardingQml->rootObjects();
    if (roots.isEmpty())
        return false;
    auto* window = qobject_cast<QQuickWindow*>(roots.constFirst());
    if (!window)
        return false;
    m_onboardingWindow = window;
    window->setObjectName(u"onboardingWindow"_s);
    m_onboardingFrame->setWindow(window);
    m_onboardingFrame->setResizable(true);
    m_onboarding->setWindow(window);
    styleWindow(window, SystemTheme::backdropAvailable());
    win::setDarkFrame(window, isDarkMode());
    const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
    window->resize(std::min(window->width(), area.width() - 32), std::min(window->height(), area.height() - 32));
    window->setPosition(area.center() - QPoint(window->width() / 2, window->height() / 2));
    connect(m_onboarding.get(), &Onboarding::accepted, this, [this, options] {
        m_settings = m_onboarding->settings();
        StartOptions quiet;
        quiet.background = true;
        if (!startRuntime(quiet, true, false, true)) {
            QCoreApplication::exit(1);
            return;
        }
        if (!m_settings.onboardingCapturePending) {
            finishOnboarding(options);
            return;
        }
        m_onboardingEditor = std::make_unique<SettingsEditor>(m_settings, m_renderer);
        m_onboardingEditor->setWindow(m_onboardingWindow);
        connect(m_onboardingEditor.get(), &SettingsEditor::edited, this, &App::settingsEdited);
        auto* plugins = m_onboardingEditor->plugins();
        plugins->setLocale(m_language);
        plugins->setHost(m_pluginHost.get());
        m_onboarding->attachPlugins(plugins, m_pluginHost.get());
    });
    connect(m_onboarding.get(), &Onboarding::captureActivated, this, [this] {
        m_settings.onboardingCapturePending = false;
        m_settings.save();
        if (m_onboardingEditor)
            m_onboardingEditor->setSettings(m_settings);
    });
    connect(m_onboarding.get(), &Onboarding::finished, this, [this, options] { finishOnboarding(options); });
    window->show();
    window->raise();
    win::bringToFront(window);
    window->requestActivate();
    return true;
}

void App::finishOnboarding(const StartOptions& options)
{
    // apply()/finish() can be on the QML call stack. Destroy its window and
    // model on the next turn, after all signals and permission callbacks.
    QTimer::singleShot(0, this, [this, options] {
        if (!m_onboarding)
            return;
        delete m_onboardingWindow.data();
        m_onboardingQml.reset();
        m_onboarding.reset();
        m_onboardingEditor.reset();
        m_onboardingFrame.reset();
        if (options.settings) {
            showSettings([&options](QQuickWindow* window) {
                if (!options.settingsAt.isEmpty())
                    QMetaObject::invokeMethod(window, "showPlace", Q_ARG(QVariant, QVariant(options.settingsAt)));
            });
        } else {
            // Finishing a visible welcome screen is an explicit interaction,
            // including an installer or login start that originally used --background.
            showLauncher(options.query);
        }
    });
}

bool App::startRuntime(const StartOptions& options, bool firstRun, bool clipboardIsNew, bool fromOnboarding)
{
    const QString dataDir = Settings::dataDir();
    QDir().mkpath(dataDir);
    m_history = std::make_unique<History>(dataDir + u"\\history.txt"_s);
    m_history->load();
    // The dialog bar's pinned folders of old become favorites, once.
    m_favorites = std::make_unique<Favorites>(dataDir + u"\\favorites.json"_s, dataDir + u"\\dialog-pins.txt"_s);
    m_favorites->load();
    connect(m_favorites.get(), &Favorites::changed, this, [this] {
        if (m_pluginHost)
            m_pluginHost->setFavoriteKeywords(favoriteKeywords(*m_favorites));
        applyScreenshots(); // a favorite given the word takes it
    });
    // Sites' icons: fetched when a row of theirs is first shown, not before.
    siteicons::setFolder(dataDir + u"\\icons\\web"_s);
    IndexService::Options indexStart;
    {
        AccessChoice choice = chooseVolumeAccess(m_settings.indexAccess);
        m_serviceWanted = choice.wantsService;
        m_serviceWhy = choice.why;
        m_serviceState = choice.state;
        m_foldersInEffect = foldersInEffect(choice.access->kind() == ntfs::VolumeAccess::Kind::None);
        if (m_foldersInEffect)
            takeDefaultIndexFolders(); // before the settings window and the backups are there
        indexStart = indexOptions(m_settings, m_foldersInEffect);
        // On its way, maybe; not when it is not installed or switched off.
        if (m_serviceWanted && choice.access->kind() == ntfs::VolumeAccess::Kind::None && !serviceLongAbsent(choice.state))
            indexStart.accessWaitMs = static_cast<int>(std::chrono::milliseconds(kServiceWait).count());
        indexStart.volumeAccess = std::move(choice.access);
    }
    m_index = std::make_unique<IndexService>(startIndexFolder(), std::move(indexStart));
    connect(m_index.get(), &IndexService::moveProgress, this, [this](int percent) {
        m_indexMoveProgress = percent;
        if (m_background)
            m_background->setMovePercent(percent);
        refreshIndexFolder();
    });
    connect(m_index.get(), &IndexService::moveFinished, this, &App::indexMoved);
    // Files still in the default place go where the settings say, once read.
    connect(m_index.get(), &IndexService::stateChanged, this, &App::applyIndexFolder);
    connect(m_index.get(), &IndexService::sharesChanged, this, &App::refreshShareStatuses);
    connect(m_index.get(), &IndexService::foldersChanged, this, &App::refreshFolderStatuses);
    // Shares that could not be reached are tried again when the network changes.
    m_network = std::make_unique<NetworkNotifier>([index = m_index.get()] { index->shares().networkChanged(); });
    m_idleTrim = std::make_unique<IdleTrim>([this]() -> const char* {
        // A recording: its media thread and its keycaps' low-level hooks
        // touch their pages all the time. Emptied, the next hook call can
        // wait on hard faults past Windows' timeout, and the hook is taken
        // out (GitHub #98).
        if (!m_index || m_index->state() != IndexService::State::Ready)
            return "loading";
        if (m_index->isRefreshing())
            return "catching up";
        if (m_index->readingContent())
            return "reading content";
        if (m_index->moving())
            return "moving";
        return nullptr;
    }, [this] {
        // Up and in use without the focus: the clipboard (the keys routed to
        // it, the focus left where it was), a pinned launcher, a file dialog's bar.
        return (m_clipWindow && m_clipWindow->isVisible() && !m_clipPrewarming.load())
            || (m_launcher && m_launcher->pinned() && launcherShown()) || (m_dialogBar && m_dialogBar->isShown());
    });
    // Loaded, caught up, done reading: emptied now, not at the next recheck.
    connect(m_index.get(), &IndexService::stateChanged, m_idleTrim.get(), &IdleTrim::poke);
    m_apps = std::make_unique<AppCatalog>();
    m_places = std::make_unique<SystemCatalog>();
    m_engine = std::make_unique<SearchEngine>(m_index.get(), m_apps.get(), m_places.get());
    m_launcher
        = std::make_unique<Launcher>(m_index.get(), m_apps.get(), m_places.get(), m_engine.get(), m_history.get());
    m_launcher->setFavorites(m_favorites.get());
    m_launcher->setSwitches(nullptr, statePath());
    m_preview = std::make_unique<FilePreview>();
    m_preview->setWindowFactory([this] { return createPreviewWindow(); });
    m_launcher->setPreview(m_preview.get());
    // Done with it (a result opened, a command run): hidden, unless pinned.
    connect(m_launcher.get(), &Launcher::dismissRequested, this, [this] {
        if (!m_launcher->pinned())
            hideLauncher();
    });
    // Opened by typing in an Explorer window, Esc hands the keyboard back to
    // it (allowed: the launcher is in front), as closing a menu would; not a
    // result opened, whose program is coming to the front instead. Esc hides
    // it, pinned or not (dismissRequested follows, for the rest).
    connect(m_launcher.get(), &Launcher::escaped, this, [this] {
        cancelTypingSelection();
        const HWND back = std::exchange(m_typingOrigin, {}).root;
        if (back && ::IsWindow(back) && m_window && m_window->isActive())
            ::SetForegroundWindow(back);
        hideLauncher();
    });
    // ... and a folder opened goes to that tab, which comes back to the
    // front the same way (GitHub #39).
    m_launcher->setFolderOpener([this](const QString& folder) { return openInTypingTab(folder); });
    m_launcher->setRevealer([this](const QString& path) { return revealInTypingTab(path); });
    m_typingSelectionTimer.setSingleShot(true);
    m_typingSelectionTimer.setInterval(60ms);
    connect(m_launcher.get(), &Launcher::currentRowChanged, this, &App::followTypingSelection);
    // Another query can replace the current file without changing its row or the result count (#182).
    connect(m_launcher.get(), &Launcher::resultsReplaced, this, &App::followTypingSelection);
    connect(m_launcher->results(), &ResultModel::countChanged, this, &App::followTypingSelection);
    connect(&m_typingSelectionTimer, &QTimer::timeout, this, [this] {
        const auto* row = m_launcher->results()->at(m_launcher->currentRow());
        if (!row || row->isApp() || row->elsewhere || !m_typingOrigin.tab || m_typingOrigin.opus || !launcherShown())
            return;
        const auto origin = m_typingOrigin;
        const auto stop = m_typingSelection.get_token();
        const HWND window = reinterpret_cast<HWND>(m_window->winId());
        const auto current = [stop, window, origin] {
            return !stop.stop_requested() && ::GetForegroundWindow() == window && ::IsWindowVisible(window)
                && ::IsWindow(origin.tab) && ::GetAncestor(origin.tab, GA_ROOT) == origin.root;
        };
        if (!current())
            return;
        if (!m_typingWorker)
            m_typingWorker = std::make_unique<ComWorker>();
        m_typingWorker->post([origin, path = row->path.toStdWString(), current] {
            filemanager::selectInExplorer(origin.tab, origin.folder.toStdWString(), path, current);
        });
    });
    connect(m_launcher.get(), &Launcher::pinnedChanged, this, &App::launcherPinnedChanged);
    connect(m_launcher.get(), &Launcher::pinRequested, this,
        [this](const QString& path, bool screenshot) { pictureAction(u"pin"_s, 0, path, screenshot); });
    connect(m_launcher.get(), &Launcher::settingsRequested, this, [this] { showSettings(); });
    connect(m_launcher.get(), &Launcher::shortcutSettingsRequested, this, &App::showShortcutSettings);
    // Only the chosen folders are indexed (GitHub #52): the notes under the results.
    connect(m_launcher.get(), &Launcher::folderSettingsRequested, this, [this] { showSettingsFor(u"Index/Folders"_s); });
    // Not a setting the AI settings know of: the section, by its row's title (SettingsWindow.qml).
    connect(m_launcher.get(), &Launcher::shareSettingsRequested, this, [this] {
        showSettings([](QQuickWindow* window) {
            QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, 2), Q_ARG(QVariant, 3),
                Q_ARG(QVariant, QCoreApplication::translate("SettingsWindow", "网络与 WSL 文件夹")));
        });
    });
    connect(m_launcher.get(), &Launcher::folderAddRequested, this, &App::addFolderFromLauncher);
    connect(m_launcher.get(), &Launcher::folderNoteDismissed, this, [this] {
        if (m_settings.folderHintDismissed)
            return;
        m_settings.folderHintDismissed = true;
        m_settings.save();
        if (m_settingsEditor)
            m_settingsEditor->setSettings(m_settings); // its copy must not bring the note back with its next change
        m_backup->soon();
    });
    connect(m_launcher.get(), &Launcher::panelToggleRequested, this, &App::togglePanel);
    connect(m_launcher.get(), &Launcher::openFailed, this, [this](const QString& name) {
        m_messages->showNotification(tr("没有打开“%1”").arg(name),
            integrity::processElevated()
                ? tr("找不到资源管理器，没法用普通权限打开它。请稍后再试，或按 Ctrl+Shift+Enter 以管理员身份运行。")
                : tr("Windows 没能打开它。请稍后再试，或按 Ctrl+Shift+Enter 以管理员身份运行。"));
    });
    connect(m_launcher.get(), &Launcher::namesShown, this, [this] {
        if (m_launcherReveal.pending())
            armReveal();
    });
    connect(m_launcher.get(), &Launcher::webSettingsRequested, this, &App::showWebSettings);
    connect(m_launcher.get(), &Launcher::fileTypeSettingsRequested, this, &App::showFileTypeSettings);
    connect(m_launcher.get(), &Launcher::favoriteSettingsRequested, this, &App::showFavoriteSettings);
    connect(m_launcher.get(), &Launcher::pluginSettingsRequested, this, &App::showPluginSettings);
    // The 内容 strip's switch: as the settings' 建立内容索引 (built or cleared a second later).
    connect(m_launcher.get(), &Launcher::contentIndexRequested, this, [this](bool on) {
        if (m_settings.contentIndex == on)
            return;
        Settings settings = m_settings;
        settings.contentIndex = on;
        settings.save();
        settingsEdited(settings);
        if (m_settingsEditor)
            m_settingsEditor->setSettings(m_settings);
    });
    connect(m_launcher.get(), &Launcher::fieldReported, this, &App::mirrorField);
    connect(m_launcher.get(), &Launcher::scopeChanged, this, &App::updateSearchBoxLook); // the placeholder
    m_boxPressGrace.setSingleShot(true);
    m_boxPressGrace.setInterval(400ms);
    connect(&m_boxPressGrace, &QTimer::timeout, this, [this] {
        if (m_window && m_window->isVisible() && !m_window->isActive() && !m_launcher->pinned())
            hideLauncher();
    });
    m_strandedWatch.setInterval(30ms); // a click is held longer than that
    m_adminTapDwell.setSingleShot(true);
    m_adminTapDwell.setInterval(kAdminTapDwell);
    connect(&m_adminTapDwell, &QTimer::timeout, this, &App::adminTapCheck);
    connect(&m_strandedWatch, &QTimer::timeout, this, &App::checkStrandedLauncher);
    // The footer's switches (the order, the start list): remembered.
    const auto remember = [this](QString Settings::* field, QString value) {
        if (m_settings.*field == value)
            return;
        m_settings.*field = std::move(value);
        m_settings.save();
        if (m_settingsEditor)
            m_settingsEditor->setSettings(m_settings);
    };
    connect(m_launcher.get(), &Launcher::sortChanged, this, [this, remember] {
        remember(&Settings::sort, kSortNames[m_launcher->sort()].toString());
    });
    connect(m_launcher.get(), &Launcher::startListChanged, this, [this, remember] {
        static constexpr QStringView kLists[] {u"opened", u"modified", u"none"};
        remember(&Settings::startList, kLists[m_launcher->startList()].toString());
    });
    connect(m_launcher.get(), &Launcher::regexModeChanged, this, [this] {
        if (m_settings.regexMode == m_launcher->regexMode())
            return;
        m_settings.regexMode = m_launcher->regexMode();
        m_settings.save();
        if (m_settingsEditor)
            m_settingsEditor->setSettings(m_settings);
    });
    createPluginHost();
    moveCapture();

    // Clipboard history (Win+V), in clipboard\ next to the index.
    m_clipStore = std::make_unique<ClipStore>(dataDir + u"\\clipboard"_s);
    // The user's limits before open(), which prunes: with the defaults it
    // would drop what a longer limit (or none) keeps, on every start.
    m_clipStore->setLimits({m_settings.clipboardMaxItems, m_settings.clipboardMaxDays}, QDateTime::currentMSecsSinceEpoch());
    m_clipStore->setKeepsPlaceOnPaste(m_settings.clipboardKeepPlace);
    if (m_settings.clipboard && !m_clipStore->open(QDateTime::currentMSecsSinceEpoch()))
        qWarning() << "Clipboard history: cannot be kept";
    ClipboardWatcher::Callbacks clipCallbacks;
    // On the watcher's thread: hop to the GUI thread.
    clipCallbacks.captured = [this](ClipCapture capture) {
        auto copied = std::make_shared<ClipCapture>(std::move(capture));
        QMetaObject::invokeMethod(this, [this, copied] { clipCaptured(*copied); }, Qt::QueuedConnection);
    };
    clipCallbacks.reused = [this](qint64 id) { // pasted from the history: it moves up
        QMetaObject::invokeMethod(this, [this, id] {
            if (!m_settings.clipboard || !m_clipStore->isOpen())
                return;
            m_clipStore->touch(id, QDateTime::currentMSecsSinceEpoch());
            m_clipboard->historyChanged();
        }, Qt::QueuedConnection);
    };
    m_clipWatcher = std::make_unique<ClipboardWatcher>(clipboardOptions(), std::move(clipCallbacks));
    m_clipboard = std::make_unique<Clipboard>(m_clipStore.get(), m_clipWatcher.get());
    connect(m_ocrPackage.get(), &OcrPackage::changed, m_clipboard.get(), [this] {
        if (m_ocrPackage->available())
            m_clipboard->refreshRecognition();
    });
    connect(m_clipboard.get(), &Clipboard::quickPasteFailed, this,
        [this](const QString& message) { m_messages->showNotification(tr("快捷粘贴"), message); });
    connect(m_clipboard.get(), &Clipboard::saveProblem, this,
        [this](const QString& message) { m_messages->showNotification(tr("保存为文件"), message); });
    connect(m_clipboard.get(), &Clipboard::activeChanged, this, &App::applyQuickPasteHotkeys);
    connect(qApp, &QGuiApplication::focusWindowChanged, this, &App::applyQuickPasteHotkeys);
    connect(qApp, &QGuiApplication::focusWindowChanged, this, &App::updateNumberKeyHook);
    // Pinned, it stays up through a paste or a file opened (hideClipboard);
    // Esc hands the focus back too.
    connect(m_clipboard.get(), &Clipboard::dismissRequested, this, [this] { hideClipboard(); });
    connect(m_clipboard.get(), &Clipboard::shortcutSettingsRequested, this, [this] {
        hideClipboard(false);
        showShortcutSettings();
    });
    connect(m_clipboard.get(), &Clipboard::pinRequested, this, [this](qint64 id) { pictureAction(u"pin"_s, id, {}, false); });
    connect(m_clipboard.get(), &Clipboard::editRequested, this, [this](qint64 id) { pictureAction(u"edit"_s, id, {}, false); });
    connect(m_clipboard.get(), &Clipboard::lanSendSetupRequested, this, [this](bool update) {
        hideClipboard(false);
        if (update) {
            showSettings([](QQuickWindow* window) {
                QMetaObject::invokeMethod(window, "showPluginLibrary", Q_ARG(QVariant, QVariant(u"winshun.localsend"_s)));
            });
        } else {
            showPluginSettings(u"winshun.localsend"_s);
        }
    });
    connect(m_clipboard.get(), &Clipboard::lanSendRequested, this, [this](const QString& text, const QStringList& files) {
        if (!m_pluginHost || !m_pluginHost->catalog().enabled(u"winshun.localsend"_s)
            || localSendNeedsUpdate(m_pluginHost->catalog())) {
            hideClipboard(false);
            showPluginSettings(u"winshun.localsend"_s);
            return;
        }
        if (m_launcher->panel())
            setPanel(false);
        showLauncher();
        m_launcher->runPluginCommand(u"winshun.localsend"_s, u"send"_s, u"clipboard"_s, text, files);
    });
    // The history changed (a screenshot taken, removed, put back): the
    // launcher's screenshot list too, while it shows it.
    connect(m_clipboard.get(), &Clipboard::categoriesChanged, m_launcher.get(), &Launcher::refreshScreenshots);
    connect(m_clipboard.get(), &Clipboard::keysReleased, this, [this] {
        if (m_clipWindow && m_clipWindow->isVisible())
            releaseClipboard(true);
    });
    connect(m_clipboard.get(), &Clipboard::pinnedChanged, this, &App::clipboardPinnedChanged);
    connect(m_clipboard.get(), &Clipboard::keysWanted, this, &App::engageClipboard);
    connect(m_clipboard.get(), &Clipboard::focusNeeded, this, &App::activateClipboard);
    // A group's name is typed in the window itself, Chinese with the input
    // method, which only works in the window with the focus.
    connect(m_clipboard.get(), &Clipboard::groupNameRequested, this, &App::activateClipboard);
    connect(m_clipboard.get(), &Clipboard::fieldRequested, this, &App::returnToField);
    connect(m_clipboard.get(), &Clipboard::turnOnRequested, this, [this] {
        Settings settings = m_settings;
        settings.clipboard = true;
        settings.save();
        settingsEdited(settings);
        if (m_settingsEditor)
            m_settingsEditor->setSettings(m_settings);
    });
    // Its entries of copied files that are gone since: hidden or cleaned out
    // as set, the disk asked while the window is put away.
    m_missingFiles
        = std::make_unique<MissingFiles>(m_clipStore.get(), m_settings.clipboard
                ? MissingFiles::modeOf(m_settings.clipboardMissingFiles) : MissingFiles::Mode::Keep);
    connect(m_clipboard.get(), &Clipboard::activeChanged, m_missingFiles.get(),
        [this] { m_missingFiles->setListShown(m_clipboard->active()); });
    connect(m_missingFiles.get(), &MissingFiles::changed, this, [this] {
        m_clipboard->historyChanged();
        if (m_settingsEditor)
            m_settingsEditor->setClipboardCount(static_cast<int>(m_clipStore->clips().size()));
    });

    m_updater = std::make_unique<Updater>();
    m_background = std::make_unique<BackgroundStatus>(m_index.get(), m_updater.get());
    // A big job (more than two minutes) is over: told once, as a tray notice.
    m_background->onFinished = [this](const QString& title, const QString& text) {
        if (m_messages)
            m_messages->showNotification(title, text);
    };    // The 运行状况 page's readings: every 10 seconds from now on (the index
    // loading is the first thing it sees), every second while it is shown.
    HealthMonitor::Sources sources;
    sources.facts = [this] {
        health::Facts f;
        using State = IndexService::State;
        const State state = m_index->state();
        f.loading = state == State::Idle || state == State::Loading;
        f.building = state == State::Building;
        f.rebuilding = state == State::Ready && m_index->isRefreshing() && m_background->rebuilding();
        const ContentIndexer::Status content = m_index->contentIndexer()->status();
        f.contentReading = content.reading;
        f.contentDone = content.done;
        f.saving = m_index->saving();
        f.moving = m_index->moving();
        return f;
    };
    sources.fileIndexBlocks = [this](std::vector<std::pair<const void*, std::size_t>>& blocks) {
        const auto index = m_index->index();
        if (!index)
            return true; // none: nothing held
        const auto lock = index->tryReadLock(); // never wait for a writer on the GUI thread
        if (!lock.owns_lock())
            return false;
        blocks = index->memoryBlocks();
        return true;
    };
    sources.tasks = [this] { return m_background->toVariant(); };
    sources.lastTrim = [this] {
        const IdleTrim::LastTrim last = m_idleTrim->lastTrim();
        return std::pair<qint64, qint64>(last.atMs, last.megabytes);
    };
    m_health = std::make_unique<HealthMonitor>(std::move(sources));
    connect(m_index.get(), &IndexService::stateChanged, m_health.get(), &HealthMonitor::poke);
    connect(m_index.get(), &IndexService::activityChanged, m_health.get(), &HealthMonitor::poke);
    m_backup = std::make_unique<Backup>();
    connect(m_backup.get(), &Backup::restartRequested, this, [this] { restart({u"--settings"_s}); });
    m_updater->setChinese(m_language == u"zh");
    connect(m_updater.get(), &Updater::found, this, [this](bool manual) {
        // Asked for, or the settings window is open: show it there. Otherwise
        // a tray notification, which does not take the keyboard from anyone.
        if (manual || (m_settingsWindow && m_settingsWindow->isVisible())) {
            showUpdate();
            return;
        }
        m_messages->showNotification(tr("Win顺 %1 可以更新了").arg(m_updater->availableVersion()),
            tr("点这里查看更新内容"), [this] { showUpdate(); });
    });
    // After an update put in place from here: whether it came in.
    connect(m_updater.get(), &Updater::notice, this,
        [this](const QString& title, const QString& text) { m_messages->showNotification(title, text); });
    m_updater->afterStart();

    if (!createWindow())
        return false;

    MessageWindow::Callbacks callbacks;
    callbacks.trayActivated = [this](std::uint32_t time) {
        const std::uint64_t pressedAt = tickTime(time, ::GetTickCount64());
        beginTrigger(TriggerSource::Tray, pressedAt);
        toggleLauncher(false, pressedAt);
    };
    callbacks.trayMenuRequested = [this] { showTrayMenu(); };
    callbacks.trayMenuSoon = [this] { prefetchTrayMenus(); };
    callbacks.hotkeyPressed = [this](int id, std::uint32_t time) {
        // When the keys were pressed, on the clock of a window's reveal (seenBy).
        const std::uint64_t pressedAt = tickTime(time, ::GetTickCount64());
        if (id > kQuickPasteHotkeyId && id <= kQuickPasteHotkeyId + 9) {
            const int number = id - kQuickPasteHotkeyId;
            // A message already queued before the keys were suspended must
            // not paste into the program behind our newly opened panel.
            if ((m_quickPasteKeys & (1u << number)) && !quickPasteSuspended())
                m_clipboard->pasteDirect(m_clipStore->quickPasteEntry(number, m_clipboard->pinnedFirst()),
                    ::GetForegroundWindow());
        } else if (id == kWinVHotkeyId || id == kClipboardHotkeyId) {
            // Pressed again while held since it opened the window (#107): a
            // step, never a second press (no seenBy, not a trigger).
            if (m_settings.clipboardPauseInFullScreen && fullScreenShortcutBlocked(foreground::facts(), true))
                return;
            if (clipboardStepPress(id))
                return;
            beginTrigger(id == kWinVHotkeyId ? TriggerSource::WinV : TriggerSource::ClipboardHotkey, pressedAt);
            armClipboardHold(id, pressedAt); // before the window: the keys from the opening press on
            toggleClipboard(pressedAt);
            if (m_hold && !m_keyRouter) { // own keyboard: browse without a hook or a paste on release
                endClipboardHold("not-routed");
                m_browseHotkey = m_holdHotkey;
                m_browseModifiers = m_holdModifiers;
                m_browseVk = m_holdVk;
                m_holdTimer.start();
            }
        } else if (id == kWinSHotkeyId)
            toggleAtTaskbar(false);
        else if (id == kScreenClipHotkeyId)
            startScreenClip();
        else if (id == kFolderMenuHotkeyId)
            folderMenuHotkey();
        else if (id != kDialogJumpHotkeyId) {
            beginTrigger(TriggerSource::Hotkey, pressedAt);
            toggleLauncher(true, pressedAt);
        } else if (m_dialogJump && m_settings.dialogJump)
            m_dialogJump->jump();
    };
    callbacks.shellRestarted = [this] { // Explorer let go of Win+V or Win+S, or took them back
        applyClipboardHotkeys();
        applyTaskbarHotkeys();
        if (m_searchBox)
            m_searchBox->reattach(); // onto the new taskbar
        if (m_dropPieHelper)
            m_dropPieHelper->retry(); // the user's token to start it with is there now
        QTimer::singleShot(3s, this, [this] { // once it has registered its own keys
            applyClipboardHotkeys();
            applyTaskbarHotkeys();
            applyWinKeyedHotkeys(); // ours, as the user chose them, if Explorer left them
        });
    };
    callbacks.commandReceived = [this](const QString& command) { handleCommand(command); };
    // Explorer's "search here" (WinShunSearch.exe): the folder typed, words to come.
    callbacks.searchHere = [this](const QString& folder) {
        m_launcher->noteSearchHere(folder); // not indexed (GitHub #52): searched on the disk all the same
        showLauncher(folder.endsWith(u'\\') ? folder + u' ' : folder + u"\\ "_s);
    };
    // Directory Opus's double click on blank space (WinShunSearch.exe, put
    // into Opus's settings by SettingsEditor::setOpusFolderMenu). Not with
    // the double click turned off: the menu may be there for its hotkey alone.
    callbacks.folderMenu = [this](const QString& folder) {
        if (m_folderMenu && m_settings.folderMenu != u"off")
            m_folderMenu->requested(folder);
    };
    // Windows ends us right after this returns, or a few seconds in: the
    // clipboard history and the index saved first. Past those seconds its
    // "preventing shutdown" screen says why rather than a bare name.
    callbacks.sessionEnding = [this] {
        const HWND window = m_messages ? m_messages->hwnd() : nullptr;
        if (window)
            ::ShutdownBlockReasonCreate(window, reinterpret_cast<LPCWSTR>(tr("正在保存文件索引").utf16()));
        if (m_clipStore)
            m_clipStore->flush(); // what was copied just now
        m_index->shutdown();
        if (window)
            ::ShutdownBlockReasonDestroy(window);
    };
    callbacks.deviceChange = [this](WPARAM event, LPARAM data) -> LRESULT {
        return m_volumeNotifier ? m_volumeNotifier->handle(event, data) : TRUE;
    };
    // Once the disks have settled: after hibernating, the file cache is gone.
    callbacks.resumed = [this] {
        m_index->prewarmSoon(30s);
        m_index->shares().networkChanged(); // the servers may have gone, or come back, meanwhile
        if (m_switches)
            m_switches->refresh(); // keep awake whose end time passed while asleep ends now
        // Its pages are read back from the disk now, while the user signs in,
        // not at the first double Ctrl: after a hibernation, Windows had
        // dropped those IdleTrim handed back (~8,700 page faults, a first
        // opening ~260 ms instead of ~30, GitHub #111). Once the drivers and
        // the graphics device are back; after a sleep, everything is still
        // in memory and this costs one frame. IdleTrim keeps them until the
        // first use (IdleTrim::resumed): handed back at wake, they went while
        // every program read its own back (GitHub #111). The clipboard's too:
        // the first hotkey after a while away may be its.
        m_idleTrim->resumed();
        QTimer::singleShot(5s, this, [this] {
            if (!launcherShown())
                prewarmLauncher();
        });
        QTimer::singleShot(6s, this, &App::prewarmClipboard);
    };
    callbacks.drawingMessage = [](UINT message, WPARAM wParam) { diagnostics::noteSystemMessage(message, wParam); };
    m_messages = std::make_unique<MessageWindow>(std::move(callbacks));

    // Drives: let go of one being ejected or locked; index new ones, drop gone ones.
    VolumeNotifier::Callbacks volumeCallbacks;
    volumeCallbacks.releaseRequested = [this](const std::wstring& root) { m_index->suspendVolume(root); };
    volumeCallbacks.released = [this](const std::wstring& root) { m_index->resumeVolume(root); };
    volumeCallbacks.volumesChanged = [this] { m_index->refreshVolumes(); };
    m_volumeNotifier = std::make_unique<VolumeNotifier>(m_messages->hwnd(), std::move(volumeCallbacks));
    connect(m_index.get(), &IndexService::volumesChanged, this,
        [this] { m_volumeNotifier->track(m_index->volumeRoots()); });
    m_messages->showTrayIcon(trayTooltip());

    // The button on the taskbar (WinShunSearch.exe) sets it: over the taskbar,
    // or away again. Set before Win顺 was up (it started Win顺), it opens now.
    if (Settings::profile().isEmpty() || !Settings::portableFolder().isEmpty()) {
        m_taskbarEvent = win32::UniqueHandle(taskbar::createEvent());
        if (m_taskbarEvent.valid()) {
            m_taskbarNotifier = std::make_unique<QWinEventNotifier>(m_taskbarEvent.get());
            connect(m_taskbarNotifier.get(), &QWinEventNotifier::activated, this, [this] { toggleAtTaskbar(true); });
        } else {
            qWarning() << "No event for the taskbar button:" << ::GetLastError();
        }
    }

    applySettings(true);
    // The launcher's strip of kinds of file as its user left it (after
    // applySettings: the kinds are known by then).
    m_launcher->setFileChoice(QSettings(statePath(), QSettings::IniFormat).value(u"Search/FileType"_s).toString());
    {
        QSettings state(statePath(), QSettings::IniFormat);
        m_launcher->setGallery(state.value(u"Search/Gallery"_s, false).toBool());
        m_launcher->setThumbnailSize(state.value(u"Search/ThumbnailSize"_s, 160).toInt());
    }
    connect(m_launcher.get(), &Launcher::galleryChanged, this, [this] {
        QSettings state(statePath(), QSettings::IniFormat);
        state.setValue(u"Search/Gallery"_s, m_launcher->gallery());
        state.setValue(u"Search/ThumbnailSize"_s, m_launcher->thumbnailSize());
    });
    connect(m_launcher.get(), &Launcher::fileChoiceChanged, this, [this] {
        QSettings(statePath(), QSettings::IniFormat).setValue(u"Search/FileType"_s, m_launcher->fileChoice());
    });
    // 0.11 and before: where the taskbar box was dragged to. It has one
    // place now (taskbar::SearchBox), not to be dragged.
    if (QSettings state(statePath(), QSettings::IniFormat); state.childGroups().contains(u"SearchBox"_s))
        state.remove(u"SearchBox"_s);
    applyTheme();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &App::applyTheme);

    m_settingsWatcher.addPath(Settings::filePath());
    connect(&m_settingsWatcher, &QFileSystemWatcher::fileChanged, this, [this] { m_settingsReload.start(); });

    m_index->start();
    if (m_serviceWanted) {
        if (m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None)
            startServiceRetry(); // the index waits for it meanwhile (accessWaitMs)
        else
            m_serviceRetry.start(kServiceRecheck); // in use: looked at now and then, in case it goes away
    }
    m_apps->refresh(true); // in the background; about half a second
    m_places->refresh(true); // likewise

    if (Settings::profile().isEmpty() || !Settings::portableFolder().isEmpty())
        autostart::adoptIfOrphaned();
    if (firstRun) {
        // Autostart is on by default; the settings switch turns it off for good
        // (the settings file exists from now on, so this runs once). The task
        // is one for every copy: not taken from another copy that has it (the
        // installed one, when a portable copy first starts), and not by a
        // --profile copy at all (a second set of settings, or a test run).
        const bool otherProfile = !Settings::profile().isEmpty() && Settings::portableFolder().isEmpty();
        if (!otherProfile && !autostart::isSetUp())
            autostart::setEnabled(true);
        // A portable copy walks the folders (no UAC, like Everything's): the
        // settings offer the index service and running as administrator.
        const bool walking = !Settings::portableFolder().isEmpty()
            && m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None;
        // An account that cannot install the service (GitHub #52) is not told
        // to; with no service, the known folders are indexed, not the disks.
        const bool cannotInstall = m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None
            && m_serviceState == ntfs::ServiceState::NotInstalled && !userIsAdministrator();
        // None of the known folders on this PC's disks (all on a server, say:
        // GitHub #52 review): nothing is indexed until some are added.
        const bool noFolders = m_foldersInEffect && m_settings.indexFolderPaths().empty();
        std::function<void()> onClick;
        if (noFolders)
            onClick = [this] { showSettingsFor(u"Index/Folders"_s); };
        if (!fromOnboarding)
            m_messages->showNotification(tr("Win顺已在后台运行"),
            noFolders ? tr("双击 Ctrl 打开搜索。还没有要索引的文件夹，点这里在设置里添加；放在服务器上的文件夹，请加到“网络共享”里。")
                : m_foldersInEffect ? tr("双击 Ctrl 打开搜索。Win顺 先索引你常用的文件夹（桌面、文档、下载等），可以在设置里增减。")
                : cannotInstall ? tr("双击 Ctrl 打开搜索。Win顺 按文件夹遍历建立索引，首次运行需要一点时间。")
                : walking ? tr("双击 Ctrl 打开搜索。便携版按文件夹遍历建立索引；要快一些，可以在设置里安装索引服务，或以管理员身份运行。")
                          : tr("双击 Ctrl 打开搜索。首次运行需要一点时间建立文件索引。"),
            std::move(onClick));
    } else if (clipboardIsNew) {
        m_messages->showNotification(tr("Win顺 现在有剪贴板历史了"),
            m_settings.clipboard ? tr("复制过的文字、图片和文件都能找回来。点这里设置用 Win+V 打开它。")
                                 : tr("复制过的文字、图片和文件都能找回来。点这里打开它。"),
            [this] { showClipboardSettings(); });
    } else if (m_settings.onboardingCapturePending && !fromOnboarding) {
        m_messages->showNotification(tr("截图尚未启用"), tr("点这里在插件库中继续安装并确认权限。"),
            [this] { showPluginSettings(u"winshun.capture"_s); });
    } else if (!m_indexFolderProblem.isEmpty()) {
        m_messages->showNotification(tr("索引位置"), m_indexFolderProblem);
    }
    if (options.settings) {
        showSettings([&options](QQuickWindow* window) {
            if (!options.settingsAt.isEmpty())
                QMetaObject::invokeMethod(window, "showPlace", Q_ARG(QVariant, QVariant(options.settingsAt)));
        });
    } else if (!options.background || !options.query.isEmpty())
        showLauncher(options.query);
    QTimer::singleShot(1s, this, &App::prewarmLauncher);
    QTimer::singleShot(1500ms, this, &App::prewarmClipboard);
    return true;
}

bool App::createWindow()
{
    m_qml = std::make_unique<QQmlApplicationEngine>();
    m_qml->addImageProvider(u"fileicon"_s, new FileIconProvider); // engine takes ownership
    m_qml->addImageProvider(u"thumbnail"_s, new ThumbnailProvider); // result rows' pictures, videos, PDFs
    m_qml->addImageProvider(u"clipthumb"_s, new ClipThumbnailProvider); // clipboard rows' pictures
    m_qml->addImageProvider(u"preview"_s, m_preview->imageProvider()); // the picture in the preview (Alt+P)
    m_placement = std::make_unique<Placement>(Settings::dataDir() + u"\\state.ini"_s, u"Launcher"_s);
    m_frame = std::make_unique<WindowFrame>(false);
    m_qml->setInitialProperties({
        {u"launcher"_s, QVariant::fromValue(m_launcher.get())},
        {u"clipboard"_s, QVariant::fromValue(m_clipboard.get())},
        {u"placement"_s, QVariant::fromValue(m_placement.get())},
        {u"frame"_s, QVariant::fromValue(m_frame.get())},
    });
    m_qml->loadFromModule(u"WinShun"_s, u"Main"_s);
    const auto roots = m_qml->rootObjects();
    if (roots.isEmpty())
        return false;
    m_window = qobject_cast<QQuickWindow*>(roots.constFirst());
    if (!m_window)
        return false;

    // This window changes height on every frame of the scope strip's
    // animation. A vsync-throttled D3D11 swapchain can hold its first resize
    // for ~100 ms, then skip most of the transition. Present without that
    // wait; the desktop compositor still controls what reaches the screen.
    if (m_renderer == u"d3d11") {
        QSurfaceFormat format = m_window->requestedFormat();
        format.setSwapInterval(0);
        m_window->setFormat(format);
    }
    prepareBackdrop(m_window);
    m_window->create(); // native handle now, so DWM styling applies before the first show
    m_window->installEventFilter(this);
    diagnostics::watchWindow(m_window, u"launcher"_s);
    shellcontext::watchKeys(reinterpret_cast<HWND>(m_window->winId())); // which key asks for a context menu (eventFilter)
    m_launcher->setWindow(m_window);
    m_preview->setLauncherWindow(m_window);
    m_placement->setWindow(m_window);
    m_frame->setWindow(m_window);
    // Its width is its user's: dragged at the left or right edge, both edges
    // move (the centre stays); a double click there goes back to 760.
    m_placement->rememberWidth(760, 640);
    m_frame->setSideSizing(true);
    connect(m_frame.get(), &WindowFrame::sideGrabbed, m_placement.get(), &Placement::beginSizing);
    connect(m_frame.get(), &WindowFrame::sideDoubleClicked, m_placement.get(), &Placement::resetWidth);
    connect(m_placement.get(), &Placement::widthReset, this, [this] {
        if (launcherShown())
            m_launcher->flash(tr("已恢复默认宽度"));
    });
    const auto tellSettings = [this] {
        if (m_settingsEditor)
            m_settingsEditor->setLauncherWidthSet(m_placement->customWidth());
    };
    connect(m_placement.get(), &Placement::boxWidthChanged, this, tellSettings);
    connect(m_placement.get(), &Placement::widthReset, this, tellSettings);
    m_searchField = m_window->findChild<QQuickItem*>(u"searchField"_s);
    if (m_searchField)
        m_searchField->installEventFilter(this); // the input method's questions, while it is in the box
    connect(m_window, &QWindow::activeChanged, this, [this] {
        if (m_window && !m_window->isActive()) {
            launcherLostFront();
        } else if (m_window && m_window->isActive()) {
            if (::GetForegroundWindow() == reinterpret_cast<HWND>(m_window->winId()))
                stopLauncherRouting(); // in front after all (a click on it): its keys are its own
            m_launcher->systemCommandDone();
            m_boxPressGrace.stop();
            lettersOnOpen();
            deliverTyping(); // asked for by typing elsewhere: the keys, if it is ready for them now
        }
        updateNumberKeys();
    });
    // Windows refused to bring it to the front (bringToFront: the taskbar's
    // box pressed again and again, say). The refused try made it the active
    // window of this thread all the same, in the background; another
    // program's window coming to the front then tells it nothing, and a
    // click elsewhere left it up. The front changing to another program's
    // window, Qt still holding it active, is that click. Out of context:
    // told through this (the GUI) thread's message loop.
    g_launcherApp = this;
    m_launcherFrontHook = ::SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &App::foregroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
    // Fresh rows coming in while it slides in beat a launcher that comes
    // late: keep the existing 60 ms fallback even if no frame arrives.
    m_launcherReveal.setWindow(m_window, 60ms, [this] { revealLauncher(); });
    connect(m_window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_prewarming.load())
            QMetaObject::invokeMethod(this, &App::finishPrewarm, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    // Keys typed elsewhere wait for it (deliverTyping): its field becoming
    // the focus object (Qt has given the window its input method context by
    // then), and the cap. Then a moment's watch of what came of them. The
    // input method into English first, should that be set (the keys go
    // through it).
    connect(qGuiApp, &QGuiApplication::focusObjectChanged, this, &App::lettersOnOpen);
    connect(qGuiApp, &QGuiApplication::focusObjectChanged, this, &App::deliverTyping);
    m_typingCap.setSingleShot(true);
    m_typingCap.setTimerType(Qt::PreciseTimer);
    connect(&m_typingCap, &QTimer::timeout, this, &App::deliverTyping);
    m_handoffWatch.setSingleShot(true);
    m_handoffWatch.setInterval(500ms);
    connect(&m_handoffWatch, &QTimer::timeout, this, &App::endHandoff);
    m_keysWatch = std::make_unique<LauncherKeysWatch>(); // kept: the render thread's calls below use it
    connect(m_window, &QQuickWindow::afterSynchronizing, this, [watch = m_keysWatch.get()] {
        watch->synchronized();
    }, Qt::DirectConnection);
    connect(m_window, &QQuickWindow::frameSwapped, this, [watch = m_keysWatch.get()] {
        watch->frameSwapped();
    }, Qt::DirectConnection);
    return true;
}

// The clipboard's own window (ClipboardWindow.qml): not the launcher's, so
// that it can drop down under any search box of ours, and be as wide as it
// needs.
bool App::createClipWindow()
{
    m_clipPlacement = std::make_unique<Placement>(Settings::dataDir() + u"\\state.ini"_s, u"Clipboard"_s);
    m_textCaret = std::make_unique<win::TextCaret>();
    m_clipFrame = std::make_unique<WindowFrame>(false);
    QQmlComponent component(m_qml.get(), u"WinShun"_s, u"ClipboardWindow"_s);
    QObject* object = component.createWithInitialProperties({
        {u"clipboard"_s, QVariant::fromValue(m_clipboard.get())},
        {u"placement"_s, QVariant::fromValue(m_clipPlacement.get())},
        {u"frame"_s, QVariant::fromValue(m_clipFrame.get())},
    });
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        qWarning().noquote() << component.errorString();
        delete object;
        return false;
    }
    m_clipWindow = window;
    prepareBackdrop(window);
    window->create();
    window->installEventFilter(this); // the Menu key, pasting into its search box
    styleWindow(window, SystemTheme::backdropAvailable());
    win::setDarkFrame(window, isDarkMode());
    m_clipboard->setWindow(window);
    m_clipPlacement->setWindow(window);
    m_clipPlacement->enableWindowSizing();
    m_clipFrame->setWindow(window);
    m_clipFrame->setResizable(true);
    m_clipDisplay = std::make_unique<ClipboardDisplay>(m_clipboard.get(), window, m_clipPlacement.get(), m_clipDisplayProbe);
    window->setProperty("displayController", QVariant::fromValue<QObject*>(m_clipDisplay.get()));
    connect(m_clipDisplay.get(), &ClipboardDisplay::geometryEdited, m_clipPlacement.get(), &Placement::adoptDisplayGeometry);
    connect(m_clipDisplay.get(), &ClipboardDisplay::focusLost, this, [this] { hideClipboard(); });
    connect(m_clipDisplay.get(), &ClipboardDisplay::lost, this, [this] {
        if (!m_clipDisplayWanted || !m_clipWindow) return;
        m_clipDisplayWanted = false;
        m_clipWindow->setProperty("controllerEventsEnabled", true);
        emit m_clipboard->queryChanged();
        emit m_clipboard->placed();
        const HWND target = m_clipboard->target();
        stopKeyRouting();
        if (!m_clipWindow->isVisible()) return;
        if (win::isShellFlyout(target)) avoidClipboardShellFlyout(target);
        const bool keepFocus = startKeyRouting(target) || win::isShellFlyout(target);
        m_clipFrame->setNoActivate(keepFocus);
        win::setCloaked(m_clipWindow, false);
        ::SetWindowPos(reinterpret_cast<HWND>(m_clipWindow->winId()), HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    });
    m_clipDisplay->start();
    connect(window, &QWindow::activeChanged, this, [this] {
        // Clicking elsewhere or switching apps hides it, like a menu. Not at
        // once: on the way to another window of ours (the field it goes back
        // to), the focus may first go to none.
        if (m_clipWindow && !m_clipWindow->isActive())
            QMetaObject::invokeMethod(this, [this] {
                if (m_clipWindow && !m_clipWindow->isActive())
                    hideClipboard();
            }, Qt::QueuedConnection);
    });
    m_clipReveal.setWindow(window, 150ms, [this] { uncloakClipboard(); });
    connect(window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_clipPrewarming.load())
            QMetaObject::invokeMethod(this, &App::finishClipPrewarm, Qt::QueuedConnection);
    }, Qt::DirectConnection); // on the render thread
    updateCompanions();
    return true;
}

// The preview's window (FilePreview, PreviewWindow.qml): made the first time
// Alt+P is pressed, styled as the launcher, never activated.
QQuickWindow* App::createPreviewWindow()
{
    m_previewFrame = std::make_unique<WindowFrame>(false);
    QQmlComponent component(m_qml.get(), u"WinShun"_s, u"PreviewWindow"_s);
    QObject* object = component.createWithInitialProperties({
        {u"preview"_s, QVariant::fromValue(m_preview.get())},
        {u"frame"_s, QVariant::fromValue(m_previewFrame.get())},
    });
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        qWarning().noquote() << component.errorString();
        delete object;
        return nullptr;
    }
    prepareBackdrop(window);
    window->create();
    styleWindow(window, SystemTheme::backdropAvailable());
    win::setDarkFrame(window, isDarkMode());
    m_previewFrame->setWindow(window);
    m_previewFrame->setNoActivate(true); // the launcher keeps the keyboard; Mica drawn as active all the same
    return window;
}

// The first search of the web shortcuts is the default (defaultWebSearchUrl).
// WINSHUN_WEB_DRYRUN (tests): said in the log (WINSHUN_LOG_INFO), not opened.
const WebShortcuts& App::activeWebShortcuts() const
{
    static const WebShortcuts empty;
    return m_pluginHost && webplugin::available(m_pluginHost->catalog()) ? m_settings.webShortcuts : empty;
}

void App::applyWebSearch()
{
    if (!m_pluginHost || !m_launcher)
        return;
    const auto& shortcuts = activeWebShortcuts();
    QList<std::pair<QString, QString>> keywords;
    for (const auto& shortcut : shortcuts)
        keywords.append({shortcut.keyword, shortcut.name});
    m_pluginHost->setWebKeywords(keywords);
    m_launcher->setWebShortcuts(shortcuts);
    m_launcher->setWebSuggestions(!shortcuts.isEmpty() && m_settings.webSuggestions);
}

bool App::openWebSearch(const QString& query)
{
    if (activeWebShortcuts().isEmpty())
        return false;
    const QString url = defaultWebSearchUrl(activeWebShortcuts(), query);
    if (url.isEmpty())
        return false;
    static const bool dryRun = qEnvironmentVariableIsSet("WINSHUN_WEB_DRYRUN");
    qInfo().noquote() << (dryRun ? u"Web: would open %1 (the words read from a picture)"_s : u"Web: open %1 (the words read from a picture)"_s).arg(url);
    if (!dryRun)
        shell::openUrl(url);
    return true;
}

bool App::eventFilter(QObject* watched, QEvent* event)
{
    // Qt has just set this window's frame light, and every other frameless
    // one's: the application palette changed, the scheme maybe not (an
    // accent colour; Windows' own colours coming in after sign-in, GitHub
    // #120), and then applyTheme does not run.
    if (event->type() == QEvent::ApplicationPaletteChange)
        guardDarkFrames();
    // A click in the clipboard window while its hotkey is held (#107): the
    // mouse picks the row now, letting go pastes nothing.
    if (m_hold && watched == m_clipWindow && event->type() == QEvent::MouseButtonPress)
        endClipboardHold("click");
    if (m_browseHotkey && watched == m_clipWindow) {
        bool done = event->type() == QEvent::MouseButtonPress;
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
            const auto* key = static_cast<QKeyEvent*>(event);
            const unsigned vk = key->nativeVirtualKey();
            const unsigned modifier = hotkeysteps::modifierOf(vk);
            done = event->type() == QEvent::KeyRelease
                ? (modifier & m_browseModifiers) != 0
                : vk != m_browseVk && modifier == 0;
        }
        if (done)
            m_browseHotkey = 0;
    }
    if (event->type() == QEvent::Close && (watched == m_window || watched == m_clipWindow)) {
        // These windows are kept between openings. QWindow::close would
        // destroy the HWND and detach Placement and WindowFrame.
        event->ignore();
        if (watched == m_window)
            hideLauncher();
        else {
            m_clipboard->setPinned(false); // a system close also closes a pinned clipboard
            hideClipboard();
        }
        return true;
    }
    // The launcher's field, drawn in the box on the taskbar: the input
    // method's window goes by the caret there.
    if (watched == m_searchField && event->type() == QEvent::InputMethodQuery && m_launcher->inTaskbarBox()) {
        auto* query = static_cast<QInputMethodQueryEvent*>(event);
        const Qt::InputMethodQueries rects = query->queries() & (Qt::ImCursorRectangle | Qt::ImAnchorRectangle);
        if (const std::optional<QRectF> caret = rects ? boxCaretInField() : std::nullopt) {
            static_cast<QObject*>(m_searchField)->event(event); // the field's own answers to the rest
            if (rects & Qt::ImCursorRectangle)
                query->setValue(Qt::ImCursorRectangle, *caret);
            if (rects & Qt::ImAnchorRectangle)
                query->setValue(Qt::ImAnchorRectangle, *caret);
            return true;
        }
    }
    // Keys typed elsewhere just went to the launcher (GitHub #103): when its
    // field first got a composition or text from them. Never what.
    if (watched == m_searchField && m_keysWatch && m_keysWatch->watching()) {
        if (event->type() == QEvent::InputMethod) {
            const auto* input = static_cast<QInputMethodEvent*>(event);
            if (!input->commitString().isEmpty())
                m_keysWatch->fieldGot(true);
            else if (!input->preeditString().isEmpty())
                m_keysWatch->fieldGot(false);
        } else if (event->type() == QEvent::KeyPress && !static_cast<QKeyEvent*>(event)->text().isEmpty()) {
            m_keysWatch->fieldGot(true);
        }
    }
    // Pasted into a one-line field (the search boxes, the settings' fields):
    // several lines go in as one. Before the field sees the keys.
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->matches(QKeySequence::Paste)) {
        if (auto* window = qobject_cast<QQuickWindow*>(watched); window && textfield::pasteOneLine(window->activeFocusItem()))
            return true;
    }
    if (watched == m_window && event->type() == QEvent::ContextMenu
        && static_cast<QContextMenuEvent*>(event)->reason() == QContextMenuEvent::Keyboard) {
        // Shift+Menu and Ctrl+Shift+F10 ask for the shell's menu, the Menu
        // key and Shift+F10 for Win顺's. Main.qml takes the chords as keys
        // when Qt hands them on as such; an IME in its Chinese mode keeps
        // the press for itself, and only this event (from the release)
        // tells, so the key is looked up (shellcontext::watchKeys). Not
        // when the shell's menu is up or about to be (opened from the key):
        // that is the release's event, and the menu has the keyboard.
        const Qt::KeyboardModifiers modifiers = static_cast<QContextMenuEvent*>(event)->modifiers();
        const UINT key = shellcontext::lastKeyPressed();
        const bool system = (modifiers & Qt::ShiftModifier)
            && (key == VK_APPS || (key == VK_F10 && (modifiers & Qt::ControlModifier)));
        if (!m_launcher->systemMenuBusy())
            emit m_launcher->contextMenuKeyPressed(system);
        return true;
    }
    if (watched == m_clipWindow && event->type() == QEvent::ContextMenu
        && static_cast<QContextMenuEvent*>(event)->reason() == QContextMenuEvent::Keyboard) {
        emit m_clipboard->contextMenuKeyPressed();
        return true;
    }
    if (watched == m_barWindow && m_dialogBar && event->type() == QEvent::ContextMenu
        && static_cast<QContextMenuEvent*>(event)->reason() == QContextMenuEvent::Keyboard) {
        emit m_dialogBar->contextMenuKeyPressed();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

void App::applyTheme()
{
    const bool dark = isDarkMode();
    win::setMenuTheme(dark);
    if (m_pluginHost)
        m_pluginHost->setAppearance(m_language, dark ? u"dark"_s : u"light"_s, pluginLook());
    if (m_window)
        styleWindow(m_window, SystemTheme::backdropAvailable());
    if (m_clipWindow)
        styleWindow(m_clipWindow, SystemTheme::backdropAvailable());
    if (QQuickWindow* preview = m_preview ? m_preview->window() : nullptr)
        styleWindow(preview, SystemTheme::backdropAvailable());
    if (m_preview)
        m_preview->setDark(dark);
    if (m_settingsWindow)
        styleWindow(m_settingsWindow, SystemTheme::backdropAvailable());
    if (m_barWindow)
        styleWindow(m_barWindow);
    updateSearchBoxLook(); // the accent
    guardDarkFrames();
}

void App::guardDarkFrames()
{
    // The frames' dark mode only once Qt has set its own (light) one, a few
    // milliseconds later; then watch them for a while (see win::setDarkFrame).
    m_darkFrame = isDarkMode();
    m_darkFrameWatch.start();
    m_darkFrameGuard.start();
}

// Theme and language take effect at once, in every window, without a restart.
void App::applyAppearance()
{
    SystemTheme::setRoundedCorners(m_settings.corners == u"round");
    updateSearchBoxLook(); // the native child redraws on its own thread
    // 界面大小: QML follows SystemTheme; the bar by file dialogs is placed by its heights.
    if (SystemTheme::uiSize() != m_settings.uiSize) {
        SystemTheme::setUiSize(m_settings.uiSize);
        DialogBar::setUiSize(m_settings.uiSize);
        if (m_clipPlacement)
            m_clipPlacement->resized();
    }
    SystemTheme::setBackdropOn(m_settings.transparency == u"on"); // Mica stays set up behind, covered when off
    // Unknown follows Windows. Everything else reads the scheme from the style
    // hints and listens to colorSchemeChanged (see applyTheme, SystemTheme).
    QGuiApplication::styleHints()->setColorScheme(m_settings.theme == u"dark" ? Qt::ColorScheme::Dark
            : m_settings.theme == u"light"                                     ? Qt::ColorScheme::Light
                                                                               : Qt::ColorScheme::Unknown);

    const QString language = Settings::resolveLanguage(m_settings.language);
    if (language == m_language) {
        if (m_pluginHost) // the plugins' windows look the same (corners, size, transparency, theme)
            m_pluginHost->setAppearance(m_language, isDarkMode() ? u"dark"_s : u"light"_s, pluginLook());
        return;
    }
    m_language = language;
    QCoreApplication::removeTranslator(&m_translator);
    if (language == u"en") {
        if (m_translator.load(u":/i18n/winshun_en.qm"_s))
            QCoreApplication::installTranslator(&m_translator);
        else
            qWarning() << "No English translation in the resources";
    }
    // Numbers and sizes ("1,234", "172 MB") the language's way.
    QLocale::setDefault(language == u"en" ? QLocale(QLocale::English, QLocale::UnitedStates)
                                          : QLocale(QLocale::Chinese, QLocale::China));
    QGuiApplication::setApplicationDisplayName(tr("Win顺"));
    if (m_updater)
        m_updater->setChinese(language == u"zh");

    if (m_pluginHost)
        m_pluginHost->setAppearance(language, isDarkMode() ? u"dark"_s : u"light"_s, pluginLook());

    if (!m_qml)
        return; // starting up: nothing shown yet
    m_qml->retranslate();
    m_launcher->retranslate();
    updateSearchBoxLook();
    m_clipboard->retranslate();
    if (m_settingsEditor) {
        m_settingsEditor->plugins()->setLocale(language); // the cards' names and texts
        m_settingsEditor->retranslateFolders(); // GitHub #52: the indexed folders' cards
        refreshIndexAccess(); // and the line saying what is indexed now
    }
    m_messages->setTrayTooltip(trayTooltip());
    applyHotkey(); // its error message
    applyFolderMenuHotkey();
    refreshContentIndexStatus();
}

QString App::trayTooltip() const
{
    const QString name = m_settings.doubleCtrl ? tr("Win顺 — %1 打开").arg(doubleCtrlName()) : tr("Win顺");
    const SwitchState awake = m_switches ? m_switches->state(u"keep-awake") : SwitchState {};
    if (awake.on != SwitchState::Value::On)
        return name;
    return awake.until.isValid() ? tr("%1 · 保持唤醒到 %2").arg(name, awake.until.time().toString(u"HH:mm"_s))
                                 : tr("%1 · 保持唤醒").arg(name);
}

// "双击 Ctrl", "双击左 Alt", "双击 Shift": the key as set.
QString App::doubleCtrlName() const
{
    const QString key = m_settings.doubleTapKey == u"alt" ? u"Alt"_s : m_settings.doubleTapKey == u"shift" ? u"Shift"_s : u"Ctrl"_s;
    if (m_settings.doubleCtrlKey == u"left")
        return tr("双击左 %1").arg(key);
    if (m_settings.doubleCtrlKey == u"right")
        return tr("双击右 %1").arg(key);
    return tr("双击 %1").arg(key);
}

void App::applyKeyListener(bool force)
{
    if (m_keyListenerMode < 0) return;
    // Double clicks: for File Explorer and the desktop; Directory Opus says
    // so itself. Not when the menu is there for its hotkey alone.
    const bool clickMenu = m_folderMenu && m_settings.folderMenu != u"off"
        && (m_settings.folderMenuExplorer || m_settings.folderMenuDesktop);
    const bool doubleClicks = clickMenu && m_settings.folderMenuDouble;
    const bool middleClicks = clickMenu && m_settings.folderMenuMiddle;
    const int listenTo = (m_settings.doubleCtrl ? 1 : 0) | (doubleClicks ? 2 : 0) | (m_dropPie ? 4 : 0) | (middleClicks ? 8 : 0);
    if (force || listenTo != m_keyListenerMode) {
        m_serviceKeyListener.reset();
        m_keyListener.reset(); // the process's one mouse registration goes with it first
        m_keyListenerMode = listenTo;
        if (listenTo != 0) {
            // Called on the listener thread: hop to the GUI thread.
            KeyListener::Callbacks callbacks;
            if (m_settings.doubleCtrl)
                callbacks.doubleCtrl = [this](const KeyListener::Tap& tap) {
                    // A program made the taps: which keys came, for "it opens by itself".
                    // A few times a run (a key remapped to Ctrl makes every one).
                    static std::atomic<int> logged {0};
                    if (!tap.sent.empty() && logged.fetch_add(1) < 10)
                        qWarning().noquote() << "Double tap partly sent by a program:" << QString::fromStdString(tap.sent);
                    // Counted only by a release and press no finger makes
                    // that quick: held up by a program's slow hook (GitHub #111).
                    static std::atomic<int> queued {0};
                    if (tap.queuedMs && queued.fetch_add(1) < 10)
                        qWarning() << "Double tap: its first press seemed" << tap.queuedMs
                                   << "ms long, let go as the second was pressed: both waited behind a program's slow "
                                      "keyboard hook";
                    // With the tap's own time: how long it waited for the
                    // GUI thread, and whether the launcher was there to be
                    // seen before it (GitHub #111).
                    QMetaObject::invokeMethod(this, [this, tap] { onDoubleCtrl(tap); }, Qt::QueuedConnection);
                };
            if (m_settings.doubleCtrl) // a double tap that came to nothing, for the journal
                callbacks.note = [this](const KeyListener::Note& note) {
                    QMetaObject::invokeMethod(this, [this, note] { journalTapNote(note); }, Qt::QueuedConnection);
                };
            if (FolderMenu* menu = doubleClicks ? m_folderMenu.get() : nullptr)
                callbacks.doubleClick = [menu](HWND window, POINT pt) { menu->doubleClicked(window, pt); };
            if (FolderMenu* menu = middleClicks ? m_folderMenu.get() : nullptr)
                callbacks.middleClick = [menu](HWND window, POINT pt) { menu->doubleClicked(window, pt); };
            if (DropPie* pie = m_dropPie.get()) {
                callbacks.dragMenu = [pie](POINT pt, HWND capture, DWORD pid) { pie->summoned(pt, capture, pid); };
                callbacks.leftButtonUp = [pie] { pie->buttonReleased(); };
            }
            callbacks.log = [](const std::string& line) { qWarning().noquote() << QString::fromStdString(line); };
            m_keyListener = std::make_unique<KeyListener>(std::move(callbacks));
            m_keyListenerMade.start();
            if (m_settings.doubleCtrl && m_keyListener->isActive())
                m_serviceKeyListener = std::make_unique<ServiceKeyListener>(m_keyListener->window(),
                    [listener = m_keyListener.get()](DWORD pid) { listener->setServiceProcess(pid); });
        }
    }
    if (m_keyListener) {
        m_keyListener->setKeys(m_settings.doubleTapKey == u"alt"     ? KeyListener::Keys::Alt
                : m_settings.doubleTapKey == u"shift"                ? KeyListener::Keys::Shift
                                                                      : KeyListener::Keys::Ctrl);
        m_keyListener->setSide(m_settings.doubleCtrlKey == u"left" ? DoubleTapDetector::Side::Left
                : m_settings.doubleCtrlKey == u"right"            ? DoubleTapDetector::Side::Right
                                                                  : DoubleTapDetector::Side::Both);
    }
    // With double tap paused in games: whether a hidden pointer was Windows'
    // doing for typing, by the keys the listener saw (GameGuard, GitHub #111).
    if (m_keyListener && m_settings.doubleCtrl && m_settings.doubleCtrlPauseInGames) {
        if (!m_pointerWatch)
            m_pointerWatch = std::make_unique<PointerWatch>([this](std::uint32_t atOrBefore) {
                return m_keyListener ? m_keyListener->keyDownAtOrBefore(atOrBefore) : std::nullopt;
            });
    } else {
        m_pointerWatch.reset();
    }
}

void App::applySettings(bool initial)
{
    // 网络代理 (GitHub #84): for every request over WinHTTP, from the next on. A
    // manual address that is not a good one follows the system's, as the page says.
    if (const proxy::Address address = proxy::parse(m_settings.proxyAddress); m_settings.proxyMode == u"none")
        http::setProxy(http::Proxy::None);
    else if (m_settings.proxyMode == u"manual" && address.ok())
        http::setProxy(http::Proxy::Named, address.server);
    else
        http::setProxy(http::Proxy::System);
    applyFolderMenu(); // before the listener, which tells it double clicks
    applyDropPie(); // likewise its key
    applyKeyListener();
    applyDialogs();
    applyClipboard(initial);
    applyTaskbar(initial);
    // The tray's icon and the taskbar box's: Win顺's colours, or the taskbar's one.
    m_messages->setTrayIconMono(m_settings.taskbarIcon == u"mono");
    m_messages->setTrayTooltip(trayTooltip()); // which Ctrl
    updateSearchBoxLook();
    applyMcp();
    filemanager::setCustomCommands(m_settings.fileManagerOpenCommand, m_settings.fileManagerRevealCommand);
    for (const auto& [kind, name] : filemanager::kSettingNames) {
        if (m_settings.fileManager == QLatin1StringView(name))
            shell::setFileManager(kind);
    }

    if (!initial)
        applyAppearance();
    shellmenu::apply(m_settings.explorerMenu); // in Windows' language, not Win顺's (GitHub #190)
    shellmenu::applyPasteFile(m_settings.pasteFileEnabled && m_settings.pasteFileMenu);
    m_updater->setAutomatic(m_settings.autoUpdate);
    if (m_pluginUpdates) {
        m_pluginUpdates->setAutomatic(m_settings.autoUpdate);
        m_pluginUpdates->setPolicy(m_settings.pluginDeveloperMode, m_settings.plugins);
    }
    m_backup->setFolder(m_settings.backupFolder);
    m_launcher->setRecordHistory(m_settings.recordHistory);
    m_launcher->setResultOptions(m_settings.foldersFirst ? KindOrder::FoldersFirst : KindOrder::FilesFirst,
        m_settings.showModified, m_settings.showFileSize, m_settings.thumbnails, m_settings.numberKeys);
    m_launcher->setDoubleClickToOpen(m_settings.doubleClickToOpen);
    m_launcher->setTabEntersFavoriteFolder(m_settings.tabEntersFavoriteFolder);
    m_launcher->setSearchKinds(m_settings.searchApps, m_settings.searchSystem);
    m_launcher->setRegexMode(m_settings.regexMode);
    m_launcher->setFolderTyping(m_settings.folderTyping);
    m_launcher->results()->setShowModifiedTime(m_settings.showModifiedTime);
    m_launcher->setPreferredTypes(m_settings.preferredTypes);
    m_launcher->setFolderKeyword(m_settings.folderKeyword);
    m_launcher->setExcludedFiles(m_settings.excludedTypes, m_settings.excludedFileNames);
    m_launcher->setSort(static_cast<Launcher::Sort>(sortIndex(m_settings.sort)));
    m_launcher->setStartList(m_settings.startList == u"modified" ? Launcher::RecentlyModified
            : m_settings.startList == u"none"                     ? Launcher::NoStartList
                                                                 : Launcher::RecentlyOpened);
    m_launcher->setStartOptions(m_settings.recentItems, m_settings.favoritesOnStart);
    m_preview->setBuiltInOnly(m_settings.previewWith == u"builtin");
    m_launcher->setPreviewHotkey(m_settings.previewHotkey);
    m_preview->setTextExtensions(m_settings.contentExtensions);
    m_clipboard->setNumberKeys(m_settings.clipboardNumberKeys);
    m_launcher->setNumberHints(m_settings.numberHints);
    m_launcher->setKeyboardHints(m_settings.shortcutHelp, m_settings.footerHints);
    tellLauncherFolders();
    m_clipboard->setNumberHints(m_settings.numberHints);
    const QString clipboardSize = m_settings.clipboardUiSize == u"follow" ? m_settings.uiSize : m_settings.clipboardUiSize;
    const bool clipboardResized = clipboardSize != m_clipboard->uiSize();
    m_clipboard->setRowOptions(m_settings.clipboardClickToPaste, m_settings.clipboardLargePictures, clipboardSize);
    if (clipboardResized && m_clipPlacement)
        m_clipPlacement->resized(); // the rows have another height
    m_clipboard->setLayoutOptions(m_settings.clipboardFlipAbove, m_settings.clipboardSearchBoxSide == u"far");
    m_clipboard->setHoldToStep(m_settings.clipboardHoldToStep);
    if (!m_settings.clipboardHoldToStep && (m_hold || m_browseHotkey))
        endClipboardHold("setting-off");
    updateNumberKeys();
    applyHotkey();
    applyFolderMenuHotkey(); // after applyFolderMenu: the menu it brings up
    applyScreenshots();
    restorePrintScreen();
    m_launcher->setContentOptions(m_settings.contentSearch, m_settings.contentExtensions,
        m_settings.contentSizeLimits(), m_settings.contentInLowPriority, m_settings.contentDocuments);
    m_launcher->setContentIndex(m_settings.contentIndex);
    m_launcher->setFileTypes(m_settings.namedFileTypes()); // after applyAppearance: the built-in ones' names
    applyPlugins();
    if (!initial) {
        m_indexOptionsApply.start();
        applyIndexFolder();
    }
}

void App::applyHotkey()
{
    m_messages->unregisterHotkey(kHotkeyId);
    if (m_settingsEditor && m_settingsEditor->recordingHotkey())
        return; // the keys being pressed must reach the settings window
    const QString hotkey = m_settings.hotkey.trimmed();
    QString hotkeyError;
    if (!hotkey.isEmpty() && !m_messages->registerHotkey(kHotkeyId, hotkey)) {
        UINT modifiers = 0;
        UINT vk = 0;
        hotkeyError = MessageWindow::parseHotkey(hotkey, &modifiers, &vk)
            ? tr("“%1” 已被其他程序或系统占用，请换一个。").arg(hotkey)
            : tr("无法识别“%1”，请重新设置。").arg(hotkey);
    }
    noteApplyProblem(u"Launcher/Hotkey"_s, hotkeyError);
    // Over a program run as administrator only a hotkey with a modifier
    // comes (docs/pitfalls.md): the one AdminTapHint names.
    UINT hotkeyModifiers = 0;
    UINT hotkeyKey = 0;
    m_hotkeyOverAdmin = !hotkey.isEmpty() && hotkeyError.isEmpty()
        && MessageWindow::parseHotkey(hotkey, &hotkeyModifiers, &hotkeyKey)
        && (hotkeyModifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN)) != 0;
    if (m_settingsEditor)
        m_settingsEditor->setHotkeyError(hotkeyError); // shown next to the shortcut
    if (!settingsShown() && !hotkeyError.isEmpty() && !m_aiApplying)
        m_messages->showNotification(tr("快捷键不可用"), hotkeyError);
}

// The folder menu's hotkey (GitHub #25). Over the launcher itself, the
// launcher goes first (as on Esc) and the menu is for the window Windows
// brings to the front after it: a window of ours in front could not have
// the keys taken for the menu (keyboard Raw Input, see KeyListener), and
// the user pressed the menu's key, not the launcher's. The launcher slides
// out: the menu waits for it to be gone.
void App::folderMenuHotkey()
{
    if (!m_folderMenu)
        return;
    if (m_folderMenu->isOpen() || !launcherShown() || !m_window->isActive()) {
        m_folderMenu->hotkeyPressed();
        return;
    }
    hideLauncher();
    auto wait = std::make_shared<std::function<void(int)>>();
    *wait = [this, wait](int left) {
        if (!m_folderMenu)
            return;
        if (m_window && m_window->isVisible() && left > 0) {
            QTimer::singleShot(30ms, this, [wait, left] { (*wait)(left - 1); });
            return;
        }
        m_folderMenu->hotkeyPressed();
    };
    (*wait)(20);
}

void App::applyFolderMenuHotkey()
{
    m_messages->unregisterHotkey(kFolderMenuHotkeyId);
    if (m_settingsEditor && m_settingsEditor->recordingHotkey())
        return; // the keys being pressed must reach the settings window
    const QString hotkey = m_settings.folderMenuHotkey.trimmed();
    QString hotkeyError;
    if (!hotkey.isEmpty() && !m_messages->registerHotkey(kFolderMenuHotkeyId, hotkey)) {
        UINT modifiers = 0;
        UINT vk = 0;
        hotkeyError = MessageWindow::parseHotkey(hotkey, &modifiers, &vk)
            ? tr("“%1” 已被其他程序或系统占用，请换一个。").arg(hotkey)
            : tr("无法识别“%1”，请重新设置。").arg(hotkey);
    }
    noteApplyProblem(u"Launcher/FolderMenuHotkey"_s, hotkeyError);
    if (m_settingsEditor)
        m_settingsEditor->setFolderMenuHotkeyError(hotkeyError); // shown next to the shortcut
    if (!settingsShown() && !hotkeyError.isEmpty() && !m_aiApplying)
        m_messages->showNotification(tr("快捷键不可用"), hotkeyError);
}

// ---- 截图历史 (jt): screenshots in the clipboard history, while the screenshot plugin is on ----

QString App::screenshotKeyword() const
{
    const QString word = m_settings.captureKeyword.trimmed();
    if (!screenshotPluginOn() || word.isEmpty() || !isValidWebKeyword(word))
        return {};
    // They keep theirs (the settings refuse the word; WinShun.ini may be
    // edited by hand).
    for (const WebShortcut& w : activeWebShortcuts()) {
        if (w.keyword.compare(word, Qt::CaseInsensitive) == 0)
            return {};
    }
    if (m_favorites) {
        for (const Favorite& f : m_favorites->items()) {
            if (f.keyword.compare(word, Qt::CaseInsensitive) == 0)
                return {};
        }
    }
    return word;
}

// The screenshot history (jt) is the clipboard history's pictures the
// capture took (Clip::screenshot), listed by the launcher in a keyword mode
// of its own; a plugin's keyword alike gives way to it.
void App::applyScreenshots()
{
    const QString keyword = screenshotKeyword();
    if (m_pluginHost) {
        QList<std::pair<QString, QString>> builtin;
        if (!keyword.isEmpty())
            builtin.append({keyword, tr("截图历史")});
        m_pluginHost->setBuiltinKeywords(builtin);
    }
    if (!m_launcher)
        return;
    Launcher::ScreenshotHooks hooks;
    if (!keyword.isEmpty()) {
        hooks.list = [this](const QString& words) {
            std::vector<Launcher::Screenshot> shots;
            if (!m_clipStore || !m_clipStore->isOpen())
                return shots;
            ClipFilter filter;
            filter.screenshots = true;
            filter.query = words;
            const std::vector<Clip>& clips = m_clipStore->clips();
            for (const int i : m_clipStore->find(filter)) {
                const Clip& c = clips[static_cast<std::size_t>(i)];
                shots.push_back({c.id, m_clipStore->imagePath(c.id), c.ocr, c.width, c.height, c.created});
            }
            return shots;
        };
        // As 粘贴 in the clipboard window: into what was in front when the
        // launcher opened (on the clipboard only, if nothing can take it).
        hooks.paste = [this](qint64 id) {
            if (m_clipStore->imagePending(id)) { // taken a moment ago: said where the user looks
                m_launcher->flash(tr("图片还在保存，请稍后再试"));
                return;
            }
            const HWND target = m_launcherTarget;
            emit m_launcher->dismissRequested();
            m_clipboard->pasteEntry(id, target);
        };
        hooks.copy = [this](qint64 id) { m_clipboard->pasteEntry(id, nullptr); };
        hooks.edit = [this](qint64 id) { pictureAction(u"edit"_s, id, {}, true); };
        hooks.remove = [this](qint64 id) { m_clipboard->removeEntries({id}); };
        hooks.undo = [this] {
            if (!m_clipboard->canUndo())
                return false;
            m_clipboard->undo();
            return true;
        };
    }
    m_launcher->setScreenshots(keyword, std::move(hooks));
}

// ---- plugins with windows of their own: the screenshot (GitHub #138) ---------------

namespace {

constexpr QStringView kScreenshotPlugin = u"winshun.capture";

// A picture the screenshot plugin hands over (clipboard_add_image): its
// pixels behind a small header (the plugin's writePicture), so neither side
// encodes a PNG for the other. Null if it is not one.
QImage readHandedPicture(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    qint32 header[6] {};
    if (file.read(reinterpret_cast<char*>(header), sizeof header) != qint64(sizeof header))
        return {};
    const auto [magic, version, width, height, stride, format] = header;
    // At most 2 GB of pixels: a long screenshot is tall, not that tall.
    if (magic != 0x474D4957 || version != 1 || format != QImage::Format_ARGB32_Premultiplied || width <= 0 || height <= 0
        || width > 65535 || stride < width * 4 || qint64(stride) * height > (qint64(1) << 31)
        || file.size() != qint64(sizeof header) + qint64(stride) * height)
        return {};
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};
    const qint64 row = qint64(width) * 4;
    for (int y = 0; y < height; ++y) {
        if (file.read(reinterpret_cast<char*>(image.scanLine(y)), row) != row || (stride > row && !file.skip(stride - row)))
            return {};
    }
    return image;
}

// `path` is a file inside `folder` (a plugin's data folder).
bool insideFolder(const QString& path, const QString& folder)
{
    if (path.isEmpty() || !QFileInfo(path).isAbsolute())
        return false;
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const QString base = QDir::cleanPath(QDir::fromNativeSeparators(folder)) + u'/';
    return clean.startsWith(base, Qt::CaseInsensitive) && !clean.contains(u"/../"_s);
}

mcp::ToolResult toolError(const QString& text)
{
    return {{}, true, text};
}

} // namespace

bool App::screenshotPluginOn() const
{
    return m_pluginHost && m_pluginHost->catalog().enabled(kScreenshotPlugin);
}

QString App::picturePlugin(const QString& verb) const
{
    if (!m_pluginHost)
        return {};
    for (const plugin::Entry& e : m_pluginHost->catalog().entries()) {
        if (e.manifest && e.manifest->pictures.contains(verb) && m_pluginHost->catalog().enabled(e.id))
            return e.id;
    }
    return {};
}

void App::pluginPicturesChanged()
{
    const bool pin = !picturePlugin(u"pin"_s).isEmpty();
    const bool edit = !picturePlugin(u"edit"_s).isEmpty();
    if (m_launcher)
        m_launcher->setPictureVerbs(pin, edit);
    if (m_clipboard)
        m_clipboard->setPictureVerbs(pin, edit);
    updateScreenshotPlugin();
}

// What the settings say of the screenshot plugin: the history's keyword (剪贴板
// page) and the hotkey row (常规 page).
void App::updateScreenshotPlugin()
{
    if (!m_settingsEditor || !m_pluginHost)
        return;
    const auto keys = m_pluginHotkeys.find(kScreenshotPlugin.toString());
    m_settingsEditor->setScreenshotPlugin(m_pluginHost->catalog().find(kScreenshotPlugin) != nullptr,
                                          screenshotPluginOn(), keys == m_pluginHotkeys.end() ? QStringList() : keys->second);
}

// 贴到屏幕 and 编辑 go to the plugin that does them (`picture`), with the
// picture's file, and its project when the entry has one; the plugin reads
// both from where they are.
void App::pictureAction(const QString& verb, qint64 clipId, const QString& path, bool screenshot)
{
    const QString plugin = picturePlugin(verb);
    if (plugin.isEmpty())
        return;
    QJsonObject params {{u"action"_s, verb}, {u"screenshot"_s, screenshot}};
    if (clipId > 0) {
        if (!m_clipStore || !m_clipStore->isOpen())
            return;
        if (m_clipStore->imagePending(clipId)) { // taken a moment ago: said where the user looks
            m_messages->showNotification(verb == u"edit" ? tr("编辑") : tr("贴到屏幕"), tr("图片还在保存，请稍后再试"));
            return;
        }
        const QString file = m_clipStore->imagePath(clipId);
        if (file.isEmpty() || !QFileInfo::exists(file)) {
            m_messages->showNotification(tr("没能打开这张图片"), tr("它已不在历史里，或读不出来。"));
            return;
        }
        params.insert(u"clipId"_s, clipId);
        params.insert(u"file"_s, QDir::toNativeSeparators(file));
        if (const QString project = m_clipStore->projectPath(clipId); !project.isEmpty() && QFileInfo::exists(project))
            params.insert(u"project"_s, QDir::toNativeSeparators(project));
        if (const Clip* clip = m_clipStore->clip(clipId))
            params.insert(u"screenshot"_s, clip->screenshot);
    } else {
        params.insert(u"file"_s, QDir::toNativeSeparators(path));
    }
    if (!m_pluginHost->request(plugin, u"picture"_s, params, {}, true))
        m_messages->showNotification(tr("截图插件还没准备好"), tr("请稍后再试。"));
}

// The tray icon is pressed: each plugin's submenu is asked for, and is here
// (a millisecond or two) before the button comes up and the menu is shown.
void App::prefetchTrayMenus()
{
    if (!m_pluginHost)
        return;
    for (const plugin::Entry& e : m_pluginHost->catalog().entries()) {
        if (!e.manifest || e.manifest->trayMenu.isEmpty())
            continue;
        if (!m_pluginHost->catalog().enabled(e.id)) {
            m_trayMenus.erase(e.id);
            continue;
        }
        m_pluginHost->request(e.id, u"trayMenu"_s, {}, [this, id = e.id](bool ok, const QJsonValue& result) {
            if (ok)
                m_trayMenus[id] = result.toObject().value(u"items"_s).toArray();
        });
    }
}

QJsonObject App::pluginLook() const
{
    return {{u"locale"_s, m_language}, {u"themeSetting"_s, m_settings.theme}, {u"corners"_s, m_settings.corners},
        {u"uiSize"_s, m_settings.uiSize}, {u"transparency"_s, m_settings.transparency}, {u"renderer"_s, m_settings.renderer}};
}

void App::setUpAppTools()
{
    std::vector<plugin::Host::AppTool> tools;
    tools.push_back({u"register_hotkeys"_s, plugin::Permission::Ui,
        [this](const QString& owner, const QString&, const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done) {
            const QJsonValue value = arguments.value(u"keys"_s);
            QStringList keys;
            if (value.isArray()) {
                for (const QJsonValue& key : value.toArray()) {
                    if (!key.isString()) {
                        done(toolError(u"keys must contain shortcut strings"_s));
                        return;
                    }
                    keys.append(key.toString());
                }
            }
            if (!value.isArray() || !PluginHotkeys::valid(keys)) {
                done(toolError(u"keys must contain at most 32 global shortcuts"_s));
                return;
            }
            // A key not allowed is reported in errors; the others still register.
            done({{{u"errors"_s, m_pluginKeyRelay->replace(owner, keys)}}});
        }});
    // A picture into the clipboard history: the plugin's file in its own
    // data folder (and the annotations beside it), read off the GUI thread
    // and deleted. `copy`: onto the clipboard too (else recorded only, as
    // Ctrl+S and F3 do); `screenshot`: for 截图历史. Nothing kept while the
    // history or its pictures are off, or paused, as the clipboard's own.
    tools.push_back({u"clipboard_add_image"_s, plugin::Permission::ClipboardWrite,
        [this](const QString&, const QString& dataDir, const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done) {
            const QString file = arguments.value(u"file"_s).toString();
            const QString project = arguments.value(u"project"_s).toString();
            if (!insideFolder(file, dataDir) || (!project.isEmpty() && !insideFolder(project, dataDir))) {
                done(toolError(u"file and project must be files in the plugin's data folder"_s));
                return;
            }
            const bool copy = arguments.value(u"copy"_s).toBool(true);
            const bool screenshot = arguments.value(u"screenshot"_s).toBool();
            const QString imageOrigin = arguments.value(u"origin"_s).toString();
            if (imageOrigin.toUtf8().size() > 96 || imageOrigin.contains(QChar::Null)) {
                done(toolError(u"origin must be at most 96 UTF-8 bytes without NUL"_s));
                return;
            }
            QPointer<App> self(this);
            std::thread([self, file, project, copy, screenshot, imageOrigin, done = std::move(done)]() mutable {
                QImage image = readHandedPicture(file);
                QByteArray bytes;
                if (!project.isEmpty()) {
                    QFile in(project);
                    if (in.open(QIODevice::ReadOnly))
                        bytes = in.readAll();
                }
                QFile::remove(file);
                if (!project.isEmpty())
                    QFile::remove(project);
                QMetaObject::invokeMethod(qApp, [self, image = std::move(image), bytes = std::move(bytes), copy, screenshot, imageOrigin,
                                                    done = std::move(done)]() mutable {
                    if (!self)
                        return;
                    if (image.isNull()) {
                        done(toolError(u"Not a picture Win顺 can read"_s));
                        return;
                    }
                    const bool remember = self->m_settings.clipboard && self->m_settings.clipboardImages && !self->m_clipboardPaused;
                    if (copy)
                        self->m_clipboard->copyImage(image, remember, screenshot, std::move(bytes), imageOrigin);
                    else if (remember)
                        self->m_clipboard->recordImage(image, std::move(bytes));
                    done({{{u"kept"_s, remember}}});
                }, Qt::QueuedConnection);
            }).detach();
        }});
    // Text onto the clipboard (a colour, the words read in a picture), into
    // the history as the clipboard's own copies.
    tools.push_back({u"clipboard_copy"_s, plugin::Permission::ClipboardWrite,
        [this](const QString&, const QString&, const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done) {
            const QString text = arguments.value(u"text"_s).toString();
            if (text.isEmpty() || text.size() > 1'000'000) {
                done(toolError(u"text must be 1 to 1,000,000 characters"_s));
                return;
            }
            m_clipboard->copyText(text, m_settings.clipboard && !m_clipboardPaused);
            done({});
        }});
    // An entry of the history onto the clipboard again (编辑's copy of a
    // picture left as it was), which moves it to the top.
    tools.push_back({u"clipboard_copy_entry"_s, plugin::Permission::ClipboardWrite,
        [this](const QString&, const QString&, const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done) {
            if (!m_clipboard->pasteEntry(arguments.value(u"id"_s).toInteger(), nullptr))
                done(toolError(u"No such entry"_s));
            else
                done({});
        }});
    // 识别文字并搜索: the launcher with the words.
    tools.push_back({u"show_launcher"_s, plugin::Permission::Ui,
        [this](const QString&, const QString&, const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done) {
            showLauncher(arguments.value(u"text"_s).toString().left(1000));
            done({});
        }});
    // 识别文字并网页搜索: Win顺's default web search.
    tools.push_back({u"web_search"_s, plugin::Permission::WinshunOpen,
        [this](const QString&, const QString&, const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done) {
            const QString text = arguments.value(u"text"_s).toString().simplified().left(1000);
            if (text.isEmpty()) {
                done(toolError(u"text is empty"_s));
                return;
            }
            if (!openWebSearch(text)) {
                if (m_messages)
                    m_messages->showNotification(tr("网页搜索插件未启用"),
                        tr("请在设置 → 插件中安装并启用网页搜索，并添加要搜索的网站。"), [this] { showWebSettings(); });
                done(toolError(tr("网页搜索插件未启用或没有可用的网站，请在设置 → 插件中安装并启用网页搜索")));
            } else
                done({});
        }});
    tools.push_back({u"show_ocr_setup"_s, plugin::Permission::Ui,
        [this](const QString&, const QString&, const QJsonObject&, std::function<void(mcp::ToolResult)> done) {
            showSettings([](QQuickWindow* window) {
                QMetaObject::invokeMethod(window, "showSetting",
                    Q_ARG(QVariant, window->property("clipboardPage")), Q_ARG(QVariant, QVariant(0)),
                    Q_ARG(QVariant, QVariant(QCoreApplication::translate("OcrPackageRow", "增强文字识别"))));
            });
            done({});
        }});
    m_pluginHost->setAppTools(std::move(tools));
}

// PrtScn taken over by a Win顺 before 0.17, whose value then is kept in
// [Capture] PrintScreenBefore: the screenshot plugin keeps it once the
// settings moved to it (MovedPlugins); until then, and without the plugin,
// it goes back to the Snipping Tool, so the key is not dead.
void App::restorePrintScreen()
{
    if (m_settings.capturePrintScreenBefore.isEmpty() || screenshotPluginOn())
        return;
    std::optional<DWORD> value;
    if (printscreen::decode(m_settings.capturePrintScreenBefore, &value) && !printscreen::write(value)) {
        qWarning() << "PrtScn: could not give it back to the Snipping Tool";
        return; // tried again at the next start
    }
    m_settings.capturePrintScreenBefore.clear();
    m_settings.save();
    if (m_settingsEditor) // its copy must not bring the old value back with its next change
        m_settingsEditor->setSettings(m_settings);
}

ClipboardWatcher::Options App::clipboardOptions() const
{
    return {m_settings.clipboard && !m_clipboardPaused, m_settings.clipboardImages, m_settings.clipboardExcludedApps};
}

// How a reconcile of Explorer's DisabledHotkeys came about (winv::Origin).
static winv::Origin winKeyOrigin(bool initial)
{
    if (!initial)
        return winv::Origin::Toggle;
    return Settings::profile().isEmpty() ? winv::Origin::Startup : winv::Origin::StartupOtherProfile;
}

void App::applyClipboard(bool initial)
{
    // Explorer gives Win+V up (or gets it back) when it next starts. A V we
    // did not put there is not ours to take out (winv::reconcile).
    m_winVRegistryFailed = !winv::reconcile(L'V', m_settings.clipboardWinV, winKeyOrigin(initial));
    m_clipStore->setLimits({m_settings.clipboardMaxItems, m_settings.clipboardMaxDays}, QDateTime::currentMSecsSinceEpoch());
    m_clipStore->setKeepsPlaceOnPaste(m_settings.clipboardKeepPlace);
    m_clipboard->items()->setMarksPasted(m_settings.clipboardKeepPlace);
    const bool reopening = m_settings.clipboard && !m_clipStore->isOpen();
    if (reopening && !m_clipStore->open(QDateTime::currentMSecsSinceEpoch()))
        qWarning() << "Clipboard history: cannot be kept";
    m_clipboard->setState(m_settings.clipboard, m_clipboardPaused, m_clipboardShortcut);
    m_clipWatcher->setOptions(clipboardOptions());
    m_missingFiles->setMode(m_settings.clipboard
            ? MissingFiles::modeOf(m_settings.clipboardMissingFiles) : MissingFiles::Mode::Keep);
    if (reopening && m_clipStore->isOpen())
        m_clipboard->resumeHistory();
    if (!m_settings.clipboard) {
        // A drag can still be inside OLE's nested loop. Retire the view and
        // history once it returns, without invalidating the drag's source.
        if (m_clipboard->dragging()) {
            QTimer::singleShot(100ms, this, [this] {
                if (!m_settings.clipboard)
                    applyClipboard();
            });
        } else {
            m_clipboard->setPinned(false);
            hideClipboard(false);
            m_clipPrewarming = false;
            m_clipReveal.cancel();
            m_clipboard->setWindow(nullptr);
            m_clipDisplay.reset();
            m_clipDisplayWanted = false;
            delete m_clipWindow.data();
            updateCompanions();
            m_clipFrame.reset();
            m_clipPlacement.reset();
            m_textCaret.reset();
            m_clipboard->suspendHistory();
            // An explicit import keeps its store until its last batch lands.
            if (!m_clipImport)
                m_clipStore->close();
            m_qml->trimComponentCache();
            m_idleTrim->poke();
        }
    }
    m_clipboard->historyChanged();
    if (m_settingsEditor)
        m_settingsEditor->setClipboardCount(m_clipStore->isOpen() ? static_cast<int>(m_clipStore->clips().size()) : -1);
    applyClipboardHotkeys();
}

void App::applyClipboardHotkeys()
{
    m_messages->unregisterHotkey(kWinVHotkeyId);
    m_messages->unregisterHotkey(kClipboardHotkeyId);
    // The digits first: one that is the clipboard's hotkey now is let go of
    // before that is registered (and all of them while one is recorded).
    applyQuickPasteHotkeys();
    if (m_settingsEditor && m_settingsEditor->recordingHotkey())
        return; // the keys being pressed must reach the settings window
    const bool winV = m_settings.clipboardWinV && m_messages->registerHotkey(kWinVHotkeyId, u"Win+V"_s);
    QString state = u"off"_s;
    if (m_settings.clipboardWinV) {
        state = winV ? u"on"_s : m_winVRegistryFailed ? u"failed"_s : u"waiting"_s; // waiting: Explorer still has it
    } else if (winv::releasedByExplorer(L'V')) {
        // A V that is not ours: Win+V is dead until it goes. Failed: taking it
        // out (还给 Windows, or ours as the switch went off) was not written.
        state = m_winVRegistryFailed ? u"failed"_s : u"blocked"_s;
    } else if (m_messages->registerHotkey(kWinVHotkeyId, u"Win+V"_s)) {
        m_messages->unregisterHotkey(kWinVHotkeyId); // nobody has it: Explorer takes it back when it restarts
        state = u"releasing"_s;
    }
    const QString hotkey = m_settings.clipboardHotkey.trimmed();
    QString hotkeyError;
    const bool custom = !hotkey.isEmpty() && m_messages->registerHotkey(kClipboardHotkeyId, hotkey);
    if (!hotkey.isEmpty() && !custom) {
        UINT modifiers = 0;
        UINT vk = 0;
        hotkeyError = MessageWindow::parseHotkey(hotkey, &modifiers, &vk)
            ? tr("“%1” 已被其他程序或系统占用，请换一个。").arg(hotkey)
            : tr("无法识别“%1”，请重新设置。").arg(hotkey);
    }
    m_winVState = state;
    noteApplyProblem(u"Clipboard/Hotkey"_s, hotkeyError);
    m_clipboardShortcut = winV ? u"Win+V"_s : custom ? hotkey : QString();
    m_clipboard->setState(m_settings.clipboard, m_clipboardPaused, m_clipboardShortcut);
    if (m_settingsEditor) {
        m_settingsEditor->setWinVState(state, winv::canRestartExplorer());
        m_settingsEditor->setClipboardHotkeyError(hotkeyError);
    }
    if (!settingsShown() && !hotkeyError.isEmpty() && !m_aiApplying) {
        m_messages->showNotification(tr("快捷键不可用"), hotkeyError);
    }
}

void App::applyQuickPasteHotkeys()
{
    if (!m_messages)
        return;
    // Only what changes is let go of or registered: one unregistered for a
    // moment, even to be registered again at once, lets that chord through
    // to the program in front (a browser's Ctrl+4 goes to its fourth tab,
    // GitHub #195). One still held is let go of only once it is up (at most
    // kDrainCapMs): its auto-repeats would be a fresh press there (#175).
    const QString& keys = m_settings.clipboardQuickPaste;
    const bool recording = m_settingsEditor && m_settingsEditor->recordingHotkey();
    const bool wanted = !recording && m_settings.clipboard && (keys == u"ctrl" || keys == u"alt");
    const QString modifier = keys == u"ctrl" ? u"Ctrl"_s : u"Alt"_s;
    const bool suspended = quickPasteSuspended();
    m_quickPasteSuspended = suspended;
    const unsigned own = wanted ? ownNumberDigits(keys) : 0;
    const bool sameChord = m_quickPasteChord == keys;
    const auto down = [](int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; };
    const int heldModifier = m_quickPasteChord == u"alt" ? VK_MENU : VK_CONTROL;
    const bool waitedEnough = m_quickPasteHeldSince.isValid()
        && m_quickPasteHeldSince.elapsed() >= static_cast<qint64>(hotkeysteps::kDrainCapMs);
    // Those registered: kept, held on to, or let go of.
    unsigned held = 0;
    m_quickPasteKeys = 0;
    for (int n = 1; n <= 9; ++n) {
        const unsigned bit = 1u << n;
        if (!(m_quickPasteRegistered & bit))
            continue;
        if (wanted && !(own & bit) && sameChord && !suspended) { // as it is
            m_quickPasteKeys |= bit;
        } else if (!waitedEnough && down(heldModifier) && down('0' + n)) {
            held |= bit; // pastes nothing, until let go
        } else {
            m_messages->unregisterHotkey(kQuickPasteHotkeyId + n);
            m_quickPasteRegistered &= ~bit;
        }
    }
    // The rest: registered, or found taken. Another chord only once the old
    // one's digits are all let go of.
    QStringList unavailable;
    for (int n = 1; wanted && (sameChord || !held) && n <= 9; ++n) {
        const unsigned bit = 1u << n;
        if (m_quickPasteRegistered & bit)
            continue;
        const QString chord = modifier + u'+' + QString::number(n);
        if ((own & bit) || !m_messages->registerHotkey(kQuickPasteHotkeyId + n, chord)) {
            unavailable.append(chord);
        } else if (suspended) {
            m_messages->unregisterHotkey(kQuickPasteHotkeyId + n); // free: the window in front has them
        } else {
            m_quickPasteRegistered |= bit;
            m_quickPasteKeys |= bit;
        }
    }
    if (!m_quickPasteRegistered)
        m_quickPasteChord.clear();
    else if (sameChord || !held)
        m_quickPasteChord = keys;
    if (!held) {
        m_quickPasteHeldSince.invalidate();
    } else if (!std::exchange(m_quickPasteRetrying, true)) {
        if (!m_quickPasteHeldSince.isValid())
            m_quickPasteHeldSince.start();
        QTimer::singleShot(std::chrono::milliseconds(hotkeysteps::kPollMs), this, [this] {
            m_quickPasteRetrying = false;
            applyQuickPasteHotkeys();
        });
    }
    // For a report (GitHub #195): a window of ours active with another
    // program in front (no longer suspending), the clipboard active with its
    // window hidden, a digit held on to. Written as it begins and ends.
    const QWindow* focus = QGuiApplication::focusWindow();
    DWORD pid = 0;
    ::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
    const bool foregroundOurs = pid == ::GetCurrentProcessId();
    const bool staleActive = m_clipboard->active() && !(m_clipWindow && m_clipWindow->isVisible());
    const bool odd = held || (focus && !foregroundOurs) || staleActive;
    const QString state = u"%1 %2 %3 %4"_s.arg(m_quickPasteKeys).arg(held).arg(suspended).arg(odd);
    if ((odd || m_quickPasteOdd) && state != std::exchange(m_quickPasteLogged, state)) {
        m_quickPasteOdd = odd;
        diagnostics::record(u"clipboard-quick-paste-keys"_s,
            {{u"keys"_s, static_cast<qint64>(m_quickPasteKeys)}, {u"held"_s, static_cast<qint64>(held)},
             {u"suspended"_s, suspended}, {u"chord"_s, keys}, {u"recording"_s, recording},
             {u"focus"_s, focus ? QString::fromLatin1(focus->metaObject()->className()) + u' ' + focus->objectName() : QString()},
             {u"clipboardActive"_s, m_clipboard->active()}, {u"clipboardShown"_s, !staleActive},
             {u"foregroundOurs"_s, foregroundOurs}});
    }
    const QString error = unavailable.isEmpty() ? QString()
        : tr("%1 已被其他快捷键占用，这几条无法直接粘贴；其余数字仍可用。可更换组合键，或调整冲突的快捷键。")
              .arg(unavailable.join(tr("、")));
    noteApplyProblem(u"Clipboard/QuickPaste"_s, error);
    if (m_settingsEditor) {
        m_settingsEditor->setClipboardQuickPasteError(error);
        if (QGuiApplication::focusWindow())
            m_settingsEditor->refreshNumberKeys();
    }
}

bool App::quickPasteSuspended() const
{
    if (m_clipboard->active())
        return true; // over the program in front too: its rows have the digits (KeyRouter)
    if (!QGuiApplication::focusWindow())
        return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
    return pid == ::GetCurrentProcessId();
}

void App::applyTaskbar(bool initial)
{
    // As Win+V (applyClipboard): Explorer gives Win+S up, or takes it back,
    // when it next starts.
    m_winSRegistryFailed = !winv::reconcile(L'S', m_settings.taskbarWinS, winKeyOrigin(initial));
    applyTaskbarHotkeys();

    applyTypeToSearch();
    applySearchBox();
}

void App::applyTypeToSearch()
{
    const TypeToSearch::Surfaces wanted {.startMenu = m_settings.taskbarStartTyping,
        .explorer = m_settings.explorerTyping,
        .opus = m_settings.opusTyping,
        .pasteFile = m_settings.pasteFileEnabled && m_settings.pasteFileCtrlV};
    if (m_typing && m_typing->surfaces() == wanted)
        return;
    m_typing.reset();
    if (wanted == TypeToSearch::Surfaces {})
        return;
    // Called on its thread: hop to the GUI thread.
    TypeToSearch::Callbacks callbacks;
    callbacks.typed = [this] { QMetaObject::invokeMethod(this, &App::startMenuTyped, Qt::QueuedConnection); };
    callbacks.typedInExplorer = [this](HWND root, HWND tab, unsigned vk, TypeToSearch::RequestId request) {
        QMetaObject::invokeMethod(
            this,
            [this, root, tab, vk, request] {
                qInfo().noquote() << u"Explorer typing: key 0x%1 in window 0x%2"_s.arg(vk, 2, 16, QLatin1Char('0'))
                                         .arg(reinterpret_cast<quintptr>(root), 0, 16);
                folderTyped(root, tab, false, request);
            },
            Qt::QueuedConnection);
    };
    callbacks.typedInOpus = [this](HWND root, HWND tab, unsigned vk, TypeToSearch::RequestId request) {
        QMetaObject::invokeMethod(
            this,
            [this, root, tab, vk, request] {
                qInfo().noquote() << u"Opus typing: key 0x%1 in window 0x%2, tab 0x%3"_s
                                         .arg(vk, 2, 16, QLatin1Char('0'))
                                         .arg(reinterpret_cast<quintptr>(root), 0, 16)
                                         .arg(reinterpret_cast<quintptr>(tab), 0, 16);
                folderTyped(root, tab, true, request);
            },
            Qt::QueuedConnection);
    };
    callbacks.pastedInto = [this](HWND view) {
        QMetaObject::invokeMethod(this, [this, view] { pasteAsFile(view); }, Qt::QueuedConnection);
    };
    // The keys held for a launcher whose keys are routed to it (GitHub
    // #203): into it as the router's would go, ahead of those, which come
    // after them from the router's thread. Gone meanwhile: to no one.
    callbacks.handedOver = [this](std::vector<KeyRouter::Key> keys) {
        QMetaObject::invokeMethod(this, [this, keys = std::move(keys)] {
            for (const KeyRouter::Key& key : keys)
                launcherRoutedKey(key);
        }, Qt::QueuedConnection);
    };
    m_typing = std::make_unique<TypeToSearch>(wanted, std::move(callbacks));
    m_typing->setLauncherRouted(launcherRouted());
}

void App::pasteAsFile(HWND view)
{
    const std::wstring stub =
        QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + u"/WinShunSearch.exe"_s).toStdWString();
    // The window as a number (main.cpp's --paste-file-in); what it says, it takes from Windows.
    const std::wstring args = L"--paste-file-in " + std::to_wstring(reinterpret_cast<std::uintptr_t>(view));
    if (!shell::startUnelevated(stub, args).valid())
        qWarning() << "Paste as file: WinShunSearch.exe did not start, error" << ::GetLastError();
}

// For the settings page: empty while the box is on the taskbar.
static QString searchBoxHiddenName(taskbar::SearchBox::Hidden why)
{
    static const char* const names[] = {"", "pending", "notaskbar", "unreadable", "offscreen", "vertical", "noroom"};
    return QString::fromLatin1(names[static_cast<int>(why)]);
}

void App::applySearchBox()
{
    const bool wanted = m_settings.taskbarSearchBox && taskbar::SearchBox::supported();
    if (wanted && !m_searchBox) {
        // Called on the box's thread: over to the GUI thread.
        taskbar::SearchBox::Callbacks callbacks;
        callbacks.pressed = [this](const taskbar::SearchBox::Press& press) {
            QMetaObject::invokeMethod(this, [this, press] { searchBoxPressed(press); }, Qt::QueuedConnection);
        };
        callbacks.dragged = [this](int anchor, int at) {
            QMetaObject::invokeMethod(this, [this, anchor, at] {
                if (m_launcher->inTaskbarBox())
                    emit m_launcher->selectionRequested(anchor, at);
            }, Qt::QueuedConnection);
        };
        callbacks.shownChanged = [this](taskbar::SearchBox::Hidden why) {
            QMetaObject::invokeMethod(this, [this, why] { setSearchBoxShown(why); }, Qt::QueuedConnection);
        };
        callbacks.caretMoved = [this] {
            QMetaObject::invokeMethod(this, [this] {
                if (m_launcher->inTaskbarBox())
                    QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
            }, Qt::QueuedConnection);
        };
        callbacks.wideChanged = [this](bool wide) {
            QMetaObject::invokeMethod(this, [this, wide] { setSearchBoxWide(wide); }, Qt::QueuedConnection);
        };
        callbacks.classicChanged = [this](bool classic) {
            QMetaObject::invokeMethod(this, [this, classic] { setClassicTaskbar(classic); }, Qt::QueuedConnection);
        };
        m_searchBox = std::make_unique<taskbar::SearchBox>(std::move(callbacks), taskbar::SearchBox::Look {});
        // Until the box has measured it (classicChanged).
        m_classicTaskbar = taskbar::classicTaskList(::FindWindowW(L"Shell_TrayWnd", nullptr)) != nullptr;
        if (m_settingsEditor)
            m_settingsEditor->setClassicTaskbar(m_classicTaskbar);
        setSearchBoxShown(taskbar::SearchBox::Hidden::Pending);
        updateSearchBoxLook();
    } else if (!wanted && m_searchBox) {
        if (m_launcher->inTaskbarBox())
            hideLauncher();
        m_searchBox.reset();
        setSearchBoxShown(taskbar::SearchBox::Hidden::Pending);
    }
}

void App::setClassicTaskbar(bool classic)
{
    if (m_classicTaskbar == classic)
        return;
    m_classicTaskbar = classic;
    if (m_settingsEditor)
        m_settingsEditor->setClassicTaskbar(classic);
}

void App::setSearchBoxShown(taskbar::SearchBox::Hidden why)
{
    using Hidden = taskbar::SearchBox::Hidden;
    if (!m_searchBox)
        why = Hidden::Pending; // a late word from a box gone since
    const bool shown = why == Hidden::None;
    m_searchBoxShown = shown;
    m_searchBoxHidden = why;
    if (m_settingsEditor)
        m_settingsEditor->setSearchBoxShown(searchBoxHiddenName(why));
    if (!shown && m_launcher->inTaskbarBox())
        hideLauncher(); // what is typed there would be out of sight
}

void App::setSearchBoxWide(bool wide)
{
    // Narrowed under what is being typed (the taskbar filled up): on in the
    // launcher's own field, the box looking open. Not back the other way
    // while it is open, under the user's hands.
    if (wide || !m_searchBox || !m_launcher->inTaskbarBox() || m_launcherClosing)
        return;
    m_launcher->setInTaskbarBox(false);
    m_searchBox->setText({.active = true});
    updateSearchBoxLook();
}

void App::updateSearchBoxLook()
{
    if (!m_searchBox)
        return;
    const QColor accent = SystemTheme::accentColor();
    taskbar::SearchBox::Look look;
    look.name = tr("Win顺 搜索").toStdWString();
    // One short line, typing there or not: the launcher's tabs say what it
    // searches (the scope's words here changed with it, a fade each time).
    look.placeholder = tr("想搜索什么…").toStdWString();
    look.label = tr("搜索").toStdWString();
    look.accent = RGB(accent.red(), accent.green(), accent.blue());
    look.monoIcon = m_settings.taskbarIcon == u"mono";
    look.roundedCorners = win::roundedCorners();
    using Style = taskbar::SearchBox::Style;
    look.style = m_settings.taskbarSearchBoxStyle == u"icon" ? Style::Icon
        : m_settings.taskbarSearchBoxStyle == u"label"       ? Style::Label
                                                             : Style::Box;
    m_searchBox->setLook(std::move(look));
}

void App::searchBoxPressed(const taskbar::SearchBox::Press& press)
{
    if (!m_window || !m_searchBox)
        return;
    // Pinned, a window of its own (not over the box, see showLauncher): the
    // box opens and closes it as double Ctrl does. In front a moment ago
    // (the press took the focus from it): hidden; else to the front.
    if (m_launcher->pinned() && launcherShown() && !m_launcherOverBox) {
        const bool wasInFront = m_window->isActive()
            || (m_pinnedLostToBox.isValid() && m_pinnedLostToBox.elapsed() < 500);
        m_pinnedLostToBox.invalidate();
        if (wasInFront)
            hideLauncher();
        else
            showLauncherInBox();
        return;
    }
    if (!launcherShown() || !m_launcherOverBox) {
        showLauncherInBox();
        return;
    }
    if (!m_launcher->inTaskbarBox()) { // its label or icon alone: a button, pressed again
        hideLauncher();
        return;
    }
    // Open over it already: the press took the keyboard (see m_boxPressGrace).
    // Back to the launcher, the caret where the box was pressed.
    m_boxPressGrace.stop();
    if (win::bringToFront(m_window) == win::Front::Refused)
        watchStrandedLauncher();
    m_window->requestActivate();
    if (press.at)
        emit m_launcher->caretRequested(*press.at, press.word);
}

void App::showLauncherInBox()
{
    const std::optional<RECT> box = m_searchBox ? m_searchBox->rect() : std::nullopt;
    if (!box) {
        showLauncher({}, taskbar::locate(false));
        return;
    }
    showLauncher({}, taskbar::locateAt({(box->left + box->right) / 2, box->top}), true);
}

void App::mirrorField(
    const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition)
{
    m_fieldShown.active = true;
    m_fieldShown.text = text.toStdWString();
    m_fieldShown.cursor = cursor;
    m_fieldShown.selectionStart = selectionStart;
    m_fieldShown.selectionEnd = selectionEnd;
    m_fieldShown.composition = composition.toStdWString();
    if (m_searchBox && m_launcher->inTaskbarBox() && !m_launcherClosing) // closing: the box is idle already
        m_searchBox->setText(m_fieldShown);
}

std::optional<QRectF> App::boxCaretInField() const
{
    const std::optional<RECT> caret = m_searchBox ? m_searchBox->caret() : std::nullopt;
    if (!caret || !m_window || !m_searchField)
        return std::nullopt;
    // Physical pixels on the screen to the window's logical ones, then the field's.
    POINT origin {};
    ::ClientToScreen(reinterpret_cast<HWND>(m_window->winId()), &origin);
    const qreal scale = m_window->devicePixelRatio();
    const QPointF inWindow((caret->left - origin.x) / scale, (caret->top - origin.y) / scale);
    return QRectF(m_searchField->mapFromScene(inWindow), QSizeF(1, (caret->bottom - caret->top) / scale));
}

void App::startMenuTyped()
{
    // Over the taskbar, where the Start menu was (showing it closes the
    // Start menu, see win::bringToFront), over Win顺's box if it is there;
    // the keys follow once it is ready for them.
    beginHandoff(u"start"_s);
    if (m_searchBoxShown)
        showLauncherInBox();
    else
        showLauncher({}, taskbar::locate(false));
    deliverTyping(); // pinned and up already: it may be now
}

void App::beginHandoff(const QString& surface)
{
    if (m_deliverTyping)
        m_handoff.outcome = u"replaced"_s; // asked for again before the keys went
    endHandoff(); // the last one's, should its watch still be on
    m_deliverTyping = true;
    m_handoff = {.surface = surface, .asked = std::chrono::steady_clock::now()};
}

void App::deliverTyping()
{
    using Clock = std::chrono::steady_clock;
    using std::chrono::milliseconds;
    if (!m_deliverTyping)
        return;
    if (!m_typing || !m_window) { // gone meanwhile: nothing left to wait for
        m_typingCap.stop();
        m_deliverTyping = false;
        m_handoff.outcome = u"gone"_s;
        endHandoff();
        return;
    }
    // Ready as a user's typing would find it (searchtyping::handoff): active,
    // its field Qt's focus object (the one with the window's input method
    // context), its first frame on the screen. In front too, which the hook
    // waits for as it types (GitHub #66).
    const Clock::time_point now = Clock::now();
    const HWND window = reinterpret_cast<HWND>(m_window->winId());
    const QObject* focus = QGuiApplication::focusObject();
    const searchtyping::LauncherState state {
        .active = m_window->isActive(),
        .fieldFocused = focus && (focus == m_searchField || focus->objectName() == u"panelSearchField"),
        .revealed = m_window->isVisible() && !m_launcherReveal.pending() && !m_prewarming.load() && !isCloaked(window),
        .preparing = m_launcherOpening,
        .routed = launcherRouted(),
    };
    const auto note = [now](Clock::time_point& at, bool came) {
        if (came && at == Clock::time_point {})
            at = now;
    };
    note(m_handoff.active, state.active);
    note(m_handoff.focused, state.active && state.fieldFocused);
    note(m_handoff.revealed, state.revealed);
    note(m_handoff.routed, state.routed);
    // Since it took typing: active, or (refused the front) its keys routed to it.
    const Clock::time_point took = state.routed ? m_handoff.routed : m_handoff.active;
    const bool wasActive = took != Clock::time_point {};
    std::optional<unsigned> sinceActive;
    if (wasActive)
        sinceActive = static_cast<unsigned>(std::chrono::duration_cast<milliseconds>(now - took).count());
    const searchtyping::Handoff handoff = searchtyping::handoff(state, sinceActive);
    if (handoff == searchtyping::Handoff::Wait) {
        // Never active: the hook's hold has dropped the keys by then.
        const Clock::time_point until
            = wasActive ? took + milliseconds(searchtyping::kHandoffCapMs)
                        : m_handoff.asked + milliseconds(searchtyping::kWaitForLauncherMs);
        if (!wasActive && now >= until) {
            m_deliverTyping = false;
            m_handoff.outcome = u"never-active"_s;
            endHandoff();
            return;
        }
        m_typingCap.start(std::max(1, static_cast<int>(std::chrono::ceil<milliseconds>(until - now).count())));
        return;
    }
    m_typingCap.stop();
    m_deliverTyping = false;
    m_handoff.delivered = now;
    m_handoff.outcome = state.routed ? u"routed"_s : handoff == searchtyping::Handoff::Deliver ? u"ready"_s : u"cap"_s;
    QStringList missing;
    if (!state.active)
        missing << u"active"_s;
    if (!state.fieldFocused)
        missing << u"focused"_s;
    if (!state.revealed)
        missing << u"revealed"_s;
    m_handoff.missing = missing.join(u',');
    m_handoff.inFront = ::GetForegroundWindow() == window;
    // Routed, the keys go as the layout of the window in front types them.
    m_handoff.layout = static_cast<quint32>(reinterpret_cast<quintptr>(::GetKeyboardLayout(
        state.routed ? ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr) : 0)));
    if (state.routed) {
        // Refused the front, its keys come through its router: the keys held
        // go to it as they are, never typed again (refused too, GitHub #203).
        // No input method composes them, nor the ones after (KeyRouter).
        m_typing->handOver();
    } else {
        lettersOnOpen(); // ahead of the keys, which go through the input method (at the cap, maybe not focused yet)
        m_typing->deliver();
    }
    m_keysWatch->start(window);
    m_handoffWatch.start();
}

void App::endHandoff()
{
    using Clock = std::chrono::steady_clock;
    m_handoffWatch.stop();
    const LauncherKeysWatch::Seen seen = m_keysWatch ? m_keysWatch->stop() : LauncherKeysWatch::Seen {};
    if (m_handoff.surface.isEmpty())
        return;
    // From the first key of the hold (the hook's), unless that is a later
    // hold's already; -1: did not come.
    const TypeToSearch::Timing timing = m_typing ? m_typing->timing() : TypeToSearch::Timing {};
    const bool sameHold = timing.firstKey != Clock::time_point {} && timing.firstKey <= m_handoff.asked;
    const Clock::time_point from = sameHold ? timing.firstKey : m_handoff.asked;
    const auto ms = [from](Clock::time_point at) {
        return at == Clock::time_point {} ? -1
                                          : static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(at - from).count());
    };
    const InputMethodInfo inputMethod = activeInputMethod(); // TSF's, off the keys' way (a moment after)
    const quint32 layout = m_handoff.layout ? m_handoff.layout : static_cast<quint32>(inputMethod.layout);
    QJsonObject details {
        {u"surface"_s, m_handoff.surface},
        {u"outcome"_s, m_handoff.outcome},
        {u"missing"_s, m_handoff.missing},
        {u"inFront"_s, m_handoff.inFront},
        {u"presses"_s, sameHold && timing.sent != Clock::time_point {} ? timing.presses : -1},
        {u"hkl"_s, u"0x%1"_s.arg(layout, 8, 16, u'0')},
        {u"tip"_s, QString::fromStdWString(inputMethod.service)},
        {u"tipName"_s, QString::fromStdWString(inputMethod.name)},
        {u"askedMs"_s, ms(m_handoff.asked)},
        {u"activeMs"_s, ms(m_handoff.active)},
        {u"focusedMs"_s, ms(m_handoff.focused)},
        {u"revealedMs"_s, ms(m_handoff.revealed)},
        {u"routedMs"_s, ms(m_handoff.routed)},
        {u"deliveredMs"_s, ms(m_handoff.delivered)},
        {u"sentMs"_s, ms(sameHold ? timing.sent : Clock::time_point {})},
        {u"keyMs"_s, ms(seen.key)},
        {u"imeTook"_s, seen.imeTook},
        {u"keyDowns"_s, seen.keys},
        {u"compositionMs"_s, ms(seen.composition)},
        {u"candidatesMs"_s, ms(seen.candidates)},
        {u"preeditMs"_s, ms(seen.preedit)},
        {u"textMs"_s, ms(seen.text)},
        {u"drawnMs"_s, ms(seen.drawn)},
    };
    if (!m_handoff.placed.isEmpty())
        details.insert(u"placed"_s, m_handoff.placed);
    m_handoff = {};
    diagnostics::record(u"typing-handoff"_s, details);
}

// Once an opening, when its field (the box's or the panel's) has the focus:
// Qt has given the window its input method context by then. The user
// switching back to Chinese in the box is not undone. Set however it reads
// (WeChat's Chinese can read as letters). Only Win顺's own context (this thread's):
// the program it came from keeps its input method as it was.
void App::lettersOnOpen()
{
    if (!m_lettersPending || !m_window || !m_window->isActive())
        return;
    const QObject* focus = QGuiApplication::focusObject();
    if (!focus || (focus != m_searchField && focus->objectName() != u"panelSearchField"))
        return;
    const win::LettersSwitch done = win::switchToLetters(reinterpret_cast<HWND>(m_window->winId()));
    if (!done.context)
        return; // none yet: when it has one
    m_lettersPending = false;
    if (!done.changed)
        return;
    // Asked for again while up (pinned): put back, on hiding, as it was first.
    const win::InputMode before = m_lettersSwitch.changed ? m_lettersSwitch.before : done.before;
    m_lettersSwitch = done;
    m_lettersSwitch.before = before;
    qInfo().noquote() << u"Input method to English on opening: open %1 -> %2, conversion 0x%3 -> 0x%4"_s
                             .arg(int(done.before.open))
                             .arg(int(done.after.open))
                             .arg(done.before.conversion, 0, 16)
                             .arg(done.after.conversion, 0, 16);
}

void App::restoreLauncherInputMode()
{
    m_lettersPending = false;
    win::restoreInputMode(std::exchange(m_lettersSwitch, {}));
}

void App::folderTyped(HWND root, HWND tab, bool opus, std::uint64_t request)
{
    if (!m_typing || !m_typing->pending(request))
        return;
    // Explorer's COM and Opus's DOpusRT query can wait on the file manager:
    // on a worker of its own, never the GUI thread. The hook holds keys meanwhile.
    if (!m_typingWorker)
        m_typingWorker = std::make_unique<ComWorker>();
    m_typingWorker->post([this, root, tab, opus, request] {
        QElapsedTimer clock;
        clock.start();
        const QString folder
            = QString::fromStdWString(opus ? filemanager::opusFolder(tab) : filemanager::explorerFolder(tab));
        qInfo().noquote() << (opus ? u"Opus typing:" : u"Explorer typing:")
                          << u"folder \"%1\" %2 ms"_s.arg(folder).arg(clock.elapsed());
        QMetaObject::invokeMethod(
            this, [this, root, tab, opus, request, folder] { typingFolderKnown(root, tab, opus, request, folder); },
            Qt::QueuedConnection);
    });
}

void App::typingFolderKnown(HWND root, HWND tab, bool opus, std::uint64_t request, const QString& folder)
{
    if (!m_typing || !m_typing->pending(request))
        return;
    // No folder on disk there (Home, This PC, the Recycle Bin, a search): the
    // keys were Explorer's after all, which jumps by them as it would have.
    GUITHREADINFO focus {sizeof focus};
    const bool sameFocus = ::GetForegroundWindow() == root
        && ::GetGUIThreadInfo(::GetWindowThreadProcessId(root, nullptr), &focus)
        && searchtyping::focusOf(static_cast<unsigned>(focus.flags), true) == searchtyping::Focus::ItemsView
        && (opus ? focus.hwndFocus == tab : folderview::classify(focus.hwndFocus, {}).tab == tab);
    if (folder.isEmpty() || !(opus ? m_settings.opusTyping : m_settings.explorerTyping) || !m_window || !sameFocus) {
        qInfo() << "Folder typing: no folder or focus changed; give back to the original view only";
        m_typing->giveBack(request);
        return;
    }
    // The folder typed, the words to come after it (as Explorer's "search
    // here", GitHub #20); on the monitor of that window, which need not be
    // the mouse's. The keys follow once the launcher is ready for them.
    beginHandoff(opus ? u"opus"_s : u"explorer"_s);
    showFolderSearch(root, tab, opus, folder);
    deliverTyping(); // pinned and up already: it may be now
}

void App::showFolderSearch(HWND root, HWND tab, bool opus, const QString& folder)
{
    // Typed in Opus, the box sits in the corner of its file list (as Listary's
    // does) rather than over it, and may search the whole PC (GitHub #82).
    const bool everywhere = opus && m_settings.opusTypingScope == u"all";
    const QString prefix = everywhere ? QString() : folder.endsWith(u'\\') ? folder + u' ' : folder + u"\\ "_s;
    // On the monitor of the list typed in (a lister over two may have the other on the other).
    // Typed in Explorer, beside its window, as long as all of the box keeps
    // clear of it there (GitHub #141); else, as in Opus, in the bottom right
    // corner of its file list, never in a corner of the screen (GitHub #203).
    const RECT list = opus ? RECT {} : folderview::explorerFileList(root, tab);
    // Pinned and up, or the panel: where it is (showLauncher).
    const QString kept = m_launcher->pinned() && launcherShown() ? u"pinned"_s : m_launcher->panel() ? u"panel"_s : QString();
    showLauncher(prefix, {}, false, screenOf(::MonitorFromWindow(opus ? tab : root, MONITOR_DEFAULTTONEAREST)), opus ? tab : root, opus,
        list);
    m_typingOrigin = {root, tab, opus, folder}; // after showLauncher, which clears it
    // For the journal's typing-handoff: how it was placed, the window typed
    // in, its file list and the work area (screen pixels), the box where Qt
    // put it (its pixels on that screen). No titles, no folders.
    const auto rectOf = [](const RECT& r) {
        return QJsonArray {int(r.left), int(r.top), int(r.right - r.left), int(r.bottom - r.top)};
    };
    QJsonObject placed {{u"by"_s, !kept.isEmpty()   ? kept
                                 : !m_placement->avoiding() ? u"home"_s
                                 : m_placement->avoidingInside() ? u"list"_s
                                                                 : u"beside"_s}};
    RECT bounds {};
    if (::GetWindowRect(root, &bounds))
        placed.insert(u"window"_s, rectOf(bounds));
    if (!opus)
        placed.insert(u"list"_s, rectOf(list));
    MONITORINFO monitor {sizeof monitor};
    if (::GetMonitorInfoW(::MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST), &monitor))
        placed.insert(u"work"_s, rectOf(monitor.rcWork));
    if (m_window) {
        const QRect box = m_window->geometry();
        placed.insert(u"box"_s, QJsonArray {box.x(), box.y(), box.width(), box.height()});
        placed.insert(u"scale"_s, m_window->devicePixelRatio());
    }
    m_handoff.placed = placed;
    // What is found in that folder first, then elsewhere (or there alone, as
    // set: Launcher::setFolderTypingOnly), as Listary lists it (GitHub #110).
    // After showLauncher too, which forgets the folder typed in before.
    m_launcher->setTypingFolder(prefix);
    followTypingSelection();
}

void App::cancelTypingSelection()
{
    m_typingSelectionTimer.stop();
    m_typingSelection.request_stop();
    m_typingSelection = std::stop_source {};
}

void App::followTypingSelection()
{
    cancelTypingSelection();
    if (m_typingOrigin.tab && !m_typingOrigin.opus)
        m_typingSelectionTimer.start();
}

bool App::revealInTypingTab(const QString& path)
{
    // Opus keeps the originating pane. Explorer follows the chosen manager.
    // A drive has nothing above it to select it in.
    const QFileInfo info(path);
    if ((!m_typingOrigin.opus && shell::fileManager() != filemanager::Kind::Explorer) || info.isRoot())
        return false;
    return openInTypingTab(QDir::toNativeSeparators(info.absolutePath()), QDir::toNativeSeparators(path));
}

bool App::openInTypingTab(const QString& folder, const QString& select)
{
    cancelTypingSelection();
    const TypingOrigin from = std::exchange(m_typingOrigin, {});
    if (!from.tab)
        return false;
    const HWND root = ::IsWindow(from.tab) ? ::GetAncestor(from.tab, GA_ROOT) : nullptr;
    if (!root || root != from.root) {
        qWarning() << "Folder typing: the tab typed in is gone; the folder opens in a new window";
        return false;
    }
    // Back to the front first (allowed: the launcher is in front), as Esc
    // does; the tab goes there on the worker that asks the file manager its folder,
    // which waits until it shows it: a key typed at once, asked behind it,
    // searches the new folder.
    ::SetForegroundWindow(root);
    if (!m_typingWorker)
        m_typingWorker = std::make_unique<ComWorker>();
    if (from.opus) {
        m_typingWorker->post([from, path = QDir::toNativeSeparators(folder), select] {
            if (!filemanager::goToInOpusView(from.root, from.tab, path.toStdWString(), select.toStdWString())) {
                qWarning()
                    << "Opus typing: cannot navigate the original view; open through the chosen file manager";
                if (select.isEmpty())
                    shell::open(path);
                else
                    shell::reveal(select);
            }
        });
        return true;
    }
    m_typingWorker->post([tab = from.tab, path = QDir::toNativeSeparators(folder), select] {
        QElapsedTimer clock;
        clock.start();
        if (filemanager::goToKeepingFocus(tab, path.toStdWString(), select.toStdWString())) {
            qInfo().noquote() << u"Explorer typing: the tab went to \"%1\", %2 ms"_s.arg(path).arg(clock.elapsed());
            return;
        }
        qWarning() << "Explorer typing: the tab is not among the shell's windows; a new window instead";
        if (select.isEmpty())
            shell::open(path);
        else
            shell::reveal(select);
    });
    return true;
}

void App::applyTaskbarHotkeys()
{
    m_messages->unregisterHotkey(kWinSHotkeyId);
    m_messages->unregisterHotkey(kScreenClipHotkeyId);
    if (m_settingsEditor && m_settingsEditor->recordingHotkey())
        return; // the keys being pressed must reach the settings window
    const bool winS = m_settings.taskbarWinS && m_messages->registerHotkey(kWinSHotkeyId, u"Win+S"_s);
    QString state = u"off"_s;
    if (m_settings.taskbarWinS) {
        state = winS ? u"on"_s : m_winSRegistryFailed ? u"failed"_s : u"waiting"_s; // waiting: Explorer still has it
    } else if (winv::releasedByExplorer(L'S')) {
        // An S that is not ours: Win+S and Win+Shift+S are dead until it goes.
        // Failed: taking it out (还给 Windows, or ours) was not written.
        state = m_winSRegistryFailed ? u"failed"_s : u"blocked"_s;
    } else if (m_messages->registerHotkey(kWinSHotkeyId, u"Win+S"_s)) {
        m_messages->unregisterHotkey(kWinSHotkeyId); // nobody has it: Explorer takes it back when it restarts
        state = u"releasing"_s;
    }
    // Explorer let go of every Win+S key, the screen capture's too: started
    // from here while Win+S is ours. Not while it goes back (releasing):
    // held as the new Explorer starts, it would be gone for good, Explorer
    // registers its keys once.
    // A plugin's shortcut (the screenshot's), if it is that very chord, wins.
    const QString screenClip = hotkeys::normalized(u"Win+Shift+S"_s);
    const bool pluginHas = std::ranges::any_of(m_pluginHotkeys, [&](const auto& entry) {
        return std::ranges::any_of(entry.second, [&](const QString& key) { return hotkeys::normalized(key) == screenClip; });
    });
    if (winS && !pluginHas)
        m_messages->registerHotkey(kScreenClipHotkeyId, u"Win+Shift+S"_s);
    m_winSState = state;
    if (m_settingsEditor)
        m_settingsEditor->setWinSState(state, winv::canRestartExplorer());
}

void App::toggleAtTaskbar(bool clicked)
{
    if (launcherShown() && m_window->isActive()) {
        hideLauncher();
        return;
    }
    // Clicking the button took the keyboard from the launcher, which went
    // away for it: that click closes the launcher, it does not open it
    // again. The button's program starts on the release, 0.1–0.3 s later.
    if (clicked && m_launcherHidden.isValid() && m_launcherHidden.elapsed() < 500)
        return;
    if (!clicked && m_searchBoxShown) // Win+S: into Win顺's box, as Windows' own goes into its box
        showLauncherInBox();
    else
        showLauncher({}, taskbar::locate(clicked));
}

void App::startScreenClip()
{
    // Not started from here, where it would run elevated: the taskbar
    // button's program starts it with the user's rights, as Explorer does.
    const QString button = QCoreApplication::applicationDirPath() + u"/WinShunSearch.exe"_s;
    if (QFileInfo::exists(button))
        shell::run(u'"' + QDir::toNativeSeparators(button) + u"\" --screenclip"_s);
    else
        shell::openUrl(u"ms-screenclip:///?source=HotKey"_s);
}

void App::clipCaptured(const ClipCapture& capture)
{
    if (!m_settings.clipboard || m_clipboardPaused)
        return; // turned off since the watcher read it
    m_clipStore->add(capture, QDateTime::currentMSecsSinceEpoch());
    m_clipboard->historyChanged();
    if (m_settingsEditor)
        m_settingsEditor->setClipboardCount(static_cast<int>(m_clipStore->clips().size()));
}

void App::restartExplorer()
{
    if (m_restartingExplorer)
        return;
    m_restartingExplorer = true;
    releaseWinChordsForExplorer();
    // Seconds of waiting for Explorer to end and to start again: not on the
    // GUI thread.
    if (m_explorerRestart.joinable())
        m_explorerRestart.join(); // the last one, over already
    m_explorerRestart = std::jthread([self = QPointer<App>(this)] {
        const bool back = winv::restartExplorer();
        QMetaObject::invokeMethod(qApp, [self, back] {
            if (self)
                self->explorerRestarted(back);
        }, Qt::QueuedConnection);
    });
}

void App::releaseWinChordsForExplorer()
{
    const std::pair<int, QString> ours[] = {
        {kHotkeyId, m_settings.hotkey},
        {kFolderMenuHotkeyId, m_settings.folderMenuHotkey},
        {kClipboardHotkeyId, m_settings.clipboardHotkey},
        {kWinVHotkeyId, u"Win+V"_s},
        {kWinSHotkeyId, u"Win+S"_s},
        {kScreenClipHotkeyId, u"Win+Shift+S"_s},
    };
    for (const auto& [id, chord] : ours) {
        UINT modifiers = 0;
        UINT vk = 0;
        if (!MessageWindow::parseHotkey(chord.trimmed(), &modifiers, &vk) || !(modifiers & MOD_WIN) || (vk != 'S' && vk != 'V'))
            continue;
        // A letter in DisabledHotkeys is one Explorer leaves alone.
        if (!winv::releasedByExplorer(static_cast<wchar_t>(vk)))
            m_messages->unregisterHotkey(id);
    }
}

void App::giveBackWinKey(const QString& letter)
{
    const wchar_t key = letter == u"V" ? L'V' : letter == u"S" ? L'S' : L'\0';
    if (key == L'\0')
        return;
    const bool ok = winv::giveBack(key);
    if (key == L'V') {
        m_winVRegistryFailed = !ok;
        applyClipboardHotkeys();
    } else {
        m_winSRegistryFailed = !ok;
        applyTaskbarHotkeys();
    }
    if (ok && winv::canRestartExplorer())
        restartExplorer();
}

void App::applyWinKeyedHotkeys()
{
    applyHotkey();
    applyFolderMenuHotkey();
}

void App::explorerRestarted(bool back)
{
    m_restartingExplorer = false;
    if (!back && winv::canRestartExplorer()) {
        m_messages->showNotification(tr("没能重启资源管理器"), tr("注销后重新登录 Windows 也能生效。"));
    } else if (!back) {
        m_messages->showNotification(tr("资源管理器没有重新启动"),
            tr("按 Ctrl+Shift+Esc 打开任务管理器，选“运行新任务”，输入 explorer 启动它；注销后重新登录也行。"));
    }
    // Win+V and Win+S are free once the old Explorer is gone; the new one
    // also says when it is up (shellRestarted).
    applyClipboardHotkeys();
    applyTaskbarHotkeys();
    if (!back)
        applyWinKeyedHotkeys(); // no new Explorer to race
}

void App::setClipboardPaused(bool paused)
{
    m_clipboardPaused = paused;
    m_clipWatcher->setOptions(clipboardOptions());
    m_clipboard->setState(m_settings.clipboard, m_clipboardPaused, m_clipboardShortcut);
}

void App::reloadSettings()
{
    const QString file = Settings::filePath();
    if (!m_settingsWatcher.files().contains(file))
        m_settingsWatcher.addPath(file); // editors that save by replacing the file drop the watch
    Settings fresh;
    fresh.load();
    if (fresh == m_settings)
        return; // our own save from the settings window
    const bool accessChanged = fresh.indexAccess != m_settings.indexAccess;
    m_settings = fresh;
    applySettings(false);
    if (m_settingsEditor)
        m_settingsEditor->setSettings(m_settings);
    if (m_onboardingEditor)
        m_onboardingEditor->setSettings(m_settings);
    if (accessChanged)
        applyIndexAccess();
}

void App::settingsEdited(const Settings& settings)
{
    const bool accessChanged = settings.indexAccess != m_settings.indexAccess;
    m_settings = settings;
    applySettings(false);
    if (m_onboardingEditor)
        m_onboardingEditor->setSettings(m_settings);
    m_backup->soon();
    if (accessChanged)
        applyIndexAccess();
}

bool App::settingsShown() const
{
    return m_settingsWindow && m_settingsWindow->isVisible();
}

// Made the first time, then kept: closed, it is hidden (SettingsWindow.onClosing)
// and puts itself back as a window made anew would be (resetForReopen). What
// changes meanwhile is either told to its editor as it happens (the hotkeys'
// state, the taskbar's search box, …) or read again after the first frame of
// each opening (settingsFirstFrame), not before it: opening again takes some
// 30 ms instead of 500.
void App::showSettings(const std::function<void(QQuickWindow*)>& at)
{
    const bool created = !m_settingsWindow;
    const bool opening = created || !m_settingsWindow->isVisible();
    if (opening) {
        m_settingsOpenClock.start();
        ++m_settingsOpening.count;
        m_settingsOpening.reused = !created;
        m_settingsOpening.firstFrameMs = -1;
    }
    // Centred on the screen under the mouse, as tall as it fits.
    const auto place = [](QQuickWindow* window) {
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        const QRect area = screen->availableGeometry();
        window->resize(window->width(), std::min(window->height(), area.height() - 40));
        window->setPosition(area.center() - QPoint(window->width() / 2, window->height() / 2));
    };
    if (!m_settingsWindow) {
        auto* editor = new SettingsEditor(m_settings, m_renderer);
        editor->useOcrPackage(m_ocrPackage.get());
        auto* frame = new WindowFrame(true, editor); // its own title bar; goes with the editor
        QQmlComponent component(m_qml.get(), u"WinShun"_s, u"SettingsWindow"_s);
        QObject* object = component.createWithInitialProperties({
            {u"editor"_s, QVariant::fromValue(editor)},
            {u"frame"_s, QVariant::fromValue(frame)},
            {u"updater"_s, QVariant::fromValue(m_updater.get())},
            {u"backup"_s, QVariant::fromValue(m_backup.get())},
        });
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << component.errorString();
            delete object;
            delete editor;
            return;
        }
        editor->setParent(window);
        editor->setWindow(window);
        editor->setPlaces(m_places.get()); // Windows' settings, when its search finds none of its own
        connect(editor, &SettingsEditor::edited, this, &App::settingsEdited);
        editor->setMonitor(m_health.get());
        connect(editor, &SettingsEditor::giveBackMemoryRequested, this, &App::giveMemoryBack);
        connect(editor, &SettingsEditor::recordingHotkeyChanged, this, &App::applyHotkey);
        connect(editor, &SettingsEditor::recordingHotkeyChanged, this, &App::applyClipboardHotkeys);
        connect(editor, &SettingsEditor::recordingHotkeyChanged, this, &App::applyTaskbarHotkeys);
        connect(editor, &SettingsEditor::recordingHotkeyChanged, this, &App::applyFolderMenuHotkey);
        connect(editor, &SettingsEditor::explorerRestartRequested, this, &App::restartExplorer);
        connect(editor, &SettingsEditor::giveBackRequested, this, &App::giveBackWinKey);
        connect(editor, &SettingsEditor::autostartRequested, this, &App::setAutostart);
        connect(editor, &SettingsEditor::clipboardClearRequested, this, [this, editor] {
            if (!m_clipStore->isOpen() && !m_clipStore->open(QDateTime::currentMSecsSinceEpoch()))
                return;
            m_clipStore->clearHistory();
            m_clipboard->historyChanged();
            editor->setClipboardCount(static_cast<int>(m_clipStore->clips().size()));
            if (!m_settings.clipboard && !m_clipImport)
                applyClipboard();
        });
        connect(editor, &SettingsEditor::clipboardImportRequested, this, [this, editor](const QString& path, int split) {
            clipimport::Report none;
            if (m_clipImport) {
                none.error = clipimport::Error::Busy;
                editor->setClipboardImportDone(none, {});
                return;
            }
            if (!m_clipStore || (!m_clipStore->isOpen() && !m_clipStore->open(QDateTime::currentMSecsSinceEpoch()))) {
                none.error = clipimport::Error::NoHistory;
                editor->setClipboardImportDone(none, {});
                return;
            }
            m_clipImport = std::make_unique<ClipImportJob>(m_clipStore.get());
            ClipImportJob* job = m_clipImport.get();
            connect(job, &ClipImportJob::progress, editor, &SettingsEditor::setClipboardImportProgress);
            // The window's rows are places in the store's list: told as soon as it changes.
            connect(job, &ClipImportJob::batchAdded, this, [this, guard = QPointer<SettingsEditor>(editor)] {
                m_clipboard->historyChanged();
                if (guard)
                    guard->setClipboardCount(static_cast<int>(m_clipStore->clips().size()));
            });
            connect(editor, &SettingsEditor::clipboardImportCancelled, job, &ClipImportJob::cancel);
            // The history is told even when the settings window was closed meanwhile.
            connect(job, &ClipImportJob::finished, this, [this, guard = QPointer<SettingsEditor>(editor)](const clipimport::Report& report, const QString& group) {
                m_clipboard->historyChanged();
                if (guard) {
                    guard->setClipboardCount(static_cast<int>(m_clipStore->clips().size()));
                    guard->setClipboardImportDone(report, group);
                }
                m_clipImport.release()->deleteLater();
                if (!m_settings.clipboard)
                    applyClipboard();
            });
            job->start(path, split == 0 ? clipimport::Split::Lines : split == 2 ? clipimport::Split::Whole : clipimport::Split::Paragraphs);
        });
        connect(editor, &SettingsEditor::restartRequested, this, [this] { restart({u"--settings"_s}); });
        connect(editor, &SettingsEditor::historyClearRequested, this, [this, editor] {
            m_launcher->clearHistory();
            editor->setHistoryCount(0);
        });
        editor->setLauncherWidthSet(m_placement->customWidth());
        connect(editor, &SettingsEditor::launcherWidthResetRequested, this, [this, editor] {
            m_placement->resetWidth();
            editor->setLauncherWidthSet(m_placement->customWidth());
        });
        connect(editor, &SettingsEditor::shareVerifyRequested, this,
            [this](const QString& path) { m_index->shares().verifyNow(path.toStdWString()); });
        connect(editor, &SettingsEditor::shareAcceptRequested, this,
            [this](const QString& path) { m_index->shares().acceptMissing(path.toStdWString()); });
        connect(editor, &SettingsEditor::folderVerifyRequested, this,
            [this](const QString& path) { m_index->verifyFolder(path.toStdWString()); });
        connect(editor, &SettingsEditor::signInRequested, this, [this](const QString& path) { signInToServer(path); });
        connect(editor, &SettingsEditor::elevatedRestartRequested, this,
            [this] { restart({u"--settings"_s, u"--settings-at"_s, u"indexAccess"_s}, Elevation::Elevated); });
        connect(editor, &SettingsEditor::indexServiceInstallRequested, this, &App::installIndexService);
        connect(editor, &SettingsEditor::diagnosticsExportRequested, this, [this] {
            if (m_diagnosticsExporting) {
                m_settingsEditor->finishDiagnosticsExport(tr("诊断包正在生成，完成后会打开文件夹"));
                return;
            }
            const auto log = diagnostics::current();
            if (!log) {
                m_settingsEditor->finishDiagnosticsExport(tr("诊断记录尚未初始化，请重新启动 Win顺 后重试"));
                return;
            }
            QJsonObject context {{u"content_search_enabled"_s, m_settings.contentSearch}, {u"content_index_enabled"_s, m_settings.contentIndex},
                {u"documents_enabled"_s, m_settings.contentDocuments},
                {u"include_system_folders"_s, m_settings.contentInLowPriority}};
            const KeyboardRawInput pieKeyboard = keyboardRawInput();
            context[u"drop_pie"_s] = QJsonObject {
                {u"enabled"_s, m_settings.dropPie},
                {u"runtime"_s, m_dropPie ? u"local"_s : m_dropPieHelper ? u"helper"_s : u"off"_s},
                {u"local_drop_target_ready"_s, m_dropPie && m_dropPie->dropReady()},
                {u"local_listener_active"_s, m_dropPie && m_keyListener && m_keyListener->isActive()},
                {u"local_keyboard_input"_s, !m_dropPie || !m_keyListener ? u"off"_s
                    : pieKeyboard.error ? u"unknown"_s : !pieKeyboard.registration ? u"none"_s
                    : pieKeyboard.registration->hwndTarget == m_keyListener->window() ? u"ok"_s : u"other"_s},
            };
            if (m_dropPieHelper)
                context[u"drop_pie_helper"_s] = m_dropPieHelper->state();
            if (const auto service = std::dynamic_pointer_cast<ntfs::ServiceVolumeAccess>(m_index->volumeAccess()))
                context[u"index_service_version"_s] = QString::fromStdWString(service->serviceVersion());
            context[u"index_service_state"_s] = QString::fromLatin1(ntfs::ServiceVolumeAccess::stateName(ntfs::ServiceVolumeAccess::state()));
            context[u"index_service_installable"_s] = userIsAdministrator();
            auto* index = m_index.get(); // export thread joins before this object is destroyed
            const auto access = m_index->volumeAccessKind();
            context[u"index_access"_s] = access == ntfs::VolumeAccess::Kind::Direct ? u"direct"_s
                : access == ntfs::VolumeAccess::Kind::Service ? u"service"_s : u"directory-walk"_s;
            // 索引哪些地方 (GitHub #52): in effect, as set, and each chosen folder's
            // last walk (no paths: they name the user).
            context[u"index_scope"_s] = m_foldersInEffect ? u"folders"_s : u"whole-disks"_s;
            context[u"index_scope_setting"_s] = m_settings.indexScope;
            context[u"folder_verify_hours"_s] = m_settings.folderVerifyHours;
            context[u"chosen_folder_count"_s] = static_cast<int>(m_settings.indexFolders.size());
            QJsonArray chosen;
            for (const IndexService::FolderStatus& f : m_index->folderStatuses()) {
                using FolderState = IndexService::FolderStatus::State;
                static constexpr std::pair<FolderState, const char*> kStates[] {{FolderState::Waiting, "waiting"},
                    {FolderState::Verifying, "verifying"}, {FolderState::Ready, "ready"}, {FolderState::NotFound, "not-found"},
                    {FolderState::Unreadable, "unreadable"}, {FolderState::Excluded, "excluded"},
                    {FolderState::Offline, "offline"}, {FolderState::WholeDrive, "whole-drive"}};
                const auto state = std::ranges::find(kStates, f.state, &std::pair<FolderState, const char*>::first);
                chosen.append(QJsonObject {
                    {u"state"_s, QString::fromLatin1(state != std::end(kStates) ? state->second : "?")},
                    {u"items"_s, static_cast<qint64>(f.items)},
                    {u"failed"_s, static_cast<qint64>(f.failed)},
                    {u"last_verified"_s, f.lastVerified ? QDateTime::fromSecsSinceEpoch(f.lastVerified).toString(Qt::ISODate) : QString()},
                    {u"took_ms"_s, static_cast<qint64>(f.took.count())},
                    {u"watching"_s, f.watching},
                });
            }
            context[u"chosen_folders"_s] = chosen;
            context[u"display"_s] = diagnostics::display(m_renderer, SystemTheme::backdropAvailable()); // fonts: this thread's
            if (m_clipWatcher)
                context[u"clipboard"_s] = m_clipWatcher->diagnosticStatus();
            // Installed plugins: id, version, switched on (nothing else: no settings, no paths).
            if (m_pluginHost) {
                QJsonArray plugins;
                for (const plugin::Entry& e : m_pluginHost->catalog().entries())
                    plugins.append(QJsonObject {{u"id"_s, e.id}, {u"version"_s, e.manifest ? e.manifest->version : QString()},
                        {u"enabled"_s, m_pluginHost->catalog().enabled(e.id)}, {u"usable"_s, e.usable()}});
                context[u"plugins"_s] = plugins;
            }
            const QString dataFolder = Settings::dataDir();
            const QString serviceFolder = QDir(QString::fromStdWString(win32::expandEnvironment(L"%ProgramData%")))
                                              .filePath(u"WinShun"_s);
            m_diagnosticsExporting = true;
            m_diagnosticsExport = std::jthread([this, log, context = std::move(context), index, dataFolder, serviceFolder]() mutable {
                context[u"file_index_ready"_s] = index->state() == IndexService::State::Ready;
                context[u"file_index_entries"_s] = static_cast<qint64>(index->itemCount());
                context[u"file_index_refreshing"_s] = index->isRefreshing();
                context[u"content"_s] = index->contentIndexer()->diagnosticStatus();
                QJsonArray disks;
                for (const auto& root : index->volumeRoots()) {
                    const VolumePlacement placement = placementOf(root);
                    disks.append(QJsonObject {{u"volume"_s, QString::fromStdWString(root)},
                        {u"type"_s, placement.seeks ? u"HDD-or-unknown"_s : u"SSD"_s}});
                }
                context[u"disks"_s] = disks;
                const auto exported = diagnostics::exportArchive(*log, dataFolder, serviceFolder, std::move(context));
                QMetaObject::invokeMethod(this, [this, exported] {
                    m_diagnosticsExporting = false;
                    if (m_settingsEditor)
                        m_settingsEditor->finishDiagnosticsExport(exported.error);
                    if (exported.error.isEmpty())
                        shell::open(QFileInfo(exported.path).absolutePath());
                }, Qt::QueuedConnection);
            });
        });
        connect(editor, &SettingsEditor::opusRestarted, this, [this] { // the settings window back over it
            if (m_settingsWindow && m_settingsWindow->isVisible()) {
                m_settingsWindow->raise();
                win::bringToFront(m_settingsWindow);
                m_settingsWindow->requestActivate();
            }
        });
        editor->plugins()->setLocale(m_language);
        editor->plugins()->setHost(m_pluginHost.get());
        editor->plugins()->setUpdates(m_pluginUpdates.get());
        editor->plugins()->setRecoveryState(m_pluginRecoveryMessage, m_pluginRecoveryBusy);
        connect(editor->plugins(), &PluginSettings::recoveryRequested, this, &App::installMovedPlugins);
        editor->favorites()->setCatalogs(m_apps.get(), m_places.get());
        editor->favorites()->setStore(m_favorites.get());
        m_settingsWindow = window;
        m_settingsEditor = editor;
        // What the app tells the editor as it changes, from now on (hidden or not):
        // where it stands now.
        updateScreenshotPlugin();
        applyHotkey(); // shows whether the current hotkey works
        applyClipboardHotkeys(); // ... and where Win+V stands
        applyTaskbarHotkeys(); // ... and Win+S
        applyFolderMenuHotkey(); // ... and the folder menu's
        editor->setSearchBoxShown(searchBoxHiddenName(m_searchBoxHidden));
        editor->setClassicTaskbar(m_classicTaskbar);

        prepareBackdrop(window); // once: the format is the window's for good
        window->create();
        window->installEventFilter(this); // pasting into its fields
        frame->setWindow(window);
        styleWindow(window, SystemTheme::backdropAvailable()); // applyTheme styles it again, hidden or not
        win::setDarkFrame(window, isDarkMode()); // likewise guardDarkFrames, every window of ours
        place(window);
        connect(window, &QWindow::visibleChanged, this, [this, window](bool visible) {
            if (!visible) {
                // Hidden under the pointer (its ✕ clicked): nothing left hovered for the next opening.
                QEvent leave(QEvent::Leave);
                QCoreApplication::sendEvent(window, &leave);
                m_contentStatusTimer.stop();
                m_idleTrim->poke(); // Qt may stay "active" after this close; do not wait for a focus event
            }
        });
    } else if (opening) {
        // Shown again. A hotkey another program held may be free now: tried
        // again where one did not work (working ones are left registered).
        if (!m_settingsEditor->hotkeyError().isEmpty())
            applyHotkey();
        if (!m_settingsEditor->folderMenuHotkeyError().isEmpty())
            applyFolderMenuHotkey();
        if (!m_settingsEditor->clipboardHotkeyError().isEmpty())
            applyClipboardHotkeys();
        // The size the user gave it this session stays, within the screen it
        // opens on; maximized it stays maximized, where it was (as Windows'
        // own windows reopen), never minimized.
        QQuickWindow* window = m_settingsWindow;
        if (window->windowStates() & Qt::WindowMinimized)
            window->setWindowStates(window->windowStates() & ~Qt::WindowMinimized);
        if (!(window->windowStates() & (Qt::WindowMaximized | Qt::WindowFullScreen)))
            place(window);
    }
    // Cheap, and on the first page or the next: before the first frame.
    m_settingsEditor->setHistoryCount(static_cast<int>(m_history->items().size()));
    m_settingsEditor->setClipboardCount(m_clipStore->isOpen() ? static_cast<int>(m_clipStore->clips().size()) : -1);
    m_contentStatusTimer.start();
    hideLauncher();
    if (opening) {
        // The rest waits for the first frame (or a second, should none come:
        // shown minimized). Activation waits for it too (windowActivated).
        m_settingsEditor->beginOpening();
        const int serial = m_settingsOpening.count;
        connect(m_settingsWindow, &QQuickWindow::frameSwapped, this, [this, serial] { settingsFirstFrame(serial); },
            Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection)); // emitted on the render thread
        QTimer::singleShot(1s, this, [this, serial] { settingsFirstFrame(-serial); });
    } else {
        // Already on show: read again now, as each showSettings() always did.
        refreshContentIndexStatus();
        refreshShareStatuses();
        refreshFolderStatuses();
        readAutostart();
    }
    // Where it opens, before it is shown: its pages are made when first needed
    // (SettingsWindow.loadPage), and showing makes the one current then.
    if (at)
        at(m_settingsWindow);
    if (m_settingsProbe) {
        // Isolated runtime QA: on screen, never activated or brought to the
        // front (bringToFront may send an empty mouse event).
        m_settingsWindow->setProperty("_q_showWithoutActivating", true);
        m_settingsWindow->show();
        return;
    }
    m_settingsWindow->show();
    m_settingsWindow->raise();
    win::bringToFront(m_settingsWindow);
    m_settingsWindow->requestActivate();
}

// `opening`: the serial of the opening whose first frame was drawn; negated
// when none came within a second (it is done all the same then).
void App::settingsFirstFrame(int opening)
{
    const bool drawn = opening > 0;
    if (std::abs(opening) != m_settingsOpening.count || !m_settingsEditor || !m_settingsEditor->opening())
        return; // done already, or a later opening's
    const qint64 ms = m_settingsOpenClock.elapsed();
    if (drawn)
        m_settingsOpening.firstFrameMs = ms;
    // One line per opening, in the diagnostics journal: made or shown again, how long to its first frame.
    diagnostics::record(u"settings-open"_s, {{u"reused"_s, m_settingsOpening.reused}, {u"firstFrameMs"_s, drawn ? ms : -1}});
    m_settingsEditor->finishOpening(); // the taskbar, file managers, AI clients, places offered …
    if (m_settingsWindow)
        QMetaObject::invokeMethod(m_settingsWindow, "refreshAfterOpen");
    refreshContentIndexStatus();
    refreshShareStatuses();
    refreshFolderStatuses();
    refreshIndexFolder(true); // measures the index folder
    refreshIndexAccess(); // asks the service manager
    refreshMcpStatus();
    readAutostart(); // changed outside WinShun since it was last read? (in the background)
    if (m_serviceWanted && m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None)
        probeService(); // installed meanwhile? Looked for now, not in five minutes
}

void App::showClipboardSettings()
{
    showSettings([](QQuickWindow* window) { QMetaObject::invokeMethod(window, "showClipboardPage"); });
}

void App::showShortcutSettings()
{
    showSettings([](QQuickWindow* window) { QMetaObject::invokeMethod(window, "showShortcutsPage"); });
}

void App::showWebSettings()
{
    showSettings([](QQuickWindow* window) { QMetaObject::invokeMethod(window, "showWebPage"); });
}

void App::showFileTypeSettings()
{
    showSettings([](QQuickWindow* window) { QMetaObject::invokeMethod(window, "showFileTypesPage"); });
}

void App::showFavoriteSettings(const QString& path)
{
    showSettings([&path](QQuickWindow* window) {
        QMetaObject::invokeMethod(window, "showFavoritesPage", Q_ARG(QVariant, QVariant(path)));
    });
}

void App::showPluginSettings(const QString& pluginId)
{
    showSettings([&pluginId](QQuickWindow* window) {
        QMetaObject::invokeMethod(window, "showPluginPage", Q_ARG(QVariant, QVariant(pluginId)));
    });
}

void App::showPluginUpdates()
{
    showSettings([](QQuickWindow* window) { QMetaObject::invokeMethod(window, "showPluginUpdates"); });
}

void App::showUpdate()
{
    showSettings();
    if (m_settingsWindow)
        QMetaObject::invokeMethod(m_settingsWindow, "showUpdateDialog");
}

void App::refreshContentIndexStatus()
{
    if (!m_settingsEditor) {
        m_contentStatusTimer.stop();
        return;
    }
    QString status;
    if (m_settings.contentSearch && m_settings.contentIndex) {
        const ContentIndex::Stats stats = m_index->contentIndex()->stats();
        // This PC's files alone: the network locations' are told on their cards (GitHub #114).
        std::size_t remote = 0;
        for (const remotecontent::Indexer::Status& s : m_index->shareContent())
            remote += s.progress.indexed;
        const int files = static_cast<int>(stats.documents - std::min(remote, stats.documents));
        if (m_index->readingContent())
            status = tr("正在建立索引… 已收录 %Ln 个文件", nullptr, files)
                + tr("，这一轮读到 %1 / %2")
                      .arg(QLocale().toString(static_cast<qulonglong>(m_index->contentIndexer()->progress().done)),
                          QLocale().toString(static_cast<qulonglong>(m_index->contentIndexer()->progress().total)));
        else if (files == 0)
            status = tr("文件索引一建好，就开始建立");
        else
            status = tr("已收录 %Ln 个文件，占用 %1 磁盘空间", nullptr, files)
                         .arg(QLocale().formattedDataSize(static_cast<qint64>(stats.segmentBytes + stats.textBytes), 0,
                             QLocale::DataSizeTraditionalFormat));
        if (stats.pending > 0 && !m_index->readingContent())
            status += tr("；%Ln 个文件有改动，稍后更新", nullptr, static_cast<int>(stats.pending));
    }
    m_settingsEditor->setContentIndexStatus(status);
}

// 〔现在就归还〕: this process and Win顺's own under it, busy or not (what the
// background reads next comes back by itself). Off the GUI thread: ~10 ms per
// 100 MB, a little more with several processes.
void App::giveMemoryBack()
{
    std::thread([self = QPointer<App>(this)] {
        const qint64 megabytes = memory::trimAll();
        QMetaObject::invokeMethod(qApp, [self, megabytes] {
            if (!self)
                return;
            self->m_idleTrim->noteTrim(megabytes);
            self->m_health->noteGiveBack(megabytes, self->m_idleTrim->lastTrim().atMs);
        }, Qt::QueuedConnection);
    }).detach();
}

void App::refreshShareStatuses()
{
    using State = ShareIndexer::State;
    std::vector<ShareIndexer::Status> statuses = m_index->shares().statuses();
    // A share read for the first time: told once it is, unless the settings,
    // where it was added, still show it.
    for (const ShareIndexer::Status& s : statuses) {
        const auto building = std::ranges::find(m_sharesBuilding, s.path);
        if (s.state == State::Building && building == m_sharesBuilding.end()) {
            m_sharesBuilding.push_back(s.path);
        } else if (s.state != State::Building && building != m_sharesBuilding.end()) {
            m_sharesBuilding.erase(building);
            const bool shown = m_settingsWindow && m_settingsWindow->isVisible();
            if (s.state == State::Online && !shown)
                m_messages->showNotification(tr("%1 可以搜索了").arg(QString::fromStdWString(s.name.empty() ? s.path : s.name)),
                    tr("已收录 %L1 项，之后有变化会自动更新。").arg(static_cast<qulonglong>(s.items)));
        }
    }
    if (m_settingsEditor) {
        m_settingsEditor->setShareStatuses(std::move(statuses));
        m_settingsEditor->setShareContentStatuses(m_index->shareContent()); // GitHub #114
    }
}

// Windows' own dialog for the user name and password of the server of
// `path`, then connecting with them in the background; asked again while
// they are wrong. The share is read again once connected.
void App::signInToServer(const QString& path, unsigned long error, const QString& user)
{
    const std::wstring host(unc::host(path.toStdWString()));
    if (host.empty())
        return;
    const HWND owner = m_settingsWindow ? reinterpret_cast<HWND>(m_settingsWindow->winId()) : nullptr;
    auto entered = std::make_shared<credentials::Entered>();
    const QString server = QString::fromStdWString(host);
    if (!credentials::prompt(owner, tr("连接到 %1").arg(server).toStdWString(),
            tr("输入 %1 上的账户（例如 NAS 的用户名）和密码。勾选“记住我的凭据”，资源管理器以后也不用再输入。")
                .arg(server)
                .toStdWString(),
            error, user.toStdWString(), *entered))
        return;
    std::thread([this, guard = QPointer<QObject>(this), path, entered] {
        const unsigned long result = credentials::connect(path.toStdWString(), *entered);
        // An http WebDAV server that takes the password in the clear alone,
        // which Windows does not send (BasicAuthLevel): Windows says the
        // folder is not there (67), whatever is typed; the server, asked,
        // wants a password (GitHub #114).
        const bool clear = result != NO_ERROR && webdav::httpSignInBlocked(path.toStdWString())
            && webdav::wantsClearPassword(path.toStdWString());
        QMetaObject::invokeMethod(qApp, [this, guard, path, entered, result, clear] {
            if (!guard)
                return;
            // Asked again, it would fail again, and say nothing of why.
            if (clear) {
                if (m_settingsEditor)
                    m_settingsEditor->setSignedIn(path, SettingsEditor::httpSignInText(true));
                return;
            }
            if (result == ERROR_LOGON_FAILURE || result == ERROR_INVALID_PASSWORD || result == ERROR_ACCOUNT_RESTRICTION) {
                signInToServer(path, result, QString::fromStdWString(entered->user)); // wrong: again, saying so
                return;
            }
            QString problem;
            if (result != NO_ERROR) {
                const share::Problem p = share::classify(result, true);
                problem = p == share::Problem::Offline ? tr("连不上 %1").arg(QString::fromStdWString(std::wstring(unc::host(path.toStdWString()))))
                    : p == share::Problem::GuestBlocked ? tr("这台服务器只允许来宾访问，Windows 不允许这样连接")
                    : p == share::Problem::NotFound     ? tr("服务器上没有这个共享文件夹")
                                                        : tr("没有连上（错误 %1）").arg(result);
            }
            if (m_settingsEditor)
                m_settingsEditor->setSignedIn(path, problem);
            if (result != NO_ERROR)
                return;
            // Every share on the server is worth trying again now.
            const std::wstring_view host = unc::host(path.toStdWString());
            for (const ShareIndexer::Status& s : m_index->shares().statuses()) {
                if (s.state == ShareIndexer::State::Problem && unc::host(s.path).size() == host.size()
                    && ::_wcsnicmp(unc::host(s.path).data(), host.data(), host.size()) == 0)
                    m_index->shares().verifyNow(s.path);
            }
        });
    }).detach();
}

QString App::startIndexFolder()
{
    const QString dataDir = folderPath(Settings::dataDir());
    const QString wanted = folderPath(m_settings.indexDir());
    m_indexFolder = m_settings.indexFolder;
    m_indexDir = dataDir;
    if (sameFolder(wanted, dataDir))
        return m_indexDir;
    const QString native = QDir::toNativeSeparators(wanted);
    // A disk that is not there today (not plugged in, not unlocked yet): the
    // default place for now, and the setting is kept for the next start.
    const auto later = [&] {
        m_indexFolderProblem
            = tr("%1 现在用不了，这次先把索引放在默认位置，下次启动 Win顺 时再试。").arg(native);
        return m_indexDir;
    };
    if (driveType(wanted) == DRIVE_NO_ROOT_DIR)
        return later();
    // A place that will not do (an external disk, say; set in the INI file):
    // back to the default one, setting and all.
    QString problem = indexFolderProblem(wanted);
    if (problem.isEmpty() && !indexfolder::onlyIndexFiles(wanted))
        problem = tr("那里已有别的文件");
    if (!problem.isEmpty()) {
        m_indexFolderProblem = tr("索引不能放在 %1：%2。已改回默认位置。").arg(native, problem);
        m_settings.indexFolder.clear();
        m_settings.save();
        m_indexFolder.clear();
        return m_indexDir;
    }
    if (!QDir().mkpath(wanted))
        return later();
    if (indexfolder::hasIndex(wanted) || !indexfolder::hasIndex(dataDir)) {
        indexfolder::remove(dataDir); // left from a start without that disk, or a move cut short
        m_indexDir = wanted;
    } else {
        // Still in the default place (the setting was changed while Win顺 was
        // not running): moved there once it runs.
        m_indexFolder.clear();
    }
    return m_indexDir;
}

void App::applyIndexFolder()
{
    // Not before the snapshot has been read (the worker would wait for the
    // move, and stopWorker() for the worker): stateChanged() comes then.
    // After a move, indexMoved() looks again.
    const IndexService::State state = m_index->state();
    if (state == IndexService::State::Idle || state == IndexService::State::Loading || m_index->moving()
        || m_settings.indexFolder == m_indexFolder)
        return;
    const QString to = folderPath(m_settings.indexDir());
    if (sameFolder(to, m_indexDir)) { // written another way, or back where the files stayed
        m_indexFolder = m_settings.indexFolder;
        m_indexFolderProblem.clear();
        refreshIndexFolder();
        return;
    }
    const QString native = QDir::toNativeSeparators(to);
    const bool toDefault = sameFolder(to, Settings::dataDir());
    QString problem = toDefault ? QString() : indexFolderProblem(to);
    if (problem.isEmpty() && !toDefault && !indexfolder::onlyIndexFiles(to))
        problem = tr("%1 里已有别的文件，请选择一个空文件夹").arg(native);
    if (problem.isEmpty()) {
        const auto needed = static_cast<qint64>(indexfolder::size(m_indexDir)) + (64ll << 20); // and room to grow
        const QStorageInfo disk(to.left(3));
        if (disk.isValid() && disk.bytesAvailable() < needed) {
            const auto size = [](qint64 bytes) {
                return QLocale().formattedDataSize(bytes, 0, QLocale::DataSizeTraditionalFormat);
            };
            problem = tr("%1 盘的剩余空间不够：索引需要 %2，只剩 %3")
                          .arg(to.left(1).toUpper(), size(needed), size(disk.bytesAvailable()));
        }
    }
    if (!problem.isEmpty()) {
        indexMoveFailed(m_settings.indexFolder, problem);
        return;
    }
    m_indexMoveFolder = m_settings.indexFolder;
    m_indexMoveDir = to;
    m_indexMoveMadeDir = !QFileInfo::exists(to);
    m_indexMoveProgress = 0;
    m_indexFolderProblem.clear();
    m_index->moveTo(to);
    refreshIndexFolder();
}

QString App::indexFolderProblem(const QString& folder) const
{
    // On a disk built into this computer: "D:/...".
    if (driveRoot(folder).isEmpty())
        return tr("%1 不在这台电脑的硬盘上").arg(QDir::toNativeSeparators(folder));
    const QString external = tr("U 盘、移动硬盘这类外接的磁盘拔下或弹出后就用不了索引了，请选择电脑内置的硬盘");
    switch (driveType(folder)) {
    case DRIVE_FIXED: // 移动硬盘 as well
        return indexfolder::onExternalDisk(folder) ? external : QString();
    case DRIVE_NO_ROOT_DIR:
        return tr("找不到 %1 盘").arg(folder.left(1).toUpper());
    case DRIVE_REMOVABLE:
        return external;
    case DRIVE_REMOTE:
        return tr("网络位置不能存放索引，请选择这台电脑硬盘上的文件夹");
    default:
        return tr("%1 盘不能存放索引，请选择这台电脑硬盘上的文件夹").arg(folder.left(1).toUpper());
    }
}

void App::indexMoved(int error)
{
    if (error == ERROR_CANCELLED)
        return; // Win顺 is closing
    if (error == 0) {
        const QString from = std::exchange(m_indexDir, m_indexMoveDir);
        m_indexFolder = m_indexMoveFolder;
        if (!sameFolder(from, Settings::dataDir()))
            QDir().rmdir(from); // a folder of its own, empty now
        refreshIndexFolder(true);
        if (!m_settingsWindow || !m_settingsWindow->isVisible()) {
            m_messages->showNotification(
                tr("索引已经移好"), tr("现在存放在 %1").arg(QDir::toNativeSeparators(m_indexDir)));
        }
    } else {
        if (m_indexMoveMadeDir)
            QDir().rmdir(m_indexMoveDir); // made for the move (and empty again)
        indexMoveFailed(m_indexMoveFolder,
            tr("没能把索引移到 %1：%2")
                .arg(QDir::toNativeSeparators(m_indexMoveDir), qt_error_string(error).trimmed()));
    }
    applyIndexFolder(); // the setting may have changed again meanwhile
}

// The files stay where they are, and so the setting goes back to that
// (unless it was changed again since `attempted`). Why is shown next to it
// in the settings window, or in a notification.
void App::indexMoveFailed(const QString& attempted, const QString& problem)
{
    m_indexFolderProblem = problem;
    if (m_settings.indexFolder == attempted) {
        m_settings.indexFolder = m_indexFolder;
        m_settings.save();
        m_indexOptionsApply.start(); // the folder left out of searches goes back too
        if (m_settingsEditor)
            m_settingsEditor->setSettings(m_settings);
    }
    if (m_settingsWindow && m_settingsWindow->isVisible())
        refreshIndexFolder();
    else
        m_messages->showNotification(tr("索引没有移动"), problem);
}

void App::refreshIndexFolder(bool measure)
{
    if (!m_settingsEditor)
        return;
    if (measure) {
        m_indexBytes = indexfolder::size(m_indexDir);
        m_indexOnHardDisk = indexfolder::onSpinningDisk(m_indexDir) == true;
    }
    SettingsEditor::IndexFolderState state;
    state.folder = QDir::toNativeSeparators(m_indexDir);
    state.isDefault = m_settings.indexFolder.isEmpty();
    if (m_indexBytes > 0)
        state.size = QLocale().formattedDataSize(
            static_cast<qint64>(m_indexBytes), 0, QLocale::DataSizeTraditionalFormat);
    state.moveProgress = m_index->moving() ? m_indexMoveProgress : -1;
    state.problem = m_indexFolderProblem;
    if (m_indexOnHardDisk) // nothing to choose: a hint, not a warning (IndexService::prewarmSoon)
        state.note = tr("这是机械硬盘：文件名搜索不受影响；内容索引会在后台预读进内存，硬盘休眠后第一次内容搜索可能要等它转起来");
    m_settingsEditor->setIndexFolderState(state);
}

void App::handleCommand(const QString& command)
{
    if (command == u"toggle") {
        beginTrigger(TriggerSource::Command, ::GetTickCount64()); // its time is when it got here
        toggleLauncher();
    } else if (command == u"quit")
        // Explicit exit must bypass the retained windows' Close vetoes.
        // App's destructor saves the index and disposes of those windows.
        QCoreApplication::exit(0);
    else if (QString place; settingscommand::parse(command, &place)) {
        showSettings([&place](QQuickWindow* window) {
            if (!place.isEmpty())
                QMetaObject::invokeMethod(window, "showPlace", Q_ARG(QVariant, QVariant(place)));
        });
    } else if (command.startsWith(u"query:"))
        showLauncher(command.mid(6));
    else if (command.startsWith(u"dump:"))
        dumpLauncher(command.mid(5));
    else
        showLauncher();
}

void App::dumpLauncher(const QString& file) const
{
    QJsonObject state;
    state[u"visible"_s] = m_window && m_window->isVisible();
    state[u"panel"_s] = m_launcher->panel();
    state[u"pinned"_s] = m_launcher->pinned();
    state[u"active"_s] = m_window && m_window->isActive();
    // Up from typing in a file manager: the original file display; "0" otherwise.
    state[u"typingTab"_s] = QString::number(reinterpret_cast<quintptr>(m_typingOrigin.tab), 16);
    state[u"typingManager"_s] = m_typingOrigin.tab ? (m_typingOrigin.opus ? u"dopus"_s : u"explorer"_s) : QString();
    state[u"doubleCtrlService"_s] = m_keyListener && m_keyListener->serviceActive();
    if (m_window) {
        const QRect g = m_window->geometry();
        state[u"geometry"_s] = QJsonArray {g.x(), g.y(), g.width(), g.height()};
        RECT r {};
        ::GetWindowRect(reinterpret_cast<HWND>(m_window->winId()), &r);
        state[u"native"_s] = QJsonArray {int(r.left), int(r.top), int(r.right - r.left), int(r.bottom - r.top)};
        state[u"dpr"_s] = m_window->devicePixelRatio();
        state[u"boxWidth"_s] = m_placement->boxWidth();
        state[u"customWidth"_s] = m_placement->customWidth();
        // The tip over a row whose name is cut (NameTip.qml): up, where, saying what.
        if (const auto* tip = m_window->findChild<QQuickWindow*>(u"nameTip"_s)) {
            const QRect t = tip->geometry();
            state[u"tip"_s] = QJsonObject {{u"visible"_s, tip->isVisible()},
                {u"geometry"_s, QJsonArray {t.x(), t.y(), t.width(), t.height()}},
                {u"name"_s, tip->property("name").toString()}, {u"folder"_s, tip->property("folder").toString()},
                {u"details"_s, tip->property("details").toString()}};
        }
    }
    state[u"query"_s] = m_launcher->query();
    state[u"regexMode"_s] = m_launcher->regexMode();
    state[u"scope"_s] = static_cast<int>(m_launcher->scope());
    state[u"fileType"_s] = m_launcher->fileType();
    state[u"dateChoice"_s] = m_launcher->dateChoice();
    state[u"sizeChoice"_s] = m_launcher->sizeChoice();
    state[u"folderFilter"_s] = m_launcher->folderFilter();
    state[u"sortKey"_s] = m_launcher->panelSortKey();
    state[u"sortDescending"_s] = m_launcher->panelSortDescending();
    state[u"totalMatches"_s] = m_launcher->totalMatches();
    state[u"status"_s] = m_launcher->statusText();
    state[u"searching"_s] = m_launcher->searching();
    state[u"selected"_s] = m_launcher->results()->selectedCount();
    state[u"currentRow"_s] = m_launcher->currentRow();
    state[u"keyHints"_s] = QJsonObject::fromVariantMap(m_launcher->keyHints()); // Enter, the second key, Tab: what they do on the current row
    // Typed in a folder's window (GitHub #110): that folder, whether only it
    // is searched, where 其他位置 begins, and the hint rows under the list.
    state[u"typingFolder"_s] = m_launcher->typingFolder();
    state[u"typingFolderOnly"_s] = m_launcher->typingFolderOnly();
    state[u"typingFolderLevel"_s] = m_launcher->typingFolderLevel();
    state[u"elsewhereRow"_s] = m_launcher->results()->elsewhereRow();
    state[u"leftOutHints"_s] = QJsonArray::fromVariantList(m_launcher->leftOutHints());
    state[u"elapsedUs"_s] = static_cast<double>(m_launcher->elapsedUs());
    QJsonArray rows;
    const ResultModel* model = m_launcher->results();
    for (int i = 0; i < model->count(); ++i) {
        const SearchResult* r = model->at(i);
        const QModelIndex at = model->index(i);
        QJsonObject row {{u"name"_s, r->name}, {u"path"_s, r->path}, {u"isDir"_s, r->isDir},
            {u"size"_s, static_cast<double>(r->size)}, {u"sizeText"_s, model->data(at, ResultModel::SizeRole).toString()},
            {u"modified"_s, static_cast<double>(r->modified)},
            {u"modifiedDate"_s, model->data(at, ResultModel::ModifiedDateRole).toString()}, {u"app"_s, r->isApp()},
            {u"elsewhere"_s, r->elsewhere}};
        if (!r->switchId.isEmpty()) { // a system switch's row: its title, what it asks, and its state as the row draws it
            static constexpr const char16_t* kAvailability[] {u"unknown", u"ready", u"noDevice", u"denied", u"blocked", u"unsupported"};
            static constexpr const char16_t* kKind[] {u"toggle", u"on", u"off", u"setLevel", u"stepLevel", u"awakeFor", u"awakeUntil", u"openSettings"};
            const SwitchState switchState = m_switches ? m_switches->state(r->switchId) : SwitchState {};
            row[u"switchId"_s] = r->switchId;
            row[u"title"_s] = model->data(at, ResultModel::NameRole).toString();
            row[u"secondLine"_s] = model->data(at, ResultModel::FolderRole).toString();
            row[u"intent"_s] = QString::fromUtf16(kKind[static_cast<int>(r->intent.kind)]);
            row[u"intentValue"_s] = r->intent.value;
            row[u"switchAvailability"_s] = QString::fromUtf16(kAvailability[static_cast<int>(switchState.availability)]);
            row[u"switchReady"_s] = model->data(at, ResultModel::SwitchReadyRole).toBool();
            row[u"switchOn"_s] = model->data(at, ResultModel::SwitchOnRole).toInt();
            row[u"switchLevel"_s] = model->data(at, ResultModel::SwitchLevelRole).toInt();
            row[u"switchBusy"_s] = model->data(at, ResultModel::SwitchBusyRole).toBool();
            row[u"switchNote"_s] = model->data(at, ResultModel::SwitchNoteRole).toString();
            row[u"switchError"_s] = model->data(at, ResultModel::SwitchErrorRole).toString();
            row[u"openInstead"_s] = switchState.openInstead;
            row[u"statusRow"_s] = r->statusRow;
            row[u"switchUntil"_s] = switchState.until.isValid() ? switchState.until.toString(Qt::ISODate) : QString();
        }
        rows.append(row);
    }
    state[u"rows"_s] = rows;
    QFile out(file);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        out.write(QJsonDocument(state).toJson());
}

void App::applyFolderMenu()
{
    // On with File Explorer and the desktop both off too: Directory Opus may
    // ask for it (requested()). And with the double click off, for its
    // hotkey (GitHub #25).
    const bool on = m_settings.folderMenu != u"off" || !m_settings.folderMenuHotkey.trimmed().isEmpty();
    if (!on && m_folderMenu) {
        m_serviceKeyListener.reset();
        m_keyListener.reset(); // it tells the menu double clicks: gone first
        m_keyListenerMode = 0;
        delete m_folderMenuWindow;
        m_folderMenu.reset();
    } else if (on && !m_folderMenu) {
        auto menu = std::make_unique<FolderMenu>(m_history.get(), m_favorites.get());
        menu->setCatalogs(m_apps.get(), m_places.get());
        QQmlComponent component(m_qml.get(), u"WinShun"_s, u"FolderMenuWindow"_s);
        QObject* object = component.createWithInitialProperties({{u"folderMenu"_s, QVariant::fromValue(menu.get())}});
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << component.errorString();
            delete object;
            return;
        }
        menu->setWindow(window);
        menu->setBlocked([this] { return clipboardInCharge(); }); // its keys over File Explorer
        menu->setDialogGo([this](HWND dialog, const QString& folder) { // the hotkey over a file dialog
            if (!m_dialogJump)
                return false;
            m_dialogJump->go(dialog, folder.toStdWString(), false);
            return true;
        });
        menu->setDialogOf([this](HWND window) -> HWND { // ... over the bar beside one: that dialog
            if (!m_dialogJump || !m_dialogJump->dialog() || !m_dialogJump->isCompanion(window))
                return nullptr;
            return m_dialogJump->dialog();
        });
        connect(menu.get(), &FolderMenu::openChanged, this, [this](bool open) {
            if (m_keyListener)
                m_keyListener->setMenuOpen(open);
        });
        connect(menu.get(), &FolderMenu::searchRequested, this, [this](const QString& folder) {
            showLauncher(folder.endsWith(u'\\') ? folder + u' ' : folder + u"\\ "_s);
        });
        connect(menu.get(), &FolderMenu::settingsRequested, this, [this] { showSettings(); });
        connect(menu.get(), &FolderMenu::notice, this, [this](const QString& text) {
            m_messages->showNotification(tr("文件夹菜单"), text);
        });
        m_folderMenuWindow = window;
        m_folderMenu = std::move(menu);
    }
    if (m_folderMenu) {
        m_folderMenu->setOptions(m_settings.folderMenu == u"auto", m_settings.folderMenuExplorer, m_settings.folderMenuDesktop);
        m_folderMenu->setGroups(m_settings.folderMenuFavorites, m_settings.folderMenuRecent, m_settings.folderMenuWindows);
        m_folderMenu->setItems(m_settings.folderMenuItemSearch, m_settings.folderMenuItemCopyPath, m_settings.folderMenuItemCmd, m_settings.folderMenuItemPowerShell,
            m_settings.folderMenuItemFavorite, m_settings.folderMenuItemSettings);
        m_folderMenu->setRecordHistory(m_settings.recordHistory);
    }
}

// 拖放圆盘: on unless switched off, and never while Win顺 runs as
// administrator, where drops from the user's programs cannot reach it (UIPI).
void App::applyDropPie()
{
    if (m_settings.dropPie && integrity::processElevated()) {
        if (!m_dropPieHelper) {
            auto notify = [this](const QString& title, const QString& text, std::function<void()> retry) {
                if (m_messages) m_messages->showNotification(title, text, std::move(retry));
            };
            m_dropPieHelper = std::make_unique<DropPieHelper>(m_favorites.get(), std::move(notify));
        }
        m_dropPieHelper->update(m_settings);
    } else {
        m_dropPieHelper.reset();
    }
    const bool on = m_settings.dropPie && !integrity::processElevated();
    if (!on && m_dropPie && m_dropPie->inDrop()) {
        // Switched off from inside its drop (a dialog's loop): once it returns.
        QTimer::singleShot(100ms, this, &App::applyDropPie);
        return;
    }
    if (!on && m_dropPie) {
        m_serviceKeyListener.reset();
        m_keyListener.reset(); // it summons the pie: gone first
        m_keyListenerMode = 0;
        delete m_dropPieWindow;
        m_dropPie.reset();
    } else if (on && !m_dropPie) {
        if (!m_fileOperations)
            m_fileOperations = std::make_unique<piedrop::FileOperations>();
        // From any thread (a file operation's): shown on this one, if the
        // app is still there.
        auto notify = [this](const QString& title, const QString& text) {
            QMetaObject::invokeMethod(this, [this, title, text] {
                if (m_messages)
                    m_messages->showNotification(title, text);
            }, Qt::QueuedConnection);
        };
        auto pie = std::make_unique<DropPie>(m_favorites.get(), m_places.get(), m_fileOperations.get(), notify);
        QQmlComponent component(m_qml.get(), u"WinShun"_s, u"DropPieWindow"_s);
        QObject* object = component.createWithInitialProperties({{u"dropPie"_s, QVariant::fromValue(pie.get())}});
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << component.errorString();
            delete object;
            return;
        }
        pie->setWindow(window);
        m_dropPieWindow = window;
        m_dropPie = std::move(pie);
    }
}

namespace {
// The bar's word for an elevated dialog it can only copy for (#104). The
// service navigates for the copy of Win顺 it was installed with or from
// (installed, or a portable copy that installed it), so installing it helps
// any copy without one; one installed by another copy keeps it for that one.
QString copyOnlyHint(ServiceDialogNav::Reach reach)
{
    using Reach = ServiceDialogNav::Reach;
    return reach == Reach::OtherCopy ? u"other"_s : reach == Reach::NoService ? u"service"_s : QString();
}
} // namespace

void App::applyDialogs()
{
    if (m_settings.dialogBar && !m_dialogBar) {
        if (!m_dialogEngine)
            m_dialogEngine = std::make_unique<SearchEngine>(m_index.get(), m_apps.get(), m_places.get());
        m_dialogBar = std::make_unique<DialogBar>(m_dialogEngine.get(), m_history.get(),
            m_favorites.get(), [this](HWND dialog, std::wstring path, bool isFile, bool open) {
                // An elevated dialog goes through the service helper (folder
                // only); a failure falls back to copying the path. A normal one
                // is driven directly.
                if (integrity::higherThanOurs(dialog)) {
                    const QString copyPath = QString::fromStdWString(path);
                    if (!(m_serviceNav && m_serviceNav->available())) {
                        // A copy-only bar copies by itself; this one still
                        // thought the service could (not told yet that it
                        // went): copy here, never nothing at all.
                        if (m_dialogBar)
                            m_dialogBar->copyPathFallback(dialog, copyPath);
                        return;
                    }
                    const std::size_t slash = path.find_last_of(L'\\');
                    std::wstring folder = !isFile ? path
                        : slash == std::wstring::npos ? path
                                                      : path.substr(0, slash == 2 ? 3 : slash);
                    m_serviceNav->navigate(dialog, std::move(folder), [this, dialog, copyPath](bool ok) {
                        if (ok)
                            return;
                        QMetaObject::invokeMethod(this, [this, dialog, copyPath] {
                            if (m_dialogBar)
                                m_dialogBar->copyPathFallback(dialog, copyPath);
                        }, Qt::QueuedConnection);
                    });
                    return;
                }
                if (m_dialogJump)
                    m_dialogJump->go(dialog, std::move(path), isFile, open);
            });
        connect(m_dialogBar.get(), &DialogBar::excludeAppRequested, this, &App::excludeFromDialogBar);
        connect(m_dialogBar.get(), &DialogBar::settingsRequested, this, [this] { showSettings(); });
        connect(m_dialogBar.get(), &DialogBar::autoJumpAnswered, this, &App::answerAutoJump);
    } else if (!m_settings.dialogBar && m_dialogBar) {
        delete m_barWindow;
        m_dialogBar.reset();
        updateCompanions();
    }
    if (m_dialogBar) {
        m_dialogBar->setRecordHistory(m_settings.recordHistory);
        m_dialogBar->setExcludedFiles(m_settings.excludedTypes, m_settings.excludedFileNames);
        m_dialogBar->setExcludedApps(m_settings.dialogBarExcludedApps);
        m_dialogBar->setPlace(m_settings.dialogBarPlace);
        m_dialogBar->setResultOptions(m_settings.showModified, m_settings.thumbnails, m_settings.numberKeys);
        m_dialogBar->results()->setShowModifiedTime(m_settings.showModifiedTime);
        m_dialogBar->setAskAutoJump(!m_settings.dialogAutoJump && !m_settings.dialogAutoJumpAsked);
        m_dialogBar->setCopyOnlyHint(copyOnlyHint(m_serviceNav ? m_serviceNav->reach() : ServiceDialogNav::Reach::NoService));
        m_dialogBar->setElevatedNavAvailable(m_serviceNav && m_serviceNav->available());
    }

    const bool followDialogs = m_settings.dialogJump || m_settings.dialogBar || m_settings.dialogAutoJump;
    if (followDialogs && !m_serviceNav) {
        // The background pipe client that asks the service to navigate elevated
        // dialogs (#104); it tells us when that ability appears or goes.
        m_serviceNav = std::make_unique<ServiceDialogNav>([this](ServiceDialogNav::Reach reach) { // worker thread
            QMetaObject::invokeMethod(this, [this, reach] {
                if (m_dialogBar) {
                    m_dialogBar->setCopyOnlyHint(copyOnlyHint(reach));
                    m_dialogBar->setElevatedNavAvailable(reach == ServiceDialogNav::Reach::Available);
                }
                if (m_dialogJump)
                    m_dialogJump->refreshElevated();
            }, Qt::QueuedConnection);
        });
    }
    if (followDialogs && !m_dialogJump) {
        DialogJump::Callbacks callbacks;
        callbacks.setHotkey = [this](bool on) {
            if (on)
                return m_messages->registerHotkey(kDialogJumpHotkeyId, u"Ctrl+G"_s);
            m_messages->unregisterHotkey(kDialogJumpHotkeyId);
            return true;
        };
        callbacks.dialogChanged = [this](HWND dialog) {
            // An elevated dialog just came up: re-probe so the bar knows at
            // once whether it can jump or must fall back to copy.
            if (dialog && m_serviceNav && integrity::higherThanOurs(dialog))
                m_serviceNav->refresh();
            if (m_dialogBar && (!dialog || m_barWindow || createBarWindow()))
                m_dialogBar->setDialog(dialog);
        };
        callbacks.dialogMoved = [this] {
            if (m_dialogBar)
                m_dialogBar->dialogMoved();
        };
        callbacks.dialogRestacked = [this] {
            if (m_dialogBar)
                m_dialogBar->dialogRestacked();
        };
        callbacks.dialogDragged = [this](bool dragged) {
            if (m_dialogBar)
                m_dialogBar->dialogDragged(dragged);
        };
        callbacks.autoJumped = [this](HWND dialog, std::wstring from) { // on DialogJump's thread
            QMetaObject::invokeMethod(this, [this, dialog, from = QString::fromStdWString(from)] {
                if (m_dialogBar)
                    m_dialogBar->setOrigin(dialog, from);
            }, Qt::QueuedConnection);
        };
        callbacks.elevatedNavAvailable = [this] { return m_serviceNav && m_serviceNav->available(); };
        callbacks.navigateElevated = [this](HWND dialog, std::wstring folder) { // may be the worker thread
            if (m_serviceNav)
                m_serviceNav->navigate(dialog, std::move(folder), {});
        };
        m_dialogJump = std::make_unique<DialogJump>(std::move(callbacks));
        updateCompanions();
    } else if (!followDialogs) {
        m_dialogJump.reset();
        m_serviceNav.reset();
    }
    if (m_dialogJump) {
        m_dialogJump->setHotkeyEnabled(m_settings.dialogJump);
        m_dialogJump->setAutoJump(m_settings.dialogAutoJump, m_settings.dialogAutoJumpWhen != u"back");
    }
    // Turned on with a file dialog in front: under it at once.
    if (m_dialogBar && !m_barWindow && m_dialogJump && m_dialogJump->dialog() && createBarWindow())
        m_dialogBar->setDialog(m_dialogJump->dialog());
}

void App::answerAutoJump(const QString& when)
{
    m_settings.dialogAutoJumpAsked = true;
    m_settings.dialogAutoJump = when == u"open" || when == u"back";
    if (m_settings.dialogAutoJump)
        m_settings.dialogAutoJumpWhen = when;
    m_settings.save();
    applySettings(false);
    if (m_settingsEditor)
        m_settingsEditor->setSettings(m_settings);
}

void App::excludeFromDialogBar(const QString& app)
{
    if (app.isEmpty() || m_settings.dialogBarExcludedApps.contains(app, Qt::CaseInsensitive))
        return;
    m_settings.dialogBarExcludedApps.append(app);
    m_settings.save();
    applySettings(false);
    if (m_settingsEditor)
        m_settingsEditor->setSettings(m_settings);
}

bool App::createBarWindow()
{
    QQmlComponent component(m_qml.get(), u"WinShun"_s, u"DialogBarWindow"_s);
    QObject* object = component.createWithInitialProperties({{u"bar"_s, QVariant::fromValue(m_dialogBar.get())}});
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        qWarning().noquote() << component.errorString();
        delete object;
        return false;
    }
    window->create();
    styleWindow(window);
    win::setDarkFrame(window, isDarkMode());
    window->installEventFilter(this); // the Menu key, pasting into the box
    // Gone back to from the clipboard (returnToField): into the box as
    // soon as the bar is up again with its dialog, once it is done showing.
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible || std::exchange(m_barFocusDeadline, {}).hasExpired())
            return;
        QMetaObject::invokeMethod(this, [this] {
            if (m_barWindow && m_barWindow->isVisible() && !m_barWindow->isActive()) {
                win::bringToFront(m_barWindow);
                m_barWindow->requestActivate();
            }
        }, Qt::QueuedConnection);
    });
    connect(window, &QWindow::activeChanged, this, &App::updateNumberKeys);
    m_barWindow = window;
    m_dialogBar->setWindow(window);
    // Made for DialogJump's first look at the window in front, before it is
    // there: then applyDialogs() does this.
    updateCompanions();
    return true;
}

void App::updateNumberKeys()
{
    updateNumberKeyHook();
    const unsigned own = ownNumberDigits(m_settings.numberKeys);
    m_launcher->setNumberKeysLeft(own);
    if (m_dialogBar)
        m_dialogBar->setNumberKeysLeft(own);
}

// The window in front decides: the launcher's and the bar's number keys, or
// the clipboard's when it took the focus (over a program that keeps it, its
// KeyRouter takes them). Asked again each time, as programs start and end:
// no hook while no other program holds one of the digits.
void App::updateNumberKeyHook()
{
    const QWindow* front = QGuiApplication::focusWindow();
    const QString keys = !front ? QString()
        : front == m_window || (front == m_barWindow && m_dialogBar) ? m_settings.numberKeys
        : front == m_clipWindow && m_settings.clipboard        ? m_settings.clipboardNumberKeys
                                                                : QString();
    const auto modifier = keys == u"alt" ? numberkeys::Modifier::Alt : numberkeys::Modifier::Ctrl;
    const unsigned digits
        = keys == u"ctrl" || keys == u"alt" ? numberkeys::heldMask(modifier) & ~ownNumberDigits(keys) : 0u;
    // One Windows refused (no hook) is tried again.
    if (m_numberKeyHook && m_numberKeyHook->isActive() && m_numberKeyHook->modifier() == modifier
        && m_numberKeyHook->digits() == digits)
        return;
    if (m_numberKeyHook && m_numberKeyHook->isActive()) {
        // A digit it took still down (Ctrl+1 opened a row and hid the
        // launcher) stays its own until let go: alone, its repeats would be
        // the other program's hotkey. One still draining before it: gone.
        m_numberKeyHook->drain();
        m_drainingNumberKeyHook = std::move(m_numberKeyHook);
        m_drainingNumberKeyRun = m_numberKeyRun;
        QTimer::singleShot(2s, this, [this, run = m_drainingNumberKeyRun] {
            if (m_drainingNumberKeyRun == run)
                m_drainingNumberKeyHook.reset(); // a key held that long is the program's again
        });
    }
    m_numberKeyHook.reset(); // one Windows refused
    if (!digits)
        return;
    const std::uint64_t run = ++m_numberKeyRun;
    m_numberKeyHook = std::make_unique<numberkeys::Hook>(modifier, digits,
        [this](const numberkeys::Hook::Key& key) {
            QMetaObject::invokeMethod(this, [this, key] { numberKeyHooked(key); }, Qt::QueuedConnection);
        },
        [this, run] {
            QMetaObject::invokeMethod(this, [this, run] {
                if (m_drainingNumberKeyRun == run)
                    m_drainingNumberKeyHook.reset();
            }, Qt::QueuedConnection);
        });
}

// As the window's own key, to the item with the focus in it.
void App::numberKeyHooked(const numberkeys::Hook::Key& key)
{
    QWindow* window = QGuiApplication::focusWindow();
    if (!m_numberKeyHook || !window)
        return;
    QKeyEvent event(key.press ? QEvent::KeyPress : QEvent::KeyRelease, Qt::Key_0 + key.digit, key.modifiers,
                    key.scanCode, key.vk, 0, QString(), key.autoRepeat);
    QCoreApplication::sendEvent(window, &event);
}

unsigned App::ownNumberDigits(const QString& keys) const
{
    if (keys != u"ctrl" && keys != u"alt")
        return 0;
    QStringList own {m_settings.hotkey, m_settings.clipboardHotkey, m_settings.folderMenuHotkey};
    for (const auto& [plugin, shortcuts] : m_pluginHotkeys) // the screenshot's, say
        own << shortcuts;
    const QString modifier = keys == u"ctrl" ? u"Ctrl"_s : u"Alt"_s;
    unsigned digits = 0;
    for (int n = 1; n <= 9; ++n) {
        UINT modifiers = 0, vk = 0;
        MessageWindow::parseHotkey(modifier + u'+' + QString::number(n), &modifiers, &vk);
        const bool mine = std::ranges::any_of(own, [&](const QString& shortcut) {
            UINT otherModifiers = 0, otherVk = 0;
            return MessageWindow::parseHotkey(shortcut, &otherModifiers, &otherVk) && modifiers == otherModifiers
                && vk == otherVk;
        });
        if (mine)
            digits |= 1u << n;
    }
    return digits;
}

void App::updateCompanions()
{
    if (!m_dialogJump)
        return;
    std::vector<HWND> windows;
    for (QWindow* window : {static_cast<QWindow*>(m_barWindow), static_cast<QWindow*>(m_clipWindow)}) {
        if (window)
            windows.push_back(reinterpret_cast<HWND>(window->winId()));
    }
    m_dialogJump->setCompanions(std::move(windows));
}

void App::onDoubleCtrl(const KeyListener::Tap& tap)
{
    const std::uint64_t now = ::GetTickCount64();
    const std::uint64_t pressedAt = tickTime(tap.time, now);
    // Which listener told of it, and when: late by the listener, or by this
    // thread (queue_ms), GitHub #111.
    beginTrigger(TriggerSource::DoubleTap, pressedAt,
        TriggerJournal::Told {tap.from == KeyListener::Tap::From::Helper  ? "helper"
                : tap.from == KeyListener::Tap::From::OwnBackup ? "own-backup"
                                                                : "own",
            static_cast<std::int64_t>(static_cast<std::int32_t>(tap.toldAt - tap.time)), tap.queuedMs});
    ForegroundFacts facts = foreground::facts();
    // Hidden by Windows as the user typed ("hide pointer while typing"),
    // not by a game: the mouse is not taken (GitHub #111).
    if (facts.cursorHidden && m_pointerWatch)
        facts.cursorHiddenByTyping = m_pointerWatch->hiddenByTyping();
    const IgnoreReason reason = doubleCtrlIgnoreReason(facts,
        {m_settings.doubleCtrlPauseInGames, m_settings.doubleCtrlPauseInFullScreen, m_settings.doubleCtrlExcludedApps});
    if (reason == IgnoreReason::None) {
        // Pressed again or not by where it began (seenBy): a double tap made
        // again just before the launcher came, as nothing had, ends after it,
        // and judged by its release it closed what had just come (GitHub
        // #111). By its release when its first press is not known (an older
        // UIAccess listener's word).
        toggleLauncher(true, tickTime(tap.firstPress.value_or(tap.time), now));
        return;
    }
    // Nothing shown over a game. The log says why, for "double Ctrl does nothing".
    static const char* const why[] {"", "it is in the list", "it is full screen", "it is in exclusive full screen",
        "it has taken the mouse (cursor hidden by it, and confined, captured or full screen)"};
    static const char* const names[] {"", "listed", "full-screen", "exclusive-full-screen", "mouse-taken"};
    m_triggers->ignored(names[static_cast<int>(reason)],
        static_cast<std::uint8_t>((facts.coversMonitor ? TriggerJournal::kCovers : 0)
            | (facts.exclusiveFullScreen ? TriggerJournal::kExclusive : 0)
            | (facts.cursorHidden ? TriggerJournal::kHidden : 0) | (facts.cursorConfined ? TriggerJournal::kConfined : 0)
            | (facts.cursorCaptured ? TriggerJournal::kCaptured : 0)
            | (facts.cursorHiddenByTyping ? TriggerJournal::kTyping : 0)));
    const QString key = facts.program + u' ' + QString::number(static_cast<int>(reason));
    if (!m_doubleCtrlIgnoredLogged.contains(key)) {
        m_doubleCtrlIgnoredLogged.append(key);
        qWarning().noquote() << "Double Ctrl ignored over" << facts.program << "as"
                             << why[static_cast<int>(reason)];
    }
}

void App::toggleLauncher(bool byShortcut, std::optional<std::uint64_t> pressedAt)
{
    const std::uint64_t at = pressedAt.value_or(::GetTickCount64());
    const auto decided = [this](const char* action, std::optional<TriggerWindow> coming = std::nullopt) {
        if (m_triggers)
            m_triggers->decided(action, coming);
    };
    // The clipboard in front: back into the box it came from, or over to searching.
    if (clipboardInCharge()) {
        // Pressed before it was there to be seen (GitHub #111): it stays.
        if (!seenBy(m_clipRevealedAt, at)) {
            decided("clipboard-stays");
            return;
        }
        if (m_clipboard->field()) {
            m_clipboard->dismiss();
            decided("clipboard-back");
            return;
        }
        hideClipboard();
    }
    // In front of a file dialog: into its search bar, and back.
    if (m_dialogBar && m_dialogBar->isShown()) {
        m_dialogBar->toggleFocus();
        decided("dialog-bar");
        return;
    }
    // Pressed again (ShortcutAgain): the box becomes the panel, as with Ctrl+E.
    // The tray and the command line say "toggle" and keep closing it. Not a
    // press made before it was there to be seen (seenBy): one that waited
    // behind a slow opening, or was made again because nothing came, closed
    // the launcher the first had just opened (GitHub #111).
    switch (shortcutAction(launcherShown(), m_window->isActive(), m_launcher->panel(),
        byShortcut && m_settings.shortcutAgain == u"panel", seenBy(m_launcherRevealedAt, at))) {
    case ShortcutAction::Hide:
        hideLauncher();
        decided("hide");
        break;
    case ShortcutAction::ExpandPanel:
        setPanel(true);
        decided("panel");
        break;
    case ShortcutAction::Raise:
        // Coming, or up already: to the front as it is, neither placed again nor closed.
        toFront(m_window);
        if (refuseFrontForTest() == RefuseFront::No)
            m_window->requestActivate();
        decided("raise");
        break;
    case ShortcutAction::Show:
        decided("show", TriggerWindow::Launcher);
        showLauncher();
        if (m_triggers && !m_launcherReveal.pending()) // up already (behind a window, sliding out): seen now
            m_triggers->seen(TriggerWindow::Launcher);
        break;
    }
}

void App::toggleClipboard(std::optional<std::uint64_t> pressedAt)
{
    // Again in front: back to the field it was opened from, or hidden. Not
    // by a press made before it was there to be seen (seenBy, GitHub #111):
    // it comes, and stays.
    if (clipboardInCharge() && seenBy(m_clipRevealedAt, pressedAt.value_or(::GetTickCount64()))) {
        m_clipboard->dismiss();
        if (m_triggers)
            m_triggers->decided("clipboard-back");
        return;
    }
    if (m_triggers) // in front and still coming: it comes, as it is
        m_triggers->decided(clipboardInCharge() ? "raise" : "show", TriggerWindow::Clipboard);
    showClipboard();
    if (m_triggers && m_clipWindow && !m_clipReveal.pending()) // up already
        m_triggers->seen(TriggerWindow::Clipboard);
}

void App::beginTrigger(TriggerSource source, std::uint64_t pressedAt, std::optional<TriggerJournal::Told> told)
{
    if (!m_triggers)
        m_triggers = std::make_unique<TriggerJournal>();
    TriggerJournal::Context context;
    context.helper = m_keyListener && m_keyListener->serviceActive();
    context.trimmed = m_idleTrim && m_idleTrim->trimmed();
    context.rawInput = rawInputState();
    context.told = told;
    m_triggers->begin(source, pressedAt, context);
}

// A double tap that came to nothing (KeyListener::Note): in the journal, a
// few an hour (JournalBudget). The key by name; nothing typed.
void App::journalTapNote(const KeyListener::Note& note)
{
    if (!m_tapNotes.take(true, ::GetTickCount64()))
        return;
    const bool alt = note.key == VK_LMENU || note.key == VK_RMENU;
    const bool shift = note.key == VK_LSHIFT || note.key == VK_RSHIFT;
    const bool right = note.key == VK_RCONTROL || note.key == VK_RMENU || note.key == VK_RSHIFT;
    QJsonObject details {
        {u"key"_s, QString::fromLatin1(alt ? "alt" : shift ? "shift" : "ctrl")},
        {u"side"_s, QString::fromLatin1(right ? "right" : "left")},
        {u"ms"_s, static_cast<qint64>(note.ms)},
    };
    if (note.kind == KeyListener::Note::Kind::NearMiss) {
        details.insert(u"reason"_s, QString::fromLatin1(DoubleTapDetector::name(note.miss)));
        if (note.by != DoubleTapDetector::Cause::None) // what spoiled it, never which key
            details.insert(u"by"_s, QString::fromLatin1(DoubleTapDetector::name(note.by)));
    } else
        details.insert(u"reason"_s, note.kind == KeyListener::Note::Kind::Late ? u"late"_s : u"set-aside"_s);
    if (m_tapNotes.skipped() > 0)
        details.insert(u"skipped"_s, m_tapNotes.skipped());
    diagnostics::record(note.kind == KeyListener::Note::Kind::NearMiss ? u"double-tap-near-miss"_s
                                                                       : u"double-tap-dropped"_s,
        details);
}

win::Front App::toFront(QWindow* window)
{
    QElapsedTimer timer;
    timer.start();
    win::Front front = win::Front::Refused;
    if (const RefuseFront refuse = window == m_window ? refuseFrontForTest() : RefuseFront::No; refuse == RefuseFront::No) {
        front = win::bringToFront(window, window == m_clipWindow ? win::ShellFlyout::Preserve : win::ShellFlyout::Dismiss);
    } else if (const HWND hwnd = reinterpret_cast<HWND>(window->winId()); ::GetForegroundWindow() == hwnd) {
        front = win::Front::Already; // clicked into meanwhile: Windows let it
    } else {
        // Shown, not in front; with "active", the active window of this
        // thread all the same, as a refused SetForegroundWindow leaves it.
        if (!::IsWindowVisible(hwnd))
            ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        if (refuse == RefuseFront::ActiveHere)
            ::SetActiveWindow(hwnd);
        qWarning() << "Could not bring the window to the front (WINSHUN_TEST_REFUSE_FRONT)";
    }
    if (m_triggers && m_triggers->open())
        m_triggers->front(front, static_cast<double>(timer.nsecsElapsed()) / 1e6);
    if (front == win::Front::Refused)
        recordFrontRefused(window);
    if (window == m_window && front == win::Front::Refused) {
        watchStrandedLauncher();
        startLauncherRouting(); // GitHub #203: its keys through a hook of its own meanwhile
    } else if (window == m_window) {
        stopLauncherRouting(); // in front: its keys are its own again
    }
    return front;
}

App::RefuseFront App::refuseFrontForTest()
{
    static const RefuseFront refuse = [] {
        const QByteArray value = qgetenv("WINSHUN_TEST_REFUSE_FRONT");
        return value.isEmpty() || value == "0" ? RefuseFront::No
            : value == "active"                ? RefuseFront::ActiveHere
                                               : RefuseFront::Never;
    }();
    return refuse;
}

// GitHub #203 (Windows 10, Explorer in front): the hotkey's grant, the empty
// input and the keys typed again were all refused there, and the log said
// only that. The rights of the program in front and Win顺's own (UIPI: a
// higher one there drops what Win顺 sends, SendInput telling nothing), a
// menu or move of its on (GUITHREADINFO's flags), its window hung, the
// foreground lock and how long since the last input; whether Win顺 may set
// the foreground now; and one more input that does nothing, sent now, with
// SendInput's word on it (the error cleared first) and whether it gave that
// right. No keys, titles, folders or file names.
void App::recordFrontRefused(QWindow* window)
{
    if (!m_frontRefusals.take(true, ::GetTickCount64()))
        return;
    const HWND front = ::GetForegroundWindow();
    DWORD pid = 0;
    const DWORD thread = front ? ::GetWindowThreadProcessId(front, &pid) : 0;
    wchar_t className[256] {};
    if (front)
        ::GetClassNameW(front, className, static_cast<int>(std::size(className)));
    GUITHREADINFO info {sizeof info};
    const bool infoRead = thread && ::GetGUIThreadInfo(thread, &info);
    DWORD lockTimeout = 0;
    const bool lockRead = ::SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &lockTimeout, 0);
    LASTINPUTINFO last {sizeof last};
    const bool lastRead = ::GetLastInputInfo(&last);
    const DWORD now = ::GetTickCount();
    const HWND hwnd = window ? reinterpret_cast<HWND>(window->winId()) : nullptr;
    ::SetLastError(0);
    const bool may = ::AllowSetForegroundWindow(::GetCurrentProcessId());
    const DWORD mayError = may ? 0 : ::GetLastError();
    INPUT nothing {};
    nothing.type = INPUT_MOUSE;
    nothing.mi.dwExtraInfo = kOwnInput;
    ::SetLastError(0);
    const UINT inserted = ::SendInput(1, &nothing, sizeof nothing);
    const DWORD insertError = ::GetLastError();
    const bool mayAfter = ::AllowSetForegroundWindow(::GetCurrentProcessId());
    QJsonObject details {
        {u"window"_s, window == m_window ? u"launcher"_s : window == m_clipWindow ? u"clipboard"_s : u"other"_s},
        {u"front_class"_s, QString::fromWCharArray(className)},
        {u"front_program"_s, front ? foreground::programOf(front) : QString()},
        {u"front_rights"_s, pid ? diagnostics::rightsOf(pid) : QJsonObject()},
        {u"own_rights"_s, diagnostics::rightsOf(0)},
        {u"front_hung"_s, front && ::IsHungAppWindow(front)},
        {u"front_flags"_s, infoRead ? u"0x%1"_s.arg(info.flags, 0, 16) : u"unknown"_s},
        {u"active_here"_s, hwnd && ::GetActiveWindow() == hwnd}, // refused, but the active window of its thread
        {u"lock_timeout_ms"_s, lockRead ? static_cast<qint64>(lockTimeout) : -1},
        {u"idle_ms"_s, lastRead ? static_cast<qint64>(now - last.dwTime) : -1},
        {u"may_set_front"_s, may},
        {u"may_set_front_error"_s, static_cast<qint64>(mayError)},
        {u"nothing_inserted"_s, static_cast<qint64>(inserted)},
        {u"nothing_error"_s, static_cast<qint64>(insertError)},
        {u"may_set_front_after"_s, mayAfter},
    };
    if (m_frontRefusals.skipped() > 0)
        details.insert(u"skipped"_s, m_frontRefusals.skipped());
    diagnostics::record(u"front-refused"_s, details);
}

void App::watchStrandedLauncher()
{
    // A button held now (the press on the box that asked for it) is not a
    // press elsewhere: only one that comes after.
    m_strandedPress = (::GetAsyncKeyState(VK_LBUTTON) | ::GetAsyncKeyState(VK_RBUTTON) | ::GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
    m_strandedWatch.start();
}

void App::checkStrandedLauncher()
{
    // A refused activation may leave Qt active only in this thread, or may
    // never activate it at all (#140). Both need dismissal on an outside
    // click: a window that was never active gets no activeChanged(false).
    const HWND hwnd = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    if (!hwnd || !m_window->isVisible() || m_launcherClosing || ::GetForegroundWindow() == hwnd) {
        m_strandedWatch.stop();
        return;
    }
    const bool down = (::GetAsyncKeyState(VK_LBUTTON) | ::GetAsyncKeyState(VK_RBUTTON) | ::GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
    if (!down || m_strandedPress) {
        m_strandedPress = down;
        return;
    }
    m_strandedPress = true;
    // A press on a window of ours (the launcher, Win顺's box on the taskbar,
    // a menu of ours) is for that window; anywhere else (the rest of the
    // taskbar, which is in front already: no change of the front to hear
    // of) is a click elsewhere.
    POINT at {};
    ::GetCursorPos(&at);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(::WindowFromPoint(at), &pid);
    if (pid == ::GetCurrentProcessId())
        return;
    m_strandedWatch.stop();
    qWarning().noquote() << "Launcher: not in front (Windows refused), a press elsewhere: as if it lost the focus";
    stopLauncherRouting(); // its router tells of the press too, unless Windows dropped its hook
    launcherLostFront();
    updateNumberKeys();
}

// Double tap works only while the keyboard's Raw Input comes to the
// listener's window (KeyListener): ok, none (gone; it is set aside on
// purpose while TypeToSearch holds keys, or a shortcut is recorded), other
// (another window of ours took it), unknown (Windows could not say).
const char* App::rawInputState() const
{
    if (!m_keyListener || !m_settings.doubleCtrl)
        return "off";
    const KeyboardRawInput raw = keyboardRawInput();
    if (raw.error)
        return "unknown";
    if (!raw.registration)
        return "none";
    return raw.registration->hwndTarget == m_keyListener->window() ? "ok" : "other";
}

void App::showLauncher(const QString& query, const std::optional<taskbar::Spot>& spot, bool inBox, QScreen* on, HWND avoid,
    bool inside, const RECT& orInto)
{
    if (!m_window)
        return;
    // show()/activation and forceActiveFocus() emit signals synchronously.
    // They may call deliverTyping before handleShown selects the old words
    // or caretRequested places the caret; replay then loses the first key.
    // The typing callers deliver again after this preparation has finished.
    QScopedValueRollback opening(m_launcherOpening, true);
    cancelTypingSelection();
    m_typingOrigin = {}; // opened anew: set again by typingFolderKnown when that is why
    if (!query.isEmpty())
        m_launcher->setTypingFolder({}); // a path given ("search here") is searched alone, as typed (GitHub #110)
    if (m_idleTrim->takeTrimmed() && m_index->index()) {
        // The index's pages back while the window comes up, before the first
        // search needs them (IdleTrim, FileIndex::warm).
        std::thread([index = m_index->index()] {
            const auto lock = index->longReadLock();
            index->warm();
        }).detach();
    }
    hideClipboard(false); // from under its search box: searching again
    // Each time it is asked for, pinned and up already too: English once its
    // field has the focus (lettersOnOpen). A click into it is not asked for.
    m_lettersPending = m_settings.englishOnOpen;
    m_boxPressGrace.stop();
    m_launcherShown.start();
    m_idleTrim->used();
    m_index->shares().lookedAt(); // a WebDAV server stopped meanwhile shows as offline (GitHub #114)
    if (!launcherShown()) { // what a screenshot of the history pastes into (jt, Enter)
        const HWND front = ::GetForegroundWindow();
        if (front != reinterpret_cast<HWND>(m_window->winId()))
            m_launcherTarget = front;
    }
    if (m_launcher->pinned() && launcherShown()) {
        // Pinned and up already, without the focus (or under another topmost
        // window): to the front where it stands, the words as they are. Not
        // placed again: that would take a window dragged aside back to its
        // spot, or the field into the taskbar's box.
        if (!query.isEmpty())
            m_launcher->setQuery(query);
        toFront(m_window);
        if (refuseFrontForTest() == RefuseFront::No)
            m_window->requestActivate();
        m_launcher->handleShown(); // refreshed (something was opened meanwhile), all typed selected
        if (!query.isEmpty())
            emit m_launcher->caretRequested(static_cast<int>(query.size()), false);
        lettersOnOpen(); // active and focused already, nothing else to come
        return;
    }
    const bool wasClosing = std::exchange(m_launcherClosing, false); // back from on its way out
    // Opened anew (not back from on its way out, not up already): as at its
    // first opening, if so set. Still hidden or cloaked until the rows for
    // it are drawn, so the old words and rows are never seen.
    if (m_settings.clearOnOpen && !wasClosing && (!m_window->isVisible() || m_prewarming.load()))
        m_launcher->startOver();
    // Nor there to be seen until then, however it went last (seenBy, GitHub #111).
    if (!m_window->isVisible() || m_prewarming.load())
        m_launcherRevealedAt.reset();
    const bool wasInBox = m_launcher->inTaskbarBox();
    // Pinned, it stays while the user works elsewhere: a window of its own,
    // with its own field, the box idle (see launcherPinnedChanged).
    m_launcherOverBox = inBox && m_searchBox && !m_launcher->pinned();
    m_launcher->setInTaskbarBox(m_launcherOverBox && m_searchBox->wide());
    updateSearchBoxLook();
    if (m_searchBox && !m_launcher->inTaskbarBox()) // opened elsewhere: the box is idle; over it, it looks open
        m_searchBox->setText({.active = m_launcherOverBox});
    else if (m_searchBox && wasClosing && wasInBox) // the field has not changed, nor said so again
        m_searchBox->setText(m_fieldShown);
    if (m_launcher->panel()) {
        // Up as the panel (asked for again while it is): as it is.
    } else if (spot) {
        m_placement->attachTaskbar(*spot);
    } else {
        // On the monitor asked for, else the one under the mouse; where the
        // user last put it (see Placement).
        QScreen* screen = on ? on : QGuiApplication::screenAt(QCursor::pos());
        screen = screen ? screen : QGuiApplication::primaryScreen();
        RECT bounds {};
        const auto native = [](const RECT& r) { return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top); };
        if (avoid && ::GetWindowRect(avoid, &bounds)) {
            // Over an Opus file list: its lister's other lists and client
            // area are where it may reach when the list is too small for it.
            // Into Explorer's (orInto): within its window's client area.
            const bool orIntoList = !inside && !::IsRectEmpty(&orInto);
            const HWND root = inside || orIntoList ? ::GetAncestor(avoid, GA_ROOT) : nullptr;
            RECT client {};
            const bool framed = root && ::GetClientRect(root, &client)
                && ::ClientToScreen(root, reinterpret_cast<POINT*>(&client.left))
                && ::ClientToScreen(root, reinterpret_cast<POINT*>(&client.right));
            m_placement->avoidNative(native(bounds), screen, inside, inside ? native(folderview::opusFileLists(root)) : QRect(),
                framed ? native(client) : QRect(), orIntoList ? native(orInto) : QRect());
        } else
            m_placement->placeOn(screen);
    }
    if (!query.isEmpty())
        m_launcher->setQuery(query);

    const bool prewarming = m_prewarming.exchange(false); // up already, cloaked
    if (wasClosing) {
        // Back from where it got to on its way out.
        m_placement->slideIn();
    } else if (!m_window->isVisible() || prewarming) {
        // Shown cloaked: Windows would first put up the window as it looked
        // when it was hidden (old selection, old rows), then our repaint.
        // Revealed once a frame with the refreshed results is drawn.
        win::setCloaked(m_window, true);
        m_launcherReveal.begin();
    }
    m_window->show();
    toFront(m_window);
    m_launcherFrontTick = ::GetTickCount(); // input after this is not its own empty one (logLauncherLostFront)
    if (refuseFrontForTest() == RefuseFront::No)
        m_window->requestActivate();
    m_launcher->handleShown(); // selects all that is typed
    if (!query.isEmpty()) // typed for the user (a folder, --query): words go after it
        emit m_launcher->caretRequested(static_cast<int>(query.size()), false);
    if (m_launcherReveal.pending() && !m_launcher->waitingForNames())
        armReveal(); // nothing to wait for (内容 refreshes in the background)
    lettersOnOpen(); // back from on its way out: it may have kept its focus
}

void App::showClipboard()
{
    if (!m_clipWindow && !createClipWindow())
        return;
    m_idleTrim->used(); // it never takes the focus, which would tell IdleTrim
    // Digits that are Win顺's own shortcuts stay so (KeyRouter passes them
    // on): their rows show the chord struck through.
    m_clipboard->setNumberKeysLeft(ownNumberDigits(m_settings.clipboardNumberKeys));
    if (m_keyRouter) // up already, over the program that keeps the focus
        return;
    if (m_clipboard->pinned() && m_clipWindow->isVisible() && !m_clipPrewarming.load()) {
        engageClipboard(); // where it is, over whatever is in front now
        return;
    }
    const bool prewarming = m_clipPrewarming.exchange(false); // up already, cloaked
    bool keepFocus = false; // where it is, with the program it pastes into
    if (!m_clipWindow->isVisible() || prewarming) {
        m_clipWindow->setProperty("controllerEventsEnabled", true);
        m_clipWindow->setProperty("shellWidth", 0);
        m_clipDisplayWanted = false;
        m_clipRevealedAt.reset(); // there to be seen from its first frame on, however it went last (seenBy)
        // It pastes into what had the keyboard before it: a text field of
        // Win顺's own (read before the window takes the focus from it), or
        // else the window in front.
        QQuickItem* field = focusedField();
        m_clipboard->setField(field, field && field->window() == m_settingsWindow ? tr("输入框") : tr("搜索框"));
        const HWND foreground = ::GetForegroundWindow();
        m_clipboard->setTarget(foreground);
        m_clipboard->setPinned(false); // as it opens, whatever closed it last (a close of Qt's own)
        m_clipboard->setActive(true); // under the launcher's search box, it folds its rows away (Main.qml)
        m_clipboard->handleShown(); // the list alone again: its width as it opens
        if (field && !m_settings.clipboardAtPointer) {
            // Dropping down under the field's box (the launcher's search bar,
            // the bar by a file dialog, a box in the settings window).
            const QQuickItem* box = field->parentItem() ? field->parentItem() : field;
            m_clipPlacement->attach(QRectF(box->mapToGlobal(QPointF(0, 0)), box->size()).toAlignedRect());
        } else {
            // By the text caret in the program it pastes into, as Windows'
            // own clipboard opens; else by the mouse pointer, its top left
            // corner at the pointer's tip, as a menu.
            const HWND target = paste::usableTarget(foreground);
            const std::optional<QRect> caret = target && !m_settings.clipboardAtPointer
                ? m_textCaret->find(target, kCaretWait) : std::nullopt;
            if (caret)
                m_clipPlacement->attachNative(*caret);
            else
                m_clipPlacement->attach(QRect(QCursor::pos(), QSize(1, 0)));
            // As Windows' own: that program stays in front, whatever closes
            // without the focus stays open there (VS Code's command palette,
            // a browser's suggestions), the caret stays where the paste goes.
            // Search/Start must keep their input field too (#179). They can
            // cover ordinary topmost windows, so put the clipboard beside
            // the flyout instead of dismissing it to get the foreground.
            const bool shellFlyout = win::isShellFlyout(foreground);
            m_clipDisplayWanted = shellFlyout && m_clipDisplay && m_clipDisplay->ready();
            keepFocus = target && (startKeyRouting(target) || shellFlyout);
            if (shellFlyout && !m_clipDisplayWanted)
                avoidClipboardShellFlyout(foreground);
        }
        m_clipboard->handlePlaced(); // the preview, if left open: as wide as there is room for here
        // Cloaked until its first frame: Windows would first put up the
        // window as it looked when it was hidden, then our repaint.
        win::setCloaked(m_clipWindow, true);
        m_clipReveal.begin();
        m_clipReveal.arm();
    }
    if (keepFocus) {
        m_clipFrame->setNoActivate(true);
        m_clipWindow->setProperty("_q_showWithoutActivating", true);
        m_clipWindow->show();
        m_clipWindow->setProperty("_q_showWithoutActivating", QVariant());
        const HWND panel = reinterpret_cast<HWND>(m_clipWindow->winId());
        // STARTF_USESHOWWINDOW/SW_HIDE can override Qt's first show while
        // Qt already reports visible. Recover without activating the target.
        if (!::IsWindowVisible(panel))
            ::ShowWindow(panel, SW_SHOWNOACTIVATE);
        // On top of the other topmost windows, as an activated one would be.
        ::SetWindowPos(panel, HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    m_clipWindow->show();
    toFront(m_clipWindow);
    m_clipWindow->requestActivate();
}

bool App::clipboardInCharge() const
{
    return m_clipWindow && m_clipWindow->isVisible() && !m_clipPrewarming.load()
        && (m_clipWindow->isActive() || m_keyRouter || (m_clipDisplay && m_clipDisplay->active()
            && ::GetForegroundWindow() == m_clipDisplay->handle()));
}

void App::avoidClipboardShellFlyout(HWND foreground)
{
    const QRect rect = win::shellFlyoutBounds(foreground);
    if (rect.isEmpty())
        return;
    const HMONITOR monitor = ::MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    QScreen* screen = screenOf(monitor);
    MONITORINFO info {sizeof info};
    if (screen && ::GetMonitorInfoW(monitor, &info)) {
        const qreal scale = screen->devicePixelRatio();
        const int vertical = std::max(rect.top() - info.rcWork.top, info.rcWork.bottom - rect.y() - rect.height());
        if (vertical < m_clipWindow->minimumHeight() * scale + 12 * scale) {
            const int beside = std::max(rect.left() - info.rcWork.left, info.rcWork.right - rect.x() - rect.width());
            m_clipWindow->setProperty("shellWidth", std::max(240, int(std::floor(beside / scale)) - 12));
        }
    }
    m_clipPlacement->avoidNative(rect, screen);
}

bool App::startKeyRouting(HWND target)
{
    // An elevated program while Win顺 is not: Windows keeps the keys typed
    // there from the hook (UIPI). The clipboard takes the focus instead.
    if (integrity::higherThanOurs(target)) {
        diagnostics::record(u"clipboard-key-routing-skipped"_s, {{u"reason"_s, u"higher-integrity"_s}});
        return false;
    }
    const std::uint64_t run = ++m_keyRouterRun;
    // From the router's thread, in order; a key still on its way when the
    // routing has ended goes nowhere.
    const auto post = [this, run](auto call) {
        QMetaObject::invokeMethod(this, [this, run, call = std::move(call)] {
            if (m_keyRouter && m_keyRouterRun == run)
                call();
        }, Qt::QueuedConnection);
    };
    KeyRouter::Callbacks callbacks;
    callbacks.key = [this, post](const KeyRouter::Key& key) { post([this, key] { routedKey(key); }); };
    callbacks.clickedAway = [this, post] { post([this] { hideClipboard(); }); }; // as a menu
    callbacks.foregroundChanged = [this, post](HWND window) { post([this, window] { routedForeground(window); }); };
    callbacks.shiftTapped = [this, post] { post([this] { clipboardShiftTapped(); }); };
    callbacks.drained = [this, run] {
        QMetaObject::invokeMethod(this, [this, run] {
            if (m_drainingRun == run)
                m_drainingRouter.reset();
        }, Qt::QueuedConnection);
    };
    const Qt::KeyboardModifier numberKeys = m_settings.clipboardNumberKeys == u"ctrl" ? Qt::ControlModifier
        : m_settings.clipboardNumberKeys == u"off"                                    ? Qt::NoModifier
                                                                                      : Qt::AltModifier;
    auto router = std::make_unique<KeyRouter>(
        target, m_clipDisplayWanted ? m_clipDisplay->handle() : reinterpret_cast<HWND>(m_clipWindow->winId()),
        std::move(callbacks), numberKeys, ownNumberDigits(m_settings.clipboardNumberKeys));
    diagnostics::record(u"clipboard-key-routing-start"_s,
        {{u"session"_s, static_cast<qint64>(run)}, {u"router"_s, router->diagnosticStatus()}});
    if (!router->isActive()) {
        qWarning() << "Clipboard: keyboard routing is unavailable";
        return false;
    }
    m_keyRouter = std::move(router);
    m_keyRouterTarget = target;
    m_clipboard->setKeysRouted(true);
    return true;
}

void App::stopKeyRouting()
{
    // Its own window in front (activated), hidden, given up: the hook would
    // hear nothing more (docs/pitfalls.md), or nothing would be pasted into.
    if (m_hold || m_browseHotkey)
        endClipboardHold("unrouted");
    if (m_keyRouter) {
        // No key is taken any more; the releases of those it took still are
        // its own, for a moment.
        m_keyRouter->drain();
        diagnostics::record(u"clipboard-key-routing-stop"_s,
            {{u"session"_s, static_cast<qint64>(m_keyRouterRun)}, {u"router"_s, m_keyRouter->diagnosticStatus()}});
        m_drainingRouter = std::move(m_keyRouter);
        m_drainingRun = m_keyRouterRun;
        QTimer::singleShot(2s, this, [this, run = m_drainingRun] {
            if (m_drainingRun == run)
                m_drainingRouter.reset(); // a key held that long is the program's again
        });
    }
    m_keyRouterTarget = nullptr;
    if (!m_clipboard->keysRouted())
        return;
    m_clipFrame->setNoActivate(false);
    m_clipboard->setKeysRouted(false);
}

// What the window's own focus would do with the key: to the item that has
// the focus in it (not the active focus, the window has none), then up its
// parents until one takes it.
void App::routedKey(const KeyRouter::Key& key)
{
    if (!m_clipWindow || !m_clipWindow->isVisible())
        return;
    if (key.press && (key.vk == VK_APPS || (key.vk == VK_F10 && key.modifiers == Qt::ShiftModifier))) {
        emit m_clipboard->contextMenuKeyPressed(); // as the window's ContextMenu event (eventFilter)
        return;
    }
    if (key.press && key.vk == VK_F4 && key.modifiers.testFlag(Qt::AltModifier)) {
        m_clipboard->setPinned(false); // closed, pinned or not
        m_clipboard->dismiss(); // closes it, not the program behind
        return;
    }
    if (m_clipDisplay && m_clipDisplay->active()) {
        m_clipDisplay->key(key);
        return;
    }
    QQuickItem* item = m_clipWindow->contentItem();
    while (item->isFocusScope() && item->scopedFocusItem() && item->scopedFocusItem()->isEnabled())
        item = item->scopedFocusItem();
    QKeyEvent event(key.press ? QEvent::KeyPress : QEvent::KeyRelease, key.key, key.modifiers, key.scanCode, key.vk, 0,
                    key.text, key.autoRepeat);
    if (key.press && event.matches(QKeySequence::Paste) && textfield::pasteOneLine(item))
        return; // as eventFilter does for the window's own keys
    for (; item; item = item->parentItem()) {
        event.setAccepted(true);
        QCoreApplication::sendEvent(item, &event);
        if (event.isAccepted())
            break;
    }
}

void App::routedForeground(HWND window)
{
    if (window == m_keyRouterTarget)
        return;
    // Activated after all: from now on as any window with the focus, which
    // goes when it loses it. Only if it still is: the event may be old.
    const HWND clip = m_clipWindow ? reinterpret_cast<HWND>(m_clipWindow->winId()) : nullptr;
    const HWND display = m_clipDisplay && m_clipDisplay->active() ? m_clipDisplay->handle() : nullptr;
    if ((clip && window == clip) || (display && window == display)) {
        if (::GetForegroundWindow() == window)
            stopKeyRouting();
        return;
    }
    hideClipboard(); // another program in front: as a menu
}

bool App::startLauncherRouting()
{
    const HWND front = ::GetForegroundWindow();
    const HWND target = front ? ::GetAncestor(front, GA_ROOT) : nullptr;
    if (launcherRouted() && m_launcherRouting.target == target)
        return true; // there already (asked for again)
    stopLauncherRouting();
    DWORD pid = 0;
    if (target)
        ::GetWindowThreadProcessId(target, &pid);
    // Nothing in front, or a window of Win顺's: nothing to route from. The
    // taskbar (a press on Win顺's box: it comes back with that press), the
    // Start menu (its keys wait for the launcher, which it gives way to) and
    // the shell's other flyouts keep their own ways.
    if (!m_window || !target || pid == ::GetCurrentProcessId() || taskbar::isTaskbar(target) || win::isShellFlyout(target))
        return false;
    // An elevated program while Win顺 is not: Windows keeps the keys typed
    // there from the hook (UIPI).
    if (integrity::higherThanOurs(target)) {
        diagnostics::record(u"launcher-key-routing-skipped"_s, {{u"reason"_s, u"higher-integrity"_s}});
        return false;
    }
    const std::uint64_t run = ++m_launcherRouting.run;
    // From the router's thread, in order; a key still on its way when the
    // routing has ended goes nowhere.
    const auto post = [this, run](auto call) {
        QMetaObject::invokeMethod(this, [this, run, call = std::move(call)] {
            if (m_launcherRouting.router && m_launcherRouting.run == run)
                call();
        }, Qt::QueuedConnection);
    };
    KeyRouter::Callbacks callbacks;
    callbacks.key = [this, post](const KeyRouter::Key& key) { post([this, key] { launcherRoutedKey(key); }); };
    callbacks.clickedAway = [this, post] { post([this] { launcherClickedAway(); }); };
    callbacks.foregroundChanged = [this, post](HWND) { post([this] { launcherRoutedForeground(); }); };
    callbacks.drained = [this, run] {
        QMetaObject::invokeMethod(this, [this, run] {
            if (m_launcherRouting.drainingRun == run)
                m_launcherRouting.draining.reset();
        }, Qt::QueuedConnection);
    };
    const Qt::KeyboardModifier numberKeys = m_settings.numberKeys == u"ctrl" ? Qt::ControlModifier
        : m_settings.numberKeys == u"alt"                                     ? Qt::AltModifier
                                                                              : Qt::NoModifier;
    auto router = std::make_unique<KeyRouter>(target, reinterpret_cast<HWND>(m_window->winId()), std::move(callbacks),
        numberKeys, ownNumberDigits(m_settings.numberKeys));
    const QString program = foreground::programOf(target);
    diagnostics::record(u"launcher-key-routing-start"_s,
        {{u"session"_s, static_cast<qint64>(run)}, {u"front_program"_s, program},
            {u"router"_s, router->diagnosticStatus()}});
    if (!router->isActive()) {
        qWarning() << "Launcher: keyboard routing is unavailable";
        return false;
    }
    m_launcherRouting.router = std::move(router);
    m_launcherRouting.target = target;
    if (m_typing)
        m_typing->setLauncherRouted(true);
    m_launcher->setKeysRouted(true);
    qWarning().noquote() << "Launcher: refused the front, its keys routed to it while" << program << "keeps the focus";
    return true;
}

void App::stopLauncherRouting()
{
    if (m_typing)
        m_typing->setLauncherRouted(false);
    m_launcherRouting.target = nullptr;
    if (!m_launcherRouting.router)
        return;
    // No key is taken any more; the releases of those it took still are its
    // own, for a moment.
    m_launcherRouting.router->drain();
    diagnostics::record(u"launcher-key-routing-stop"_s,
        {{u"session"_s, static_cast<qint64>(m_launcherRouting.run)},
            {u"router"_s, m_launcherRouting.router->diagnosticStatus()}});
    m_launcherRouting.draining = std::move(m_launcherRouting.router);
    m_launcherRouting.drainingRun = m_launcherRouting.run;
    QTimer::singleShot(2s, this, [this, run = m_launcherRouting.drainingRun] {
        if (m_launcherRouting.drainingRun == run)
            m_launcherRouting.draining.reset(); // a key held that long is the program's again
    });
    m_launcher->setKeysRouted(false);
}

// A key routed to the launcher, or held for it and handed over: as the
// clipboard's (routedKey), to the item that has the focus in it (the window
// has no active focus, or only in this thread), then up its parents until
// one takes it.
void App::launcherRoutedKey(const KeyRouter::Key& key)
{
    if (!m_window || !m_window->isVisible() || m_launcherClosing)
        return;
    if (key.press && key.vk == VK_F4 && key.modifiers.testFlag(Qt::AltModifier)) {
        hideLauncher(); // closes it, not the program behind
        return;
    }
    if (key.press && (key.vk == VK_APPS || (key.vk == VK_F10 && key.modifiers == Qt::ShiftModifier))) {
        // As the window's ContextMenu event (eventFilter): Shift+Menu asks for the shell's menu.
        if (!m_launcher->systemMenuBusy())
            emit m_launcher->contextMenuKeyPressed(key.vk == VK_APPS && key.modifiers.testFlag(Qt::ShiftModifier));
        return;
    }
    QQuickItem* item = m_window->contentItem();
    while (item->isFocusScope() && item->scopedFocusItem() && item->scopedFocusItem()->isEnabled())
        item = item->scopedFocusItem();
    QKeyEvent event(key.press ? QEvent::KeyPress : QEvent::KeyRelease, key.key, key.modifiers, key.scanCode, key.vk, 0,
                    key.text, key.autoRepeat);
    if (key.press && event.matches(QKeySequence::Paste) && textfield::pasteOneLine(item))
        return; // as eventFilter does for the window's own keys
    for (; item; item = item->parentItem()) {
        event.setAccepted(true);
        QCoreApplication::sendEvent(item, &event);
        if (event.isAccepted())
            break;
    }
}

// Another window came to the front while the launcher's keys were routed to
// it (its router's event; looked at as of now, the event may be old).
void App::launcherRoutedForeground()
{
    const HWND front = ::GetForegroundWindow();
    const HWND root = front ? ::GetAncestor(front, GA_ROOT) : nullptr;
    if (!root || root == m_launcherRouting.target)
        return; // that window still (told again after the refusal), or none for a moment
    stopLauncherRouting();
    // In front itself after all (a click on it): as any window with the
    // focus from now on. Anything else: as when it loses the focus.
    if (m_window && root != reinterpret_cast<HWND>(m_window->winId()))
        launcherLostFront();
}

// A button pressed outside Win顺's windows while the launcher's keys were
// routed to it: the window it went to had the front all along, no change of
// the front tells of it. As when the launcher loses the focus: it goes,
// unless pinned (it stays, the keys that window's again).
void App::launcherClickedAway()
{
    stopLauncherRouting();
    launcherLostFront();
}

namespace {

// A hotkey's modifiers as the footer names them, in its own order: "Ctrl+Shift".
QString holdKeysOf(const QString& shortcut)
{
    QStringList keys;
    for (const QString& part : shortcut.split(u'+', Qt::SkipEmptyParts)) {
        const QString key = part.trimmed().toLower();
        if (key == u"ctrl" || key == u"control")
            keys << u"Ctrl"_s;
        else if (key == u"alt")
            keys << u"Alt"_s;
        else if (key == u"shift")
            keys << u"Shift"_s;
        else if (key == u"win" || key == u"meta")
            keys << u"Win"_s;
    }
    return keys.join(u'+');
}

} // namespace

void App::armClipboardHold(int hotkey, std::uint64_t pressedAt)
{
    m_browseHotkey = 0;
    if (m_hold || m_holdWatch)
        endClipboardHold("again");
    if (!m_settings.clipboardHoldToStep || (m_clipWindow && m_clipWindow->isVisible() && !m_clipPrewarming.load()))
        return; // off, or the window up already: a press as ever
    const QString shortcut = hotkey == kWinVHotkeyId ? u"Win+V"_s : m_settings.clipboardHotkey.trimmed();
    UINT modifiers = 0;
    UINT vk = 0;
    if (!MessageWindow::parseHotkey(shortcut, &modifiers, &vk) || !(modifiers & hotkeysteps::kModifiers))
        return; // nothing to hold
    // Over a program that keeps the keyboard, as showClipboard routes its
    // keys: else the window takes the keyboard, and the hook hears nothing.
    const HWND foreground = ::GetForegroundWindow();
    const HWND target = paste::usableTarget(foreground);
    // Whether the keys stayed down since the press: only so soon after it.
    const std::uint64_t now = ::GetTickCount64();
    const std::uint64_t late = now > pressedAt ? now - pressedAt : 0;
    if (late > hotkeysteps::kArmWithinMs) {
        diagnostics::record(u"clipboard-hold-skipped"_s, {{u"reason"_s, u"late"_s}, {u"ms"_s, static_cast<qint64>(late)}});
        return;
    }
    if (!target || integrity::higherThanOurs(target)) {
        m_browseHotkey = hotkey;
        m_browseModifiers = modifiers & hotkeysteps::kModifiers;
        m_browseVk = vk;
        m_holdTimer.start();
        return;
    }
    const std::uint64_t run = ++m_holdRun;
    HoldWatch::Callbacks callbacks;
    callbacks.note = [this, run](hotkeysteps::Note note, std::uint32_t time) {
        if (m_holdWatch && m_holdRun == run)
            clipboardHoldNote(note, tickTime(time, ::GetTickCount64()));
    };
    callbacks.drained = [this, run] { // the watch's thread
        QMetaObject::invokeMethod(this, [this, run] {
            if (m_drainingWatchRun == run)
                m_drainingWatch.reset();
        }, Qt::QueuedConnection);
    };
    auto watch = std::make_unique<HoldWatch>(modifiers & hotkeysteps::kModifiers, vk, std::move(callbacks));
    if (!watch->isActive()) { // let go already, or no hook
        diagnostics::record(u"clipboard-hold-skipped"_s,
            {{u"reason"_s, QString::fromLatin1(watch->inactiveReason())},
             {u"error"_s, static_cast<qint64>(watch->setupError())}, {u"ms"_s, static_cast<qint64>(late)}});
        return;
    }
    m_holdWatch = std::move(watch);
    m_hold.emplace(pressedAt);
    m_holdHotkey = hotkey;
    m_holdModifiers = modifiers & hotkeysteps::kModifiers;
    m_holdVk = vk;
    m_holdLate = late;
    m_clipboard->beginSteps(holdKeysOf(shortcut));
    m_holdTimer.start();
}

bool App::clipboardStepPress(int hotkey)
{
    if (m_browseHotkey) {
        if (hotkey == m_browseHotkey && clipboardInCharge()) {
            emit m_clipboard->browseStepRequested();
            return true;
        }
        m_browseHotkey = 0;
    }
    if (!m_hold || hotkey != m_holdHotkey)
        return false;
    if (m_hold->press() == hotkeysteps::Hold::Press::Normal) {
        endClipboardHold("pressed-again"); // part of it let go first: closes, or raises, as ever
        return false;
    }
    m_clipboard->step();
    return true;
}

void App::clipboardHoldNote(hotkeysteps::Note note, std::uint64_t at)
{
    if (!m_hold)
        return;
    using Outcome = hotkeysteps::Hold::Outcome;
    switch (m_hold->note(note, at)) {
    case Outcome::None:
        return;
    case Outcome::Paste: { // as Enter on that row: the window goes, the paste keys to the program, past the hook
        const bool pasted = m_clipboard->finishSteps(m_holdWatch ? m_holdWatch->guardPaste() : nullptr);
        endClipboardHold(pasted ? "paste" : "gone");
        return;
    }
    case Outcome::Cancel: // nothing pasted; the window stays, on that row, until Esc again
        m_clipboard->endSteps();
        return;
    case Outcome::Close: // nothing was waiting to be pasted: Esc as ever
        endClipboardHold("escape");
        m_clipboard->dismiss();
        return;
    case Outcome::End:
        endClipboardHold(note == hotkeysteps::Note::Other ? "other-key"
                : m_hold->cancelled()                      ? "cancelled"
                                                           : "browse");
        return;
    }
}

void App::checkClipboardHold()
{
    if (!m_hold && !m_browseHotkey) {
        m_holdTimer.stop();
        return;
    }
    const auto down = [](int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; };
    unsigned held = 0;
    if (down(VK_CONTROL))
        held |= hotkeysteps::kCtrl;
    if (down(VK_MENU))
        held |= hotkeysteps::kAlt;
    if (down(VK_SHIFT))
        held |= hotkeysteps::kShift;
    if (down(VK_LWIN) || down(VK_RWIN))
        held |= hotkeysteps::kWin;
    const bool keyDown = down(static_cast<int>(m_holdVk));
    const unsigned modifiers = held & m_holdModifiers;
    if (m_browseHotkey) {
        if ((held & m_browseModifiers) != m_browseModifiers || !clipboardInCharge()) {
            m_browseHotkey = 0;
            m_holdTimer.stop();
        }
        return;
    }
    using Held = hotkeysteps::Hold::Held;
    using Poll = hotkeysteps::Hold::Poll;
    // Whole: the modifiers, its key tapped or held (#199: Win held, V let go).
    switch (m_hold->poll(modifiers == m_holdModifiers ? Held::Whole
                         : keyDown || modifiers      ? Held::Part
                                                     : Held::None,
        ::GetTickCount64())) {
    case Poll::None:
        return;
    case Poll::Hint: // letting go pastes the first entry, and the footer says so from now on
        m_clipboard->holdFirst();
        return;
    case Poll::Silent: // Windows dropped the hook (too slow once, the secure desktop): nothing pasted
        endClipboardHold("no-word");
        return;
    }
}

void App::endClipboardHold(const char* why)
{
    m_browseHotkey = 0;
    m_holdTimer.stop();
    if (m_hold) {
        diagnostics::record(u"clipboard-hold-end"_s,
            {{u"why"_s, QString::fromLatin1(why)}, {u"steps"_s, m_hold->steps()},
             {u"heldMs"_s, static_cast<qint64>(m_hold->heldFor(::GetTickCount64()))},
             {u"lateMs"_s, static_cast<qint64>(m_holdLate)}});
        m_hold.reset();
    }
    m_clipboard->endSteps();
    if (m_holdWatch) {
        // The keys as ever from here on; an Esc it took stays its own until
        // let go. Not destroyed here: this may be its window's message.
        m_holdWatch->drain();
        m_drainingWatch = std::move(m_holdWatch); // one still draining before it: gone
        m_drainingWatchRun = m_holdRun;
        QTimer::singleShot(std::chrono::milliseconds(hotkeysteps::kDrainCapMs), this,
            [this, run = m_drainingWatchRun] { capDrainingWatch(run); });
    }
}

void App::capDrainingWatch(std::uint64_t run)
{
    if (m_drainingWatchRun != run || !m_drainingWatch)
        return;
    // Not while the paste keys go past it: a modifier put back halfway could
    // be left as the hand no longer has it.
    if (m_drainingWatch->pastePassing()) {
        QTimer::singleShot(std::chrono::milliseconds(50), this, [this, run] { capDrainingWatch(run); });
        return;
    }
    m_drainingWatch.reset(); // an Esc held that long is the program's again; a paste that long in coming goes unwatched
}

void App::clipboardShiftTapped()
{
    // The layout typed with: the program's in front, as KeyRouter types the
    // keys with it. Not a Chinese one: a Shift alone means nothing to it.
    // Taking the focus for the input method would close what closes without
    // it there (VS Code's quick open), and the paste would go nowhere.
    const HKL layout = ::GetKeyboardLayout(::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr));
    if (win::isChineseLayout(layout))
        emit m_clipboard->pinyinHintRequested();
}

void App::activateClipboard()
{
    if (!m_clipWindow || (!m_keyRouter && !m_clipboard->pinned()))
        return;
    if (m_clipDisplay && m_clipDisplay->active()) {
        m_clipDisplay->activate();
        return; // routedForeground retires routing once the peer owns focus
    }
    if (!m_keyRouter) // pinned, the keys elsewhere: it takes no focus until told
        m_clipFrame->setNoActivate(false);
    // Active first, then no longer drawn as active without being so: the
    // backdrop does not flicker. Keys pressed in between are its own already.
    win::bringToFront(m_clipWindow, win::ShellFlyout::Preserve);
    // Refused (bringToFront says so in the log; it knows at once, the window
    // is this thread's): the keys go on coming through the router. Stopped,
    // they would go back to the program, the window left neither routed nor
    // active.
    if (m_keyRouter && ::GetForegroundWindow() != reinterpret_cast<HWND>(m_clipWindow->winId()))
        return;
    m_clipWindow->requestActivate();
    stopKeyRouting();
}

void App::releaseClipboard(bool giveFocus)
{
    const bool hadFocus = m_clipWindow->isActive() || (m_clipDisplay && m_clipDisplay->active()
        && ::GetForegroundWindow() == m_clipDisplay->handle());
    stopKeyRouting();
    m_clipFrame->setNoActivate(true); // clicks paste into the program in front, which keeps the focus
    if (m_clipDisplay && m_clipDisplay->active()) m_clipDisplay->passive();
    if (giveFocus && hadFocus) {
        if (const HWND target = paste::usableTarget(m_clipboard->target()))
            paste::activate(target);
    }
}

void App::engageClipboard()
{
    if (!m_clipWindow || !m_clipWindow->isVisible())
        return;
    if (m_keyRouter) {
        if (paste::isForeground(m_keyRouterTarget)) {
            diagnostics::record(u"clipboard-key-routing-refresh"_s,
                {{u"session"_s, static_cast<qint64>(m_keyRouterRun)}, {u"router"_s, m_keyRouter->diagnosticStatus()}});
            m_keyRouter->refresh(); // a search-field click, still without taking the focus
            return;
        }
        stopKeyRouting(); // a stale foreground notification left it over a different window
    }
    // As when it opens over a program: that one keeps the focus, the keys
    // come here. Its own window in front (nothing else to paste into): it
    // takes the focus, and pastes where it did last.
    const HWND foreground = ::GetForegroundWindow();
    if (const HWND target = paste::usableTarget(foreground)) {
        m_clipWindow->setProperty("shellWidth", 0);
        m_clipboard->setTarget(target);
        const bool shellFlyout = win::isShellFlyout(foreground);
        if (startKeyRouting(target) || shellFlyout) {
            if (shellFlyout)
                avoidClipboardShellFlyout(foreground);
            m_clipFrame->setNoActivate(true);
            ::SetWindowPos(reinterpret_cast<HWND>(m_clipWindow->winId()), HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return;
        }
    }
    m_clipFrame->setNoActivate(false);
    toFront(m_clipWindow);
    m_clipWindow->requestActivate();
}

void App::clipboardPinnedChanged()
{
    if (!m_clipWindow || !m_clipWindow->isVisible() || !m_clipboard->active() || m_clipPrewarming.load())
        return;
    if (m_clipboard->pinned())
        releaseClipboard(false); // the keys back to the program in front; the window stays
    else
        engageClipboard(); // a menu again: clicking elsewhere hides it
}

void App::uncloakClipboard()
{
    if (!m_clipWindow)
        return;
    if (m_clipDisplayWanted && m_clipDisplay && m_clipDisplay->ready()) {
        m_clipWindow->setProperty("controllerEventsEnabled", false);
        if (m_clipDisplay->show()) {
            m_clipRevealedAt = ::GetTickCount64();
            if (m_triggers) m_triggers->seen(TriggerWindow::Clipboard);
            return; // the ordinary-process scene stays cloaked behind the peer
        }
    }
    if (std::exchange(m_clipDisplayWanted, false)) {
        m_clipWindow->setProperty("controllerEventsEnabled", true);
        emit m_clipboard->queryChanged();
        emit m_clipboard->placed();
        const HWND target = m_clipboard->target();
        stopKeyRouting();
        if (win::isShellFlyout(target)) avoidClipboardShellFlyout(target);
        const bool keepFocus = startKeyRouting(target) || win::isShellFlyout(target);
        m_clipFrame->setNoActivate(keepFocus);
    }
    win::setCloaked(m_clipWindow, false);
    if (m_clipWindow->isVisible() && !m_clipPrewarming.load()) {
        m_clipRevealedAt = ::GetTickCount64(); // there to be seen from now on (seenBy)
        if (m_triggers)
            m_triggers->seen(TriggerWindow::Clipboard);
    }
}

void App::hideClipboard(bool launcherToo)
{
    if (m_clipboard->dragging())
        return;
    if (m_clipboard->pinned() && m_clipWindow && m_clipWindow->isVisible() && !m_clipPrewarming.load()) {
        releaseClipboard(false); // clicked elsewhere, another program in front, the launcher opened
        return;
    }
    if (m_keyRouter)
        m_keyRouter->drain(); // first: the keys go to the program from here on
    if (!m_clipWindow || !m_clipWindow->isVisible() || m_clipPrewarming.load()) {
        stopKeyRouting();
        return;
    }
    const bool underLauncher = clipboardUnderLauncher();
    if (m_clipDisplay) m_clipDisplay->hide();
    m_clipDisplayWanted = false;
    m_clipWindow->hide();
    m_clipRevealedAt.reset(); // only here: pinned, it stays up when it lets go of the keys
    stopKeyRouting(); // hidden first: no inactive backdrop on the way out
    m_clipReveal.cancel();
    win::setCloaked(m_clipWindow, false);
    m_clipboard->setActive(false); // the launcher's rows come back
    m_clipboard->handleHidden();
    // Gone without going back to the search box (pasted elsewhere, clicked
    // away): the launcher goes too.
    if (launcherToo && underLauncher && m_window && !m_window->isActive() && !m_launcher->pinned())
        hideLauncher();
}

bool App::clipboardUnderLauncher() const
{
    const QQuickItem* field = m_clipboard->field();
    return m_clipboard->active() && field && field->window() == m_window;
}

// The first frame a window draws sets up the graphics device, shaders and
// glyphs: drawn ahead, as for the launcher (see prewarmLauncher).
void App::prewarmClipboard()
{
    if (!m_settings.clipboard || (m_clipWindow && m_clipWindow->isVisible()))
        return;
    if (!m_clipWindow && !createClipWindow())
        return;
    m_clipPlacement->placeOn(QGuiApplication::primaryScreen()); // drawn at that scale
    m_clipPrewarming = true;
    win::setCloaked(m_clipWindow, true);
    m_clipWindow->setProperty("_q_showWithoutActivating", true);
    m_clipWindow->show();
    m_clipWindow->setProperty("_q_showWithoutActivating", QVariant());
    QTimer::singleShot(2s, this, &App::finishClipPrewarm); // should no frame come
}

void App::finishClipPrewarm()
{
    if (!m_clipPrewarming.exchange(false))
        return;
    m_clipWindow->hide();
    win::setCloaked(m_clipWindow, false);
}

QQuickItem* App::focusedField() const
{
    auto* window = qobject_cast<QQuickWindow*>(QGuiApplication::focusWindow());
    if (!window || window == m_clipWindow || reinterpret_cast<HWND>(window->winId()) != ::GetForegroundWindow())
        return nullptr; // the clipboard's own search box included
    QQuickItem* item = window->activeFocusItem();
    return textfield::takesTyping(item) ? item : nullptr;
}

// The field's window comes to the front again, which hides the clipboard as
// any window would (see createClipWindow); the field gets the keyboard back
// with it (Clipboard waits for that). The launcher under it stays.
void App::returnToField(QQuickItem* field)
{
    QQuickWindow* window = field->window();
    if (!window)
        return;
    // The dialog bar shows only while its dialog (or a window of ours) is in
    // front: should it be gone, the dialog first, then the bar as soon as it
    // is up again (see createBarWindow).
    if (window == m_barWindow && m_dialogBar && !m_barWindow->isVisible()) {
        m_barFocusDeadline.setRemainingTime(1s);
        m_dialogBar->back();
        return;
    }
    win::bringToFront(window);
    window->requestActivate();
}

void App::armReveal()
{
    m_launcherReveal.arm();
}

void App::revealLauncher()
{
    if (!m_window)
        return;
    // Slide in before revealing, unless it was hidden before its first frame.
    if (m_window->isVisible())
        m_placement->slideIn();
    win::setCloaked(m_window, false);
    if (launcherShown()) {
        m_launcherRevealedAt = ::GetTickCount64(); // there to be seen from now on (seenBy)
        if (m_triggers)
            m_triggers->seen(TriggerWindow::Launcher);
    }
    deliverTyping(); // asked for by typing elsewhere: the keys, if they waited for its first frame
}

bool App::launcherShown() const
{
    return m_window && m_window->isVisible() && !m_launcherClosing && !m_prewarming.load();
}

// The first frame a window draws sets up the graphics device, shaders and
// glyphs. Drawn once ahead, cloaked and without taking the focus, so the first
// open is as quick as later ones (which reuse all that: it is kept while the
// window is hidden).
void App::prewarmLauncher()
{
    win::prepareTextInput(); // also for the tray menu, which takes the focus
    if (!m_window || m_window->isVisible())
        return;
    m_prewarming = true;
    win::setCloaked(m_window, true);
    m_window->setProperty("_q_showWithoutActivating", true);
    m_window->show();
    m_window->setProperty("_q_showWithoutActivating", QVariant());
    QTimer::singleShot(2s, this, &App::finishPrewarm); // should no frame come
}

void App::finishPrewarm()
{
    if (!m_prewarming.exchange(false))
        return;
    m_window->hide();
    win::setCloaked(m_window, false);
}

// The launcher lost the front just after it was asked for, and goes: said
// in the log, with what took it and whether any input came since it was
// put in front (a click or a key elsewhere: the user's doing; none: the
// other program's, a window of its own up or the front taken back with an
// empty SendInput, which counts as input too). Before, it just went, with
// no word anywhere, and a double Ctrl seemed to do nothing (GitHub #111).
// Twenty a run at most.
void App::launcherLostFront()
{
    // Clicking elsewhere or switching apps dismisses the launcher, like a
    // menu; the clipboard under its search box goes with it (see there).
    // Not a press on Win顺's box on the taskbar: that brings it back. Nor
    // the press that just brought it back: Qt may hear of the taskbar
    // taking the focus for it only after that (it slid back up while on
    // its way out). Those leave no window in front (or the taskbar, the
    // pointer on the box), nor does the Start menu closing as it opens
    // over it; another window in front (a click on it, on the desktop,
    // on the rest of the taskbar) dismisses it at once, however soon
    // after it opened.
    if (!m_window || !m_window->isVisible() || clipboardUnderLauncher())
        return;
    // Its keys routed to it, the window it was refused over still in front
    // (GitHub #203): nothing was lost. Qt can say so all the same (active
    // only in Win顺), and Windows 10 tells of that window coming to the front
    // again after the refusal. A press elsewhere is the router's to tell.
    if (launcherRouted() && ::GetAncestor(::GetForegroundWindow(), GA_ROOT) == m_launcherRouting.target)
        return;
    if (m_launcher->contentPane()->modalOperation() || m_launcher->contentPane()->ownsForeground())
        return;
    // The preview (Alt+P) took the focus for a moment: a handler's
    // window of ours (handed back at once), or QuickLook's or Seer's
    // (which hand it back themselves). The launcher stays.
    if (m_preview->ownsForeground()) {
        if (m_preview->foregroundIsOurs())
            QMetaObject::invokeMethod(this, [this] {
                if (m_window && m_window->isVisible() && !m_window->isActive())
                    win::bringToFront(m_window);
            }, Qt::QueuedConnection);
        return;
    }
    // A command of the shell's menu that keeps the launcher (删除,
    // say) runs, or just did: its dialogs, owned by a hidden window
    // of ours, take the focus and hand it back (Launcher::
    // runSystemMenu); another program's window ends that.
    if (m_launcher->systemCommandRunning()) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
        if (pid == ::GetCurrentProcessId())
            return;
        m_launcher->systemCommandDone();
    }
    if (m_launcher->pinned()) { // stays up, behind nothing (topmost), until Esc
        cancelTypingSelection();
        m_typingOrigin = {}; // used elsewhere now: a folder opened later is not for that tab
        m_boxPressGrace.stop();
        // A press on Win顺's box took the focus: that press hides it
        // (searchBoxPressed), as double Ctrl would.
        if (m_searchBox && m_searchBox->underPointer())
            m_pinnedLostToBox.start();
        else
            m_pinnedLostToBox.invalidate();
        return;
    }
    const bool justShown = m_launcherShown.isValid() && m_launcherShown.elapsed() < 500;
    const bool onBox = m_searchBox && m_searchBox->underPointer();
    const HWND front = ::GetForegroundWindow();
    const bool elsewhere = front && front != reinterpret_cast<HWND>(m_window->winId())
        && !(onBox && taskbar::isTaskbar(front)) && !win::isShellFlyout(front);
    if (m_searchBox && m_launcherOverBox && !elsewhere && (justShown || onBox))
        m_boxPressGrace.start();
    else {
        logLauncherLostFront(front);
        hideLauncher();
    }
}

void CALLBACK App::foregroundChanged(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD)
{
    // After what this thread is told of the change (WM_ACTIVATE, Qt's
    // activeChanged): a launcher that heard of it is no longer active here.
    if (!g_launcherApp)
        return;
    QMetaObject::invokeMethod(g_launcherApp, [window] {
        App* app = g_launcherApp;
        if (app)
            app->adminTapFront(window);
        // Another program's window in front while Qt still has one of ours
        // active (no focusWindowChanged comes): the digits are its again.
        if (app && app->quickPasteSuspended() != app->m_quickPasteSuspended)
            app->applyQuickPasteHotkeys();
        if (!app || !app->m_window || !app->m_window->isVisible() || !app->m_window->isActive())
            return;
        DWORD pid = 0;
        const HWND front = ::GetForegroundWindow();
        if (!front || (::GetWindowThreadProcessId(front, &pid), pid == ::GetCurrentProcessId()))
            return;
        if (app->launcherRouted() && ::GetAncestor(front, GA_ROOT) == app->m_launcherRouting.target)
            return; // the window it was refused over, its keys routed to it (GitHub #203): nothing lost
        if (!app->m_launcher->pinned())
            qWarning().noquote() << "Launcher: active only in Win顺, another program's window in front: as if it lost the focus";
        app->launcherLostFront();
        app->updateNumberKeys();
    }, Qt::QueuedConnection);
}

// ---- double tap over a program run as administrator (AdminTapHint) ------------

namespace {

// The program's own name for itself ("注册表编辑器", "微信"): its file
// description, else its file name without ".exe".
QString programTitle(HWND window)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.valid())
        return {};
    wchar_t path[MAX_PATH] {};
    DWORD size = MAX_PATH;
    if (!::QueryFullProcessImageNameW(process.get(), 0, path, &size))
        return {};
    QString title;
    DWORD ignored = 0;
    if (const DWORD bytes = ::GetFileVersionInfoSizeW(path, &ignored); bytes > 0) {
        std::vector<char> info(bytes);
        struct Translation {
            WORD language;
            WORD codePage;
        };
        Translation* translations = nullptr;
        UINT length = 0;
        if (::GetFileVersionInfoW(path, 0, bytes, info.data())
            && ::VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length)
            && length >= sizeof(Translation)) {
            wchar_t key[64];
            ::swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].language,
                translations[0].codePage);
            wchar_t* description = nullptr;
            if (::VerQueryValueW(info.data(), key, reinterpret_cast<void**>(&description), &length) && length > 1)
                title = QString::fromWCharArray(description).trimmed();
        }
    }
    return title.isEmpty() ? QFileInfo(QString::fromWCharArray(path, static_cast<int>(size))).completeBaseName() : title;
}

} // namespace

// Any window came to the front: one of a program run as administrator is
// looked at once it has stayed there a moment (adminTapCheck). Nothing to
// look at when there is nothing to say: no double tap, Win顺 elevated (it
// hears every key), or said already.
void App::adminTapFront(HWND window)
{
    if (!m_settings.doubleCtrl || m_adminTapShownThisRun || integrity::processElevated()
        || m_settings.adminTapHinted.size() >= admintap::kMostPrograms) {
        m_adminTapDwell.stop();
        return;
    }
    m_adminTapWindow = window;
    m_adminTapDwell.start();
}

void App::adminTapCheck()
{
    using namespace admintap;
    const HWND front = ::GetForegroundWindow();
    if (!front || front != m_adminTapWindow || !m_keyListener)
        return;
    Facts f;
    f.level = integrity::levelOf(front);
    if (f.level != kAdministratorLevel)
        return; // the usual case: not even logged
    f.elevated = integrity::elevated(front);
    f.doubleTap = m_settings.doubleCtrl && m_keyListener->isActive();
    // Who hears the tap there: the service's UIAccess listener for this key;
    // an old one (from before double Alt or Shift); a service that runs and
    // has not answered yet; one stopped or disabled; none.
    if (integrity::processElevated() || m_keyListener->serviceTells()) {
        f.listener = Listener::Hears;
    } else if (m_keyListener->serviceActive()) {
        f.listener = Listener::Old;
    } else {
        const ntfs::ServiceState state = ntfs::ServiceVolumeAccess::state();
        f.listener = state == ntfs::ServiceState::NotInstalled ? Listener::None
            : state == ntfs::ServiceState::Disabled || state == ntfs::ServiceState::Stopped ? Listener::Off
                                                                                               : Listener::Connecting;
    }
    f.settled = m_keyListenerMade.isValid() && m_keyListenerMade.elapsed() >= kAdminTapSettleMs;
    f.trayShown = m_messages && m_messages->trayVisible();
    f.program = foreground::programOf(front);
    f.ignoredThere = doubleCtrlIgnoreReason(foreground::facts(),
                         {m_settings.doubleCtrlPauseInGames, m_settings.doubleCtrlPauseInFullScreen,
                             m_settings.doubleCtrlExcludedApps})
        != IgnoreReason::None;
    f.shownThisRun = m_adminTapShownThisRun;
    f.shown = m_settings.adminTapHinted;
    const Verdict verdict = decide(f);
    // Only a portable copy's settings install the service (the row under
    // 索引方式); an installed one's have no button for it.
    const bool canInstall = userIsAdministrator() && !Settings::portableFolder().isEmpty()
        && QFile::exists(QCoreApplication::applicationDirPath() + u"/WinShunService.exe"_s);
    const Advice advice = advise(m_hotkeyOverAdmin, fixFor(f.listener, canInstall));
    // Each program and verdict once a run, in the log and the journal.
    if (const QString seen = f.program.toLower() + u' ' + QLatin1StringView(name(verdict)); !m_adminTapLogged.contains(seen)) {
        m_adminTapLogged.push_back(seen);
        diagnostics::record(u"admin-tap-hint"_s,
            {{u"program"_s, f.program}, {u"verdict"_s, QLatin1StringView(name(verdict))},
                {u"listener"_s, QLatin1StringView(name(f.listener))}, {u"fix"_s, QLatin1StringView(name(advice.fix))},
                {u"hotkey"_s, advice.hotkey}});
        const QString line = u"Double tap over %1 (run as administrator): %2; service listener %3, fix %4, hotkey %5"_s.arg(
            f.program, QLatin1StringView(name(verdict)), QLatin1StringView(name(f.listener)),
            QLatin1StringView(name(advice.fix)), advice.hotkey ? m_settings.hotkey.trimmed() : u"none"_s);
        if (verdict == Verdict::Show)
            qWarning().noquote() << line;
        else
            qInfo().noquote() << line;
    }
    if (verdict != Verdict::Show)
        return;
    m_adminTapShownThisRun = true;
    m_settings.adminTapHinted = remember(m_settings.adminTapHinted, f.program);
    m_settings.save();
    if (m_settingsEditor)
        m_settingsEditor->setSettings(m_settings); // its copy must not forget it with its next change
    m_backup->soon();

    const QString tap = doubleCtrlName();
    const QString hotkey = m_settings.hotkey.trimmed();
    QString text;
    if (advice.hotkey) {
        text = advice.fix == Fix::Install
            ? tr("它在最前面时，Win顺 收不到%1，可以改按 %2。点这里去装上索引服务，%1 就也能用了。").arg(tap, hotkey)
            : advice.fix == Fix::Update
            ? tr("它在最前面时，Win顺 收不到%1，可以改按 %2。点这里去更新索引服务，%1 就也能用了。").arg(tap, hotkey)
            : tr("它在最前面时，Win顺 收不到%1，请改按 %2。").arg(tap, hotkey);
    } else {
        text = advice.fix == Fix::Install
            ? tr("它在最前面时，Win顺 收不到%1。点这里去装上索引服务就好；也可以在设置里加一个快捷键。").arg(tap)
            : advice.fix == Fix::Update
            ? tr("它在最前面时，Win顺 收不到%1。点这里去更新索引服务就好；也可以在设置里加一个快捷键。").arg(tap)
            : tr("它在最前面时，Win顺 收不到%1。点这里设一个快捷键（比如 Alt+Space），在它前面也能用。").arg(tap);
    }
    std::function<void()> onClick;
    if (advice.click == Click::Service) {
        // The row with the install button, under 索引方式.
        onClick = [this] {
            showSettings([this](QQuickWindow* window) {
                refreshIndexAccess(); // a window made just now knows nothing yet of this copy (portable: that row shown)
                QMetaObject::invokeMethod(window, "showPlace", Q_ARG(QVariant, QVariant(u"indexService"_s)));
            });
        };
    }
    else if (advice.click == Click::Hotkey)
        onClick = [this] { showSettingsFor(u"Launcher/Hotkey"_s); };
    QString who = programTitle(front);
    if (who.isEmpty())
        who = f.program;
    m_messages->showNotification(tr("“%1”以管理员身份运行").arg(shortTitle(who)), text, std::move(onClick));
}

void App::logLauncherLostFront(HWND front)
{
    static constexpr qint64 kJustShownMs = 1500;
    if (!m_launcherShown.isValid() || m_launcherShown.elapsed() > kJustShownMs || m_launcherLostFrontLogged >= 20)
        return;
    ++m_launcherLostFrontLogged;
    LASTINPUTINFO last {sizeof last};
    const bool input = !::GetLastInputInfo(&last) || static_cast<std::int32_t>(last.dwTime - m_launcherFrontTick) > 0;
    QString what = u"no window"_s;
    if (front) {
        wchar_t className[256] {};
        ::GetClassNameW(front, className, static_cast<int>(std::size(className)));
        what = QString::fromWCharArray(className) + u" (" + foreground::programOf(front) + u')';
    }
    qWarning().noquote() << "Launcher: lost the front" << m_launcherShown.elapsed() << "ms after it was asked for, to"
                         << what << (input ? "- input since" : "- no input since") << "- it goes";
}

void App::hideLauncher()
{
    cancelTypingSelection();
    m_typingOrigin = {}; // whatever opened it, that opening is over
    stopLauncherRouting(); // the keys are the window's in front again
    if (!m_window || !m_window->isVisible() || m_launcherClosing)
        return;
    if (m_launcher->systemMenuOpen())
        ::EndMenu(); // the shell's menu over it (Shift+right-click) goes with it: double Ctrl, say
    m_launcherHidden.start();
    m_boxPressGrace.stop();
    restoreLauncherInputMode(); // while it may still have the focus (an input method shared by all programs follows)
    if (std::exchange(m_deliverTyping, false)) {
        // Going before the keys typed for it went (they wait for it to be
        // ready, deliverTyping): once it was active, the hook has them as it
        // did before that wait, asking for the launcher again from the Start
        // menu (GitHub #66). Not ready: they go once it says it is again,
        // shown anew, never into it as it is now (GitHub #103). Never active,
        // its hold gives up on them.
        m_typingCap.stop();
        const bool wasActive = m_handoff.active != std::chrono::steady_clock::time_point {};
        m_handoff.outcome = wasActive ? u"hidden"_s : u"hidden-before-active"_s;
        if (wasActive && m_typing) {
            m_handoff.delivered = std::chrono::steady_clock::now();
            m_typing->deliver(false);
        }
    }
    endHandoff();
    m_launcher->closePreview(); // at once, not after the slide
    if (clipboardUnderLauncher())
        hideClipboard(false);
    // Slid out, unless it was never seen (still waiting for its first frame).
    if (m_launcherReveal.pending() || m_prewarming.load()) {
        finishHidingLauncher();
        return;
    }
    m_launcherClosing = true;
    // The box idle at once; the launcher keeps its look (in the box: no
    // field) until it is gone.
    if (m_searchBox && m_launcherOverBox) {
        m_searchBox->setText({}); // the placeholder again
        updateSearchBoxLook();
    }
    m_placement->slideOut([this] {
        if (m_launcherClosing)
            finishHidingLauncher();
    });
}

void App::finishHidingLauncher()
{
    // Gone, no longer on its way out: what comes next is a new opening. Not
    // earlier (hideLauncher): brought back while it slid out, it was seen
    // all along, and a press after that closes it.
    m_launcherRevealedAt.reset();
    m_launcherClosing = false;
    m_launcherOverBox = false;
    if (m_launcher->inTaskbarBox()) {
        m_launcher->setInTaskbarBox(false);
        if (m_searchBox) {
            m_searchBox->setText({});
            updateSearchBoxLook();
        }
    }
    if (!m_window || !m_window->isVisible())
        return;
    m_window->hide();
    m_launcherReveal.cancel();
    win::setCloaked(m_window, false); // hidden before its first frame: drop the cloak
    m_launcher->handleHidden();
    if (m_launcher->panel())
        setPanel(false); // the next opening is the box (hidden: nothing to see of it)
}

void App::togglePanel()
{
    if (m_window && m_window->isVisible() && !m_launcherClosing)
        setPanel(!m_launcher->panel());
}

void App::launcherPinnedChanged()
{
    // Over the box, its field drawn there: pinned, the window is one of its
    // own (it stays while the user works elsewhere, and can be dragged
    // away), so the field comes back into it and the box goes idle. Where it
    // stands, it stays; the next press on the box brings it to the front.
    if (!m_launcher->pinned() || !launcherShown() || !m_launcherOverBox)
        return;
    if (m_launcher->inTaskbarBox())
        m_launcher->setInTaskbarBox(false);
    if (m_searchBox) {
        m_searchBox->setText({});
        updateSearchBoxLook();
    }
    m_launcherOverBox = false;
}

void App::setPanel(bool on)
{
    if (!m_window || on == m_launcher->panel())
        return;
    if (on) {
        if (!m_window->isVisible() || m_launcherClosing || m_launcherReveal.pending() || m_prewarming.load())
            return;
        // Over Win顺's box on the taskbar: the field comes back into the
        // window, the box goes idle (the panel is a window of its own).
        if (m_launcher->inTaskbarBox())
            m_launcher->setInTaskbarBox(false);
        if (m_searchBox && m_launcherOverBox) {
            m_searchBox->setText({});
            updateSearchBoxLook();
        }
        m_launcherOverBox = false;
        m_frame->setResizable(true);
        m_launcher->setPanel(true); // Main.qml lets go of the window's size and lays the panel out
        m_placement->enterPanel(m_window->screen()); // one move and resize: the panel's size at its spot
        return;
    }
    // Back to the box. A window made smaller shows its last frame cut until
    // it draws the next (pitfalls: the type strip): cloaked meanwhile, and
    // seen again once a frame of the box is on the screen.
    const std::optional<taskbar::Spot> spot = m_placement->taskbarSpot();
    const bool seen = m_window->isVisible() && !m_launcherClosing && !m_launcherReveal.pending() && !m_prewarming.load();
    if (seen) {
        win::setCloaked(m_window, true);
        auto connection = std::make_shared<QMetaObject::Connection>();
        const auto uncloak = [this, connection] {
            QObject::disconnect(*connection);
            if (m_window && !m_launcherReveal.pending() && !m_prewarming.load())
                win::setCloaked(m_window, false);
        };
        *connection = connect(m_frame.get(), &WindowFrame::frameShown, this, uncloak);
        QTimer::singleShot(250ms, this, uncloak); // should no frame come
        m_frame->afterNextFrame();
    }
    m_placement->leavePanel();
    m_frame->setResizable(false);
    m_launcher->setPanel(false); // Main.qml: the box's size again, before it is placed
    if (spot)
        m_placement->attachTaskbar(*spot);
    else
        m_placement->placeOn(m_window->screen() ? m_window->screen() : QGuiApplication::primaryScreen());
}

void App::showTrayMenu()
{
    // The click that asked for the menu was the last input (tick count, ms).
    LASTINPUTINFO input {sizeof input};
    ::GetLastInputInfo(&input);
    const DWORD asked = ::GetTickCount();
    const bool ready = m_index->state() == IndexService::State::Ready;
    // What is going on, in the words the settings' 运行状况 card uses (loading
    // is not building: the snapshot is read, nothing is built).
    const QStringList doing = m_background->lines(2);
    const QString info = !doing.isEmpty() ? doing.first()
        : ready ? tr("已索引 %Ln 项", nullptr, static_cast<int>(m_index->itemCount()))
                : tr("正在加载索引…");
    const QString show = tr("打开 Win顺");
    std::vector<shell::MenuItem> items {
        {ShowCommand, m_settings.doubleCtrl ? show + u'\t' + doubleCtrlName() : show, false, true, true},
        {ClipboardCommand, m_clipboardShortcut.isEmpty() ? tr("剪贴板历史") : tr("剪贴板历史") + u'\t' + m_clipboardShortcut},
        {PauseClipboardCommand, tr("暂停记录剪贴板"), m_clipboardPaused, m_settings.clipboard},
        shell::MenuItem::separator(),
        {IndexInfo, info, false, false},
        {RebuildCommand, tr("重建索引")},
        shell::MenuItem::separator(),
        {AutostartCommand, tr("开机自动启动"), autostart::isEnabled()},
        {SettingsCommand, tr("设置…")},
        {CheckUpdateCommand, tr("检查更新…") + u'\t' + QCoreApplication::applicationVersion()},
        shell::MenuItem::separator(),
        {RestartCommand, tr("重新启动")},
        {QuitCommand, tr("退出")},
    };
    // A second job (content being read while the snapshot is saved, say).
    if (doing.size() > 1) {
        const auto at = std::ranges::find_if(items, [](const shell::MenuItem& i) { return i.id == IndexInfo; });
        items.insert(at + 1, {ContentInfo, doing.at(1), false, false});
    }
    // The index service not reached (GitHub #21): said here, the reason in the settings.
    if (m_serviceWanted && m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None) {
        const auto at = std::ranges::find_if(items, [](const shell::MenuItem& i) { return i.id == IndexInfo; });
        QString text = tr("索引服务未连接，正在遍历文件夹");
        if (m_serviceState == ntfs::ServiceState::NotInstalled)
            text = m_foldersInEffect ? tr("没有索引服务，只索引选中的文件夹") : tr("没有索引服务，正在遍历文件夹");
        else if (m_serviceState == ntfs::ServiceState::Disabled)
            text = m_foldersInEffect ? tr("索引服务已停用，只索引选中的文件夹") : tr("索引服务已停用，正在遍历文件夹");
        else if (m_serviceState == ntfs::ServiceState::Starting)
            text = tr("正在等索引服务启动…");
        else if (m_foldersInEffect)
            text = tr("索引服务未连接，只索引选中的文件夹");
        items.insert(at + 1, {AccessInfo, text, false, false});
    }
    // The plugins' submenus (trayMenu: the screenshot's), under the
    // clipboard's: what each said as the icon was pressed (prefetchTrayMenus).
    std::vector<std::pair<QString, QString>> pluginChoices; // PluginMenuCommand + i: the plugin, its entry
    if (m_pluginHost) {
        auto at = std::ranges::find_if(items, [](const shell::MenuItem& i) { return i.id == PauseClipboardCommand; });
        for (const auto& [id, entries] : m_trayMenus) {
            const plugin::Entry* entry = m_pluginHost->catalog().find(id);
            if (!entry || !m_pluginHost->catalog().enabled(id) || entries.isEmpty())
                continue;
            // {id, title, checked, enabled, children} or {} for a line; two levels at most.
            const auto convert = [&](const QJsonArray& array, auto& self, int depth) -> std::vector<shell::MenuItem> {
                std::vector<shell::MenuItem> out;
                for (const QJsonValue& v : array) {
                    const QJsonObject o = v.toObject();
                    const QString title = o.value(u"title"_s).toString();
                    if (title.isEmpty()) {
                        if (!out.empty() && out.back().id != 0)
                            out.push_back(shell::MenuItem::separator());
                        continue;
                    }
                    shell::MenuItem item {0, title, o.value(u"checked"_s).toBool(), o.value(u"enabled"_s).toBool(true)};
                    if (o.value(u"children"_s).isArray() && depth < 2) {
                        item.children = self(o.value(u"children"_s).toArray(), self, depth + 1);
                    } else {
                        item.id = PluginMenuCommand + static_cast<int>(pluginChoices.size());
                        pluginChoices.emplace_back(id, o.value(u"id"_s).toString());
                    }
                    out.push_back(std::move(item));
                }
                return out;
            };
            shell::MenuItem menu {0, entry->manifest->trayMenu.pick(m_language)};
            menu.children = convert(entries, convert, 1);
            if (menu.text.isEmpty() || menu.children.empty())
                continue;
            at = items.insert(at + 1, std::move(menu));
        }
    }
    // Win顺 keeps the computer awake: said at the top, and a click ends it. Not there when it is off.
    if (const SwitchState awake = m_switches ? m_switches->state(u"keep-awake") : SwitchState {};
        awake.on == SwitchState::Value::On) {
        items.insert(items.begin(),
            {{KeepAwakeCommand,
                 awake.until.isValid() ? tr("保持唤醒（到 %1）").arg(awake.until.time().toString(u"HH:mm"_s)) : tr("保持唤醒"),
                 true},
                shell::MenuItem::separator()});
    }
    if (m_pluginUpdates && m_pluginUpdates->count() > 0) // under the line for Win顺's own update, if there is one
        items.insert(items.begin(), {{PluginUpdatesCommand, tr("%n 个插件有新版本…", "", m_pluginUpdates->count())}, shell::MenuItem::separator()});
    if (m_updater->available() && !m_updater->skipped()) // at the top, where it is seen
        items.insert(items.begin(),
            {{UpdateCommand, tr("Win顺 %1 可以更新了…").arg(m_updater->availableVersion())}, shell::MenuItem::separator()});
    POINT pos {};
    ::GetCursorPos(&pos);
    const DWORD built = ::GetTickCount();
    // A slow menu leaves a line in the log: which part it waited on.
    for (auto& item : items) {
        char16_t glyph = 0;
        switch (item.id) {
        case ShowCommand: glyph = 0xE721; break;
        case ClipboardCommand: glyph = 0xE77F; break;
        case PauseClipboardCommand: glyph = 0xE769; break;
        case IndexInfo: glyph = 0xE968; break;
        case ContentInfo: glyph = 0xE8A5; break;
        case AccessInfo: glyph = 0xE7BA; break;
        case RebuildCommand: glyph = 0xE895; break;
        case AutostartCommand: glyph = 0xE7E8; break;
        case SettingsCommand: glyph = 0xE713; break;
        case CheckUpdateCommand: case UpdateCommand: case PluginUpdatesCommand: glyph = 0xE896; break;
        case KeepAwakeCommand: glyph = 0xE823; break;
        case RestartCommand: glyph = 0xE72C; break;
        case QuitCommand: glyph = 0xE8BB; break;
        default: if (!item.children.empty()) glyph = 0xEA86; break;
        }
        if (glyph)
            item.glyph = QString(QChar(glyph));
        item.secondary = item.id == IndexInfo || item.id == ContentInfo || item.id == AccessInfo;
    }
    QObject* theme = m_qml->singletonInstance<QObject*>(u"WinShun"_s, u"Theme"_s);
    const int chosen = traymenu::popup(m_messages->hwnd(), items, pos, theme, [&](HWND, HMENU) {
        const DWORD shown = ::GetTickCount();
        if (shown - input.dwTime >= kSlowMenuMs)
            qWarning().nospace() << "Tray menu shown " << shown - input.dwTime << " ms after the click: "
                                 << asked - input.dwTime << " ms until it was asked for, " << built - asked
                                 << " ms to build, " << shown - built << " ms for Windows to show it";
    });
    if (chosen != QuitCommand && chosen != RestartCommand)
        readAutostart(); // changed outside WinShun? Ready for the next time
    if (chosen >= PluginMenuCommand && chosen - PluginMenuCommand < static_cast<int>(pluginChoices.size())) {
        const auto& [plugin, entry] = pluginChoices[chosen - PluginMenuCommand];
        m_pluginHost->notify(plugin, u"menu"_s, {{u"id"_s, entry}}, true);
    }
    prefetchTrayMenus(); // what changed meanwhile, for a menu opened by the keyboard
    switch (chosen) {
    case ShowCommand:
        showLauncher();
        break;
    case ClipboardCommand:
        showClipboard();
        break;
    case PauseClipboardCommand:
        setClipboardPaused(!m_clipboardPaused);
        break;
    case RebuildCommand:
        m_index->rebuild();
        m_background->rebuildRequested();
        m_apps->refresh(true); // the apps too: what people mean by it (#207)
        m_messages->showNotification(tr("正在重建索引"), tr("可以照常搜索，完成后结果会自动更新。"));
        break;
    case AutostartCommand:
        setAutostart(!autostart::isEnabled());
        break;
    case SettingsCommand:
        showSettings();
        break;
    case KeepAwakeCommand:
        if (m_switches)
            (void)m_switches->apply(u"keep-awake", SwitchIntent {SwitchIntent::Kind::Off, 0});
        break;
    case UpdateCommand:
        showUpdate();
        break;
    case PluginUpdatesCommand:
        showPluginUpdates();
        break;
    case CheckUpdateCommand:
        // A version already known is shown at once; otherwise the dialog says "checking".
        if (!m_updater->available())
            m_updater->check(true);
        showUpdate();
        break;
    case RestartCommand:
        restart({u"--background"_s});
        break;
    case QuitCommand:
        QCoreApplication::exit(0);
        break;
    default:
        break;
    }
}

void App::restart(const QStringList& arguments, Elevation elevation)
{
    m_restartArguments = arguments;
    m_restartElevation = elevation;
    // From the settings: they open again where they were.
    if (arguments.contains(u"--settings"_s) && !arguments.contains(u"--settings-at"_s) && m_settingsWindow
        && m_settingsWindow->isVisible()) {
        QVariant place;
        if (QMetaObject::invokeMethod(m_settingsWindow, "place", Q_RETURN_ARG(QVariant, place)) && place.isValid())
            *m_restartArguments << u"--settings-at"_s << place.toString();
    }
    QCoreApplication::exit(0);
}

// ---- 索引方式: how the volumes are read (GitHub #21) --------------------------

void App::applyAutostartLevel()
{
    const std::optional<bool> task = autostart::taskElevated();
    // A portable copy upgraded in place: its task from before 0.8.0 runs it
    // elevated at login, while a double click runs it with normal rights now
    // (the manifest's asInvoker), with another way to the volumes and other
    // folders seen. The old way is kept, as the mode, said so in the
    // settings; another mode chosen rewrites the task. An installed copy's
    // task is rewritten by the installer (--take-autostart).
    if (task == true && m_settings.indexAccess == u"auto" && !Settings::portableFolder().isEmpty()) {
        m_settings.indexAccess = u"direct"_s;
        m_settings.save();
        m_accessTakenFromTask = true; // worded in refreshIndexAccess, once the language is applied
    }
    autostart::setElevated(m_settings.indexAccess == u"direct" && userIsAdministrator());
    reconcileAutostart(task);
}

void App::reconcileAutostart(const std::optional<bool>& task)
{
    const bool want = autostart::elevated();
    if (!task || *task == want)
        return;
    // Only an elevated process may: a task that runs elevated is refused to
    // one with normal rights even to make it run with those (or to delete
    // it; measured 2026-10-14, pitfalls), and one may not raise a task
    // either. The elevated copy does it as it starts: at login, by the very
    // task, or restarted as administrator from the settings.
    if (!integrity::processElevated())
        return;
    qWarning("WinShun: the autostart task now starts Win顺 %s", want ? "as administrator" : "with normal rights");
    autostart::setEnabled(true);
}

void App::setAutostart(bool on)
{
    autostart::setEnabled(on);
    // Windows refuses a process with normal rights to remove a task that
    // starts Win顺 elevated (管理员直读; GitHub #67: the switch would not
    // move): removed as administrator then, UAC asks.
    if (!on && autostart::isEnabled() && !integrity::processElevated()) {
        autostart::removeAsAdministrator([this](bool ran) {
            QMetaObject::invokeMethod(
                this,
                [this, ran] {
                    showAutostart();
                    if (autostart::isEnabled()) {
                        m_messages->showNotification(tr("开机自启没有关闭"),
                            ran ? tr("Windows 没有让 Win顺 删除它。")
                                : tr("它是以管理员身份设置的，关闭时要在 Windows 的提示里点“是”。"));
                    }
                },
                Qt::QueuedConnection);
        });
    }
    showAutostart(); // what it is now, written or not
}

void App::readAutostart()
{
    autostart::refresh([this] { QMetaObject::invokeMethod(this, [this] { showAutostart(); }, Qt::QueuedConnection); });
}

void App::showAutostart()
{
    if (m_settingsEditor)
        m_settingsEditor->refreshAutostart();
}

void App::applyIndexAccess()
{
    const bool elevated = integrity::processElevated();
    const bool wantElevated = m_settings.indexAccess == u"direct" && userIsAdministrator();
    autostart::setElevated(wantElevated);
    if (wantElevated && !elevated) {
        // Only an elevated process may register the task so: the copy started
        // as administrator does, as it starts. UAC asks once.
        restart({u"--settings"_s, u"--settings-at"_s, u"indexAccess"_s}, Elevation::Elevated);
        return;
    }
    reconcileAutostart(autostart::taskElevated()); // elevated, this copy may write either level
    if (!wantElevated && elevated) {
        restart({u"--settings"_s, u"--settings-at"_s, u"indexAccess"_s}, Elevation::Unelevated); // the user's normal rights, as the task has it now
        return;
    }
    // The same rights as before: the way to the volumes changes in place.
    AccessChoice choice = chooseVolumeAccess(m_settings.indexAccess);
    m_serviceWanted = choice.wantsService;
    m_serviceWhy = choice.why;
    m_serviceState = choice.state;
    const bool none = choice.access->kind() == ntfs::VolumeAccess::Kind::None;
    m_index->setVolumeAccess(std::move(choice.access));
    m_serviceRetry.stop();
    if (m_serviceWanted) {
        if (none)
            startServiceRetry();
        else
            m_serviceRetry.start(kServiceRecheck);
    }
    applyIndexScope(false); // the user's own choice: the settings say what came of it
}

void App::startServiceRetry()
{
    m_serviceRetrySince.start();
    // Not installed or switched off: no hurry, it is looked for every few minutes.
    m_serviceRetry.start(serviceLongAbsent(m_serviceState) ? kServiceRecheck : kServiceRetry);
}

void App::probeService()
{
    if (m_serviceProbing.exchange(true))
        return; // the last look is not back yet
    if (m_serviceProbe.joinable())
        m_serviceProbe.join(); // done: its outcome was handled (or dropped) on this thread
    // Not on the GUI thread: connecting waits for an instance of the pipe
    // (2 s at most) when every one is taken.
    m_serviceProbe = std::jthread([this] {
        auto service = std::make_shared<ntfs::ServiceVolumeAccess>();
        const ntfs::ServiceState state = ntfs::ServiceVolumeAccess::state(); // looked at each time: it may be installed or started meanwhile
        const bool reached = !serviceAbsent(state) && service->probe();
        QMetaObject::invokeMethod(
            this, [this, service = std::move(service), reached, state] { serviceProbed(service, reached, state); },
            Qt::QueuedConnection);
    });
}

void App::serviceProbed(std::shared_ptr<ntfs::ServiceVolumeAccess> service, bool reached, ntfs::ServiceState state)
{
    m_serviceProbing = false;
    m_serviceState = state;
    if (!m_serviceWanted || !m_index)
        return;
    if (reached) {
        // Not in use yet (it came up after us), or in use but failing of late
        // (away for a while: its journals were given up, those volumes
        // walked): taken from now on, the walked volumes read from their MFT.
        const auto inUse = std::dynamic_pointer_cast<ntfs::ServiceVolumeAccess>(m_index->volumeAccess());
        if (!inUse || !inUse->lastError().empty()) {
            qWarning("WinShun: the index service (%ls) is used from now on", service->serviceVersion().c_str());
            m_index->setVolumeAccess(service);
        }
        m_serviceWhy.clear();
    } else {
        m_serviceWhy = serviceAbsent(state) ? kServiceNotRunning : QString::fromStdWString(service->lastError());
        // Uninstalled or disabled while in use (GitHub #52): given up, so
        // that the chosen folders take over where they would. With whole
        // disks the failing service stays, as before: those are walked.
        if (serviceLongAbsent(state) && m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::Service
            && foldersInEffect(true)) {
            qWarning("WinShun: the index service is gone (%s): only the chosen folders from now on",
                ntfs::ServiceVolumeAccess::stateName(state));
            m_index->setVolumeAccess(std::make_shared<ntfs::NoVolumeAccess>());
        }
    }
    const bool none = m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None;
    const bool soon = none && !serviceLongAbsent(state) && m_serviceRetrySince.isValid()
        && m_serviceRetrySince.elapsed() < std::chrono::milliseconds(kServiceWait).count();
    m_serviceRetry.start(soon ? kServiceRetry : kServiceRecheck);
    applyIndexScope(true);
}

bool App::foldersInEffect(bool walked) const
{
    return m_settings.foldersInEffect(walked, integrity::processElevated(), m_serviceWanted, m_serviceState);
}

void App::applyIndexScope(bool told)
{
    const bool folders = foldersInEffect(m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None);
    if (folders != m_foldersInEffect) {
        if (told)
            m_messages->showNotification(tr("索引范围"), folders ? tr("索引服务不可用：改为只索引选中的文件夹（可在设置里改）")
                                                                  : tr("索引服务可用了：改为索引整个磁盘"));
        m_foldersInEffect = folders;
        if (folders)
            takeDefaultIndexFolders();
        m_index->setScope(folders ? std::optional(m_settings.indexFolderPaths()) : std::nullopt);
        tellLauncherFolders();
    }
    refreshIndexAccess();
    refreshFolderStatuses();
}

void App::takeDefaultIndexFolders()
{
    if (!m_settings.chooseDefaultIndexFolders())
        return;
    m_settings.save();
    if (m_settingsEditor)
        m_settingsEditor->setSettings(m_settings); // its copy must not drop the flag with its next change
    if (m_backup)
        m_backup->soon();
}

void App::tellLauncherFolders()
{
    if (!m_launcher)
        return;
    QStringList chosen;
    for (const std::wstring& path : m_settings.indexFolderPaths())
        chosen.push_back(QString::fromStdWString(path));
    m_launcher->setChosenFolders(m_foldersInEffect, chosen, m_settings.folderHintDismissed);
}

void App::addFolderFromLauncher(const QString& path)
{
    QString problem;
    bool added = false;
    if (m_settingsEditor) { // open: its copy of the settings is the one to change
        problem = m_settingsEditor->addFolder(path);
        added = problem.isEmpty();
    } else {
        problem = SettingsEditor::addFolderTo(m_settings, path, added);
        if (added) {
            m_settings.save();
            applySettings(false);
            m_backup->soon();
        }
    }
    if (added) {
        m_launcher->flash(tr("已加入索引，几秒后就能直接搜到"));
    } else if (!problem.isEmpty()) {
        m_messages->showNotification(tr("没能加入索引"), problem);
    }
}

void App::refreshFolderStatuses()
{
    if (m_settingsEditor)
        m_settingsEditor->setFolderStatuses(m_index->folderStatuses());
}

QString App::serviceProblem() const
{
    // What is indexed meanwhile (GitHub #52): the chosen folders, or every disk, walked.
    const bool folders = m_foldersInEffect;
    switch (m_serviceState) {
    case ntfs::ServiceState::NotInstalled:
        // An account that cannot install it is not told to (GitHub #52).
        if (!userIsAdministrator())
            return folders ? tr("你在这台电脑上没有管理员权限，装不了索引服务。Win顺 会索引你选的文件夹，并定时核对。")
                           : tr("你在这台电脑上没有管理员权限，装不了索引服务。现在按文件夹遍历整个磁盘。");
        return folders ? tr("没有安装索引服务，现在只索引你选的文件夹。装上服务可以索引整个磁盘，更快。")
                       : tr("没有安装索引服务，现在按文件夹遍历整个磁盘。装上服务可以更快。");
    case ntfs::ServiceState::Disabled:
        return folders ? tr("索引服务被停用了，现在只索引你选的文件夹。") : tr("索引服务被停用了，现在按文件夹遍历整个磁盘。");
    case ntfs::ServiceState::Stopped:
        return folders ? tr("索引服务没有运行，现在只索引你选的文件夹。") : tr("索引服务没有运行，现在按文件夹遍历整个磁盘。");
    case ntfs::ServiceState::Starting:
        return tr("正在等索引服务启动…");
    case ntfs::ServiceState::Unknown:
    case ntfs::ServiceState::Running:
        break;
    }
    if (m_serviceWhy.contains(u"another version") || m_serviceWhy.contains(u"run the installer again"))
        return tr("索引服务的版本和 Win顺 不配，请重新运行安装程序");
    if (m_serviceWhy.contains(u"not running"))
        return folders ? tr("索引服务没有运行，现在只索引你选的文件夹。") : tr("索引服务没有运行，现在按文件夹遍历整个磁盘。");
    return folders ? tr("索引服务在运行，但连不上（%1），现在只索引你选的文件夹。").arg(m_serviceWhy)
                   : tr("索引服务在运行，但连不上（%1），现在按文件夹遍历。").arg(m_serviceWhy);
}

void App::refreshIndexAccess()
{
    if (!m_settingsEditor)
        return;
    SettingsEditor::IndexAccessState state;
    const ntfs::VolumeAccess::Kind kind = m_index->volumeAccessKind();
    state.elevated = integrity::processElevated();
    state.administrator = userIsAdministrator();
    state.portable = !Settings::portableFolder().isEmpty();
    state.serviceInstalled = ntfs::ServiceVolumeAccess::installed();
    state.serviceState = QString::fromLatin1(ntfs::ServiceVolumeAccess::stateName(m_serviceState));
    state.serviceInstallable = state.administrator;
    // An older portable copy may have registered the service from a
    // user-writable folder, where a non-administrator could replace it and run
    // as SYSTEM (a local privilege escalation). The settings offer to repair it
    // (reinstall, which now copies into a protected folder).
    state.serviceUnsafeLocation = state.serviceInstalled && ntfs::ServiceVolumeAccess::registeredInUnsafeLocation();
    state.serviceBusy = m_serviceBusy;
    state.serviceMessage = m_serviceMessage;
    switch (kind) {
    case ntfs::VolumeAccess::Kind::Direct:
        state.effective = u"direct"_s;
        state.status = tr("现在：以管理员身份直接读取磁盘的主文件表和变更日志");
        break;
    case ntfs::VolumeAccess::Kind::Service: {
        state.effective = u"service"_s;
        const auto service = std::dynamic_pointer_cast<ntfs::ServiceVolumeAccess>(m_index->volumeAccess());
        const QString version = service ? QString::fromStdWString(service->serviceVersion()) : QString();
        state.status = version.isEmpty() ? tr("现在：通过索引服务读取磁盘的主文件表和变更日志")
                                         : tr("现在：通过索引服务读取磁盘的主文件表和变更日志（服务版本 %1）").arg(version);
        break;
    }
    case ntfs::VolumeAccess::Kind::None:
        state.effective = u"walk"_s;
        state.status = m_serviceWanted ? tr("现在：%1").arg(serviceProblem())
            : m_foldersInEffect        ? tr("现在：按文件夹遍历，只索引你选的文件夹")
                                       : tr("现在：按文件夹遍历，不读取主文件表");
        break;
    }
    // 索引哪些地方 (GitHub #52): with the disks read whole, the chosen folders
    // cannot be; nor while the index service is on its way (Settings::foldersInEffect).
    const bool serviceOnItsWay
        = kind == ntfs::VolumeAccess::Kind::None && m_serviceWanted && !serviceLongAbsent(m_serviceState);
    state.foldersInEffect = m_foldersInEffect;
    state.foldersPossible = kind == ntfs::VolumeAccess::Kind::None && !serviceOnItsWay;
    state.scopeStatus = m_foldersInEffect                ? tr("现在：只索引选中的文件夹")
        : kind == ntfs::VolumeAccess::Kind::Service      ? tr("现在：索引整个磁盘。使用索引服务时总是索引整个磁盘")
        : kind == ntfs::VolumeAccess::Kind::Direct       ? tr("现在：索引整个磁盘。管理员直读时总是索引整个磁盘")
        : serviceOnItsWay ? tr("现在：索引整个磁盘。装了索引服务、没有停用它时总是索引整个磁盘，它暂时没在运行也一样")
                          : tr("现在：索引整个磁盘");
    QStringList notes;
    if (m_settings.indexAccess == u"direct" && !state.elevated) {
        notes << (state.administrator ? tr("“管理员直读”要以管理员身份运行；这次是以普通权限启动的，点“以管理员身份重新启动”。")
                                      : tr("这个账户不是管理员，“管理员直读”在这里没有意义，请改选别的方式。"));
    }
    if (m_accessTakenFromTask)
        notes << tr("升级前开机自启是以管理员身份运行的，所以记成了“管理员直读”；改选别的方式，开机自启就改用普通权限。");
    if (const std::optional<bool> task = autostart::taskElevated(); task && *task != autostart::elevated()) {
        // Written by the elevated copy only (reconcileAutostart).
        notes << (*task ? tr("开机自启的任务还在以管理员身份启动 Win顺：下次登录由它启动时会改成普通权限。")
                        : tr("开机自启的任务还在以普通权限启动 Win顺：以管理员身份重新启动后会改过来。"));
    }
    state.note = notes.join(u'\n');
    m_settingsEditor->setIndexAccessState(state);
}

void App::installIndexService(bool install)
{
    if (m_serviceBusy)
        return;
    // A Scoop copy is registered through its "current" (the service records where it was installed from).
    const QString exe = scoop::stablePath(QCoreApplication::applicationDirPath() + u"/WinShunService.exe"_s);
    if (!QFile::exists(exe)) {
        m_serviceMessage = tr("Win顺 旁边没有 WinShunService.exe");
        refreshIndexAccess();
        return;
    }
    m_serviceBusy = true;
    m_serviceMessage = install ? tr("正在安装索引服务…") : tr("正在卸载索引服务…");
    refreshIndexAccess();
    // Registered from where this copy is: moved, it has to be installed again.
    shell::runElevated(exe.toStdWString(), install ? L"--install" : L"--uninstall",
        [this, install](std::optional<unsigned long> code) {
            QMetaObject::invokeMethod(
                this,
                [this, install, code] {
                    m_serviceBusy = false;
                    if (!code) {
                        m_serviceMessage = tr("没有进行：需要管理员权限（在 UAC 里选“是”）");
                    } else if (*code != 0) {
                        m_serviceMessage = (install ? tr("安装索引服务失败（错误 %1）") : tr("卸载索引服务失败（错误 %1）"))
                                               .arg(*code);
                    } else if (install) {
                        m_serviceMessage = tr("索引服务已安装并启动。换了新版本的 Win顺 后，在这里再装一次，服务也会跟着更新。");
                        probeService(); // taken into use as soon as it answers
                    } else {
                        m_serviceMessage = tr("索引服务已卸载。");
                        m_serviceState = ntfs::ServiceVolumeAccess::state();
                        if (m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::Service)
                            m_index->setVolumeAccess(std::make_shared<ntfs::NoVolumeAccess>()); // walked and watched from here on
                        applyIndexScope(true); // the chosen folders, where they would be without it
                    }
                    refreshIndexAccess();
                },
                Qt::QueuedConnection);
        });
}

// ---- AI 助手: the MCP server --------------------------------------------------

namespace {

// Each call the AI clients make, in %LOCALAPPDATA%\WinShun\mcp.log (over
// 1 MB it becomes mcp.old.log and a new one starts).
void logMcpCall(const QString& line)
{
    const QString path = Settings::dataDir() + u"\\mcp.log"_s;
    if (QFile::exists(path) && QFile(path).size() > (1 << 20)) {
        const QString old = Settings::dataDir() + u"\\mcp.old.log"_s;
        QFile::remove(old);
        QFile::rename(path, old);
    }
    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text))
        file.write(line.toUtf8() + '\n');
}

QString clipKindName(ClipKind kind)
{
    switch (kind) {
    case ClipKind::Link:
        return u"link"_s;
    case ClipKind::Path:
        return u"path"_s;
    case ClipKind::Files:
        return u"files"_s;
    case ClipKind::Image:
        return u"image"_s;
    case ClipKind::Text:
        break;
    }
    return u"text"_s;
}

} // namespace

mcp::Policy App::mcpPolicy() const
{
    mcp::Policy policy;
    policy.allowOpen = m_settings.mcpAllowOpen;
    policy.allowClipboard = m_settings.mcpClipboard;
    policy.clipboardHistory = m_settings.clipboard; // without a history there is nothing to read
    policy.allowScreenshot = m_settings.mcpScreenshot;
    policy.settingsMode = m_settings.mcpSettings == u"auto" ? mcp::SettingsMode::Auto
        : m_settings.mcpSettings == u"off"                  ? mcp::SettingsMode::Off
                                                            : mcp::SettingsMode::Ask;
    policy.captureEnabled = screenshotPluginOn();
    policy.excludedPaths = m_settings.mcpExcludedPaths;
    policy.content = m_settings.contentSearch;
    policy.contentExtensions = m_settings.contentExtensions;
    policy.contentSizeLimits = m_settings.contentSizeLimits();
    policy.contentDocuments = m_settings.contentDocuments;
    policy.contentInLowPriority = m_settings.contentInLowPriority;
    policy.excludedTypes = m_settings.excludedTypes;
    policy.excludedFileNames = m_settings.excludedFileNames;
    policy.kindOrder = m_settings.foldersFirst ? KindOrder::FoldersFirst : KindOrder::FilesFirst;
    return policy;
}

// What the tool hosts need from the app: the AI's (applyMcp) and the
// plugins' (pluginTools) alike, each on an engine of its own.
mcp::Services App::toolServices() const
{
    mcp::Services services;
    services.index = m_index.get();
    services.version = QStringLiteral(WINSHUN_VERSION);
    services.history = [this] { return m_history->items(); };
    services.open = [](const QString& path, std::function<void(bool)> done) { shell::open(path, false, std::move(done)); };
    services.reveal = [](const QString& path, std::function<void(bool)> done) {
        shell::reveal(path);
        done(true);
    };
    services.clipboard = [this](const mcp::ClipQuery& query) {
        std::vector<mcp::ClipSummary> out;
        ClipFilter filter;
        filter.query = query.query;
        for (const int row : m_clipStore->find(filter)) {
            const Clip& c = m_clipStore->clips()[static_cast<std::size_t>(row)];
            const QString kind = clipKindName(c.kind);
            if (query.kind != u"all" && kind != query.kind)
                continue;
            if (std::ssize(out) >= query.limit)
                break;
            mcp::ClipSummary summary;
            summary.id = c.id;
            summary.kind = kind;
            summary.text = c.text.left(query.maxChars);
            summary.chars = c.textLength;
            summary.created = c.created;
            summary.used = c.used;
            summary.source = c.source;
            if (const ClipGroup* group = c.group != 0 ? m_clipStore->group(c.group) : nullptr)
                summary.group = group->name;
            summary.pinned = c.group == ClipStore::kPinned;
            summary.width = c.width;
            summary.height = c.height;
            out.push_back(std::move(summary));
        }
        return out;
    };
    services.clipboardText = [this](qint64 id) -> std::optional<QString> {
        if (!m_clipStore->clip(id))
            return std::nullopt;
        return m_clipStore->payload(id).text;
    };
    services.screenshots = [this](const QString& query, int limit) {
        std::vector<mcp::ScreenshotSummary> out;
        if (!m_clipStore || !m_clipStore->isOpen())
            return out;
        ClipFilter filter;
        filter.screenshots = true;
        filter.query = query;
        for (const int row : m_clipStore->find(filter)) {
            if (std::ssize(out) >= limit)
                break;
            const Clip& c = m_clipStore->clips()[static_cast<std::size_t>(row)];
            out.push_back({c.id, c.created, c.width, c.height, c.ocr,
                m_clipStore->imagePending(c.id) ? QString() : m_clipStore->imagePath(c.id)});
        }
        return out;
    };
    services.log = [](const QString& line) { logMcpCall(line); };
    return services;
}

void App::confirmScreenshot(const mcp::ScreenshotRequest& request, std::function<void(mcp::ScreenshotTaken)> done)
{
    if (m_screenshotAsk) {
        done({u"Another screenshot request is waiting for the user's answer; try again later."_s});
        return;
    }
    // 1 is the primary monitor (Qt lists it first), then the others.
    const QList<QScreen*> screens = QGuiApplication::screens();
    QScreen* screen = nullptr;
    if (request.monitor == 0)
        screen = QGuiApplication::screenAt(QCursor::pos());
    else if (request.monitor <= screens.size())
        screen = screens[request.monitor - 1];
    if (!screen && request.monitor == 0)
        screen = QGuiApplication::primaryScreen();
    const auto* native = screen ? screen->nativeInterface<QNativeInterface::QWindowsScreen>() : nullptr;
    if (!screen || !native) {
        done({u"There is no monitor %1: this PC has %2."_s.arg(request.monitor).arg(screens.size())});
        return;
    }
    const int number = static_cast<int>(screens.indexOf(screen)) + 1;
    // Its pixels as Windows has them: logical size × scale rounds (1707 × 1.5 = 2561).
    MONITORINFO info {.cbSize = sizeof(MONITORINFO)};
    ::GetMonitorInfoW(static_cast<HMONITOR>(native->handle()), &info);
    const QSize pixels(info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top);
    const bool primary = screen == QGuiApplication::primaryScreen();
    auto ask = std::make_unique<ScreenshotAsk>();
    ask->done = std::move(done);
    ask->monitor = native->handle();
    ask->label = u"monitor %1 (%2x%3%4)"_s.arg(number).arg(pixels.width()).arg(pixels.height()).arg(primary ? u", primary"_s : QString());
    const QString words = tr("显示器 %1（%2 × %3%4）").arg(number).arg(pixels.width()).arg(pixels.height())
        .arg(primary ? tr("，主显示器") : QString());

    ask->frame = std::make_unique<WindowFrame>(false);
    QQmlComponent component(m_qml.get(), u"WinShun"_s, u"ScreenshotConfirm"_s);
    QObject* object = component.createWithInitialProperties({
        {u"frame"_s, QVariant::fromValue(ask->frame.get())},
        {u"client"_s, request.client.trimmed().isEmpty() ? tr("一个 AI 客户端") : request.client.trimmed()},
        {u"monitor"_s, words},
    });
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        qWarning().noquote() << "take_screenshot: no confirmation window:" << component.errorString();
        delete object;
        ask->done({u"Win顺 could not ask the user, so no screenshot was taken."_s});
        return;
    }
    prepareBackdrop(window);
    window->create();
    styleWindow(window, SystemTheme::backdropAvailable());
    win::setDarkFrame(window, isDarkMode());
    ask->frame->setWindow(window);
    const int signal = object->metaObject()->indexOfSignal("answered(int)");
    const int slot = metaObject()->indexOfMethod("screenshotAnswered(int)");
    if (signal >= 0 && slot >= 0)
        QObject::connect(object, object->metaObject()->method(signal), this, metaObject()->method(slot));
    window->setScreen(screen);
    // In the middle of the work area, again when its wrapped text settles
    // its height (after the first layout, the window grew downwards).
    const auto centre = [window, screen = QPointer<QScreen>(screen)] {
        if (!screen)
            return;
        const QRect area = screen->availableGeometry();
        window->setPosition(area.center() - QPoint(window->width() / 2, window->height() / 2));
    };
    centre();
    connect(window, &QWindow::heightChanged, window, centre);
    ask->window = window;
    m_screenshotAsk = std::move(ask);
    window->show();
    window->raise();
    win::bringToFront(window);
    window->requestActivate();
    qInfo().noquote() << "take_screenshot: asking the user for" << m_screenshotAsk->label << "for" << request.client;
}

void App::screenshotAnswered(int how)
{
    std::unique_ptr<ScreenshotAsk> ask = std::move(m_screenshotAsk);
    if (!ask)
        return;
    // Gone after the signal that told it returned; its frame after it.
    if (ask->window) {
        // No fade-out: the grab below must not catch the dialog on its way out.
        win::disableTransitions(ask->window);
        ask->window->hide();
        ask->window->deleteLater();
    }
    ask->frame.release()->deleteLater();
    qInfo().noquote() << "take_screenshot:" << (how == 1 ? "allowed" : how == 0 ? "refused" : "no answer in time");
    if (how != 1) {
        ask->done({how == 0 ? u"The user refused the screenshot."_s
                            : u"The user did not answer within 30 seconds, so no screenshot was taken."_s});
        return;
    }
    const QString folder = QDir::tempPath() + u"/WinShun/mcp"_s;
    std::thread([monitor = ask->monitor, label = ask->label, done = std::move(ask->done), folder] {
        // The hidden dialog is off the screen once the desktop has composed twice.
        ::DwmFlush();
        ::DwmFlush();
        capture::Grabber grabber;
        QString report;
        const std::vector<capture::Grabber::Shot> shots = grabber.grab(&report);
        const auto shot = std::ranges::find_if(shots, [&](const capture::Grabber::Shot& s) { return s.monitor == monitor; });
        if (shot == shots.end() || shot->image.isNull()) {
            done({u"The monitor could not be captured (it may have been unplugged)."_s});
            return;
        }
        QDir().mkpath(folder);
        const QString path = folder + u"/screenshot-"_s
            + QDateTime::currentDateTime().toString(u"yyyyMMdd-HHmmss-zzz"_s) + u".png"_s;
        if (!shot->image.save(path, "PNG")) {
            done({u"The screenshot could not be written to the temporary folder."_s});
            return;
        }
        done({{}, path, shot->image.width(), shot->image.height(), label});
    }).detach();
}

void App::applyMcp()
{
    if (!m_settings.mcp) {
        closeSettingsConfirm(); // a card for a call that goes with the host
        m_aiBridge.reset();
        m_mcpServer.reset();
        m_mcpHost.reset();
        m_mcpEngine.reset();
        m_mcpContentEngine.reset();
        refreshMcpStatus();
        return;
    }
    if (!m_mcpServer) {
        // Its own engine: an engine runs only its newest request, so one
        // shared with the launcher would drop the launcher's searches. And
        // one more for content searches (seconds), which a name search
        // (milliseconds) would otherwise wait behind: an AI agent runs
        // several ws commands at once. Made now: an idle engine is a dozen or
        // so sleeping threads (its worker and pool), no work for the CPU.
        m_mcpEngine = std::make_unique<SearchEngine>(m_index.get(), m_apps.get(), m_places.get());
        m_mcpContentEngine = std::make_unique<SearchEngine>(m_index.get());
        mcp::Services services = toolServices();
        services.engine = m_mcpEngine.get();
        services.contentEngine = m_mcpContentEngine.get();
        // The AI's alone: plugins never take pictures of the screen.
        services.takeScreenshot = [this](const mcp::ScreenshotRequest& request, std::function<void(mcp::ScreenshotTaken)> done) {
            confirmScreenshot(request, std::move(done));
        };
        // Likewise the AI's alone: the settings tools.
        services.getSettings = [this](const mcp::SettingsQuery& query) { return describeAiSettings(query); };
        services.planSettings = [this](const QJsonArray& changes) { return planAiSettings(changes); };
        services.confirmSettings = [this](const mcp::SettingsAsk& ask, std::function<void(mcp::SettingsAnswer, bool)> done) {
            confirmSettingsChange(ask, std::move(done));
        };
        services.cancelSettings = [this] { closeSettingsConfirm(); };
        services.applySettings = [this](const mcp::SettingsApply& apply, std::function<void(mcp::ToolResult)> done) {
            applyAiSettings(apply, std::move(done));
        };
        services.openSettings = [this](const QString& key, const QString& section) { return openAiSettings(key, section); };
        m_mcpHost = std::make_unique<mcp::ToolHost>(std::move(services));
        m_mcpHost->setPolicy(mcpPolicy());
        m_mcpServer = std::make_unique<mcp::McpServer>(m_mcpHost.get(), QStringLiteral(WINSHUN_VERSION));
        connect(m_mcpServer.get(), &mcp::McpServer::sessionsChanged, this, &App::refreshMcpStatus);
        // ws calls: not one status line per call (an agent runs several a second),
        // and none at all while no settings window is there to show it.
        connect(m_mcpServer.get(), &mcp::McpServer::cliCalled, this, [this] {
            if (settingsShown() && !m_cliTallyTimer.isActive()) // hidden: read as it opens (refreshMcpStatus)
                m_cliTallyTimer.start();
        });
        connect(m_mcpHost.get(), &mcp::ToolHost::toolsChanged, this, &App::writeMcpToolsCache);
        DWORD error = 0;
        if (!m_mcpServer->listen(pipe::defaultName(), &error))
            qWarning("MCP: cannot serve on the pipe (error %lu)", error);
        writeMcpToolsCache();
    } else {
        m_mcpHost->setPolicy(mcpPolicy());
    }
    applyAiBridge();
    refreshMcpStatus();
}

void App::applyAiBridge()
{
    if (!m_mcpHost || !m_pluginHost)
        return;
    if (!m_aiBridge)
        m_aiBridge = std::make_unique<plugin::AiBridge>(m_pluginHost.get(), m_mcpHost.get());
    plugin::AiBridge::Options options;
    options.enabled = m_settings.mcpPlugins;
    options.actions = m_settings.mcpPluginActions;
    for (const PluginConfig& c : m_settings.plugins) {
        if (!c.ai)
            options.excluded.insert(c.id);
    }
    m_aiBridge->setOptions(options);
}

// The plugins' tools: not the AI's host (its switch is the AI's, and its
// engine would make the two cancel each other), one of their own, made
// when a plugin first asks (plugin::Tools, the plugin design §6.3).
plugin::Tools& App::pluginTools()
{
    if (!m_pluginTools) {
        m_pluginTools = std::make_unique<plugin::Tools>(toolServices(), m_apps.get(), m_places.get());
        m_pluginTools->setPolicy(mcpPolicy());
    }
    return *m_pluginTools;
}

// ---- 插件: the plugin host ------------------------------------------------------

void App::createPluginHost()
{
    const QString dataDir = Settings::dataDir();
    const QString appDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    plugin::Host::Options options;
    options.userPluginDir = dataDir + u"\\plugins"_s;
    if (QFileInfo::exists(appDir + u"\\plugins"_s))
        options.bundledPluginDir = appDir + u"\\plugins"_s;
    options.logPath = dataDir + u"\\plugins.log"_s;
    options.runnerExe = appDir + u"\\WinShunPluginHost.exe"_s;
    options.appDir = appDir;
    options.locale = m_language;
    options.theme = isDarkMode() ? u"dark"_s : u"light"_s;
    options.version = QStringLiteral(WINSHUN_VERSION);
    options.appearance = pluginLook();
    // What an earlier run kept aside for a plugin that was running, and what an
    // interrupted swap left: before the host can start anything.
    PluginUpdates::prepare(dataDir, options.userPluginDir, QStringLiteral(WINSHUN_VERSION), [this](const QString& id) {
        const PluginConfig* c = m_settings.plugin(id);
        return std::pair<bool, QStringList>(c && c->enabled, c ? c->confirmed : QStringList());
    });
    m_pluginHost = std::make_unique<plugin::Host>(options);
    m_pluginKeyRelay = std::make_unique<PluginHotkeys>([this](const QString& owner, const QString& key) {
        m_pluginHost->notify(owner, u"hotkey"_s, {{u"key"_s, key}}, true);
    });
    {
        PluginUpdates::Options updateOptions;
        updateOptions.dataDir = dataDir;
        updateOptions.userDir = options.userPluginDir;
        updateOptions.bundledDir = options.bundledPluginDir;
        updateOptions.source = PluginUpdates::defaultSource();
        m_pluginUpdates = std::make_unique<PluginUpdates>(m_pluginHost.get(), updateOptions);
        m_pluginUpdates->setBusy([this] { return m_pluginRecoveryBusy || (m_updater && !m_updater->installStage().isEmpty()); });
        m_pluginUpdates->setPolicy(m_settings.pluginDeveloperMode, m_settings.plugins);
        m_pluginUpdates->setAutomatic(m_settings.autoUpdate);
        // A newer version that asks for permissions the user has not agreed to,
        // of a plugin that is on: told once per version, not over the settings
        // window (it shows them there).
        connect(m_pluginUpdates.get(), &PluginUpdates::consentNeeded, this, [this](const QStringList& ids) {
            if (m_settingsWindow && m_settingsWindow->isVisible())
                return;
            QString title = tr("%n 个插件的新版本要你同意新权限", "", static_cast<int>(ids.size()));
            if (ids.size() == 1) {
                const plugin::Entry* e = m_pluginHost->catalog().find(ids.front());
                title = tr("“%1” 的新版本要你同意新权限").arg(e && e->manifest ? e->manifest->name.pick(m_language) : ids.front());
            }
            m_messages->showNotification(title, tr("点这里查看并更新"), [self = QPointer(this)] {
                if (self)
                    self->showPluginUpdates();
            });
        });
    }
    connect(&m_pluginHost->catalog(), &plugin::Catalog::changed, this, &App::pluginsChanged);
    connect(m_pluginHost.get(), &plugin::Host::stateChanged, this, [this](const QString& id) {
        if (!m_pluginHost->catalog().enabled(id))
            m_pluginKeyRelay->remove(id);
        // A deferred install completed later from the regular plugin page.
        if (!m_onboarding && id == u"winshun.capture" && m_settings.onboardingCapturePending
            && m_pluginHost->catalog().enabled(id)
            && m_pluginHost->state(id).status == plugin::Host::State::Status::Running) {
            m_settings.onboardingCapturePending = false;
            m_settings.save();
            if (m_settingsEditor)
                m_settingsEditor->setSettings(m_settings);
        }
    });
    setUpAppTools();
    // The shortcuts a plugin registers itself (the screenshot's): Win+Shift+S
    // and the global digits are left to it (applyTaskbarHotkeys, applyQuickPasteHotkeys).
    connect(m_pluginHost.get(), &plugin::Host::hotkeysChanged, this, [this](const QString& id, const QStringList& keys) {
        const auto had = m_pluginHotkeys.find(id);
        if (had == m_pluginHotkeys.end() ? keys.isEmpty() : had->second == keys)
            return;
        if (keys.isEmpty())
            m_pluginHotkeys.erase(id);
        else
            m_pluginHotkeys[id] = keys;
        updateScreenshotPlugin();
        applyTaskbarHotkeys();
        applyQuickPasteHotkeys();
    });
    connect(m_pluginHost.get(), &plugin::Host::processExited, this, [this](const QString& id) {
        m_pluginKeyRelay->remove(id);
        m_trayMenus.erase(id);
    });
    connect(m_pluginHost.get(), &plugin::Host::disabledAfterCrashes, this, [this](const QString& id) {
        const plugin::Entry* e = m_pluginHost->catalog().find(id);
        const QString name = e && e->manifest ? e->manifest->name.pick(m_language) : id;
        m_messages->showNotification(tr("插件已停用"), tr("“%1” 连续崩溃，已停用。详见 plugins.log。").arg(name));
    });
    connect(m_pluginHost.get(), &plugin::Host::reloaded, this, [this](const QString& id) {
        const plugin::Entry* e = m_pluginHost->catalog().find(id);
        m_launcher->flash(tr("已重新加载 %1").arg(e && e->manifest ? e->manifest->name.pick(m_language) : id));
    });
    // A plugin's system notification (winshun/notify with "toast"; the host
    // held it to the "notifications" permission and to its pace, put its
    // name first and kept of its click only its own list, a local file or
    // folder and a copy it may make: ToastPolicy). A click does those as an
    // action's effects (Launcher::applyEffects: the open policy holds); its
    // own list shows the launcher with it, the keyboard on what came in on
    // top (openedFromToast). Nothing once the plugin is off. Win顺's own
    // notifications win over it (MessageWindow: it waits while one is up).
    connect(m_pluginHost.get(), &plugin::Host::toastRequested, this,
        [this](const QString& id, const QString& title, const QString& text, const plugin::Effects& click) {
            std::function<void()> onClick;
            if (!click.isEmpty()) {
                onClick = [this, id, click] {
                    if (!m_pluginHost || !m_pluginHost->catalog().enabled(id))
                        return;
                    plugin::Effects effects;
                    QString query;
                    for (const plugin::Effect& e : click) {
                        if (e.kind == plugin::Effect::Kind::SetQuery)
                            query = e.text;
                        else if (e.kind != plugin::Effect::Kind::Hide && e.kind != plugin::Effect::Kind::Keep
                            && e.kind != plugin::Effect::Kind::Replace)
                            effects.push_back(e);
                    }
                    if (!query.isEmpty()) {
                        m_launcher->openedFromToast(id);
                        showLauncher(query);
                    }
                    m_launcher->applyPluginEffects(id, effects);
                };
            }
            m_messages->showNotification(title, text, std::move(onClick), id);
        });
    // Its winshun/tool calls: the plugins' own tools, made when first asked.
    plugin::ToolBridge bridge;
    bridge.call = [this](std::uint64_t callId, const QString& client, const QString& tool, const QJsonObject& arguments,
                      std::function<void(mcp::ToolResult)> done) { pluginTools().host().call(callId, client, tool, arguments, std::move(done)); };
    bridge.cancel = [this](std::uint64_t callId) {
        if (m_pluginTools)
            m_pluginTools->host().cancel(callId);
    };
    bridge.known = [this](const QString& tool) { return pluginTools().host().known(tool); };
    m_pluginHost->setToolBridge(std::move(bridge));
    pluginsChanged();
    m_pluginHost->catalog().setWatching(true); // a folder put into plugins\ is a plugin at once
    m_launcher->setPluginHost(m_pluginHost.get());
    // Host starts enabled eager plugins once their configuration has settled.
    // Restore moved features asynchronously too, without a ten-second gap.
    QTimer::singleShot(0, this, &App::installMovedPlugins);
    // On the way out: shutdown requests, two seconds, then the job ends them.
    // Not the two seconds when Windows ends the session (Qt says aboutToQuit
    // then too): they are the seconds the index needs to be saved.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (m_pluginHost)
            m_pluginHost->shutdown(::GetSystemMetrics(SM_SHUTTINGDOWN) == 0);
    });
}

// The screenshot of a Win顺 before 0.17, on: the plugin winshun.capture,
// turned on with its settings (MovedPlugins.h), fetched with the others.
void App::moveCapture()
{
    if (!m_settings.captureEnabled || m_settings.plugin(moved::kCapture))
        return;
    if (!moved::moveCaptureSettings(Settings::filePath(), m_pluginHost->options().dataRoot)) {
        qWarning() << "Screenshot: its settings not moved to the plugin; tried again at the next start";
        return;
    }
    PluginConfig config;
    config.id = moved::kCapture.toString();
    config.enabled = true;
    config.confirmed = moved::capturePermissions();
    m_settings.plugins.push_back(std::move(config));
    m_settings.captureEnabled = false; // gone from the file: not moved twice
    // Keep the host's recovery record until the plugin is available: an
    // offline upgrade must still give PrtScn back to Windows.
    m_settings.save();
    qInfo() << "Screenshot: its settings moved to the plugin" << moved::kCapture;
}

// 局域网传输, 按意思找 and the screenshot came with 0.16.0 and are in the
// plugin library now (GitHub #138, MovedPlugins.h): one the user had on is
// fetched from it, then started with their existing settings.
void App::setPluginRecoveryState(const QString& message, bool busy)
{
    m_pluginRecoveryMessage = message;
    m_pluginRecoveryBusy = busy;
    if (m_settingsEditor)
        m_settingsEditor->plugins()->setRecoveryState(message, busy);
}

void App::installMovedPlugins()
{
    if (!m_pluginHost || m_pluginRecoveryBusy)
        return;
    const QStringList ids = moved::missing(m_pluginHost->catalog(), m_settings.plugins);
    if (ids.isEmpty()) {
        setPluginRecoveryState({});
        return;
    }
    QStringList names;
    for (const QString& id : ids)
        names.push_back(moved::name(id));
    setPluginRecoveryState(tr("正在从插件库恢复 %1。原来的设置和数据会保留。").arg(names.join(u"、"_s)), true);
    qInfo().noquote() << "Plugins moved to the library, installing:" << ids.join(u", "_s);
    QStringList claimed; // not changed from the background meanwhile
    if (m_pluginUpdates) {
        for (const QString& id : ids) {
            if (m_pluginUpdates->claim(id))
                claimed.push_back(id);
        }
    }
    moved::install(ids, m_pluginHost->options().userPluginDir, m_language, [self = QPointer(this), claimed](const QList<moved::Outcome>& outcomes) {
        if (!self)
            return;
        if (self->m_pluginUpdates) {
            for (const QString& id : claimed)
                self->m_pluginUpdates->release(id);
        }
        if (!self->m_pluginHost)
            return;
        QStringList installed;
        QStringList failed;
        for (const moved::Outcome& o : outcomes) {
            if (o.error.isEmpty()) {
                installed.push_back(o.name);
                continue;
            }
            qWarning().noquote() << "Plugin moved to the library, not installed:" << o.id << o.error;
            failed.push_back(tr("%1：%2").arg(o.name, o.offline ? tr("网络连接失败") : o.error));
        }
        if (!installed.isEmpty()) {
            self->m_pluginHost->catalog().scan();
            self->applyPlugins(); // its settings
            self->m_pluginHost->preload();
            if (failed.isEmpty())
                self->m_messages->showNotification(tr("插件已从插件库装好"),
                    tr("%1 不再随 Win顺 自带，已经从插件库为你装上，设置照旧。").arg(installed.join(u"、"_s)));
        }
        self->setPluginRecoveryState(failed.isEmpty() ? QString()
            : tr("以下功能尚未恢复：%1。原来的设置和数据仍在，可以重试或从本地安装插件包。").arg(failed.join(u"；"_s)));
        if (!failed.isEmpty())
            self->m_messages->showNotification(tr("部分功能尚未恢复"), self->m_pluginRecoveryMessage, [self] {
                if (!self)
                    return;
                self->showSettings([](QQuickWindow* window) {
                    QMetaObject::invokeMethod(window, "showPage",
                        Q_ARG(QVariant, window->property("pluginsPage")), Q_ARG(QVariant, QVariant(0)));
                });
            });
    });
}

void App::applySwitches()
{
    if (!m_pluginHost || !m_launcher)
        return;
    const QString provider = SwitchPlugin::provider(m_pluginHost->catalog());
    if ((m_switchPlugin && m_switchPlugin->id() == provider) || (!m_switchPlugin && provider.isEmpty()))
        return;
    m_launcher->setSwitches(nullptr, statePath());
    m_switches.reset();
    m_switchPlugin.reset();
    if (!provider.isEmpty()) {
        m_switchPlugin = std::make_unique<SwitchPlugin>(m_pluginHost.get(), provider);
        m_switches = std::make_unique<Switches>(m_switchPlugin->remote());
        m_launcher->setSwitches(m_switches.get(), statePath());
        connect(m_switchPlugin.get(), &SwitchPlugin::changed, m_switches.get(), [this](const QString& id) {
            if (id.isEmpty())
                m_switches->refresh();
            else
                m_switches->refresh(id);
        });
        connect(m_switches.get(), &Switches::changed, this, [this](const QString& id) {
            if (id == u"keep-awake" && m_messages)
                m_messages->setTrayTooltip(trayTooltip());
        });
    }
    if (m_messages)
        m_messages->setTrayTooltip(trayTooltip());
}

void App::pluginsChanged()
{
    applySwitches();
    applyWebSearch();
    QHash<QString, QString> folders;
    for (const plugin::Entry& e : m_pluginHost->catalog().entries())
        folders.insert(e.id, e.dir);
    FileIconProvider::setPluginFolders(std::move(folders));
    pluginPicturesChanged(); // one installed or taken away
    const bool update = localSendNeedsUpdate(m_pluginHost->catalog());
    if (m_clipboard)
        m_clipboard->setLanSendAvailable(m_pluginHost->catalog().enabled(u"winshun.localsend"_s) && !update, update);
    applyScreenshots();
    if (!m_pluginRecoveryBusy && moved::missing(m_pluginHost->catalog(), m_settings.plugins).isEmpty())
        setPluginRecoveryState({});
}

void App::applyPlugins()
{
    if (!m_pluginHost)
        return;
    m_pluginHost->setDeveloperMode(m_settings.pluginDeveloperMode);
    if (m_pluginUpdates)
        m_pluginUpdates->setPolicy(m_settings.pluginDeveloperMode, m_settings.plugins);
    if (m_pluginTools)
        m_pluginTools->setPolicy(mcpPolicy());
    plugin::Catalog& catalog = m_pluginHost->catalog();
    QSet<QString> enabled;
    QHash<QString, QString> overrides;
    QHash<QString, QStringList> confirmed;
    for (const PluginConfig& c : m_settings.plugins) {
        if (c.enabled)
            enabled.insert(c.id);
        confirmed.insert(c.id, c.confirmed);
        for (const auto& [command, keyword] : c.keywords)
            overrides.insert(c.id + u'/' + command, keyword);
    }
    // What the user said yes to, before the switches: a plugin of theirs
    // whose newer version asks for more waits for their OK from the moment
    // it is on (Catalog::setConfirmed; the bundled ones are trusted).
    // Allowed ones that had been switched off after repeated crashes start
    // fresh, as when switched off and on (Host::setEnabled forgets them).
    for (const QString& id : catalog.setConfirmed(std::move(confirmed))) {
        if (enabled.contains(id))
            m_pluginHost->setEnabled(id, true);
    }
    // Through the host where the switch changes (it forgets a plugin's
    // crashes when it is turned on again), the catalog otherwise. The
    // switch, not enabled(): one waiting for the user's OK is still on.
    for (const plugin::Entry& e : catalog.entries()) {
        if (catalog.switchedOn(e.id) != enabled.contains(e.id))
            m_pluginHost->setEnabled(e.id, enabled.contains(e.id));
    }
    catalog.setEnabled(enabled); // the ones not installed yet count too
    m_pluginHost->setKeywordOverrides(overrides);
    applyWebSearch();
    m_pluginHost->setFavoriteKeywords(favoriteKeywords(*m_favorites)); // so do the favorites
    applyScreenshots(); // and the screenshot history's (Win顺's own)
    // Each plugin's settings: the INI's values by the manifest's types, the
    // passwords from the Credential Manager (never in the INI).
    for (const plugin::Entry& e : catalog.entries()) {
        if (!e.manifest || e.manifest->settings.isEmpty())
            continue;
        const PluginConfig* c = m_settings.plugin(e.id);
        m_pluginHost->setPluginSettings(e.id, plugin::secrets::settingsOf(e.id, *e.manifest, c ? c->settings : QList<std::pair<QString, QString>>()));
    }
    applyAiBridge(); // the cards' "给 AI 用" switches
    pluginPicturesChanged(); // 贴到屏幕 and 编辑 come and go with the screenshot plugin's switch
    applySwitches();
    const bool update = localSendNeedsUpdate(catalog);
    m_clipboard->setLanSendAvailable(catalog.enabled(u"winshun.localsend"_s) && !update, update);
    applyScreenshots(); // and 截图历史
    restorePrintScreen();
}

void App::refreshMcpStatus()
{
    if (!m_settingsEditor)
        return;
    QString status;
    if (m_mcpServer && m_mcpServer->listening()) {
        QStringList names;
        for (const mcp::McpServer::SessionInfo& s : m_mcpServer->sessions()) {
            if (s.client.isEmpty())
                continue;
            const QString name = s.calls > 0 ? tr("%1（%2 次调用）").arg(s.client).arg(s.calls) : s.client;
            if (!names.contains(name))
                names.push_back(name);
        }
        status = names.isEmpty() ? tr("还没有 AI 客户端连接；连上以后显示在这里") : tr("已连接：%1").arg(names.join(u"、"_s));
    } else if (m_settings.mcp) {
        status = tr("没能建立连接通道，详见 WinShun.log");
    }
    m_settingsEditor->setMcpStatus(status);
    refreshCliTally();
}

void App::refreshCliTally()
{
    if (!m_settingsEditor)
        return;
    // The command line ws: its runs last a few milliseconds each and are not
    // listed as sessions; counted, with the last one's time (a clock time: it
    // does not go stale on the screen as "3 分钟前" would) and tool. Shown in
    // the 命令行 section, a row of its own: as a second line of the switch's
    // row it pushed that section down with the first run, from under the mouse.
    QString text;
    if (m_mcpServer && m_mcpServer->listening()) {
        const mcp::McpServer::CliTally& tally = m_mcpServer->cliTally();
        if (tally.calls > 0) {
            const QLocale locale;
            const QString when = tally.lastCall.date() == QDate::currentDate() ? locale.toString(tally.lastCall.time(), QLocale::ShortFormat)
                                                                              : locale.toString(tally.lastCall, QLocale::ShortFormat);
            text = tr("%1 次调用，最近 %2（%3）").arg(tally.calls).arg(when, tally.lastTool);
        }
    }
    m_settingsEditor->setCliTally(text);
}

void App::writeMcpToolsCache()
{
    if (!m_mcpHost)
        return;
    QFile file(Settings::dataDir() + u"\\mcp-tools.json"_s);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(m_mcpHost->tools()).toJson(QJsonDocument::Compact));
}

// ---- AI 助手: the settings tools (AiSettings) -----------------------------------------

aisettings::State App::aiState() const
{
    return {m_settings, autostart::isEnabled()};
}

// What only the app can tell the catalog.
aisettings::Env App::aiEnv() const
{
    aisettings::Env env;
    env.hotkeyValid = [](const QString& hotkey) {
        UINT modifiers = 0;
        UINT vk = 0;
        return MessageWindow::parseHotkey(hotkey, &modifiers, &vk);
    };
    env.isAdministrator = [] { return userIsAdministrator(); };
    env.isElevated = [] { return integrity::processElevated(); };
    env.fileManagerAvailable = [](const QString& manager) {
        for (const auto& [kind, name] : filemanager::kSettingNames) {
            if (manager == QLatin1StringView(name))
                return filemanager::installed(kind);
        }
        return false;
    };
    env.keywordOwner = [this](const QString& keyword) -> QString {
        if (!m_favorites)
            return {};
        const qsizetype i = typedFavoriteKeyword(m_favorites->items(), keyword);
        return i < 0 ? QString() : m_favorites->items()[i].name;
    };
    env.screenshotHistory = [this] { return screenshotPluginOn(); };
    return env;
}

// Only what is known already (no probing): cheap enough for the GUI thread.
QJsonObject App::aiFacts() const
{
    QJsonObject facts {{u"windows"_s, QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11 ? u"11"_s : u"10"_s},
        {u"ui_language"_s, m_language}, {u"windows_clipboard_history_on"_s, Settings::windowsClipboardHistory()},
        {u"running_as_administrator"_s, integrity::processElevated()}, {u"account_is_administrator"_s, userIsAdministrator()},
        {u"low_memory"_s, Settings::lowMemory()}, {u"monitors"_s, static_cast<int>(QGuiApplication::screens().size())},
        {u"taskbar_search_box_supported"_s, taskbar::SearchBox::supported()}};
    MEMORYSTATUSEX memory {sizeof memory};
    if (::GlobalMemoryStatusEx(&memory))
        facts.insert(u"memory_gb"_s, static_cast<int>(std::llround(static_cast<double>(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0))));
    if (const QScreen* screen = QGuiApplication::primaryScreen())
        facts.insert(u"primary_monitor_scale_percent"_s, static_cast<int>(std::lround(screen->devicePixelRatio() * 100)));
    QJsonArray managers;
    for (const auto& [kind, name] : filemanager::kSettingNames) {
        if (kind == filemanager::Kind::Custom ? !m_settings.fileManagerOpenCommand.isEmpty() : filemanager::installed(kind))
            managers.push_back(QString::fromLatin1(name));
    }
    facts.insert(u"file_managers"_s, managers);
    if (const auto previewer = extpreview::running())
        facts.insert(u"previewer_running"_s, *previewer == extpreview::Engine::QuickLook ? u"QuickLook"_s : u"Seer"_s);
    return facts;
}

mcp::ToolResult App::describeAiSettings(const mcp::SettingsQuery& query) const
{
    aisettings::Query q;
    q.sections = query.sections;
    q.keys = query.keys;
    q.words = query.words;
    q.changedOnly = query.changedOnly;
    QJsonObject out = aisettings::describe(aiState(), q);
    if (query.facts)
        out.insert(u"facts"_s, aiFacts());
    return {out};
}

mcp::SettingsPlan App::planAiSettings(const QJsonArray& changes) const
{
    const aisettings::Plan plan = aisettings::makePlan(aiState(), changes, aiEnv());
    mcp::SettingsPlan out;
    out.problems = plan.problems;
    for (const aisettings::Item& item : plan.items)
        out.items.push_back({item.entry->key, item.changed, item.askAlways, item.newValue, aisettings::itemJson(item)});
    return out;
}

void App::noteApplyProblem(const QString& key, const QString& problem)
{
    if (problem.isEmpty())
        m_applyProblems.remove(key);
    else
        m_applyProblems.insert(key, problem);
}

App::AiApplyReport App::applyAiState(const aisettings::State& wanted,
    const std::vector<std::pair<const aisettings::Entry*, QJsonValue>>& rollback, bool alwaysAllow)
{
    AiApplyReport report;
    m_applyProblems.clear();
    m_aiApplying = true; // what goes wrong is told in the answer, not in a balloon
    const auto finished = qScopeGuard([this] { m_aiApplying = false; });

    // The recipe of every change App makes to the settings itself: save, take
    // it as the editor would have (settingsEdited), show it in the window.
    const bool autostartBefore = autostart::isEnabled();
    Settings next = wanted.settings;
    if (alwaysAllow)
        next.mcpSettings = u"auto"_s; // the user ticked "from now on" on the card
    next.save();
    settingsEdited(next);
    if (m_settingsEditor)
        m_settingsEditor->setSettings(m_settings);
    if (wanted.autostart != autostartBefore) {
        autostart::setEnabled(wanted.autostart); // no UAC for an assistant's change: told below if it did not take
        showAutostart();
    }

    // What did not take is put back: a shortcut another program holds, a
    // Windows setting that could not be written.
    aisettings::State fixed {m_settings, wanted.autostart};
    bool putBack = false;
    for (const auto& [entry, before] : rollback) {
        const QString& key = entry->key;
        AiFailure failure;
        if (entry->type == aisettings::Type::Hotkey && !m_applyProblems.value(key).isEmpty()) {
            failure.why = u"Another program or Windows already uses that shortcut, so the setting stays %1."_s.arg(
                before.toString().isEmpty() ? u"empty"_s : u'"' + before.toString() + u'"');
            failure.shown = tr("被其他程序占用，未修改");
        } else if (key == u"Clipboard/WinV" && m_winVRegistryFailed) { // either way: taking it, or giving it back
            failure.why = m_settings.clipboardWinV
                ? u"Windows would not let Win顺 have Win+V (its registry setting could not be written)."_s
                : u"WinShun could not give Win+V back to Windows (its registry setting could not be written), so the setting stays on."_s;
            failure.shown = m_settings.clipboardWinV ? tr("Windows 不让 Win顺 用 Win+V，未修改") : tr("没能把 Win+V 还给 Windows，未修改");
        } else if (key == u"Taskbar/WinS" && m_winSRegistryFailed) {
            failure.why = m_settings.taskbarWinS
                ? u"Windows would not let Win顺 have Win+S (its registry setting could not be written)."_s
                : u"WinShun could not give Win+S back to Windows (its registry setting could not be written), so the setting stays on."_s;
            failure.shown = m_settings.taskbarWinS ? tr("Windows 不让 Win顺 用 Win+S，未修改") : tr("没能把 Win+S 还给 Windows，未修改");
        } else if (key == u"Launcher/Autostart" && autostart::isEnabled() != wanted.autostart) {
            failure.why = u"Windows did not take the change (the task in the Task Scheduler could not be %1), so autostart stays as it was."_s.arg(
                wanted.autostart ? u"made"_s : u"removed"_s);
            failure.shown = tr("Windows 没有接受，未修改");
            fixed.autostart = autostart::isEnabled(); // what it really is
        }
        if (!failure.why.isEmpty()) {
            report.failed.insert(key, failure);
            if (key != u"Launcher/Autostart") { // that one is no line of the settings file: `fixed` has it already
                aisettings::restore(*entry, fixed, before);
                putBack = true;
            }
            continue;
        }
        if (entry->effect == aisettings::Effect::ExplorerRestart) {
            const QString& state = key == u"Clipboard/WinV" ? m_winVState : m_winSState;
            if (state == u"waiting" || state == u"releasing") {
                report.explorerRestart = true;
                report.notes.insert(key, u"Takes effect once File Explorer restarts: Win顺 offers a button for it, or the user can sign out and in."_s);
            } else if (state == u"blocked") {
                report.notes.insert(key, u"The switch is off, but a registry setting Win顺 did not make (DisabledHotkeys) keeps Windows from using that shortcut. Win顺 leaves it alone; the settings page has a button that gives it back to Windows."_s);
            }
        } else if (entry->effect == aisettings::Effect::AppRestart) {
            report.notes.insert(key, u"Takes effect after Win顺 restarts."_s);
        } else if (entry->type == aisettings::Type::Hotkey && m_settingsEditor && m_settingsEditor->recordingHotkey()) {
            report.notes.insert(key, u"The settings window is recording a shortcut just now: this one is registered when that ends."_s);
        }
        if (key == u"Clipboard/QuickPaste" && !m_applyProblems.value(key).isEmpty())
            report.notes.insert(key, m_applyProblems.value(key)); // partial registration: name the unavailable digits
    }
    if (putBack) {
        fixed.settings.save();
        settingsEdited(fixed.settings);
        if (m_settingsEditor)
            m_settingsEditor->setSettings(m_settings);
        // The old shortcut is registered again just now: it may not come back
        // either (the other program took it in between).
        for (auto it = report.failed.begin(); it != report.failed.end(); ++it) {
            if (!m_applyProblems.value(it.key()).isEmpty())
                it->why += u" The earlier value could not be registered again either, so nothing holds that shortcut until Win顺 restarts or it is set again."_s;
        }
    }
    return report;
}

// Plans again against the settings as they are now: the card may have waited
// a minute, and the user may have used the settings window meanwhile. Only
// what the user was shown (apply.approved) is made: a change that comes to
// something else now, or to more, is refused whole.
void App::applyAiSettings(const mcp::SettingsApply& apply, std::function<void(mcp::ToolResult)> done)
{
    const QString mode = m_settings.mcpSettings;
    if (mode == u"off") { // switched off while the card waited
        done({{}, true, u"The user no longer allows AI to change settings. Nothing was changed."_s});
        return;
    }
    const aisettings::Plan plan = aisettings::makePlan(aiState(), apply.changes, aiEnv());
    if (!plan.problems.isEmpty()) {
        done({{}, true, u"Nothing was changed: the settings changed in the meantime and these no longer fit:\n- "_s + plan.problems.join(u"\n- "_s)});
        return;
    }
    std::vector<std::pair<const aisettings::Entry*, QJsonValue>> rollback;
    std::vector<const aisettings::Item*> made;
    for (const aisettings::Item& item : plan.items) {
        if (!item.changed)
            continue;
        rollback.emplace_back(item.entry, item.oldValue);
        made.push_back(&item);
    }
    const bool asApproved = made.size() == apply.approved.size()
        && std::ranges::all_of(made, [&](const aisettings::Item* item) {
               return std::ranges::any_of(apply.approved, [&](const mcp::ApprovedChange& a) {
                   return a.key.compare(item->entry->key, Qt::CaseInsensitive) == 0 && a.value == item->newValue;
               });
           });
    if (!asApproved) {
        done({{}, true, u"The settings changed while this was waiting, so it would no longer do what the user was shown. Nothing was changed; ask again."_s});
        return;
    }
    // Nothing was asked: the mode must still be auto, and nothing may ask always now.
    if (!apply.asked && (mode != u"auto" || plan.askAlways())) {
        done({{}, true, u"This change needs the user's approval now (the settings changed meanwhile). Nothing was changed; ask again."_s});
        return;
    }
    // "From now on" counts for a batch with nothing that asks always (the host checked as well).
    const bool always = apply.alwaysAllow && !plan.askAlways() && mode == u"ask";
    const AiApplyReport report = applyAiState(plan.result, rollback, always);

    QJsonArray results;
    QJsonArray batchItems;
    QJsonArray batchFailed;
    QStringList failures;
    QStringList failureNotes;
    for (const aisettings::Item* item : made) {
        const QString& key = item->entry->key;
        const bool failed = report.failed.contains(key);
        const QString note = failed ? report.failed.value(key).why : report.notes.value(key);
        QJsonObject result {{u"key"_s, key}, {u"title"_s, item->entry->titleText()}, {u"old"_s, item->oldValue},
            {u"new"_s, failed ? item->oldValue : item->newValue}, {u"effect"_s, QString::fromLatin1(aisettings::effectName(item->entry->effect))},
            {u"applied"_s, !failed}};
        if (!note.isEmpty())
            result.insert(u"note"_s, note);
        results.push_back(result);
        if (failed) {
            failures.push_back(u"%1: %2"_s.arg(key, note));
            const QString shown = report.failed.value(key).shown;
            failureNotes.push_back(u"%1：%2"_s.arg(item->entry->titleText(), shown));
            batchFailed.push_back(QJsonObject {{u"key"_s, key}, {u"title"_s, item->entry->titleText()}, {u"reason"_s, shown}});
            continue;
        }
        QJsonObject shown = aisettings::itemJson(*item);
        if (!note.isEmpty())
            shown.insert(u"note"_s, note);
        batchItems.push_back(shown);
    }
    qInfo().noquote() << "change_settings:" << apply.client << "changed" << batchItems.size() << "setting(s)"
                      << (failures.isEmpty() ? QString() : u"; not applied: "_s + failures.join(u"; "_s));
    if (batchItems.isEmpty()) {
        m_messages->showNotification(tr("AI 请求的修改没有生效"), failureNotes.join(u'\n'));
        done({{}, true, u"None of the changes took effect:\n- "_s + failures.join(u"\n- "_s)});
        return;
    }
    SettingsBatch batch;
    batch.id = ++m_settingsBatchIds;
    batch.client = apply.client;
    batch.reason = apply.reason;
    batch.items = batchItems;
    batch.failed = batchFailed;
    batch.explorerRestart = report.explorerRestart;
    m_settingsBatch = std::move(batch);
    showSettingsChangedNotice(*m_settingsBatch);

    QJsonObject out {{u"changed"_s, static_cast<int>(batchItems.size())}, {u"not_applied"_s, static_cast<int>(batchFailed.size())},
        {u"settings"_s, results}, {u"explorer_restart_needed"_s, report.explorerRestart},
        {u"undo_hint"_s, u"To undo, call change_settings again with each setting's old value. The user was told and can also press 撤销 on the notice for a few seconds."_s}};
    if (!failures.isEmpty())
        out.insert(u"warning"_s, u"Some changes did not take and stay as they were (see each setting's applied and note)."_s);
    if (always)
        out.insert(u"note"_s, u"From now on Win顺 changes settings for AI without asking each time; the user is told after each change and can undo it."_s);
    done({out});
}

QVariantMap App::SettingsBatch::toVariant() const
{
    return {{u"id"_s, id}, {u"client"_s, client}, {u"reason"_s, reason}, {u"count"_s, static_cast<int>(items.size())},
        {u"explorerRestart"_s, explorerRestart}, {u"items"_s, items.toVariantList()}, {u"failed"_s, failed.toVariantList()}};
}

// Puts back what the batch changed, where it still is what AI set (what the
// user changed since stays), through the same path as the change.
QVariantMap App::undoSettingsBatch(quint64 batchId)
{
    if (!m_settingsBatch || m_settingsBatch->id != batchId || m_settingsBatch->undone)
        return {{u"ok"_s, false}, {u"text"_s, tr("这次修改已经不能撤销了")}};
    aisettings::State state = aiState();
    std::vector<std::pair<const aisettings::Entry*, QJsonValue>> rollback;
    int kept = 0;
    for (const QJsonValue& value : m_settingsBatch->items) {
        const QJsonObject item = value.toObject();
        const aisettings::Entry* entry = aisettings::find(item.value(u"key"_s).toString());
        if (!entry)
            continue;
        const QJsonValue current = aisettings::valueOf(*entry, state);
        if (current != item.value(u"new"_s)) {
            ++kept; // changed again since: not ours to undo
            continue;
        }
        aisettings::restore(*entry, state, item.value(u"old"_s));
        rollback.emplace_back(entry, current);
    }
    if (rollback.empty())
        return {{u"ok"_s, false}, {u"text"_s, tr("这些设置之后又被改过，没有撤销")}};
    const AiApplyReport report = applyAiState(state, rollback, false);
    m_settingsBatch->undone = true;
    qInfo().noquote() << "change_settings: undone," << rollback.size() - report.failed.size() << "setting(s) back," << kept << "left as they are";
    QString text = tr("已撤销");
    if (kept > 0)
        text += tr("；有 %1 项之后又被改过，保持现状").arg(kept);
    if (!report.failed.isEmpty())
        text += tr("；有 %1 项没能改回去").arg(report.failed.size());
    return {{u"ok"_s, true}, {u"text"_s, text}, {u"explorerRestart"_s, report.explorerRestart}};
}

// open_settings, and the notice's 打开设置: the window at the setting's page and
// tab, its row found by its title (SettingsWindow.qml's showSetting).
mcp::ToolResult App::openAiSettings(const QString& key, const QString& section)
{
    int page = 0;
    int tab = -1;
    QString rowTitle;
    QString where;
    if (!key.isEmpty()) {
        const aisettings::Entry* entry = aisettings::find(key);
        if (!entry || !aisettings::visible(*entry))
            return {{}, true, u"Unknown setting '%1'; keys come from get_settings."_s.arg(key)};
        const aisettings::Section* of = aisettings::sectionOf(entry->section);
        where = of ? u"%1 › %2"_s.arg(of->titleText(), entry->titleText()) : entry->titleText();
        if (entry->section == u"web") { // the web plugin's card on the 插件 page, not a row the search finds
            showWebSettings();
            if (!m_settingsWindow)
                return {{}, true, u"The settings window could not be opened."_s};
            return {{{u"opened"_s, true}, {u"where"_s, where}}};
        }
        page = of ? of->page : 0;
        tab = entry->tab;
        rowTitle = entry->rowTitleText();
    } else {
        const aisettings::Section* of = aisettings::sectionOf(section);
        if (!of) {
            QStringList ids;
            for (const aisettings::Section& s : aisettings::sections())
                ids.push_back(s.id);
            return {{}, true, u"Unknown section '%1'; one of: %2."_s.arg(section, ids.join(u", "_s))};
        }
        page = of->page;
        where = of->titleText();
    }
    showSettings([&](QQuickWindow* window) {
        QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, page), Q_ARG(QVariant, tab), Q_ARG(QVariant, rowTitle));
    });
    if (!m_settingsWindow)
        return {{}, true, u"The settings window could not be opened."_s};
    return {{{u"opened"_s, true}, {u"where"_s, where}}};
}

void App::showSettingsFor(const QString& key)
{
    openAiSettings(key, {});
}

// ---- the screens: the confirmation card and the notice ---------------------------------

// The screen with the mouse, or the main one.
static QScreen* screenWithMouse()
{
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    return screen ? screen : QGuiApplication::primaryScreen();
}

// SettingsConfirm.qml, made as ScreenshotConfirm is (frame, backdrop, style),
// in the middle of the work area of the screen with the mouse. One at a time:
// a second request is told so at once.
void App::confirmSettingsChange(const mcp::SettingsAsk& ask, std::function<void(mcp::SettingsAnswer answer, bool always)> done)
{
    if (m_settingsAsk) {
        done(mcp::SettingsAnswer::Busy, false);
        return;
    }
    auto slot = std::make_unique<SettingsAskSlot>();
    slot->done = std::move(done);
    slot->frame = std::make_unique<WindowFrame>(false);
    QQmlComponent component(m_qml.get(), u"WinShun"_s, u"SettingsConfirm"_s);
    QObject* object = component.createWithInitialProperties({
        {u"frame"_s, QVariant::fromValue(slot->frame.get())},
        {u"client"_s, ask.client.trimmed().isEmpty() ? tr("一个 AI 客户端") : ask.client.trimmed()},
        {u"reason"_s, ask.reason.trimmed()},
        {u"items"_s, ask.items.toVariantList()},
        {u"askAlways"_s, ask.askAlways},
    });
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        qWarning().noquote() << "change_settings: no confirmation window:" << component.errorString();
        delete object;
        slot->done(mcp::SettingsAnswer::Unavailable, false);
        return;
    }
    prepareBackdrop(window);
    window->create();
    styleWindow(window, SystemTheme::backdropAvailable());
    win::setDarkFrame(window, isDarkMode());
    slot->frame->setWindow(window);
    const int signal = object->metaObject()->indexOfSignal("answered(int,bool)");
    const int answered = metaObject()->indexOfMethod("settingsAnswered(int,bool)");
    if (signal >= 0 && answered >= 0)
        QObject::connect(object, object->metaObject()->method(signal), this, metaObject()->method(answered));
    QScreen* screen = screenWithMouse();
    window->setScreen(screen);
    // In the middle of the work area; again when the wrapped text settles its height.
    const auto centre = [window, screen = QPointer<QScreen>(screen)] {
        if (!screen)
            return;
        const QRect area = screen->availableGeometry();
        window->setPosition(area.center() - QPoint(window->width() / 2, window->height() / 2));
    };
    centre();
    connect(window, &QWindow::heightChanged, window, centre);
    slot->window = window;
    m_settingsAsk = std::move(slot);
    window->show();
    window->raise();
    win::bringToFront(window);
    window->requestActivate();
    qInfo().noquote() << "change_settings: asking the user for" << ask.items.size() << "change(s) for" << ask.client;
}

void App::closeSettingsConfirm()
{
    std::unique_ptr<SettingsAskSlot> ask = std::move(m_settingsAsk);
    if (!ask)
        return;
    if (ask->window) {
        ask->window->hide();
        ask->window->deleteLater();
    }
    if (ask->frame)
        ask->frame.release()->deleteLater();
}

void App::settingsAnswered(int answer, bool always)
{
    std::unique_ptr<SettingsAskSlot> ask = std::move(m_settingsAsk);
    if (!ask)
        return;
    // Gone after the signal that told it returned; its frame after it.
    if (ask->window) {
        ask->window->hide();
        ask->window->deleteLater();
    }
    if (ask->frame)
        ask->frame.release()->deleteLater();
    // The card's own numbers (SettingsConfirm.qml): 1 allowed, 0 refused, anything else no answer in time.
    const mcp::SettingsAnswer how = answer == 1 ? mcp::SettingsAnswer::Allowed
        : answer == 0                           ? mcp::SettingsAnswer::Refused
                                                : mcp::SettingsAnswer::Timeout;
    qInfo().noquote() << "change_settings:"
                      << (how == mcp::SettingsAnswer::Allowed ? "allowed" : how == mcp::SettingsAnswer::Refused ? "refused" : "no answer in time");
    if (ask->done)
        ask->done(how, always);
}

// SettingsNotice.qml: the batch said in the corner of the work area (of the
// screen with the mouse), with 撤销. Made the first time and kept; a newer
// batch replaces what it shows. It never takes the focus from the AI client
// that is in front: the window does not accept it (Qt::WindowDoesNotAcceptFocus),
// no click activates it (WS_EX_NOACTIVATE, WM_MOUSEACTIVATE in WindowFrame),
// and it is shown without activating (Qt's _q_showWithoutActivating, then a
// SWP_NOACTIVATE move to the top, as the clipboard window is).
void App::showSettingsChangedNotice(const SettingsBatch& batch)
{
    if (!m_noticeWindow) {
        m_noticeFrame = std::make_unique<WindowFrame>(false);
        QQmlComponent component(m_qml.get(), u"WinShun"_s, u"SettingsNotice"_s);
        QObject* object = component.createWithInitialProperties({
            {u"frame"_s, QVariant::fromValue(m_noticeFrame.get())},
            {u"host"_s, QVariant::fromValue<QObject*>(this)},
        });
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << "change_settings: no notice window:" << component.errorString();
            delete object;
            m_noticeFrame.reset();
            m_messages->showNotification(tr("设置已被 AI 修改"), tr("可以在设置里改回来。"));
            return;
        }
        prepareBackdrop(window);
        window->create();
        styleWindow(window, SystemTheme::backdropAvailable());
        win::setDarkFrame(window, isDarkMode());
        m_noticeFrame->setWindow(window);
        m_noticeFrame->setNoActivate(true);
        m_noticeWindow = window;
        // Its corner, kept as it grows (the list, the answer after 撤销).
        const auto place = [window] {
            if (!window->screen())
                return;
            constexpr int margin = 16; // logical pixels from the work area's edges
            const QRect area = window->screen()->availableGeometry();
            window->setPosition(area.x() + area.width() - window->width() - margin, area.y() + area.height() - window->height() - margin);
        };
        connect(window, &QWindow::heightChanged, window, place);
        connect(window, &QWindow::widthChanged, window, place);
    }
    QQuickWindow* window = m_noticeWindow;
    QScreen* screen = screenWithMouse();
    if (window->isVisible() && window->screen() != screen)
        window->hide();
    window->setScreen(screen);
    QMetaObject::invokeMethod(window, "present", Q_ARG(QVariant, batch.toVariant()));
    window->setProperty("_q_showWithoutActivating", true);
    window->show();
    window->setProperty("_q_showWithoutActivating", QVariant());
    ::SetWindowPos(reinterpret_cast<HWND>(window->winId()), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

} // namespace ws
