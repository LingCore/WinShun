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
    void start(const QJsonObject& appearance);
    QQmlEngine& engine() { return m_qml; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void requested(const QJsonValue& id, const QString& method, const QJsonObject& params);
    void notified(const QString& method, const QJsonObject& params);
    void shutdown();

    // ---- what App did ----
    void apply();
    void applyHotkeys();
    void applyAppearance(const QJsonObject& appearance);
    capture::PinManager& pins();
    capture::CaptureEditor* editor();
    capture::CaptureOcr& captureOcr();
    capture::SmartHooks smartHooks();
    void showCaptureOcr(const QImage& image, double dpr, const QRect& where, const QStringList* texts = nullptr);
    void closeCaptureOcr(bool destroy = false);
    enum class OcrThen { Copy, Search, Web };
    void captureOcrThen(const QImage& image, double dpr, const QRect& where, OcrThen then);
    void quickOcrDone();
    void notifySaved(const QString& path, const QString& error);
    void pinPicture(const QString& path, bool screenshot);
    void editPicture(qint64 clipId, const QString& file, const QString& projectFile);
    void editPin(capture::PinWindow* pin);
    void showSettings();
    QJsonArray trayMenu() const;
    void menuChosen(const QString& id);
    void prepareWindow(QQuickWindow* window, bool backdrop);

    // ---- Win顺's side ----
    // Into the clipboard history (copy: and onto the clipboard), as a
    // screenshot when it is one; with its annotations to edit again.
    void copyImage(const QImage& image, const QByteArray& project, bool copy, bool screenshot);
    void copyText(const QString& text);
    void notify(const QString& title, const QString& text, const QString& folder = {});
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
    std::unique_ptr<WindowFrame> m_editFrame;
    QPointer<QQuickWindow> m_editWindow;
    std::unique_ptr<capture::CaptureOcr> m_captureOcr;
    std::unique_ptr<capture::CaptureOcr> m_quickOcr;
    OcrThen m_quickOcrThen = OcrThen::Copy;
    std::unique_ptr<WindowFrame> m_ocrFrame;
    QPointer<QQuickWindow> m_ocrWindow;
    bool m_resumeOcr = false; // the original picture waits while the settings window installs OCR
    std::unique_ptr<WindowFrame> m_settingsFrame;
    QPointer<QQuickWindow> m_settingsWindow;
    std::unique_ptr<RawClicks> m_clicks; // while a recording shows the clicks
    QStringList m_reportedHotkeys; // what Win顺 was told it holds (applyHotkeys)
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
};

} // namespace ws::captureplugin
