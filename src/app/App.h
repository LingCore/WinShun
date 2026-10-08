#pragma once

#include "Settings.h"

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QTranslator>

#include <atomic>
#include <memory>
#include <optional>

class QQmlApplicationEngine;
class QQuickWindow;

namespace ws {

class AppCatalog;
class DialogJump;
class SystemCatalog;
class History;
class IndexService;
class KeyListener;
class Launcher;
class MessageWindow;
class Placement;
class SearchEngine;
class SettingsEditor;
class Updater;
class VolumeNotifier;
class WindowFrame;

// Wires the pieces together and owns their lifetimes: settings, index,
// search engine, the QML window, tray icon and global hotkeys.
class App : public QObject {
    Q_OBJECT

public:
    struct StartOptions {
        bool background = false; // start in the tray without showing the window
        QString query;
        bool settings = false; // open the settings window
    };

    App();
    ~App() override;

    bool start(const StartOptions& options);
    void handleCommand(const QString& command); // from a second instance

    void toggleLauncher();
    void showLauncher(const QString& query = {});
    void hideLauncher();
    void showSettings();
    void showUpdate(); // the settings window with the update dialog
    // Set by "重新启动" in the tray menu or the settings window: the command
    // line for the new copy, which main() starts once this one has let go.
    std::optional<QStringList> restartArguments() const { return m_restartArguments; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool createWindow();
    void applySettings(bool initial);
    void applyHotkey();
    void reloadSettings();
    void settingsEdited(const Settings& settings);
    void showTrayMenu();
    void restart(const QStringList& arguments);
    void applyTheme();
    void applyAppearance(); // the chosen theme and language
    QString trayTooltip() const;
    void armReveal();
    void revealLauncher();
    void prewarmLauncher();
    void finishPrewarm();
    void refreshContentIndexStatus(); // shown in the settings window

    Settings m_settings;
    std::unique_ptr<History> m_history;
    std::unique_ptr<IndexService> m_index;
    std::unique_ptr<AppCatalog> m_apps; // before the engine, which reads it
    std::unique_ptr<SystemCatalog> m_places; // likewise
    std::unique_ptr<SearchEngine> m_engine;
    std::unique_ptr<Launcher> m_launcher;
    std::unique_ptr<MessageWindow> m_messages;
    std::unique_ptr<Updater> m_updater;
    bool m_updateNotified = false; // the last tray notification announced a new version
    std::unique_ptr<VolumeNotifier> m_volumeNotifier; // after m_messages, whose window it uses
    std::unique_ptr<KeyListener> m_keyListener; // double Ctrl
    std::unique_ptr<DialogJump> m_dialogJump; // Ctrl+G in file dialogs; after m_messages, which has the hotkey
    std::unique_ptr<Placement> m_placement; // where the launcher opens; moving it
    std::unique_ptr<WindowFrame> m_frame; // the launcher's header and footer drag it
    // The launcher is shown cloaked and revealed once it has drawn a frame
    // with fresh results (see showLauncher). Set on the GUI thread, read on
    // the render thread.
    bool m_revealing = false;
    std::atomic<bool> m_revealArmed {false}; // results are in: the next synchronised frame is the one
    std::atomic<bool> m_revealSynced {false}; // that frame has been synchronised
    QTimer m_revealTimeout;
    std::atomic<bool> m_prewarming {false}; // shown cloaked for one frame (see prewarmLauncher)
    std::unique_ptr<QQmlApplicationEngine> m_qml; // destroyed first: QML references the objects above
    QPointer<QQuickWindow> m_window;
    QPointer<QQuickWindow> m_settingsWindow; // created on demand, deleted when closed
    QPointer<SettingsEditor> m_settingsEditor; // owned by m_settingsWindow
    QString m_renderer; // the one in use; changing it takes a restart
    QString m_language; // resolved: zh or en
    QTranslator m_translator; // English; the source strings are Chinese
    std::optional<QStringList> m_restartArguments;

    QFileSystemWatcher m_settingsWatcher;
    QTimer m_settingsReload;
    QTimer m_indexOptionsApply; // rebuilding the crawler is costly: batch quick edits
    QTimer m_contentStatusTimer; // while the settings window is open
    QTimer m_darkFrameGuard; // see applyTheme
    QElapsedTimer m_darkFrameWatch;
    bool m_darkFrame = false;
};

} // namespace ws
