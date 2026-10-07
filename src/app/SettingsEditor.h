#pragma once

#include "Settings.h"

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

namespace qf {

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
    Q_PROPERTY(QString renderer READ renderer WRITE setRenderer NOTIFY changed FINAL)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY changed FINAL)
    Q_PROPERTY(bool isDefault READ isDefault NOTIFY changed FINAL)

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
    QString renderer() const { return m_settings.renderer; }
    void setRenderer(const QString& renderer);
    bool restartRequired() const { return Settings::resolveRenderer(m_settings.renderer) != m_runningRenderer; }
    bool isDefault() const;

    void setSettings(const Settings& settings); // the file was changed outside this editor
    void setWindow(QWindow* window) { m_window = window; }

    Q_INVOKABLE void addExcludedFolder(); // asks for the folder
    Q_INVOKABLE void removeExcludedPath(int index);
    Q_INVOKABLE bool addExcludedName(const QString& name);
    Q_INVOKABLE void removeExcludedName(int index);
    Q_INVOKABLE bool addContentExtensions(const QString& text); // "md, .log *.csv"
    Q_INVOKABLE void removeContentExtension(int index);
    Q_INVOKABLE void restoreDefaults();

    // 拾穗计划 page
    Q_INVOKABLE void openUrl(const QString& url);
    Q_INVOKABLE void copyText(const QString& text);
    Q_INVOKABLE void setTitleBarColors(const QColor& caption, const QColor& text);
    Q_INVOKABLE void resetTitleBarColors();

signals:
    void changed();
    void hotkeyErrorChanged();
    void recordingHotkeyChanged(); // the app suspends its hotkeys meanwhile
    void autostartChanged();
    void edited(const qf::Settings& settings);

private:
    void commit();

    Settings m_settings;
    QString m_runningRenderer; // resolved: never "auto"
    QString m_hotkeyError;
    bool m_recordingHotkey = false;
    QPointer<QWindow> m_window;
};

} // namespace qf
