#pragma once

#include "CaptureSettings.h"

#include <QImage>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QTimer>

#include <windows.h>

#include <map>
#include <memory>
#include <thread>

class QQuickWindow;

namespace ws {
class IdleTrim;
class PresenterHost;
class WindowFrame;
namespace capture {
class Capture;
class CaptureEditor;
class CaptureOcr;
class PinManager;
class PinWindow;
struct SmartHooks;
namespace recording { class ClickStream; }
} // namespace capture
} // namespace ws

namespace ws::captureplugin {

class Protocol;
class RawClicks;

// The screen capture as a plugin of Win顺's (GitHub #138): what App did for
// it up to 0.16 (App::applyCapture and the rest), in a process of its own.
// Its hotkeys and PrtScn are its own; the clipboard history, the search box
// and the web search are Win顺's, asked for through the protocol's tools.
class CaptureApp : public QObject {
    Q_OBJECT
public:
    CaptureApp(Protocol& protocol, QString appDir, QString dataDir, QObject* parent = nullptr);
    ~CaptureApp() override;

    // Once initialize is answered: the settings applied, the hotkeys registered.
    void start(const QJsonObject& appearance, bool hostHotkeys = false);
    QQmlEngine& engine() { return m_qml; }

    // 编辑 on 识别屏幕文字's card (OcrBubble.qml): the words read, in the OCR
    // window, not read again.
    Q_INVOKABLE void editOcrText(const QString& text);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void requested(const QJsonValue& id, const QString& method, const QJsonObject& params);
    void notified(const QString& method, const QJsonObject& params);
    void shutdown();

    // ---- what App did ----
    void apply();
    void applyHotkeys();
    void reportHotkeyProblems(const CaptureSettings::Problems& problems);
    void applyAppearance(const QJsonObject& appearance);
    capture::PinManager& pins();
    capture::CaptureEditor* editor();
    PresenterHost& presenter();
    bool presenterOpen() const;
    void togglePresenter();
    capture::CaptureOcr& captureOcr();
    capture::SmartHooks smartHooks();
    void showCaptureOcr(const QImage& image, double dpr, const QRect& where, const QStringList* texts = nullptr);
    void closeCaptureOcr(bool destroy = false);
    // Bubble: 识别屏幕文字's, copied and said in the card by the region.
    enum class OcrThen { Copy, Search, Web, Bubble };
    void captureOcrThen(const QImage& image, double dpr, const QRect& where, OcrThen then);
    void quickOcrDone();
    // 识别屏幕文字 (#198): the hotkey, the search box, the tray menu.
    void startOcr();
    void readScreenText(const QImage& image, double dpr, const QRect& where);
    void showOcrBubble(const QString& phase, const QString& words, const QString& detail);
    void closeOcrBubble();
    // `window` (logical size) beside `where` (physical, virtual desktop) on its monitor.
    void placeNear(QQuickWindow* window, const QRect& where);
    void notifySaved(const QString& path, const QString& error);
    void pinPicture(const QString& path, bool screenshot);
    void pinCurrentImage(); // clipboard picture first, otherwise the editable last capture
    void editPicture(qint64 clipId, const QString& file, const QString& projectFile);
    void editPin(capture::PinWindow* pin);
    void showSettings();
    QJsonArray trayMenu() const;
    void menuChosen(const QString& id);
    void prepareWindow(QQuickWindow* window, bool backdrop);

    // ---- Win顺's side ----
    // Into the clipboard history (copy: and onto the clipboard), as a
    // screenshot when it is one; with its annotations to edit again.
    void copyImage(const QImage& image, const QByteArray& project, bool copy, bool screenshot, const QString& imageOrigin = {});
    void copyText(const QString& text);
    // A click: `file` selected in its folder (reveal), else `folder` opened;
    // an older host, without reveal, opens `folder` (#194).
    void notify(const QString& title, const QString& text, const QString& folder = {}, const QString& file = {});
    void searchText(const QString& text, bool web);

    // ---- hotkeys, on a window of its own ----
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l);
    void hotkey(int id);

    Protocol& m_protocol;
    const QString m_appDir;
    const QString m_dataDir;
    QFileSystemWatcher m_ocrWatcher;
    CaptureSettings m_settings;
    QQmlEngine m_qml;
    HWND m_window = nullptr;
    std::unique_ptr<IdleTrim> m_idleTrim;
    std::unique_ptr<capture::Capture> m_capture;
    std::unique_ptr<capture::PinManager> m_pins;
    std::unique_ptr<capture::CaptureEditor> m_editor;
    std::unique_ptr<PresenterHost> m_presenter;
    std::unique_ptr<WindowFrame> m_editFrame;
    QPointer<QQuickWindow> m_editWindow;
    std::unique_ptr<capture::CaptureOcr> m_captureOcr;
    std::unique_ptr<capture::CaptureOcr> m_quickOcr;
    OcrThen m_quickOcrThen = OcrThen::Copy;
    QRect m_quickOcrWhere; // the region read, physical: the card goes by it
    std::unique_ptr<WindowFrame> m_bubbleFrame;
    QPointer<QQuickWindow> m_bubbleWindow;
    std::unique_ptr<WindowFrame> m_ocrFrame;
    QPointer<QQuickWindow> m_ocrWindow;
    bool m_resumeOcr = false; // the original picture waits while the settings window installs OCR
    std::unique_ptr<WindowFrame> m_settingsFrame;
    QPointer<QQuickWindow> m_settingsWindow;
    std::unique_ptr<RawClicks> m_clicks; // while a recording shows the clicks
    QStringList m_reportedHotkeys; // what Win顺 was told it holds (applyHotkeys)
    bool m_hostHotkeys = false;
    quint64 m_hotkeyRevision = 0;
    std::jthread m_pinSaver;
    // The history pictures being edited: what Win顺 handed over for them.
    struct HistoryPicture {
        QString file;
        QByteArray project;
    };
    std::map<qint64, HistoryPicture> m_history;
    quint64 m_nextOut = 1;
    QTimer m_applyLater;
    bool m_started = false;
    bool m_pinPending = false;
};

} // namespace ws::captureplugin
