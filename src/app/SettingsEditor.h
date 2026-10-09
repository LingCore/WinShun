#pragma once

#include "Settings.h"

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace ws {

class ShortcutCapture;

// The view-model behind the settings window. Every change is saved right
// away and reported through edited(); there is no OK / Cancel step.
class SettingsEditor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool doubleCtrl READ doubleCtrl WRITE setDoubleCtrl NOTIFY changed FINAL)
    Q_PROPERTY(bool doubleCtrlPauseInGames READ doubleCtrlPauseInGames WRITE setDoubleCtrlPauseInGames NOTIFY changed FINAL)
    Q_PROPERTY(bool doubleCtrlPauseInFullScreen READ doubleCtrlPauseInFullScreen WRITE setDoubleCtrlPauseInFullScreen
            NOTIFY changed FINAL)
    Q_PROPERTY(QStringList doubleCtrlExcludedApps READ doubleCtrlExcludedApps NOTIFY changed FINAL)
    Q_PROPERTY(QString hotkey READ hotkey WRITE setHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString hotkeyError READ hotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(bool recordingHotkey READ recordingHotkey WRITE setRecordingHotkey NOTIFY recordingHotkeyChanged FINAL)
    Q_PROPERTY(bool autostart READ autostart WRITE setAutostart NOTIFY autostartChanged FINAL)
    Q_PROPERTY(bool recordHistory READ recordHistory WRITE setRecordHistory NOTIFY changed FINAL)
    Q_PROPERTY(int historyCount READ historyCount NOTIFY historyCountChanged FINAL)
    Q_PROPERTY(bool dialogJump READ dialogJump WRITE setDialogJump NOTIFY changed FINAL)
    Q_PROPERTY(bool dialogBar READ dialogBar WRITE setDialogBar NOTIFY changed FINAL)
    Q_PROPERTY(bool dialogAutoJump READ dialogAutoJump WRITE setDialogAutoJump NOTIFY changed FINAL)
    Q_PROPERTY(QStringList dialogBarExcludedApps READ dialogBarExcludedApps NOTIFY changed FINAL)
    Q_PROPERTY(bool includeRemovableDrives READ includeRemovableDrives WRITE setIncludeRemovableDrives NOTIFY changed FINAL)
    Q_PROPERTY(bool rescanOnStartup READ rescanOnStartup WRITE setRescanOnStartup NOTIFY changed FINAL)
    Q_PROPERTY(QStringList excludedPaths READ excludedPaths NOTIFY changed FINAL) // expanded, for display
    Q_PROPERTY(QStringList excludedNames READ excludedNames NOTIFY changed FINAL)
    Q_PROPERTY(QStringList contentExtensions READ contentExtensions NOTIFY changed FINAL)
    // Size limits in MB by kind of file (ContentSizeLimits::Kind: text, code, data),
    // and the listed extensions of each kind (".c .cpp .h").
    Q_PROPERTY(QList<int> contentMaxSizeMB READ contentMaxSizeMB NOTIFY changed FINAL)
    Q_PROPERTY(QStringList contentKindExtensions READ contentKindExtensions NOTIFY changed FINAL)
    Q_PROPERTY(bool contentIndex READ contentIndex WRITE setContentIndex NOTIFY changed FINAL)
    Q_PROPERTY(bool contentInLowPriority READ contentInLowPriority WRITE setContentInLowPriority NOTIFY changed FINAL)
    Q_PROPERTY(bool contentDocuments READ contentDocuments WRITE setContentDocuments NOTIFY changed FINAL)
    Q_PROPERTY(QString contentIndexStatus READ contentIndexStatus NOTIFY contentIndexStatusChanged FINAL)
    // 剪贴板
    Q_PROPERTY(bool clipboard READ clipboard WRITE setClipboard NOTIFY changed FINAL)
    Q_PROPERTY(bool clipboardWinV READ clipboardWinV WRITE setClipboardWinV NOTIFY changed FINAL)
    // Where taking Win+V over stands (set by the app): off | on | waiting
    // (Explorer still holds it: restart Explorer) | releasing (Explorer gets
    // it back once it restarts) | failed (the registry could not be written).
    Q_PROPERTY(QString winVState READ winVState NOTIFY winVStateChanged FINAL)
    Q_PROPERTY(bool canRestartExplorer READ canRestartExplorer NOTIFY winVStateChanged FINAL)
    Q_PROPERTY(QString clipboardHotkey READ clipboardHotkey WRITE setClipboardHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString clipboardHotkeyError READ clipboardHotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(int clipboardMaxItems READ clipboardMaxItems WRITE setClipboardMaxItems NOTIFY changed FINAL)
    Q_PROPERTY(int clipboardMaxDays READ clipboardMaxDays WRITE setClipboardMaxDays NOTIFY changed FINAL)
    Q_PROPERTY(bool clipboardImages READ clipboardImages WRITE setClipboardImages NOTIFY changed FINAL)
    Q_PROPERTY(QStringList clipboardExcludedApps READ clipboardExcludedApps NOTIFY changed FINAL)
    Q_PROPERTY(int clipboardCount READ clipboardCount NOTIFY clipboardCountChanged FINAL)
    // 网页搜索: for each, {keyword, name, url, home, searches, shownUrl, shownHome}
    Q_PROPERTY(QVariantList webShortcuts READ webShortcuts NOTIFY changed FINAL)
    Q_PROPERTY(QString renderer READ renderer WRITE setRenderer NOTIFY changed FINAL)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY changed FINAL)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY changed FINAL)
    Q_PROPERTY(QString transparency READ transparency WRITE setTransparency NOTIFY changed FINAL)
    Q_PROPERTY(bool autoUpdate READ autoUpdate WRITE setAutoUpdate NOTIFY changed FINAL)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY changed FINAL)
    Q_PROPERTY(bool isDefault READ isDefault NOTIFY changed FINAL)
    Q_PROPERTY(QString dataFolder READ dataFolder CONSTANT FINAL)
    // 索引位置 (set by the app): where the index files are, and how moving them goes
    Q_PROPERTY(QString indexFolder READ indexFolder NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(bool indexFolderIsDefault READ indexFolderIsDefault NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(QString indexSize READ indexSize NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(int indexMoveProgress READ indexMoveProgress NOTIFY indexFolderChanged FINAL) // -1: not moving
    Q_PROPERTY(QString indexFolderProblem READ indexFolderProblem NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(QString indexFolderNote READ indexFolderNote NOTIFY indexFolderChanged FINAL)

public:
    SettingsEditor(const Settings& settings, const QString& runningRenderer, QObject* parent = nullptr);
    ~SettingsEditor() override;

    bool doubleCtrl() const { return m_settings.doubleCtrl; }
    void setDoubleCtrl(bool on);
    bool doubleCtrlPauseInGames() const { return m_settings.doubleCtrlPauseInGames; }
    void setDoubleCtrlPauseInGames(bool on);
    bool doubleCtrlPauseInFullScreen() const { return m_settings.doubleCtrlPauseInFullScreen; }
    void setDoubleCtrlPauseInFullScreen(bool on);
    QStringList doubleCtrlExcludedApps() const { return m_settings.doubleCtrlExcludedApps; }
    QString hotkey() const { return m_settings.hotkey; }
    void setHotkey(const QString& hotkey);
    QString hotkeyError() const { return m_hotkeyError; }
    void setHotkeyError(const QString& error);
    bool recordingHotkey() const { return m_recordingHotkey; }
    void setRecordingHotkey(bool recording);
    bool autostart() const;
    void setAutostart(bool on);
    bool recordHistory() const { return m_settings.recordHistory; }
    void setRecordHistory(bool on);
    int historyCount() const { return m_historyCount; }
    void setHistoryCount(int count); // set by the app
    bool dialogJump() const { return m_settings.dialogJump; }
    void setDialogJump(bool on);
    bool dialogBar() const { return m_settings.dialogBar; }
    void setDialogBar(bool on);
    bool dialogAutoJump() const { return m_settings.dialogAutoJump; }
    void setDialogAutoJump(bool on);
    QStringList dialogBarExcludedApps() const { return m_settings.dialogBarExcludedApps; }
    bool includeRemovableDrives() const { return m_settings.includeRemovableDrives; }
    void setIncludeRemovableDrives(bool on);
    bool rescanOnStartup() const { return m_settings.rescanOnStartup; }
    void setRescanOnStartup(bool on);
    QStringList excludedPaths() const;
    QStringList excludedNames() const { return m_settings.excludedNames; }
    QStringList contentExtensions() const { return m_settings.contentExtensions; }
    QList<int> contentMaxSizeMB() const { return {m_settings.contentMaxSizeMB.begin(), m_settings.contentMaxSizeMB.end()}; }
    Q_INVOKABLE void setContentMaxSizeMB(int kind, int mb);
    QStringList contentKindExtensions() const;
    bool contentIndex() const { return m_settings.contentIndex; }
    void setContentIndex(bool on);
    bool contentInLowPriority() const { return m_settings.contentInLowPriority; }
    void setContentInLowPriority(bool on);
    bool contentDocuments() const { return m_settings.contentDocuments; }
    void setContentDocuments(bool on);
    QString contentIndexStatus() const { return m_contentIndexStatus; }
    void setContentIndexStatus(const QString& status); // "已收录 … 个文件"; set by the app
    QString renderer() const { return m_settings.renderer; }
    void setRenderer(const QString& renderer);
    QString theme() const { return m_settings.theme; }
    void setTheme(const QString& theme);
    QString language() const { return m_settings.language; }
    void setLanguage(const QString& language);
    QString transparency() const { return m_settings.transparency; }
    void setTransparency(const QString& transparency);
    bool autoUpdate() const { return m_settings.autoUpdate; }
    void setAutoUpdate(bool on);
    bool restartRequired() const { return Settings::resolveRenderer(m_settings.renderer) != m_runningRenderer; }
    bool isDefault() const;
    QString dataFolder() const { return Settings::dataDir(); }

    // 索引位置, as the app has it.
    struct IndexFolderState {
        QString folder; // where the index files are, with native separators
        bool isDefault = true; // that is Settings::dataDir()
        QString size; // of the files there, "420 MB"
        int moveProgress = -1; // percent, while they are being moved
        QString problem; // why they could not be moved, or are not where the settings say
        QString note; // worth knowing, nothing wrong: they are on a hard disk
        bool operator==(const IndexFolderState&) const = default;
    };
    void setIndexFolderState(const IndexFolderState& state);
    QString indexFolder() const { return m_indexFolderState.folder; }
    bool indexFolderIsDefault() const { return m_indexFolderState.isDefault; }
    QString indexSize() const { return m_indexFolderState.size; }
    int indexMoveProgress() const { return m_indexFolderState.moveProgress; }
    QString indexFolderProblem() const { return m_indexFolderState.problem; }
    QString indexFolderNote() const { return m_indexFolderState.note; }

    bool clipboard() const { return m_settings.clipboard; }
    void setClipboard(bool on);
    bool clipboardWinV() const { return m_settings.clipboardWinV; }
    void setClipboardWinV(bool on);
    QString winVState() const { return m_winVState; }
    void setWinVState(const QString& state, bool canRestartExplorer); // set by the app
    bool canRestartExplorer() const { return m_canRestartExplorer; }
    QString clipboardHotkey() const { return m_settings.clipboardHotkey; }
    void setClipboardHotkey(const QString& hotkey);
    QString clipboardHotkeyError() const { return m_clipboardHotkeyError; }
    void setClipboardHotkeyError(const QString& error);
    int clipboardMaxItems() const { return m_settings.clipboardMaxItems; }
    void setClipboardMaxItems(int items);
    int clipboardMaxDays() const { return m_settings.clipboardMaxDays; }
    void setClipboardMaxDays(int days);
    bool clipboardImages() const { return m_settings.clipboardImages; }
    void setClipboardImages(bool on);
    QStringList clipboardExcludedApps() const { return m_settings.clipboardExcludedApps; }
    int clipboardCount() const { return m_clipboardCount; }
    void setClipboardCount(int count); // set by the app

    QVariantList webShortcuts() const;

    void setSettings(const Settings& settings); // the file was changed outside this editor
    void setWindow(QWindow* window) { m_window = window; }

    Q_INVOKABLE void addExcludedFolder(); // asks for the folder
    Q_INVOKABLE void removeExcludedPath(int index);
    Q_INVOKABLE bool addExcludedName(const QString& name);
    Q_INVOKABLE void removeExcludedName(int index);
    Q_INVOKABLE void removeDialogBarExcludedApp(int index);
    Q_INVOKABLE bool addContentExtensions(const QString& text); // "md, .log *.csv"
    Q_INVOKABLE void removeContentExtension(int index);
    Q_INVOKABLE bool addClipboardExcludedApp(const QString& name); // "KeePass", "keepass.exe"
    Q_INVOKABLE void removeClipboardExcludedApp(int index);
    Q_INVOKABLE bool addDoubleCtrlExcludedApp(const QString& name); // "TheFinals", "thefinals.exe"
    Q_INVOKABLE void removeDoubleCtrlExcludedApp(int index);
    // Adds a web shortcut (`index` -1) or changes the one at `index`. Returns
    // why it cannot be saved, or an empty string once it is.
    Q_INVOKABLE QString saveWebShortcut(
        int index, const QString& keyword, const QString& name, const QString& url, const QString& home);
    Q_INVOKABLE void removeWebShortcut(int index);
    Q_INVOKABLE void openWebShortcut(int index); // its site, to try it
    Q_INVOKABLE void clearClipboard() { emit clipboardClearRequested(); } // what is in no group
    Q_INVOKABLE void restartExplorer() { emit explorerRestartRequested(); }
    Q_INVOKABLE void restoreDefaults();
    Q_INVOKABLE void clearHistory() { emit historyClearRequested(); }
    Q_INVOKABLE void openDataFolder();
    // The app moves the index files once the setting changes (App::applyIndexFolder).
    Q_INVOKABLE void chooseIndexFolder(); // asks for the folder
    Q_INVOKABLE void resetIndexFolder() { setIndexFolder({}); } // back to the default place
    Q_INVOKABLE void openIndexFolder();
    Q_INVOKABLE void restart() { emit restartRequested(); } // after changing the renderer

    // 拾穗计划 page
    Q_INVOKABLE void openUrl(const QString& url);
    Q_INVOKABLE void copyText(const QString& text);

signals:
    void historyCountChanged();
    void historyClearRequested();
    void changed();
    void hotkeyErrorChanged();
    void contentIndexStatusChanged();
    void indexFolderChanged();
    void recordingHotkeyChanged(); // the app suspends its hotkeys meanwhile
    void autostartChanged();
    void winVStateChanged();
    void clipboardCountChanged();
    void clipboardClearRequested();
    void explorerRestartRequested(); // Win+V changes hands when Explorer starts again
    void edited(const ws::Settings& settings);
    void restartRequested();

private:
    void commit();
    void setIndexFolder(const QString& folder);

    Settings m_settings;
    QString m_runningRenderer; // resolved: never "auto"
    QString m_hotkeyError;
    int m_historyCount = 0;
    QString m_contentIndexStatus;
    IndexFolderState m_indexFolderState;
    bool m_recordingHotkey = false;
    QString m_winVState = QStringLiteral("off");
    bool m_canRestartExplorer = false;
    QString m_clipboardHotkeyError;
    int m_clipboardCount = 0;
    QPointer<QWindow> m_window;
    std::unique_ptr<ShortcutCapture> m_capture; // while recording a hotkey
};

} // namespace ws
