#pragma once

#include "AnnotationFormat.h"
#include "CaptureDiagnostics.h"
#include "CaptureSession.h"
#include "CaptureTools.h"
#include "CountdownWindow.h"
#include "Grabber.h"
#include "LongCapture.h"
#include "PinManager.h"
#include "Recorder.h"
#include "UiaProbe.h"
#include "WindowSnapshot.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QColor>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>

class QQmlEngine;
class QQuickWindow;
class QScreen;

namespace ws::capture {

// A picture written as PNG off the GUI thread (`thread` joined first: one at
// a time), into `path`, or with none into the Screenshots folder named for
// now (WinShun_yyyyMMdd_HHmmss.png); `done(path, error)` back on `context`'s
// thread. The capture's Ctrl+S and the pins' 保存 / 另存为… alike. With
// `hdr` (the picture's pixels as an HDR monitor showed them), a JPEG XR
// beside it, of the same name (GitHub #133).
void saveImage(std::jthread& thread, QObject* context, const QImage& image, const QString& path,
    std::function<void(const QString& path, const QString& error)> done, const QString& folder = {},
    std::shared_ptr<const HdrPixels> hdr = {});

// The screen capture (docs/design-capture.md; docs/architecture.md, 截图):
// exists while it is turned on in the settings, and holds everything it
// needs warm meanwhile, so a hotkey shows the frozen screens in a few
// frames: the grabber's D3D device and capture items, and one overlay
// window per monitor (CaptureOverlay.qml), made and drawn once hidden. A
// capture freezes every monitor, lists the windows, shows the overlays and
// hands the picture taken to the app; then the pictures, textures and the
// window list are let go of at once (the windows stay, hidden). Destroying
// it gives back the rest.
class Capture : public QObject {
    Q_OBJECT

public:
    struct Callbacks {
        // Enter, a double click, the toolbar. `project`: the annotated
        // picture's editable form (empty: it has no annotations).
        std::function<void(const QImage&, const QByteArray& project)> copy;
        // Ctrl+S and F3 also put the picture in the screenshot history,
        // leaving the system clipboard alone.
        std::function<void(const QImage&, const QByteArray& project)> record;
        // What the annotation tools remember ([Capture] AnnotateStyles, JSON):
        // read when a capture starts, written when one ends with a change.
        std::function<QString()> annotateStyles;
        std::function<void(const QString&)> annotateStylesChanged;
        std::function<void(const QString&)> copyColor; // C: "#RRGGBB"
        std::function<QString()> saveFolder;
        std::function<void(const QString& path, const QString& error)> saved; // Ctrl+S
        std::function<void()> ended; // the overlays are gone and the memory let go of
        // 识别文字: the picture, the scale of the monitor it was on, where it
        // is on the screen (physical; the OCR window goes beside it).
        std::function<void(const QImage&, double dpr, const QRect& where)> ocr;
        // 识别文字并复制 / 搜索 / 网页搜索 (the mouse buttons' choices): read, then go
        // on at once with `then` (OcrCopy, OcrSearch or OcrWeb) without the window.
        std::function<void(const QImage&, double dpr, const QRect& where, CaptureSession::Action then)> ocrThen;
        // How the annotation reads the selection's text and what it hides
        // (SmartRedact.h): 智能打码 and its pre-scan.
        Recorder::Hooks recording;
        SmartHooks smart;
        // The pins (the app's: they outlive the capture's pictures, and a
        // file can be pinned with the capture off). Made when first asked for.
        std::function<PinManager*()> pins;
        // 长截图: the stitched picture, for the editing window; 复制诊断信息's text;
        // its frames are about to begin (a notice of ours on the screen goes).
        std::function<void(const QImage&)> longCapture;
        std::function<void(const QString&)> copyText;
        std::function<void()> longStarting;
        // 二维码: the texts of the QR codes read in the selection (empty: none was
        // found) and where the selection was (physical, virtual desktop).
        std::function<void(const QStringList&, const QRect& where)> qr;
        // The colour format C copies in (hex, rgb, hsl), when Shift alone changes it.
        std::function<void(const QString&)> colorFormatChanged;
        // A fill that carries white words (the countdown's).
        std::function<QColor()> accent;
        // An overlay window was made (named "capture-overlay-<i>"), before it
        // shows: the app watches its graphics (diagnostics).
        std::function<void(QQuickWindow*, const QString& name)> windowCreated;
    };

