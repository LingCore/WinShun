#include "App.h"

#include "AppCatalog.h"
#include "ClipStore.h"
#include "Clipboard.h"
#include "DialogBar.h"
#include "FileIconProvider.h"
#include "History.h"
#include "IndexFolder.h"
#include "IndexService.h"
#include "Launcher.h"
#include "Placement.h"
#include "SearchEngine.h"
#include "SettingsEditor.h"
#include "SystemCatalog.h"
#include "SystemTheme.h"
#include "TextField.h"
#include "Updater.h"
#include "WindowFrame.h"
#include "WindowLogo.h"
#include "platform/ClipboardWatcher.h"
#include "platform/DialogJump.h"
#include "platform/Foreground.h"
#include "platform/KeyListener.h"
#include "platform/MessageWindow.h"
#include "platform/Paster.h"
#include "platform/Shell.h"
#include "platform/TextCaret.h"
#include "platform/VolumeNotifier.h"
#include "platform/WinV.h"
#include "platform/WindowEffects.h"

#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QStorageInfo>
#include <QStyleHints>
#include <QSurfaceFormat>

#include <windows.h>

#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr int kHotkeyId = 1;
constexpr int kDialogJumpHotkeyId = 2; // Ctrl+G, while a file dialog is in front
constexpr int kWinVHotkeyId = 3; // Win+V: the clipboard history, once Explorer gave it up (winv::)
constexpr int kClipboardHotkeyId = 4; // another shortcut for the clipboard history
// For another program to tell where its text caret is: longer, and the
// clipboard would open late; it opens by the mouse pointer then.
constexpr auto kCaretWait = 100ms;
constexpr DWORD kSlowMenuMs = 100; // a tray menu slower than this to appear is logged

enum TrayCommand {
    ShowCommand = 1,
    IndexInfo,
    RebuildCommand,
    AutostartCommand,
    SettingsCommand,
    UpdateCommand,
    CheckUpdateCommand,
    RestartCommand,
    QuitCommand,
    ClipboardCommand,
    PauseClipboardCommand
};

IndexService::Options indexOptions(const Settings& settings)
{
    IndexService::Options options;
    options.rules = settings.crawlRules();
    options.includeRemovable = settings.includeRemovableDrives;
    options.rescanOnStartup = settings.rescanOnStartup;
    options.content.enabled = settings.contentIndex;
    options.content.extensions = settings.contentExtensions;
    options.content.sizeLimits = settings.contentSizeLimits();
    options.content.includeLowPriority = settings.contentInLowPriority;
    options.content.documents = settings.contentDocuments;
    return options;
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

// Rounded corners and the shadow, for our windows without a system title bar
// (the launcher, the clipboard, the bar by file dialogs, the settings
// window); each draws its edge itself (WindowEdge.qml).
void styleWindow(QWindow* window, bool backdrop = false)
{
    win::styleFramelessWindow(window, backdrop);
}

} // namespace

App::App()
{
    m_settingsReload.setSingleShot(true);
    m_settingsReload.setInterval(400ms); // editors often write a file in several steps
    connect(&m_settingsReload, &QTimer::timeout, this, &App::reloadSettings);
    m_indexOptionsApply.setSingleShot(true);
    m_indexOptionsApply.setInterval(1s);
    connect(&m_indexOptionsApply, &QTimer::timeout, this, [this] { m_index->setOptions(indexOptions(m_settings)); });
    m_revealTimeout.setSingleShot(true);
    m_revealTimeout.setInterval(150ms); // never wait longer than that for the first frame
    connect(&m_revealTimeout, &QTimer::timeout, this, &App::revealLauncher);
    m_clipUncloakTimeout.setSingleShot(true);
    m_clipUncloakTimeout.setInterval(150ms); // should no frame come
    connect(&m_clipUncloakTimeout, &QTimer::timeout, this, [this] {
        m_clipUncloak = false;
        uncloakClipboard();
    });
    m_darkFrameGuard.setInterval(10ms);
    connect(&m_darkFrameGuard, &QTimer::timeout, this, [this] {
        if (m_darkFrameWatch.elapsed() < 50) // Qt's own, after some 5 ms, first
            return;
        for (QWindow* window : {static_cast<QWindow*>(m_window), static_cast<QWindow*>(m_settingsWindow),
                 static_cast<QWindow*>(m_barWindow), static_cast<QWindow*>(m_clipWindow)}) {
            if (window && win::isDarkFrame(window) != m_darkFrame)
                win::setDarkFrame(window, m_darkFrame);
        }
        if (m_darkFrameWatch.elapsed() > 1000)
            m_darkFrameGuard.stop();
    });
    m_contentStatusTimer.setInterval(2s);
    connect(&m_contentStatusTimer, &QTimer::timeout, this, &App::refreshContentIndexStatus);
}

App::~App()
{
    m_keyRouter.reset(); // no keys taken from here on
    m_drainingRouter.reset();
    delete m_settingsWindow; // before the QML engine it was created with
    delete m_barWindow;
    delete m_clipWindow;
}

