#include "App.h"

#include "AppCatalog.h"
#include "FileIconProvider.h"
#include "History.h"
#include "IndexService.h"
#include "Launcher.h"
#include "SearchEngine.h"
#include "SettingsEditor.h"
#include "platform/KeyboardHook.h"
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

namespace qf {

namespace {

constexpr int kHotkeyId = 1;

enum TrayCommand {
    ShowCommand = 1,
    IndexInfo,
    RebuildCommand,
    AutostartCommand,
    SettingsCommand,
    DataFolderCommand,
    QuitCommand
};

IndexService::Options indexOptions(const Settings& settings)
{
    IndexService::Options options;
    options.rules = settings.crawlRules();
    options.snapshotPath = Settings::dataDir() + u"\\index.bin"_s;
    options.includeRemovable = settings.includeRemovableDrives;
    options.rescanOnStartup = settings.rescanOnStartup;
    return options;
}

bool isDarkMode()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
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
}

App::~App()
{
    delete m_settingsWindow; // before the QML engine it was created with
}

bool App::start(const StartOptions& options)
{
    const bool firstRun = !Settings::exists();
    m_settings.load();
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
    connect(m_launcher.get(), &Launcher::namesShown, this, [this] {
        if (m_revealing)
            armReveal();
    });

    if (!createWindow())
        return false;

    MessageWindow::Callbacks callbacks;
    callbacks.trayActivated = [this] { toggleLauncher(); };
    callbacks.trayMenuRequested = [this] { showTrayMenu(); };
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
    m_messages->showTrayIcon(u"快搜 — 双击 Ctrl 打开"_s);

    applySettings(true);
    applyTheme();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &App::applyTheme);

    m_settingsWatcher.addPath(Settings::filePath());
    connect(&m_settingsWatcher, &QFileSystemWatcher::fileChanged, this, [this] { m_settingsReload.start(); });

    m_index->start();
    m_apps->refresh(true); // in the background; about half a second

    if (firstRun) {
        // Autostart is on by default; the settings switch turns it off for good
        // (the settings file exists from now on, so this runs once).
        autostart::setEnabled(true);
        m_messages->showNotification(
            u"快搜已在后台运行"_s, u"双击 Ctrl 打开搜索。首次运行需要一点时间建立文件索引。"_s);
    }
    if (options.settings)
        showSettings();
    else if (!options.background || !options.query.isEmpty())
        showLauncher(options.query);
    return true;
}

bool App::createWindow()
{
    m_qml = std::make_unique<QQmlApplicationEngine>();
    m_qml->addImageProvider(u"fileicon"_s, new FileIconProvider); // engine takes ownership
    m_qml->setInitialProperties({{u"launcher"_s, QVariant::fromValue(m_launcher.get())}});
    m_qml->loadFromModule(u"QuickFind"_s, u"Main"_s);
    const auto roots = m_qml->rootObjects();
    if (roots.isEmpty())
        return false;
    m_window = qobject_cast<QQuickWindow*>(roots.constFirst());
    if (!m_window)
        return false;

    m_window->create(); // native handle now, so DWM styling applies before the first show
    m_window->installEventFilter(this);
    m_launcher->setWindow(m_window);
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
        win::styleFramelessWindow(m_window, dark, dark ? QColor(0x40, 0x40, 0x40) : QColor(0xD4, 0xD4, 0xD4));
    if (m_settingsWindow)
        win::setDarkTitleBar(m_settingsWindow, dark);
    win::setMenuTheme(dark);
}

void App::applySettings(bool initial)
{
    if (m_settings.doubleCtrl && !m_keyboardHook) {
        // Called on the hook thread: hop to the GUI thread.
        m_keyboardHook = std::make_unique<KeyboardHook>(
            [this] { QMetaObject::invokeMethod(this, &App::toggleLauncher, Qt::QueuedConnection); });
    } else if (!m_settings.doubleCtrl) {
        m_keyboardHook.reset();
    }

    applyHotkey();
    m_launcher->setContentOptions(
        m_settings.contentExtensions, static_cast<qint64>(m_settings.maxContentFileSizeMB) << 20);
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
            ? u"“%1” 已被其他程序或系统占用，请换一个。"_s.arg(hotkey)
            : u"无法识别“%1”，请重新设置。"_s.arg(hotkey);
    }
    if (m_settingsEditor)
        m_settingsEditor->setHotkeyError(hotkeyError); // shown next to the shortcut
    else if (!hotkeyError.isEmpty())
        m_messages->showNotification(u"快捷键不可用"_s, hotkeyError);
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
        QQmlComponent component(m_qml.get(), u"QuickFind"_s, u"SettingsWindow"_s);
        QObject* object = component.createWithInitialProperties({{u"editor"_s, QVariant::fromValue(editor)}});
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
        m_settingsWindow = window;
        m_settingsEditor = editor;
        applyHotkey(); // shows whether the current hotkey works

        window->create();
        win::setDarkTitleBar(window, isDarkMode());
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        const QRect area = screen->availableGeometry();
        window->resize(window->width(), std::min(window->height(), area.height() - 40));
        window->setPosition(area.center() - QPoint(window->width() / 2, window->height() / 2));
        connect(window, &QWindow::visibleChanged, window, [window](bool visible) {
            if (!visible)
                window->deleteLater(); // free the window's memory until it is needed again
        });
    }
    hideLauncher();
    m_settingsWindow->show();
    m_settingsWindow->raise();
    win::bringToFront(m_settingsWindow);
    m_settingsWindow->requestActivate();
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
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect area = screen->availableGeometry();
    // Upper third of the monitor under the mouse; the window grows downwards.
    m_window->setPosition(area.x() + (area.width() - m_window->width()) / 2, area.y() + area.height() / 5);
    if (!query.isEmpty())
        m_launcher->setQuery(query);

    if (!m_window->isVisible()) {
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
    const bool ready = m_index->state() == IndexService::State::Ready;
    const QString info = ready
        ? u"已索引 %1 项"_s.arg(QLocale().toString(static_cast<qulonglong>(m_index->itemCount())))
        : u"正在建立索引…"_s;
    const std::vector<shell::MenuItem> items {
        {ShowCommand, m_settings.doubleCtrl ? u"打开快搜\t双击 Ctrl"_s : u"打开快搜"_s, false, true, true},
        shell::MenuItem::separator(),
        {IndexInfo, info, false, false},
        {RebuildCommand, u"重建索引"_s},
        shell::MenuItem::separator(),
        {AutostartCommand, u"开机自动启动"_s, autostart::isEnabled()},
        {SettingsCommand, u"设置…"_s},
        {DataFolderCommand, u"打开数据文件夹"_s},
        shell::MenuItem::separator(),
        {QuitCommand, u"退出"_s},
    };
    POINT pos {};
    ::GetCursorPos(&pos);
    switch (shell::popupMenu(m_messages->hwnd(), items, pos)) {
    case ShowCommand:
        showLauncher();
        break;
    case RebuildCommand:
        m_index->rebuild();
        m_messages->showNotification(u"正在重建索引"_s, u"可以照常搜索，完成后结果会自动更新。"_s);
        break;
    case AutostartCommand:
        autostart::setEnabled(!autostart::isEnabled());
        break;
    case SettingsCommand:
        showSettings();
        break;
    case DataFolderCommand:
        shell::open(Settings::dataDir());
        break;
    case QuitCommand:
        QGuiApplication::quit();
        break;
    default:
        break;
    }
}

} // namespace qf
