#pragma once

#include "Settings.h"

#include <QObject>
#include <QPointer>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

namespace ws {

// The view-model behind the settings window. Every change is saved right
// away and reported through edited(); there is no OK / Cancel step.
class SettingsEditor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool doubleCtrl READ doubleCtrl WRITE setDoubleCtrl NOTIFY changed FINAL)
    Q_PROPERTY(QString hotkey READ hotkey WRITE setHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString hotkeyError READ hotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(bool recordingHotkey READ recordingHotkey WRITE setRecordingHotkey NOTIFY recordingHotkeyChanged FINAL)
    Q_PROPERTY(bool autostart READ autostart WRITE setAutostart NOTIFY autostartChanged FINAL)
    Q_PROPERTY(bool includeRemovableDrives READ includeRemovableDrives WRITE setIncludeRemovableDrives NOTIFY changed FINAL)
    Q_PROPERTY(bool rescanOnStartup READ rescanOnStartup WRITE setRescanOnStartup NOTIFY changed FINAL)
    Q_PROPERTY(QStringList excludedPaths READ excludedPaths NOTIFY changed FINAL) // expanded, for display
    Q_PROPERTY(QStringList excludedNames READ excludedNames NOTIFY changed FINAL)
    Q_PROPERTY(QStringList contentExtensions READ contentExtensions NOTIFY changed FINAL)
    Q_PROPERTY(int maxContentFileSizeMB READ maxContentFileSizeMB WRITE setMaxContentFileSizeMB NOTIFY changed FINAL)
    Q_PROPERTY(bool contentIndex READ contentIndex WRITE setContentIndex NOTIFY changed FINAL)
    Q_PROPERTY(bool contentInLowPriority READ contentInLowPriority WRITE setContentInLowPriority NOTIFY changed FINAL)
    Q_PROPERTY(QString contentIndexStatus READ contentIndexStatus NOTIFY contentIndexStatusChanged FINAL)
    Q_PROPERTY(QString renderer READ renderer WRITE setRenderer NOTIFY changed FINAL)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY changed FINAL)
    Q_PROPERTY(bool isDefault READ isDefault NOTIFY changed FINAL)
    Q_PROPERTY(QString dataFolder READ dataFolder CONSTANT FINAL)

public:
    SettingsEditor(const Settings& settings, const QString& runningRenderer, QObject* parent = nullptr);

    bool doubleCtrl() const { return m_settings.doubleCtrl; }
    void setDoubleCtrl(bool on);
    QString hotkey() const { return m_settings.hotkey; }
    void setHotkey(const QString& hotkey);
    QString hotkeyError() const { return m_hotkeyError; }
    void setHotkeyError(const QString& error);
    bool recordingHotkey() const { return m_recordingHotkey; }
    void setRecordingHotkey(bool recording);
    bool autostart() const;
    void setAutostart(bool on);
    bool includeRemovableDrives() const { return m_settings.includeRemovableDrives; }
    void setIncludeRemovableDrives(bool on);
    bool rescanOnStartup() const { return m_settings.rescanOnStartup; }
    void setRescanOnStartup(bool on);
    QStringList excludedPaths() const;
    QStringList excludedNames() const { return m_settings.excludedNames; }
    QStringList contentExtensions() const { return m_settings.contentExtensions; }
    int maxContentFileSizeMB() const { return m_settings.maxContentFileSizeMB; }
    void setMaxContentFileSizeMB(int mb);
    bool contentIndex() const { return m_settings.contentIndex; }
    void setContentIndex(bool on);
    bool contentInLowPriority() const { return m_settings.contentInLowPriority; }
    void setContentInLowPriority(bool on);
    QString contentIndexStatus() const { return m_contentIndexStatus; }
    void setContentIndexStatus(const QString& status); // "已收录 … 个文件"; set by the app
    QString renderer() const { return m_settings.renderer; }
    void setRenderer(const QString& renderer);
    bool restartRequired() const { return Settings::resolveRenderer(m_settings.renderer) != m_runningRenderer; }
    bool isDefault() const;
    QString dataFolder() const { return Settings::dataDir(); }

    void setSettings(const Settings& settings); // the file was changed outside this editor
    void setWindow(QWindow* window) { m_window = window; }

    Q_INVOKABLE void addExcludedFolder(); // asks for the folder
    Q_INVOKABLE void removeExcludedPath(int index);
    Q_INVOKABLE bool addExcludedName(const QString& name);
    Q_INVOKABLE void removeExcludedName(int index);
    Q_INVOKABLE bool addContentExtensions(const QString& text); // "md, .log *.csv"
    Q_INVOKABLE void removeContentExtension(int index);
    Q_INVOKABLE void restoreDefaults();
    Q_INVOKABLE void openDataFolder();
    Q_INVOKABLE void restart() { emit restartRequested(); } // after changing the renderer

    // 拾穗计划 page
    Q_INVOKABLE void openUrl(const QString& url);
    Q_INVOKABLE void copyText(const QString& text);

signals:
    void changed();
    void hotkeyErrorChanged();
    void contentIndexStatusChanged();
    void recordingHotkeyChanged(); // the app suspends its hotkeys meanwhile
    void autostartChanged();
    void edited(const ws::Settings& settings);
    void restartRequested();

private:
    void commit();

    Settings m_settings;
    QString m_runningRenderer; // resolved: never "auto"
    QString m_hotkeyError;
    QString m_contentIndexStatus;
    bool m_recordingHotkey = false;
    QPointer<QWindow> m_window;
};

} // namespace ws