bool App::start(const StartOptions& options)
{
    const bool firstRun = !Settings::exists();
    // Updated from a version without the clipboard history: tell about it once.
    const bool clipboardIsNew = !firstRun && !Settings::hasClipboardSettings();
    m_settings.load();
    applyAppearance(); // before any window: the first frame is already in the chosen theme and language
    m_renderer = Settings::resolveRenderer(m_settings.renderer);
    if (m_renderer == u"software")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    else if (m_renderer == u"d3d11")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    // The software renderer draws a window with an alpha channel as a layered
    // window, which DWM puts no backdrop behind.
    SystemTheme::setBackdropAvailable(m_renderer == u"d3d11" && win::backdropSupported());

    const QString dataDir = Settings::dataDir();
    QDir().mkpath(dataDir);
    m_history = std::make_unique<History>(dataDir + u"\\history.txt"_s);
    m_history->load();
    m_index = std::make_unique<IndexService>(startIndexFolder(), indexOptions(m_settings));
    connect(m_index.get(), &IndexService::moveProgress, this, [this](int percent) {
        m_indexMoveProgress = percent;
        refreshIndexFolder();
    });
    connect(m_index.get(), &IndexService::moveFinished, this, &App::indexMoved);
    // Files still in the default place go where the settings say, once read.
    connect(m_index.get(), &IndexService::stateChanged, this, &App::applyIndexFolder);
    m_apps = std::make_unique<AppCatalog>();
    m_places = std::make_unique<SystemCatalog>();
    m_engine = std::make_unique<SearchEngine>(m_index.get(), m_apps.get(), m_places.get());
    m_launcher
        = std::make_unique<Launcher>(m_index.get(), m_apps.get(), m_places.get(), m_engine.get(), m_history.get());
    connect(m_launcher.get(), &Launcher::dismissRequested, this, &App::hideLauncher);
    connect(m_launcher.get(), &Launcher::openFailed, this, [this](const QString& name) {
        m_messages->showNotification(tr("没有打开“%1”").arg(name),
            tr("找不到资源管理器，没法用普通权限打开它。请稍后再试，或按 Ctrl+Shift+Enter 以管理员身份运行。"));
    });
    connect(m_launcher.get(), &Launcher::namesShown, this, [this] {
        if (m_revealing)
            armReveal();
    });
    connect(m_launcher.get(), &Launcher::webSettingsRequested, this, &App::showWebSettings);

    // Clipboard history (Win+V), in clipboard\ next to the index.
    m_clipStore = std::make_unique<ClipStore>(dataDir + u"\\clipboard"_s);
    if (!m_clipStore->open(QDateTime::currentMSecsSinceEpoch()))
        qWarning() << "Clipboard history: cannot be kept";
    ClipboardWatcher::Callbacks clipCallbacks;
    // On the watcher's thread: hop to the GUI thread.
    clipCallbacks.captured = [this](ClipCapture capture) {
        auto copied = std::make_shared<ClipCapture>(std::move(capture));
        QMetaObject::invokeMethod(this, [this, copied] { clipCaptured(*copied); }, Qt::QueuedConnection);
    };
    clipCallbacks.reused = [this](qint64 id) { // pasted from the history: it moves up
        QMetaObject::invokeMethod(this, [this, id] {
            m_clipStore->touch(id, QDateTime::currentMSecsSinceEpoch());
            m_clipboard->historyChanged();
        }, Qt::QueuedConnection);
    };
    m_clipWatcher = std::make_unique<ClipboardWatcher>(clipboardOptions(), std::move(clipCallbacks));
    m_clipboard = std::make_unique<Clipboard>(m_clipStore.get(), m_clipWatcher.get());
    connect(m_clipboard.get(), &Clipboard::dismissRequested, this, [this] { hideClipboard(); });
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

    m_updater = std::make_unique<Updater>();
    m_updater->setChinese(m_language == u"zh");
    connect(m_updater.get(), &Updater::found, this, [this](bool manual) {
        // Asked for, or the settings window is open: show it there. Otherwise
        // a tray notification, which does not take the keyboard from anyone.
        if (manual || (m_settingsWindow && m_settingsWindow->isVisible())) {
            showUpdate();
            return;
        }
        m_messages->showNotification(tr("Win顺 %1 可以更新了").arg(m_updater->availableVersion()),
            tr("点这里查看更新内容"));
        m_updateNotified = true;
    });

    if (!createWindow())
        return false;

    MessageWindow::Callbacks callbacks;
    callbacks.trayActivated = [this] { toggleLauncher(); };
    callbacks.trayMenuRequested = [this] { showTrayMenu(); };
    callbacks.notificationClicked = [this] {
        if (std::exchange(m_updateNotified, false))
            showUpdate();
        else if (std::exchange(m_clipboardNotified, false))
            showClipboardSettings();
    };
    callbacks.hotkeyPressed = [this](int id) {
        if (id == kWinVHotkeyId || id == kClipboardHotkeyId)
            toggleClipboard();
        else if (id != kDialogJumpHotkeyId)
            toggleLauncher();
        else if (m_dialogJump && m_settings.dialogJump)
            m_dialogJump->jump();
    };
    callbacks.shellRestarted = [this] { // Explorer let go of Win+V, or took it back
        applyClipboardHotkeys();
        QTimer::singleShot(3s, this, &App::applyClipboardHotkeys); // once it has registered its own keys
    };
    callbacks.commandReceived = [this](const QString& command) { handleCommand(command); };
    callbacks.sessionEnding = [this] { m_index->shutdown(); }; // save the index before Windows ends us
    callbacks.deviceChange = [this](WPARAM event, LPARAM data) -> LRESULT {
        return m_volumeNotifier ? m_volumeNotifier->handle(event, data) : TRUE;
    };
    // Once the disks have settled: after hibernating, the file cache is gone.
    callbacks.resumed = [this] { m_index->prewarmSoon(30s); };
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

    applySettings(true);
    applyTheme();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &App::applyTheme);

    m_settingsWatcher.addPath(Settings::filePath());
    connect(&m_settingsWatcher, &QFileSystemWatcher::fileChanged, this, [this] { m_settingsReload.start(); });

    m_index->start();
    m_apps->refresh(true); // in the background; about half a second
    m_places->refresh(true); // likewise

    autostart::adoptIfOrphaned();
    if (firstRun) {
        // Autostart is on by default; the settings switch turns it off for good
        // (the settings file exists from now on, so this runs once).
        autostart::setEnabled(true);
        m_messages->showNotification(
            tr("Win顺已在后台运行"), tr("双击 Ctrl 打开搜索。首次运行需要一点时间建立文件索引。"));
    } else if (clipboardIsNew) {
        m_messages->showNotification(tr("Win顺 现在有剪贴板历史了"),
            m_settings.clipboard ? tr("复制过的文字、图片和文件都能找回来。点这里设置用 Win+V 打开它。")
                                 : tr("复制过的文字、图片和文件都能找回来。点这里打开它。"));
        m_updateNotified = false;
        m_clipboardNotified = true;
    } else if (!m_indexFolderProblem.isEmpty()) {
        m_messages->showNotification(tr("索引位置"), m_indexFolderProblem);
    }
    if (options.settings)
        showSettings();
    else if (!options.background || !options.query.isEmpty())
        showLauncher(options.query);
    QTimer::singleShot(1s, this, &App::prewarmLauncher);
    QTimer::singleShot(1500ms, this, &App::prewarmClipboard);
    return true;
}

