#include "App.h"

#include "AppCatalog.h"
#include "FileIconProvider.h"
#include "History.h"
#include "IndexService.h"
#include "Launcher.h"
#include "Placement.h"
#include "SearchEngine.h"
#include "SettingsEditor.h"
#include "Updater.h"
#include "WindowFrame.h"
#include "platform/KeyListener.h"
#include "platform/MessageWindow.h"
#include "platform/Shell.h"
#include "platform/VolumeNotifier.h"
#include "platform/WindowEffects.h"

#include <QContextMenuEvent>
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>

#include <windows.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr int kHotkeyId = 1;
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
    QuitCommand
};

IndexService::Options indexOptions(const Settings& settings)
{
    IndexService::Options options;
    options.rules = settings.crawlRules();
    options.snapshotPath = Settings::dataDir() + u"\\index.bin"_s;
    options.includeRemovable = settings.includeRemovableDrives;
    options.rescanOnStartup = settings.rescanOnStartup;
    options.content.enabled = settings.contentIndex;
    options.content.extensions = settings.contentExtensions;
    options.content.maxFileBytes = static_cast<std::int64_t>(settings.maxContentFileSizeMB) << 20;
    options.content.includeLowPriority = settings.contentInLowPriority;
    return options;
}

bool isDarkMode()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

// Rounded corners, a thin border and the shadow, for our windows without a
// system title bar (the launcher, the settings window).
void styleWindow(QWindow* window, bool dark)
{
    win::styleFramelessWindow(window, dark, dark ? QColor(0x40, 0x40, 0x40) : QColor(0xD4, 0xD4, 0xD4));
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
    m_contentStatusTimer.setInterval(2s);
    connect(&m_contentStatusTimer, &QTimer::timeout, this, &App::refreshContentIndexStatus);
}

App::~App()
{
    delete m_settingsWindow; // before the QML engine it was created with
}

bool App::start(const StartOptions& options)
{
    const bool firstRun = !Settings::exists();
    m_settings.load();
    applyAppearance(); // before any window: the first frame is already in the chosen theme and language
    m_renderer = Settings::resolveRenderer(m_settings.renderer);
    if (m_renderer == u"software")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    else if (m_renderer == u"d3d11")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);

    const QString dataDir = Settings::dataDir();
    QDir().mkpath(dataDir);
    m_history = std::make_unique<History>(dataDir + u"\\history.txt"_s);
    m_history->load();
    m_index = std::make_unique<IndexService>(indexOptions(m_settings));
    m_apps = std::make_unique<AppCatalog>();
    m_engine = std::make_unique<SearchEngine>(m_index.get(), m_apps.get());
    m_launcher = std::make_unique<Launcher>(m_index.get(), m_apps.get(), m_engine.get(), m_history.get());
    connect(m_launcher.get(), &Launcher::dismissRequested, this, &App::hideLauncher);
    connect(m_launcher.get(), &Launcher::openFailed, this, [this](const QString& name) {
        m_messages->showNotification(tr("没有打开“%1”").arg(name),
            tr("找不到资源管理器，没法用普通权限打开它。请稍后再试，或按 Ctrl+Shift+Enter 以管理员身份运行。"));
    });
    connect(m_launcher.get(), &Launcher::namesShown, this, [this] {
        if (m_revealing)
            armReveal();
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
    };
    callbacks.hotkeyPressed = [this](int) { toggleLauncher(); };
    callbacks.commandReceived = [this](const QString& command) { handleCommand(command); };
    callbacks.sessionEnding = [this] { m_index->shutdown(); }; // save the index before Windows ends us
    callbacks.deviceChange = [this](WPARAM event, LPARAM data) -> LRESULT {
        return m_volumeNotifier ? m_volumeNotifier->handle(event, data) : TRUE;
    };
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

    autostart::adoptIfOrphaned();
    if (firstRun) {
        // Autostart is on by default; the settings switch turns it off for good
        // (the settings file exists from now on, so this runs once).
        autostart::setEnabled(true);
        m_messages->showNotification(
            tr("Win顺已在后台运行"), tr("双击 Ctrl 打开搜索。首次运行需要一点时间建立文件索引。"));
    }
    if (options.settings)
        showSettings();
    else if (!options.background || !options.query.isEmpty())
        showLauncher(options.query);
    QTimer::singleShot(1s, this, &App::prewarmLauncher);
    return true;
}

