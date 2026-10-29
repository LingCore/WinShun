#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <functional>

namespace ws::captureplugin {

// What the screen capture is set to: [Capture] in capture.ini, in the
// plugin's data folder, with the keys WinShun.ini had (Settings::capture*)
// up to 0.16, so moving them over is a copy (MovedPlugins).
struct CaptureOptions {
    QString hotkey; // no default (PixPin and Snipaste have the usual ones): the user records one
    bool printScreen = false; // PrtScn captures too, taken from the Snipping Tool (printscreen::)
    QString printScreenBefore; // what PrtScn did before, put back when no longer taken
    bool copyOnSelect = false;
    bool magnifier = true;
    bool pinOnCopy = false;
    QString mouseDoubleLeft = QStringLiteral("copy");
    QString mouseRight = QStringLiteral("back");
    QString mouseMiddle = QStringLiteral("none");
    QString mouseBack = QStringLiteral("none");
    QString mouseForward = QStringLiteral("none");
    QString toolbarHidden;
    QString annotateStyles;
    bool smartPrescan = true;
    bool smartIpMac = false;
    bool smartAllText = false;
    QString smartWords;
    bool cursor = false;
    bool hdrFile = false;
    bool controlsUia = true;
    int delay = 5;
    QString colorFormat = QStringLiteral("hex");
    QString pinsHotkey;
    QString pinsRestoreHotkey;
    QString lastPinHotkey;
    QString saveFolder;
    QString recordingFolder;
    bool recordingSystemAudio = false;
    bool recordingMicrophone = false;
    int recordingQuality = 1; // 0: smaller file, 1: standard, 2: higher quality
    bool recordingCursor = true;
    bool recordingCountdown = true;
    bool recordingClicks = false;
    int recordingFormat = 0; // 0: MP4, 1: GIF
    QString recordingKeys = QStringLiteral("off");
    bool recordingMouseButtons = false;
    QString pinDoubleClick = QStringLiteral("reset");