bool App::createWindow()
{
    m_qml = std::make_unique<QQmlApplicationEngine>();
    m_qml->addImageProvider(u"fileicon"_s, new FileIconProvider); // engine takes ownership
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

    prepareBackdrop(m_window);
    m_window->create(); // native handle now, so DWM styling applies before the first show
    m_window->installEventFilter(this);
    m_launcher->setWindow(m_window);
    m_placement->setWindow(m_window);
    m_frame->setWindow(m_window);
    m_launcherLogo = new WindowLogo(m_window, 64); // the search box's height (SearchBar.qml)
    connect(m_window, &QWindow::activeChanged, this, [this] {
        // Clicking elsewhere or switching apps dismisses the launcher, like a
        // menu; the clipboard under its search box goes with it (see there).
        if (m_window && !m_window->isActive() && m_window->isVisible() && !clipboardUnderLauncher())
            hideLauncher();
    });
    // Reveal (see showLauncher). Both run on the render thread: a frame
    // synchronised after arming shows the fresh results, and once it has been
    // handed to the screen the window can be uncloaked.
    connect(m_window, &QQuickWindow::afterSynchronizing, this, [this] {
        if (m_revealArmed.load())
            m_revealSynced.store(true);
    }, Qt::DirectConnection);
    connect(m_window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_revealSynced.exchange(false))
            QMetaObject::invokeMethod(this, &App::revealLauncher, Qt::QueuedConnection);
        if (m_prewarming.load())
            QMetaObject::invokeMethod(this, &App::finishPrewarm, Qt::QueuedConnection);
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
    m_clipFrame->setWindow(window);
    m_clipLogo = new WindowLogo(window, 64); // its search box's height (ClipboardPage.qml)
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
    connect(window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_clipUncloak.exchange(false))
            QMetaObject::invokeMethod(this, &App::uncloakClipboard, Qt::QueuedConnection);
        if (m_clipPrewarming.load())
            QMetaObject::invokeMethod(this, &App::finishClipPrewarm, Qt::QueuedConnection);
    }, Qt::DirectConnection); // on the render thread
    updateCompanions();
    return true;
}