    Capture(QQmlEngine* engine, Callbacks callbacks, QObject* parent = nullptr);
    ~Capture() override;

    void setCopyOnSelect(bool on) { m_session.setCopyOnSelect(on); }
    void setMagnifier(bool on)
    {
        m_session.setMagnifier(on);
        m_magnifierOn = on;
    }
    void setPinOnCopy(bool on) { m_session.setPinOnCopy(on); }
    void setMouseActions(const CaptureSession::MouseActions& actions) { m_session.setMouseActions(actions); }
    void setHiddenButtons(const QStringList& ids) { m_session.setHiddenButtons(ids); }
    // The mouse cursor drawn into the frozen picture ([Capture] IncludeCursor).
    void setIncludeCursor(bool on) { m_includeCursor = on; }
    // On an HDR monitor, saving writes an HDR file (.jxr) beside the PNG too
    // ([Capture] HdrFile, GitHub #133); copies and pins stay as they are.
    void setHdrFile(bool on) { m_hdrFile = on; }
    // UI Automation for the controls that are not child windows ([Capture] ControlsUia).
    void setControlDetection(bool on) { m_uiaEnabled = on; }
    void setColorFormat(ColorFormat format) { m_session.setColorFormat(format); }
    // 延时截图: after 3, 5 or 10 seconds (0: the last one used), a countdown at
    // the corner of the screen and then the capture as the hotkey starts it.
    // The hotkey (or a click on the countdown) cancels it.
    void setDelay(int seconds) { m_delay = normalizedDelay(seconds); }
    int delay() const { return m_delay; }
    void startDelayed(int seconds = 0);
    bool counting() const { return m_countdown != nullptr; }
    void cancelCountdown();
    // The hotkey, PrtScn, the tray menu: a capture, or the end of the one
    // under way (what a key opened, the same key closes; a long screenshot
    // is finished).
    void trigger();
    bool busy() const { return m_state != State::Idle || m_countdown || (m_long && m_long->running()) || (m_recorder && m_recorder->active()); }

    // 贴上最近一次截图 (the tray menu).
    bool hasLast() const { return !m_last.isNull(); }
    void pinLast(); // the last capture taken, where it was
    // The clipboard as it stood when that capture was taken
    // (GetClipboardSequenceNumber): a picture copied since is the one to pin.
    DWORD lastClipboardSeq() const { return m_lastSeq; }
    // 找回刚放弃的标注: a capture ended with annotations on it (hotkey, Esc)
    // keeps them for kDraftMs or until the next capture; this pins their
    // picture where the capture was, with the project.
    bool hasDraft() const { return m_draft.has_value(); }
    void restoreDraft();
    static constexpr int kDraftMs = 10 * 60 * 1000;
    // Ctrl+S, 保存 on a pin: into the Screenshots folder (or `path`), off the
    // GUI thread; saved() tells how it went.
    void save(const QImage& image, const QString& path = {}, std::shared_ptr<const HdrPixels> hdr = {});

private:
    enum class State { Idle, Waiting, Showing, Closing };
    struct Overlay {
        QPointer<QScreen> screen;
        HMONITOR monitor = nullptr;
        CaptureScreen* model = nullptr;
        QPointer<QQuickWindow> window;
        bool cloaked = false; // shown cloaked until its first frame is drawn
        bool warming = false; // the first, hidden show that sets up its graphics
        int closingFrames = 0; // frames to draw without the picture before hiding
    };