bool App::createWindow()
{
    m_qml = std::make_unique<QQmlApplicationEngine>();
    m_qml->addImageProvider(u"fileicon"_s, new FileIconProvider); // engine takes ownership
    m_placement = std::make_unique<Placement>(Settings::dataDir() + u"\\state.ini"_s);
    m_frame = std::make_unique<WindowFrame>(false);
    m_qml->setInitialProperties({
        {u"launcher"_s, QVariant::fromValue(m_launcher.get())},
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

    m_window->create(); // native handle now, so DWM styling applies before the first show
    m_window->installEventFilter(this);
    m_launcher->setWindow(m_window);
    m_placement->setWindow(m_window);
    m_frame->setWindow(m_window);
    connect(m_window, &QWindow::activeChanged, this, [this] {
        // Clicking elsewhere or switching apps dismisses the launcher, like a menu.
        if (m_window && !m_window->isActive() && m_window->isVisible())
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

bool App::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window && event->type() == QEvent::ContextMenu
        && static_cast<QContextMenuEvent*>(event)->reason() == QContextMenuEvent::Keyboard) {
        emit m_launcher->contextMenuKeyPressed();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

void App::applyTheme()
{
    const bool dark = isDarkMode();
    if (m_window)
        styleWindow(m_window, dark);
    if (m_settingsWindow)
        styleWindow(m_settingsWindow, dark);
    win::setMenuTheme(dark);
}

// Theme and language take effect at once, in every window, without a restart.
void App::applyAppearance()
{
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
            [this] { QMetaObject::invokeMethod(this, &App::toggleLauncher, Qt::QueuedConnection); });
    } else if (!m_settings.doubleCtrl) {
        m_keyListener.reset();
    }

    if (!initial)
        applyAppearance();
    m_updater->setAutomatic(m_settings.autoUpdate);
    applyHotkey();
    m_launcher->setContentOptions(m_settings.contentExtensions,
        static_cast<qint64>(m_settings.maxContentFileSizeMB) << 20, m_settings.contentInLowPriority);
    if (!initial)
        m_indexOptionsApply.start();
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
        connect(editor, &SettingsEditor::restartRequested, this, [this] { restart({u"--settings"_s}); });
        m_settingsWindow = window;
        m_settingsEditor = editor;
        applyHotkey(); // shows whether the current hotkey works

        window->create();
        frame->setWindow(window);
        styleWindow(window, isDarkMode());
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
    m_contentStatusTimer.start();
    hideLauncher();
    m_settingsWindow->show();
    m_settingsWindow->raise();
    win::bringToFront(m_settingsWindow);
    m_settingsWindow->requestActivate();
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
            status = tr("索引在 Win顺启动约半分钟后开始建立");
        else
            status = tr("已收录 %Ln 个文件，占用 %1 磁盘空间", nullptr, files)
                         .arg(QLocale().formattedDataSize(
                             static_cast<qint64>(stats.segmentBytes), 0, QLocale::DataSizeTraditionalFormat));
        if (stats.pending > 0 && !m_index->readingContent())
            status += tr("；%Ln 个文件有改动，稍后更新", nullptr, static_cast<int>(stats.pending));
    }
    m_settingsEditor->setContentIndexStatus(status);
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

void App::toggleLauncher()
{
    if (m_window && m_window->isVisible() && m_window->isActive())
        hideLauncher();
    else
        showLauncher();
}

void App::showLauncher(const QString& query)
{
    if (!m_window)
        return;
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
}

// The first frame a window draws sets up the graphics device, shaders and
// glyphs. Drawn once ahead, cloaked and without taking the focus, so the first
// open is as quick as later ones (which reuse all that: it is kept while the
// window is hidden).
void App::prewarmLauncher()
{
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
    m_window->hide();
    revealLauncher(); // hidden before its first frame: drop the cloak
    m_launcher->handleHidden();
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