bool App::eventFilter(QObject* watched, QEvent* event)
{
    // Pasted into a one-line field (the search boxes, the settings' fields):
    // several lines go in as one. Before the field sees the keys.
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->matches(QKeySequence::Paste)) {
        if (auto* window = qobject_cast<QQuickWindow*>(watched); window && textfield::pasteOneLine(window->activeFocusItem()))
            return true;
    }
    if (watched == m_window && event->type() == QEvent::ContextMenu
        && static_cast<QContextMenuEvent*>(event)->reason() == QContextMenuEvent::Keyboard) {
        emit m_launcher->contextMenuKeyPressed();
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
    if (m_window)
        styleWindow(m_window, SystemTheme::backdropAvailable());
    if (m_clipWindow)
        styleWindow(m_clipWindow, SystemTheme::backdropAvailable());
    if (m_settingsWindow)
        styleWindow(m_settingsWindow, SystemTheme::backdropAvailable());
    if (m_barWindow)
        styleWindow(m_barWindow);
    // The frames' dark mode only once Qt has set its own (light) one, a few
    // milliseconds later; then watch them for a while (see win::setDarkFrame).
    m_darkFrame = dark;
    m_darkFrameWatch.start();
    m_darkFrameGuard.start();
}

// Theme and language take effect at once, in every window, without a restart.
void App::applyAppearance()
{
    SystemTheme::setBackdropOn(m_settings.transparency == u"on"); // Mica stays set up behind, covered when off
    // Unknown follows Windows. Everything else reads the scheme from the style
    // hints and listens to colorSchemeChanged (see applyTheme, SystemTheme).
    QGuiApplication::styleHints()->setColorScheme(m_settings.theme == u"dark" ? Qt::ColorScheme::Dark
            : m_settings.theme == u"light"                                     ? Qt::ColorScheme::Light
                                                                               : Qt::ColorScheme::Unknown);

    const QString language = Settings::resolveLanguage(m_settings.language);
    if (language == m_language)
        return;
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

    if (!m_qml)
        return; // starting up: nothing shown yet
    m_qml->retranslate();
    m_launcher->retranslate();
    m_clipboard->retranslate();
    m_messages->setTrayTooltip(trayTooltip());
    applyHotkey(); // its error message
    refreshContentIndexStatus();
}

QString App::trayTooltip() const
{
    return tr("Win顺 — 双击 Ctrl 打开");
}

void App::applySettings(bool initial)
{
    if (m_settings.doubleCtrl && !m_keyListener) {
        // Called on the listener thread: hop to the GUI thread.
        m_keyListener = std::make_unique<KeyListener>(
            [this] { QMetaObject::invokeMethod(this, &App::onDoubleCtrl, Qt::QueuedConnection); });
    } else if (!m_settings.doubleCtrl) {
        m_keyListener.reset();
    }
    applyDialogs();
    applyClipboard();
    for (const auto& [kind, name] : filemanager::kSettingNames) {
        if (m_settings.fileManager == QLatin1StringView(name))
            shell::setFileManager(kind);
    }

    if (!initial)
        applyAppearance();
    m_updater->setAutomatic(m_settings.autoUpdate);
    m_launcher->setRecordHistory(m_settings.recordHistory);
    applyHotkey();
    m_launcher->setContentOptions(m_settings.contentExtensions, m_settings.contentSizeLimits(),
        m_settings.contentInLowPriority, m_settings.contentDocuments);
    m_launcher->setWebShortcuts(m_settings.webShortcuts);
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
    if (m_settingsEditor)
        m_settingsEditor->setHotkeyError(hotkeyError); // shown next to the shortcut
    else if (!hotkeyError.isEmpty())
        m_messages->showNotification(tr("快捷键不可用"), hotkeyError);
}

ClipboardWatcher::Options App::clipboardOptions() const
{
    return {m_settings.clipboard && !m_clipboardPaused, m_settings.clipboardImages, m_settings.clipboardExcludedApps};
}

void App::applyClipboard()
{
    // Explorer gives Win+V up (or gets it back) when it next starts. Only
    // when the switch moved, or it is on at start: someone else may have put
    // a V there for another program.
    if (m_winVApplied ? *m_winVApplied != m_settings.clipboardWinV : m_settings.clipboardWinV)
        m_winVRegistryFailed = !winv::setReleasedByExplorer(m_settings.clipboardWinV);
    m_winVApplied = m_settings.clipboardWinV;
    m_clipWatcher->setOptions(clipboardOptions());
    m_clipStore->setLimits({m_settings.clipboardMaxItems, m_settings.clipboardMaxDays}, QDateTime::currentMSecsSinceEpoch());
    m_clipboard->historyChanged();
    applyClipboardHotkeys();
}

