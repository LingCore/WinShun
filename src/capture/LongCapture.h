#pragma once

#include "ScrollAutoEngine.h"

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <array>
#include <functional>
#include <memory>
#include <thread>

class QQmlEngine;
class QQuickWindow;

namespace ws::capture {

class Grabber;

// The long screenshot (docs/architecture/capture-and-recording.md): the region
// chosen in the overlay, captured while what is in it scrolls, the frames
// stitched into one tall (or wide) picture that goes to the editing window.
//
// While it runs the overlay is gone and the program under the region keeps
// the focus, the wheel and the keyboard. Ours on the screen: a selection
// border in Theme.accent, kept inside the monitor at its edges, and a
// persistent action strip (LongCapturePanel.qml), with an optional preview
// window beside it. These never take focus. The border lies outside the region
// and stays in screenshots; the strip and the preview are left out of the
// frames (WDA_EXCLUDEFROMCAPTURE) while it runs and they cover the region,
// and hidden after it they keep that setting (updateExclusion). Esc and
// Enter are global hotkeys for as long as it runs (cancel, finish), always
// given back; the capture hotkey finishes too (Capture::trigger).
//
// The work is on a thread of its own (COM's multithreaded apartment, for UI
// Automation): frames (ScrollGrabber), the scrolling (ScrollInput,
// ScrollAutoEngine), the stitching (ScrollStitcher), what happened
// (ScrollDiagnostics). Auto-scrolling runs a continuous closed-loop stream
// of rhythmic scroll ticks while draining and stitching frames as they arrive
// (adapting cadence to speed mode, measured displacement and backpressure),
// ending cleanly at the page's end or the limit. Scrolling by hand stitches
// every frame that changed. Nothing of it exists before the first long
// screenshot; the big buffers go at the end.
// For us: WINSHUN_SCROLL_DUMP=<folder> keeps every frame stitched and what came
// of it in a folder a run (wsbench --scroll-replay).
class LongCapture : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Capture makes it")

    Q_PROPERTY(Status status READ status NOTIFY changed FINAL)
    Q_PROPERTY(int pictureHeight READ pictureHeight NOTIFY changed FINAL)
    Q_PROPERTY(bool autoScroll READ autoScroll WRITE setAutoScroll NOTIFY changed FINAL)
    Q_PROPERTY(bool canAutoScroll READ canAutoScroll NOTIFY changed FINAL)
    Q_PROPERTY(Speed autoScrollSpeed READ autoScrollSpeed WRITE setAutoScrollSpeed NOTIFY changed FINAL)
    // Sideways (feedback 2026-11-16): the panel's 横向, or seen in how it was
    // scrolled; pictureHeight is then the picture's width.
    Q_PROPERTY(bool horizontal READ horizontal WRITE setHorizontal NOTIFY changed FINAL)
    Q_PROPERTY(QString hint READ hint NOTIFY changed FINAL)
    Q_PROPERTY(bool partial READ partial NOTIFY changed FINAL)
    Q_PROPERTY(QImage preview READ preview NOTIFY previewChanged FINAL)
    Q_PROPERTY(bool hasPreview READ hasPreview NOTIFY previewChanged FINAL)
    Q_PROPERTY(QSize previewSize READ previewSize NOTIFY previewChanged FINAL) // device px
    Q_PROPERTY(QSize previewBox READ previewBox NOTIFY changed FINAL) // logical px: portrait for vertical, landscape for horizontal

public:
    enum class Status {
        Waiting, // the first frame only
        Stitching,
        Behind, // revisiting content already captured
        Lost, // no overlap with the last frame stitched
        Full, // at the limit
    };
    Q_ENUM(Status)

    enum class Speed {
        Slow = 0,
        Normal = 1,
        Fast = 2,
    };
    Q_ENUM(Speed)

    enum class Output { Edit, Copy, Save, Pin };
    Q_ENUM(Output)

    struct Hooks {
        std::function<void(const QImage&, Output, QPoint)> done;
        std::function<void(const QString&)> copyText; // 复制诊断信息
        std::function<void()> ended; // over and the memory let go of
    };

    LongCapture(QQmlEngine* engine, Grabber& grabber, Hooks hooks, QObject* parent = nullptr);
    ~LongCapture() override;

    // `region`: physical pixels, virtual desktop, on `monitor` (at `dpr`).
    void start(const QRect& region, HMONITOR monitor, double dpr);
    bool running() const { return m_run != nullptr; }

    Status status() const { return m_status; }
    int pictureHeight() const { return m_height; }
    bool autoScroll() const { return m_autoScroll; }
    void setAutoScroll(bool on);
    bool canAutoScroll() const { return m_canAuto; }
    Speed autoScrollSpeed() const { return m_speed; }
    void setAutoScrollSpeed(Speed speed);
    Q_INVOKABLE void cycleAutoScrollSpeed();
    bool horizontal() const { return m_horizontal; }
    // Turned while it runs: what was stitched goes, it starts again from what is in the region now.
    void setHorizontal(bool on);
    QString hint() const;
    bool partial() const { return m_partial; }
    QImage preview() const { return m_preview; }
    bool hasPreview() const { return !m_preview.isNull(); }
    QSize previewSize() const { return m_preview.size(); }
    QSize previewBox() const;

    Q_INVOKABLE void finish(); // 完成, Enter, the capture hotkey
    Q_INVOKABLE void exportTo(Output output); // the toolbar's copy, save and pin
    Q_INVOKABLE void cancel(); // 取消, Esc
    Q_INVOKABLE void copyDiagnostics();

    // What the panel says under the picture (engine → GUI).
    enum class Hint { None, Auto, Manual, MouseOutside, Elevated, NothingWorks, Reconnect, NoFrames, AtLimit, Copied };
    Q_ENUM(Hint)

    static constexpr int kPreviewWidth = 260; // vertical mode logical px
    static constexpr int kPreviewHeight = 340;
    static constexpr int kPreviewWideWidth = 340; // horizontal mode logical px
    static constexpr int kPreviewWideHeight = 220;

signals:
    void changed();
    void previewChanged();

private:
    struct Run;
    struct Update;
    class Border;

    void engine(Run& run);
    void apply(const Update& update);
    void ended(const QImage& picture, const QString& how);
    void wrapUp();
    void dumpPanel();
    QQuickWindow* panel();
    void placePanel(const QRect& region, HMONITOR monitor);
    void updateExclusion();
    void hotkeys(bool on);

    QQmlEngine* m_engine;
    Grabber& m_grabber;
    Hooks m_hooks;
    std::unique_ptr<Run> m_run;
    std::jthread m_thread;
    std::unique_ptr<Border> m_border;
    QPointer<QQuickWindow> m_panel;
    QPointer<QQuickWindow> m_previewWindow;
    bool m_hotkeys = false;

    Status m_status = Status::Waiting;
    int m_height = 0;
    bool m_autoScroll = false;
    bool m_autoScrollChosen = false; // the switch as the user left it: the engine's "lost, by hand" is one run's
    bool m_canAuto = true;
    Speed m_speed = Speed::Normal;
    bool m_horizontal = false;
    bool m_horizontalChosen = false; // the panel's, for the next run; a turn the engine saw is that run's
    Hint m_hint = Hint::None;
    Hint m_hintBefore = Hint::None; // under Copied, for a moment
    bool m_partial = false;
    QImage m_preview;
    QTimer m_copiedTimer;
};

} // namespace ws::capture