    static CaptureOptions load(const QString& file);
    void save(const QString& file) const;
    static QStringList toolbarButtonIds();
    static QString normalizedToolbarHidden(const QString& text);
    static QStringList normalizedSmartWords(const QString& text);
};

// The options for the settings window (CaptureOptions.qml), with the names
// WinShun's settings editor gave them, so the page reads as it did there.
// Each change is saved at once and told (changed); what went wrong applying
// them (a hotkey taken) comes back from CaptureApp.
class CaptureSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the plugin")
    Q_PROPERTY(QString captureHotkey READ captureHotkey WRITE setCaptureHotkey NOTIFY changed FINAL)
    Q_PROPERTY(bool capturePrintScreen READ capturePrintScreen WRITE setCapturePrintScreen NOTIFY changed FINAL)
    Q_PROPERTY(bool captureCopyOnSelect READ captureCopyOnSelect WRITE setCaptureCopyOnSelect NOTIFY changed FINAL)
    Q_PROPERTY(bool captureMagnifier READ captureMagnifier WRITE setCaptureMagnifier NOTIFY changed FINAL)
    Q_PROPERTY(bool capturePinOnCopy READ capturePinOnCopy WRITE setCapturePinOnCopy NOTIFY changed FINAL)
    Q_PROPERTY(QString captureMouseDoubleLeft READ captureMouseDoubleLeft WRITE setCaptureMouseDoubleLeft NOTIFY changed FINAL)
    Q_PROPERTY(QString captureMouseRight READ captureMouseRight WRITE setCaptureMouseRight NOTIFY changed FINAL)
    Q_PROPERTY(QString captureMouseMiddle READ captureMouseMiddle WRITE setCaptureMouseMiddle NOTIFY changed FINAL)
    Q_PROPERTY(QString captureMouseBack READ captureMouseBack WRITE setCaptureMouseBack NOTIFY changed FINAL)
    Q_PROPERTY(QString captureMouseForward READ captureMouseForward WRITE setCaptureMouseForward NOTIFY changed FINAL)
    Q_PROPERTY(QString captureToolbarHidden READ captureToolbarHidden WRITE setCaptureToolbarHidden NOTIFY changed FINAL)
    Q_PROPERTY(bool captureSmartPrescan READ captureSmartPrescan WRITE setCaptureSmartPrescan NOTIFY changed FINAL)
    Q_PROPERTY(bool captureSmartIpMac READ captureSmartIpMac WRITE setCaptureSmartIpMac NOTIFY changed FINAL)
    Q_PROPERTY(bool captureSmartAllText READ captureSmartAllText WRITE setCaptureSmartAllText NOTIFY changed FINAL)
    Q_PROPERTY(QString captureSmartWords READ captureSmartWords WRITE setCaptureSmartWords NOTIFY changed FINAL)
    Q_PROPERTY(bool captureCursor READ captureCursor WRITE setCaptureCursor NOTIFY changed FINAL)
    Q_PROPERTY(bool captureHdrFile READ captureHdrFile WRITE setCaptureHdrFile NOTIFY changed FINAL)
    Q_PROPERTY(bool captureControlsUia READ captureControlsUia WRITE setCaptureControlsUia NOTIFY changed FINAL)
    Q_PROPERTY(int captureDelay READ captureDelay WRITE setCaptureDelay NOTIFY changed FINAL)
    Q_PROPERTY(QString capturePinsHotkey READ capturePinsHotkey WRITE setCapturePinsHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString capturePinsRestoreHotkey READ capturePinsRestoreHotkey WRITE setCapturePinsRestoreHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString captureLastPinHotkey READ captureLastPinHotkey WRITE setCaptureLastPinHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString captureSaveFolder READ captureSaveFolder WRITE setCaptureSaveFolder NOTIFY changed FINAL)
    Q_PROPERTY(QString capturePinDoubleClick READ capturePinDoubleClick WRITE setCapturePinDoubleClick NOTIFY changed FINAL)
    Q_PROPERTY(QString captureHotkeyError READ captureHotkeyError NOTIFY problemsChanged FINAL)
    Q_PROPERTY(QString capturePinsHotkeyError READ capturePinsHotkeyError NOTIFY problemsChanged FINAL)
    Q_PROPERTY(QString capturePinsRestoreHotkeyError READ capturePinsRestoreHotkeyError NOTIFY problemsChanged FINAL)
    Q_PROPERTY(QString captureLastPinHotkeyError READ captureLastPinHotkeyError NOTIFY problemsChanged FINAL)
    Q_PROPERTY(QString captureNote READ captureNote NOTIFY problemsChanged FINAL)
    // A hotkey box records: the plugin's hotkeys are let go of meanwhile.
    Q_PROPERTY(bool recordingHotkey READ recordingHotkey WRITE setRecordingHotkey NOTIFY recordingHotkeyChanged FINAL)

public:
    explicit CaptureSettings(QString file, QObject* parent = nullptr);

    const CaptureOptions& options() const { return m_options; }
    // A change made elsewhere (the capture's styles, the recording panel's choices).
    void update(const std::function<void(CaptureOptions&)>& change);
    void setWindow(QObject* window) { m_window = window; }