void App::applyClipboardHotkeys()
{
    m_messages->unregisterHotkey(kWinVHotkeyId);
    m_messages->unregisterHotkey(kClipboardHotkeyId);
    if (m_settingsEditor && m_settingsEditor->recordingHotkey())
        return; // the keys being pressed must reach the settings window
    const bool winV = m_settings.clipboardWinV && m_messages->registerHotkey(kWinVHotkeyId, u"Win+V"_s);
    QString state = u"off"_s;
    if (m_settings.clipboardWinV) {
        state = winV ? u"on"_s : m_winVRegistryFailed ? u"failed"_s : u"waiting"_s; // waiting: Explorer still has it
    } else if (!winv::releasedByExplorer() && m_messages->registerHotkey(kWinVHotkeyId, u"Win+V"_s)) {
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
    m_clipboardShortcut = winV ? u"Win+V"_s : custom ? hotkey : QString();
    m_clipboard->setState(m_settings.clipboard, m_clipboardPaused, m_clipboardShortcut);
    if (m_settingsEditor) {
        m_settingsEditor->setWinVState(state, winv::canRestartExplorer());
        m_settingsEditor->setClipboardHotkeyError(hotkeyError);
    } else if (!hotkeyError.isEmpty()) {
        m_messages->showNotification(tr("快捷键不可用"), hotkeyError);
    }
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
    // Seconds of waiting for Explorer to end and to start again: not on the
    // GUI thread.
    std::thread([self = QPointer<App>(this)] {
        const bool back = winv::restartExplorer();
        QMetaObject::invokeMethod(qApp, [self, back] {
            if (self)
                self->explorerRestarted(back);
        }, Qt::QueuedConnection);
    }).detach();
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
    // Win+V is free once the old Explorer is gone; the new one also says
    // when it is up (shellRestarted).
    applyClipboardHotkeys();
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
    m_settings = fresh;
    applySettings(false);
    if (m_settingsEditor)
        m_settingsEditor->setSettings(m_settings);
}

void App::settingsEdited(const Settings& settings)
{
    m_settings = settings;
    applySettings(false);
}

void App::showSettings()
{
    if (!m_settingsWindow) {
        auto* editor = new SettingsEditor(m_settings, m_renderer);
        auto* frame = new WindowFrame(true, editor); // its own title bar; goes with the editor
        QQmlComponent component(m_qml.get(), u"WinShun"_s, u"SettingsWindow"_s);
        QObject* object = component.createWithInitialProperties({
            {u"editor"_s, QVariant::fromValue(editor)},
            {u"frame"_s, QVariant::fromValue(frame)},
            {u"updater"_s, QVariant::fromValue(m_updater.get())},
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
        connect(editor, &SettingsEditor::edited, this, &App::settingsEdited);
        connect(editor, &SettingsEditor::recordingHotkeyChanged, this, &App::applyHotkey);
        connect(editor, &SettingsEditor::recordingHotkeyChanged, this, &App::applyClipboardHotkeys);
        connect(editor, &SettingsEditor::explorerRestartRequested, this, &App::restartExplorer);
        connect(editor, &SettingsEditor::clipboardClearRequested, this, [this, editor] {
            m_clipStore->clearHistory();
            m_clipboard->historyChanged();
            editor->setClipboardCount(static_cast<int>(m_clipStore->clips().size()));
        });
        connect(editor, &SettingsEditor::restartRequested, this, [this] { restart({u"--settings"_s}); });
        connect(editor, &SettingsEditor::historyClearRequested, this, [this, editor] {
            m_launcher->clearHistory();
            editor->setHistoryCount(0);
        });
        m_settingsWindow = window;
        m_settingsEditor = editor;
        applyHotkey(); // shows whether the current hotkey works
        applyClipboardHotkeys(); // ... and where Win+V stands
        editor->refreshFileManagers();

        prepareBackdrop(window);
        window->create();
        window->installEventFilter(this); // pasting into its fields
        frame->setWindow(window);
        styleWindow(window, SystemTheme::backdropAvailable());
        win::setDarkFrame(window, isDarkMode());
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        const QRect area = screen->availableGeometry();
        window->resize(window->width(), std::min(window->height(), area.height() - 40));
        window->setPosition(area.center() - QPoint(window->width() / 2, window->height() / 2));
        connect(window, &QWindow::visibleChanged, window, [this, window](bool visible) {
            if (!visible) {
                m_contentStatusTimer.stop();
                window->deleteLater(); // free the window's memory until it is needed again
            }
        });
    }
    refreshContentIndexStatus();
    refreshIndexFolder(true);
    m_settingsEditor->setHistoryCount(static_cast<int>(m_history->items().size()));
    m_settingsEditor->setClipboardCount(static_cast<int>(m_clipStore->clips().size()));
    m_contentStatusTimer.start();
    hideLauncher();
    m_settingsWindow->show();
    m_settingsWindow->raise();
    win::bringToFront(m_settingsWindow);
    m_settingsWindow->requestActivate();
}

void App::showClipboardSettings()
{
    showSettings();
    if (m_settingsWindow)
        QMetaObject::invokeMethod(m_settingsWindow, "showClipboardPage");
}

void App::showWebSettings()
{
    showSettings();
    if (m_settingsWindow)
        QMetaObject::invokeMethod(m_settingsWindow, "showWebPage");
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
    if (m_settings.contentIndex) {
        const ContentIndex::Stats stats = m_index->contentIndex()->stats();
        const int files = static_cast<int>(stats.documents);
        if (m_index->readingContent())
            status = tr("正在建立索引… 已收录 %Ln 个文件", nullptr, files);
        else if (stats.documents == 0)
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
    if (command == u"toggle")
        toggleLauncher();
    else if (command == u"quit")
        QGuiApplication::quit();
    else if (command == u"settings")
        showSettings();
    else if (command.startsWith(u"query:"))
        showLauncher(command.mid(6));
    else
        showLauncher();
}

void App::applyDialogs()
{
    if (m_settings.dialogBar && !m_dialogBar) {
        m_dialogBar = std::make_unique<DialogBar>(m_engine.get(), m_history.get(),
            Settings::dataDir() + u"\\dialog-pins.txt"_s, [this](HWND dialog, std::wstring path, bool isFile, bool open) {
                if (m_dialogJump)
                    m_dialogJump->go(dialog, std::move(path), isFile, open);
            });
        connect(m_dialogBar.get(), &DialogBar::excludeAppRequested, this, &App::excludeFromDialogBar);
        connect(m_dialogBar.get(), &DialogBar::settingsRequested, this, [this] { showSettings(); });
    } else if (!m_settings.dialogBar && m_dialogBar) {
        delete m_barWindow;
        m_dialogBar.reset();
        updateCompanions();
    }
    if (m_dialogBar) {
        m_dialogBar->setRecordHistory(m_settings.recordHistory);
        m_dialogBar->setExcludedApps(m_settings.dialogBarExcludedApps);
        m_dialogBar->setPlace(m_settings.dialogBarPlace);
    }

    const bool followDialogs = m_settings.dialogJump || m_settings.dialogBar || m_settings.dialogAutoJump;
    if (followDialogs && !m_dialogJump) {
        DialogJump::Callbacks callbacks;
        callbacks.setHotkey = [this](bool on) {
            if (on)
                return m_messages->registerHotkey(kDialogJumpHotkeyId, u"Ctrl+G"_s);
            m_messages->unregisterHotkey(kDialogJumpHotkeyId);
            return true;
        };
        callbacks.dialogChanged = [this](HWND dialog) {
            if (m_dialogBar && (!dialog || m_barWindow || createBarWindow()))
                m_dialogBar->setDialog(dialog);
        };
        callbacks.dialogMoved = [this] {
            if (m_dialogBar)
                m_dialogBar->dialogMoved();
        };
        callbacks.autoJumped = [this](HWND dialog, std::wstring from) { // on DialogJump's thread
            QMetaObject::invokeMethod(this, [this, dialog, from = QString::fromStdWString(from)] {
                if (m_dialogBar)
                    m_dialogBar->setOrigin(dialog, from);
            }, Qt::QueuedConnection);
        };
        m_dialogJump = std::make_unique<DialogJump>(std::move(callbacks));
        updateCompanions();
    } else if (!followDialogs) {
        m_dialogJump.reset();
    }
    if (m_dialogJump) {
        m_dialogJump->setHotkeyEnabled(m_settings.dialogJump);
        m_dialogJump->setAutoJump(m_settings.dialogAutoJump);
    }
    // Turned on with a file dialog in front: under it at once.
    if (m_dialogBar && !m_barWindow && m_dialogJump && m_dialogJump->dialog() && createBarWindow())
        m_dialogBar->setDialog(m_dialogJump->dialog());
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
    m_barWindow = window;
    m_dialogBar->setWindow(window);
    // Made for DialogJump's first look at the window in front, before it is
    // there: then applyDialogs() does this.
    updateCompanions();
    return true;
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

void App::onDoubleCtrl()
{
    const ForegroundFacts facts = foreground::facts();
    const IgnoreReason reason = doubleCtrlIgnoreReason(facts,
        {m_settings.doubleCtrlPauseInGames, m_settings.doubleCtrlPauseInFullScreen, m_settings.doubleCtrlExcludedApps});
    if (reason == IgnoreReason::None) {
        toggleLauncher();
        return;
    }
    // Nothing shown over a game. The log says why, for "double Ctrl does nothing".
    static const char* const why[] {"", "it is in the list", "it is full screen", "it is in exclusive full screen",
        "it has taken the mouse (cursor hidden and confined, or full screen)"};
    const QString key = facts.program + u' ' + QString::number(static_cast<int>(reason));
    if (!m_doubleCtrlIgnoredLogged.contains(key)) {
        m_doubleCtrlIgnoredLogged.append(key);
        qWarning().noquote() << "Double Ctrl ignored over" << facts.program << "as"
                             << why[static_cast<int>(reason)];
    }
}

void App::toggleLauncher()
{
    // The clipboard in front: back into the box it came from, or over to searching.
    if (clipboardInCharge()) {
        if (m_clipboard->field()) {
            m_clipboard->dismiss();
            return;
        }
        hideClipboard();
    }
    // In front of a file dialog: into its search bar, and back.
    if (m_dialogBar && m_dialogBar->isShown()) {
        m_dialogBar->toggleFocus();
        return;
    }
    if (m_window && m_window->isVisible() && m_window->isActive())
        hideLauncher();
    else
        showLauncher();
}

void App::toggleClipboard()
{
    // Again in front: back to the field it was opened from, or hidden.
    if (clipboardInCharge())
        m_clipboard->dismiss();
    else
        showClipboard();
}

void App::showLauncher(const QString& query)
{
    if (!m_window)
        return;
    hideClipboard(false); // from under its search box: searching again
    // On the monitor under the mouse, where the user last put it (see Placement).
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    m_placement->placeOn(screen ? screen : QGuiApplication::primaryScreen());
    if (!query.isEmpty())
        m_launcher->setQuery(query);

    const bool prewarming = m_prewarming.exchange(false); // up already, cloaked
    if (!m_window->isVisible() || prewarming) {
        // Shown cloaked: Windows would first put up the window as it looked
        // when it was hidden (old selection, old rows), then our repaint.
        // Revealed once a frame with the refreshed results is drawn.
        win::setCloaked(m_window, true);
        m_revealing = true;
        m_revealTimeout.start();
    }
    m_window->show();
    win::bringToFront(m_window);
    m_window->requestActivate();
    m_launcher->handleShown();
    if (m_revealing && !m_launcher->waitingForNames())
        armReveal(); // nothing to wait for (内容 refreshes in the background)
}

void App::showClipboard()
{
    if (!m_clipWindow && !createClipWindow())
        return;
    if (m_keyRouter) // up already, over the program that keeps the focus
        return;
    const bool prewarming = m_clipPrewarming.exchange(false); // up already, cloaked
    bool keepFocus = false; // where it is, with the program it pastes into
    if (!m_clipWindow->isVisible() || prewarming) {
        // It pastes into what had the keyboard before it: a text field of
        // Win顺's own (read before the window takes the focus from it), or
        // else the window in front.
        QQuickItem* field = focusedField();
        m_clipboard->setField(field, field && field->window() == m_settingsWindow ? tr("输入框") : tr("搜索框"));
        const HWND foreground = ::GetForegroundWindow();
        m_clipboard->setTarget(foreground);
        m_clipboard->setActive(true); // under the launcher's search box, it folds its rows away (Main.qml)
        m_clipboard->handleShown(); // the list alone again: its width as it opens
        if (field) {
            // Dropping down under the field's box (the launcher's search bar,
            // the bar by a file dialog, a box in the settings window).
            const QQuickItem* box = field->parentItem() ? field->parentItem() : field;
            m_clipPlacement->attach(QRectF(box->mapToGlobal(QPointF(0, 0)), box->size()).toAlignedRect());
        } else {
            // By the text caret in the program it pastes into, as Windows'
            // own clipboard opens; else by the mouse pointer, its top left
            // corner at the pointer's tip, as a menu.
            const HWND target = paste::usableTarget(foreground);
            const std::optional<QRect> caret = target ? m_textCaret->find(target, kCaretWait) : std::nullopt;
            if (caret)
                m_clipPlacement->attachNative(*caret);
            else
                m_clipPlacement->attach(QRect(QCursor::pos(), QSize(1, 0)));
            constexpr LONG reach = 1 << 20; // the logo stays off the line typed in, all along it
            m_clipLogo->setAvoid(caret ? std::optional(RECT {caret->x() - reach, caret->y(), caret->x() + reach,
                                             caret->y() + caret->height()})
                                       : std::nullopt);
            // As Windows' own: that program stays in front, whatever closes
            // without the focus stays open there (VS Code's command palette,
            // a browser's suggestions), the caret stays where the paste goes.
            // Not over the Start menu, search and the like, which stay above
            // every window: bringToFront closes them.
            keepFocus = target && !win::isShellFlyout(foreground) && startKeyRouting(target);
        }
        // Cloaked until its first frame: Windows would first put up the
        // window as it looked when it was hidden, then our repaint.
        win::setCloaked(m_clipWindow, true);
        m_clipUncloak = true;
        m_clipUncloakTimeout.start();
    }
    if (keepFocus) {
        m_clipFrame->setNoActivate(true);
        m_clipWindow->setProperty("_q_showWithoutActivating", true);
        m_clipWindow->show();
        m_clipWindow->setProperty("_q_showWithoutActivating", QVariant());
        // On top of the other topmost windows, as an activated one would be.
        ::SetWindowPos(reinterpret_cast<HWND>(m_clipWindow->winId()), HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    m_clipWindow->show();
    win::bringToFront(m_clipWindow);
    m_clipWindow->requestActivate();
}

bool App::clipboardInCharge() const
{
    return m_clipWindow && m_clipWindow->isVisible() && !m_clipPrewarming.load()
        && (m_clipWindow->isActive() || m_keyRouter);
}

bool App::startKeyRouting(HWND target)
{
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
    callbacks.drained = [this, run] {
        QMetaObject::invokeMethod(this, [this, run] {
            if (m_drainingRun == run)
                m_drainingRouter.reset();
        }, Qt::QueuedConnection);
    };
    auto router = std::make_unique<KeyRouter>(target, reinterpret_cast<HWND>(m_clipWindow->winId()), std::move(callbacks));
    if (!router->isActive()) {
        qWarning() << "Clipboard: no keyboard hook; it takes the focus instead";
        return false;
    }
    m_keyRouter = std::move(router);
    m_keyRouterTarget = target;
    m_clipboard->setKeysRouted(true);
    return true;
}

void App::stopKeyRouting()
{
    if (m_keyRouter) {
        // No key is taken any more; the releases of those it took still are
        // its own, for a moment.
        m_keyRouter->drain();
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
        m_clipboard->dismiss(); // closes it, not the program behind
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
    if (clip && window == clip) {
        if (::GetForegroundWindow() == clip)
            stopKeyRouting();
        return;
    }
    hideClipboard(); // another program in front: as a menu
}

void App::activateClipboard()
{
    if (!m_keyRouter || !m_clipWindow)
        return;
    // Active first, then no longer drawn as active without being so: the
    // backdrop does not flicker. Keys pressed in between are its own already.
    win::bringToFront(m_clipWindow);
    m_clipWindow->requestActivate();
    stopKeyRouting();
}

void App::uncloakClipboard()
{
    m_clipUncloakTimeout.stop();
    if (!m_clipWindow)
        return;
    win::setCloaked(m_clipWindow, false);
    // The logo when it is on its own: under a box, that box's window has one.
    if (m_clipWindow->isVisible() && !m_clipboard->field() && m_clipLogo)
        m_clipLogo->reveal();
}

void App::hideClipboard(bool launcherToo)
{
    if (m_keyRouter)
        m_keyRouter->drain(); // first: the keys go to the program from here on
    if (!m_clipWindow || !m_clipWindow->isVisible() || m_clipPrewarming.load()) {
        stopKeyRouting();
        return;
    }
    const bool underLauncher = clipboardUnderLauncher();
    m_clipWindow->hide();
    stopKeyRouting(); // hidden first: no inactive backdrop on the way out
    m_clipUncloak = false;
    m_clipUncloakTimeout.stop();
    win::setCloaked(m_clipWindow, false);
    m_clipboard->setActive(false); // the launcher's rows come back
    m_clipboard->handleHidden();
    // Gone without going back to the search box (pasted elsewhere, clicked
    // away): the launcher goes too.
    if (launcherToo && underLauncher && m_window && !m_window->isActive())
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
    m_revealArmed.store(true);
    m_window->update(); // a new frame even if the results did not change
}

void App::revealLauncher()
{
    if (!m_revealing)
        return;
    m_revealing = false;
    m_revealArmed.store(false);
    m_revealSynced.store(false);
    m_revealTimeout.stop();
    if (m_window)
        win::setCloaked(m_window, false);
    if (m_launcherLogo)
        m_launcherLogo->reveal(); // not if this was the launcher hiding before its first frame
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

void App::hideLauncher()
{
    if (!m_window || !m_window->isVisible())
        return;
    const bool clipboardToo = clipboardUnderLauncher();
    m_window->hide();
    revealLauncher(); // hidden before its first frame: drop the cloak
    m_launcher->handleHidden();
    if (clipboardToo)
        hideClipboard(false);
}

void App::showTrayMenu()
{
    // The click that asked for the menu was the last input (tick count, ms).
    LASTINPUTINFO input {sizeof input};
    ::GetLastInputInfo(&input);
    const DWORD asked = ::GetTickCount();
    const bool ready = m_index->state() == IndexService::State::Ready;
    const QString info = ready ? tr("已索引 %Ln 项", nullptr, static_cast<int>(m_index->itemCount()))
                               : tr("正在建立索引…");
    const QString show = tr("打开 Win顺");
    std::vector<shell::MenuItem> items {
        {ShowCommand, m_settings.doubleCtrl ? show + u'\t' + tr("双击 Ctrl") : show, false, true, true},
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
    if (m_updater->available() && !m_updater->skipped()) // at the top, where it is seen
        items.insert(items.begin(),
            {{UpdateCommand, tr("Win顺 %1 可以更新了…").arg(m_updater->availableVersion())}, shell::MenuItem::separator()});
    POINT pos {};
    ::GetCursorPos(&pos);
    const DWORD built = ::GetTickCount();
    // A slow menu leaves a line in the log: which part it waited on.
    const int chosen = shell::popupMenu(m_messages->hwnd(), items, pos, [&] {
        const DWORD shown = ::GetTickCount();
        if (shown - input.dwTime >= kSlowMenuMs)
            qWarning().nospace() << "Tray menu shown " << shown - input.dwTime << " ms after the click: "
                                 << asked - input.dwTime << " ms until it was asked for, " << built - asked
                                 << " ms to build, " << shown - built << " ms for Windows to show it";
    });
    if (chosen != QuitCommand && chosen != RestartCommand)
        autostart::refresh(); // changed outside WinShun? Ready for the next time
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
        m_messages->showNotification(tr("正在重建索引"), tr("可以照常搜索，完成后结果会自动更新。"));
        break;
    case AutostartCommand:
        autostart::setEnabled(!autostart::isEnabled());
        break;
    case SettingsCommand:
        showSettings();
        break;
    case UpdateCommand:
        showUpdate();
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
        QGuiApplication::quit();
        break;
    default:
        break;
    }
}

void App::restart(const QStringList& arguments)
{
    m_restartArguments = arguments;
    QGuiApplication::quit();
}

} // namespace ws
