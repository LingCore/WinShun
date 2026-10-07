#pragma once

#include "Settings.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <memory>

class QQmlApplicationEngine;
class QQuickWindow;

namespace qf {

class AppCatalog;
class History;
class IndexService;
class KeyboardHook;
class Launcher;
class MessageWindow;
class SearchEngine;
class SettingsEditor;
class VolumeNotifier;

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool createWindow();
    void applySettings(bool initial);
    void applyHotkey();
    void reloadSettings();
    void settingsEdited(const Settings& settings);
    void showTrayMenu();
    void applyTheme();
    void armReveal();
    void revealLauncher();

    Settings m_settings;
    std::unique_ptr<History> m_history;
    std::unique_ptr<IndexService> m_index;
    std::unique_ptr<AppCatalog> m_apps; // before the engine, which reads it
    std::unique_ptr<SearchEngine> m_engine;
    std::unique_ptr<Launcher> m_launcher;
    std::unique_ptr<MessageWindow> m_messages;
    std::unique_ptr<VolumeNotifier> m_volumeNotifier; // after m_messages, whose window it uses
    std::unique_ptr<KeyboardHook> m_keyboardHook;
    // The launcher is shown cloaked and revealed once it has drawn a frame
    // with fresh results (see showLauncher). Set on the GUI thread, read on
    // the render thread.
    bool m_revealing = false;
    std::atomic<bool> m_revealArmed {false}; // results are in: the next synchronised frame is the one
    std::atomic<bool> m_revealSynced {false}; // that frame has been synchronised
    QTimer m_revealTimeout;
    std::unique_ptr<QQmlApplicationEngine> m_qml; // destroyed first: QML references the objects above
    QPointer<QQuickWindow> m_window;
    QPointer<QQuickWindow> m_settingsWindow; // created on demand, deleted when closed
    QPointer<SettingsEditor> m_settingsEditor; // owned by m_settingsWindow
    QString m_renderer; // the one in use; changing it takes a restart

    QFileSystemWatcher m_settingsWatcher;
    QTimer m_settingsReload;
    QTimer m_indexOptionsApply; // rebuilding the crawler is costly: batch quick edits
};

} // namespace qf