    QString captureHotkey() const { return m_options.hotkey; }
    void setCaptureHotkey(const QString& v) { change(m_options.hotkey, v.trimmed()); }
    bool capturePrintScreen() const { return m_options.printScreen; }
    void setCapturePrintScreen(bool v) { change(m_options.printScreen, v); }
    bool captureCopyOnSelect() const { return m_options.copyOnSelect; }
    void setCaptureCopyOnSelect(bool v) { change(m_options.copyOnSelect, v); }
    bool captureMagnifier() const { return m_options.magnifier; }
    void setCaptureMagnifier(bool v) { change(m_options.magnifier, v); }
    bool capturePinOnCopy() const { return m_options.pinOnCopy; }
    void setCapturePinOnCopy(bool v) { change(m_options.pinOnCopy, v); }
    QString captureMouseDoubleLeft() const { return m_options.mouseDoubleLeft; }
    void setCaptureMouseDoubleLeft(const QString& v) { setMouse(m_options.mouseDoubleLeft, v, false); }
    QString captureMouseRight() const { return m_options.mouseRight; }
    void setCaptureMouseRight(const QString& v) { setMouse(m_options.mouseRight, v, true); }
    QString captureMouseMiddle() const { return m_options.mouseMiddle; }
    void setCaptureMouseMiddle(const QString& v) { setMouse(m_options.mouseMiddle, v, false); }
    QString captureMouseBack() const { return m_options.mouseBack; }
    void setCaptureMouseBack(const QString& v) { setMouse(m_options.mouseBack, v, false); }
    QString captureMouseForward() const { return m_options.mouseForward; }
    void setCaptureMouseForward(const QString& v) { setMouse(m_options.mouseForward, v, false); }
    QString captureToolbarHidden() const { return m_options.toolbarHidden; }
    void setCaptureToolbarHidden(const QString& v) { change(m_options.toolbarHidden, CaptureOptions::normalizedToolbarHidden(v)); }
    bool captureSmartPrescan() const { return m_options.smartPrescan; }
    void setCaptureSmartPrescan(bool v) { change(m_options.smartPrescan, v); }
    bool captureSmartIpMac() const { return m_options.smartIpMac; }
    void setCaptureSmartIpMac(bool v) { change(m_options.smartIpMac, v); }
    bool captureSmartAllText() const { return m_options.smartAllText; }
    void setCaptureSmartAllText(bool v) { change(m_options.smartAllText, v); }
    QString captureSmartWords() const { return m_options.smartWords; }
    void setCaptureSmartWords(const QString& v) { change(m_options.smartWords, CaptureOptions::normalizedSmartWords(v).join(u',')); }
    bool captureCursor() const { return m_options.cursor; }
    void setCaptureCursor(bool v) { change(m_options.cursor, v); }
    bool captureHdrFile() const { return m_options.hdrFile; }
    void setCaptureHdrFile(bool v) { change(m_options.hdrFile, v); }
    bool captureControlsUia() const { return m_options.controlsUia; }
    void setCaptureControlsUia(bool v) { change(m_options.controlsUia, v); }
    int captureDelay() const { return m_options.delay; }
    void setCaptureDelay(int v);
    QString capturePinsHotkey() const { return m_options.pinsHotkey; }
    void setCapturePinsHotkey(const QString& v) { change(m_options.pinsHotkey, v.trimmed()); }
    QString capturePinsRestoreHotkey() const { return m_options.pinsRestoreHotkey; }
    void setCapturePinsRestoreHotkey(const QString& v) { change(m_options.pinsRestoreHotkey, v.trimmed()); }
    QString captureLastPinHotkey() const { return m_options.lastPinHotkey; }
    void setCaptureLastPinHotkey(const QString& v) { change(m_options.lastPinHotkey, v.trimmed()); }
    QString captureSaveFolder() const { return m_options.saveFolder; }
    void setCaptureSaveFolder(const QString& v) { change(m_options.saveFolder, v.trimmed()); }
    QString capturePinDoubleClick() const { return m_options.pinDoubleClick; }
    void setCapturePinDoubleClick(const QString& v);

    QString captureHotkeyError() const { return m_hotkeyError; }
    QString capturePinsHotkeyError() const { return m_pinsHotkeyError; }
    QString capturePinsRestoreHotkeyError() const { return m_pinsRestoreHotkeyError; }
    QString captureLastPinHotkeyError() const { return m_lastPinHotkeyError; }
    QString captureNote() const { return m_note; }
    struct Problems {
        QString hotkey, pinsHotkey, pinsRestoreHotkey, lastPinHotkey, note;
    };
    void setProblems(const Problems& problems);

    bool recordingHotkey() const { return m_recordingHotkey; }
    void setRecordingHotkey(bool on);

    Q_INVOKABLE QString pickFolder(const QString& title);

signals:
    void changed();
    void problemsChanged();
    void recordingHotkeyChanged();

private:
    template <typename T> void change(T& field, const T& value)
    {
        if (field == value)
            return;
        field = value;
        commit();
    }
    void setMouse(QString& field, const QString& action, bool allowBack);
    void commit();

    const QString m_file;
    CaptureOptions m_options;
    QString m_hotkeyError, m_pinsHotkeyError, m_pinsRestoreHotkeyError, m_lastPinHotkeyError, m_note;
    bool m_recordingHotkey = false;
    QObject* m_window = nullptr;
};

} // namespace ws::captureplugin