    void buildOverlays();
    void destroyOverlays();
    void screensChanged(); // monitors came, went or changed: everything made again
    void start();
    void show();
    void frameDrawn(QQuickWindow* window);
    void finish(CaptureSession::Action action, int screen, const QRect& rect);
    void hideAll(); // once the frames without the pictures are drawn
    void findFlyout(); // the Start menu closed: where it was in the picture
    Recorder& recorder();
    LongCapture& longCapture(); // made the first time
    void tickCountdown();
    void fireCountdown();
    void drawCursorInShots(); // [Capture] IncludeCursor
    void askUia(std::size_t window, QPoint point); // the hover is in top-level window `window`
    void uiaAnswered(const UiaProbe::Answer& answer);
    // Diagnostics (CaptureDiagnostics.h): a tick every 50 ms while the overlay
    // shows tells how long the GUI thread was held up and whether the pointer
    // over the overlay is hidden; recordCapture() writes the capture's summary.
    void startWatch();
    void watchTick();
    bool pointerOverOverlay(POINT at) const;
    void recordCapture(CaptureSession::Action action, const QRect& rect);

    QQmlEngine* m_engine;
    Callbacks m_callbacks;
    Grabber m_grabber;
    // After the grabber: its thread streams the frames, so it goes first.
    std::unique_ptr<LongCapture> m_long;
    struct LongRegion {
        QRect rect; // physical, virtual desktop
        HMONITOR monitor = nullptr;
        double dpr = 1;
    };
    std::unique_ptr<Recorder> m_recorder;
    std::optional<LongRegion> m_recordRegion;
    std::optional<LongRegion> m_longRegion; // chosen; starts once the overlays are hidden
    CaptureSession m_session;
    std::vector<Overlay> m_overlays;
    std::vector<Grabber::Shot> m_shots;
    WindowSnapshot m_snapshot;
    State m_state = State::Idle;
    QElapsedTimer m_clock; // since the hotkey
    bool m_includeCursor = false;
    bool m_hdrFile = false;
    bool m_uiaEnabled = true;
    std::unique_ptr<UiaProbe> m_uia; // made with the first question of a capture
    UiaGate m_uiaGate;
    int m_uiaLogged = 0;
    std::set<quintptr> m_uiaChromiumSkipped; // windows already logged as skipped
    int m_delay = 5;
    bool m_delayAfter = false; // D in the overlay: the countdown begins once it is gone
    std::unique_ptr<CountdownWindow> m_countdown;
    QElapsedTimer m_countClock;
    QTimer m_countTimer;
    int m_countTotal = 0;
    qint64 m_grabMs = 0;
    qint64 m_listMs = 0;
    qint64 m_flyoutMs = -1;
    QString m_grabReport;
    bool m_reported = false;
    HWND m_flyout = nullptr; // the Start menu or a shell flyout in front at the hotkey, closing
    HMONITOR m_flyoutMonitor = nullptr;
    QImage m_last; // the last capture taken (copied, saved, pinned, read), for 贴上最近一次截图
    QPoint m_lastOrigin; // where it was, physical
    QByteArray m_lastProject;
    DWORD m_lastSeq = 0; // the clipboard's sequence number as m_last was taken
    struct Draft {
        annot::Project project; // the selection's pixels and the annotations over them
        QPoint origin; // physical, virtual desktop
    };
    std::optional<Draft> m_draft;
    QTimer m_draftTimer;
    // The time between frames drawn while an annotation gesture is under way,
    // logged at the end of the capture: how smooth drawing was (nanoseconds).
    QElapsedTimer m_frameClock;
    std::vector<qint64> m_gestureFrames;
    std::vector<qint64> m_allGestureFrames;
    bool m_gestureFrameOpen = false;
    // Diagnostics: the watch runs from show() to finish().
    bool m_magnifierOn = true;
    QTimer m_watchTimer;
    QElapsedTimer m_tickClock; // the last tick
    QElapsedTimer m_shownClock; // since show()
    QElapsedTimer m_upClock; // since the overlay was up (invalid: not yet)
    diag::StallWatch m_stalls;
    int m_ticks = 0;
    QJsonObject m_pointerFacts; // read at show()
    QJsonObject m_cursorShown; // the pointer over the overlay, the first time seen
    QJsonObject m_cursorDragging; // and the first time seen while dragging
    int m_wgcShots = 0;
    int m_hdrShots = 0;
    qint64 m_prescanMs = -1; // the first scan to finish in this capture
    bool m_scanRunning = false;
    int m_scanTicket = 0;
    QTimer m_flyoutWait;
    QTimer m_hideTimeout;
    QTimer m_rebuild;
    std::jthread m_saver; // last: joined first
};

} // namespace ws::capture
