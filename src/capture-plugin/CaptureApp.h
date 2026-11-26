#pragma once

#include "CaptureSettings.h"
#include "PinHistory.h"
#include "PrintScreen.h"
#include "SmartRedact.h"
#include "platform/HotPages.h"
#include "platform/UiStall.h"

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
class RegistryWatch;
class SavedWindowSize;
class WindowFrame;
namespace capture {
class Capture;
class CaptureEditor;
class CaptureFormula;
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
    void watchStalls(); // a capture begins: its stalls written down with what they were in
    void printScreenChanged(); // the keyboard settings were written: PrtScn taken again if it went back (#322)
    void reportHotkeyProblems(const CaptureSettings::Problems& problems);
    void applyAppearance(const QJsonObject& appearance);
    // The Theme made, the first time C++ draws by its look (the pins, the
    // countdown, the tray menu): it writes SystemTheme::nativeLook.
    void ensureTheme();
    capture::PinManager& pins();
    capture::CaptureEditor* editor();
    PresenterHost& presenter();
    bool presenterOpen() const;
    void togglePresenter();
    capture::CaptureOcr& captureOcr();
    capture::SmartHooks smartHooks();
    void showCaptureOcr(const QImage& image, double dpr, const QRect& where, const QStringList* texts = nullptr, bool translate = false);
    void closeCaptureOcr(bool destroy = false);
    // 公式识别 (CaptureFormula, FormulaPanel.qml): as the OCR window, one window beside the picture.
    capture::CaptureFormula& captureFormula();
    void showFormula(const QImage& image, const QRect& where);
    void closeFormula(bool destroy = false);
    // 截图翻译 (GitHub #191): a pin where the picture was taken, its words
    // translated in place once the translation is back (TranslateOverlay).
    // Whatever stands in the way (no reader, no service, an answer that does
    // not match the blocks) goes to the OCR window instead, which says so.
    void translateInPlace(const QImage& image, double dpr, const QRect& where);
    // The same on a pin that is there (its menu's 翻译). `own`: translateInPlace
    // made the pin for this, and takes it away when the OCR window opens instead.
    void translatePin(capture::PinWindow* pin, bool own);
    // Bubble: 识别屏幕文字's, copied and said in the card by the region.
    enum class OcrThen { Copy, Search, Web, Bubble };
    void captureOcrThen(const QImage& image, double dpr, const QRect& where, OcrThen then);
    void quickOcrDone();
    // 识别屏幕文字 (#198): the hotkey, the search box, the tray menu; 截图翻译
    // (#356) with `translate`.
    void startOcr(bool translate = false);
    // 截图并直接贴图 (#450): the hotkey, the search box, the tray menu.
    void startPinCapture();
    void readScreenText(const QImage& image, double dpr, const QRect& where);
    void showOcrBubble(const QString& phase, const QString& words, const QString& detail);
    bool closeOcrBubble(); // whether it was up
    // `window` (logical size) beside `where` (physical, virtual desktop) on its monitor.
    void placeNear(QQuickWindow* window, const QRect& where);
    void notifySaved(const QString& path, const QString& error, bool toSource = false);
    void saveToSource(capture::PinWindow* pin, const QImage& image, const QString& path);
    // `slot`: its place among several pinned at once (pinimage::cascaded).
    void pinPicture(const QString& path, bool screenshot, int slot = 0, bool sourceFile = false);
    void pinInTurn(quint64 ticket, QImage image, QPoint origin, QPoint anchor, bool screenshot, QString sourcePath);
    void pinCurrentImage(); // clipboard picture first, then copied words, otherwise recall PinHistory (#446)
    // Copied words as a card on the screen (GitHub #388), at the cursor; Ctrl+C on it copies them.
    capture::PinWindow* pinText(const QString& text);
    // PinHistory (#446): record screenshots/pins, update on edit/close, recall on repeated hotkey.
    quint64 recordPinHistory(capture::PinHistoryEntry entry, capture::PinWindow* pin = nullptr);
    void encodeHistoryAsync(quint64 id, quint64 generation, QImage image);
    void onPinClosing(capture::PinWindow* pin);
    void restorePinHistoryEntry(const capture::PinHistoryEntry& entry);
    void pinNextFromHistory(bool clipboardEmpty);
    void loadNextDiskHistory(bool clipboardEmpty);
    void editPicture(qint64 clipId, const QString& file, const QString& projectFile);
    void editFile(const QString& path);
    void editPin(capture::PinWindow* pin);
    void showSettings();
    void persistPinGroups();
    capture::PinGroupStore effectivePinGroups() const;
    void cyclePinGroup();
    void promptNewPinGroup(capture::PinWindow* moveTarget = nullptr);
    void promptRenamePinGroup(const QString& groupId);
    void ensureGroupPrompt();
    QJsonArray trayMenu() const;
    void menuChosen(const QString& id);
    void prepareWindow(QQuickWindow* window, bool backdrop);

private slots:
    void onGroupPromptSubmitted(const QString& name);
    void onGroupPromptCancelled();

private:

    // ---- Win顺's side ----
    // Into the clipboard history (copy: and onto the clipboard), as a
    // screenshot when it is one; with its annotations to edit again.
    // `read`: the picture's lines of text if they have been read already (Capture::Callbacks::Read).
    void copyImage(const QImage& image, const QByteArray& project, bool copy, bool screenshot, const QString& imageOrigin = {},
        const std::optional<QList<capture::RedactLine>>& read = {});
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
    // The pages a capture needs, read back ahead at the next one (HotPages).
    std::shared_ptr<HotPages> m_hotPages = std::make_shared<HotPages>();
    bool m_recordPages = false; // this capture is the first since the working set was emptied
    // While a capture is up: what this thread and the others were in when it stopped answering.
    std::unique_ptr<UiStallWatch> m_stallWatch;
    std::unique_ptr<capture::Capture> m_capture;
    std::unique_ptr<capture::PinManager> m_pins;
    std::unique_ptr<capture::CaptureEditor> m_editor;
    std::unique_ptr<PresenterHost> m_presenter;
    std::unique_ptr<WindowFrame> m_editFrame;
    QPointer<QQuickWindow> m_editWindow;
    std::unique_ptr<capture::CaptureOcr> m_captureOcr;
    std::unique_ptr<capture::CaptureOcr> m_quickOcr;
    OcrThen m_quickOcrThen = OcrThen::Copy;
    std::unique_ptr<capture::CaptureOcr> m_translateOcr; // translateInPlace's reader, while it reads
    int m_translateTicket = 0; // the latest translateInPlace: an older one's answers are dropped
    QRect m_quickOcrWhere; // the region read, physical: the card goes by it
    std::unique_ptr<WindowFrame> m_bubbleFrame;
    QPointer<QQuickWindow> m_bubbleWindow;
    std::unique_ptr<WindowFrame> m_ocrFrame;
    QPointer<QQuickWindow> m_ocrWindow;
    bool m_resumeOcr = false; // the original picture waits while the settings window installs OCR
    std::unique_ptr<capture::CaptureFormula> m_captureFormula;
    std::unique_ptr<WindowFrame> m_formulaFrame;
    QPointer<QQuickWindow> m_formulaWindow;
    std::unique_ptr<WindowFrame> m_settingsFrame;
    QPointer<QQuickWindow> m_settingsWindow;
    QPointer<SavedWindowSize> m_settingsSize; // owned by m_settingsWindow (state.ini [SettingsWindow])
    std::unique_ptr<WindowFrame> m_groupPromptFrame;
    QPointer<QQuickWindow> m_groupPromptWindow;
    QString m_groupPromptRenameId;
    capture::PinWindow* m_groupPromptMovePin = nullptr;
    bool m_persistingPinGroups = false;
    std::unique_ptr<RawClicks> m_clicks; // while a recording shows the clicks
    QStringList m_reportedHotkeys; // what Win顺 was told it holds (applyHotkeys)
    bool m_hostHotkeys = false;
    quint64 m_hotkeyRevision = 0;
    std::unique_ptr<RegistryWatch> m_printScreenWatch; // while PrtScn is wanted; gone before the value is put back
    printscreen::RetakeLimit m_retakes;
    bool m_retakesStopped = false; // another program keeps putting the value back: until the next apply()
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
    // Picture files read side by side, pinned in the order asked for (pinInTurn).
    struct ReadPin {
        QImage image; // null: it could not be read
        QPoint origin;
        QPoint anchor;
        bool screenshot = false;
        QString sourcePath;
    };
    std::map<quint64, ReadPin> m_pinsRead;
    quint64 m_pinTicket = 0; // the next one asked for
    quint64 m_pinTurn = 0; // the next one to go on the screen
    capture::PinHistory m_pinHistory;
    quint32 m_lastPinnedClipboardSeq = 0;
    bool m_historyLoading = false;
    QStringList m_diskHistoryTried;
    std::jthread m_historyEncoder;
    QString m_uiFont; // the family of Win顺's words (its look's resolvedUiFont): copied words pinned
    bool m_themeMade = false; // ensureTheme
};

} // namespace ws::captureplugin
