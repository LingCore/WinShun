#pragma once

#include "AnnotationModel.h"
#include "EditView.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QQuickWindow>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <optional>
#include <atomic>
#include <vector>

class QScreen;

namespace ws::capture {

// The editing window (docs/design-capture-roadmap.md, 2.2, 3.1): one
// ordinary Qt Quick window (EditWindow.qml), made the first time and shown
// and hidden after, that holds the annotation model, canvas, toolbar and
// panel of the overlay on a picture opened from the history (jt, the
// clipboard window), from a pin, or on its own. This is its controller: the
// zoom and where the picture lies in the window, the mouse and keys (the
// model gets image pixels), the ways out, and the memory it gives back.
//
// Everything about the window itself (showing it above the others, cloaking
// it while it empties, putting it at an exact physical place) is done by
// the owner through Hooks::Window, so that this needs no native window and
// the tests can drive it with none.
class CaptureEditor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the app")

    Q_PROPERTY(ws::capture::AnnotationModel* model READ model CONSTANT FINAL)
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged FINAL)
    Q_PROPERTY(bool fromPin READ fromPin NOTIFY openChanged FINAL)
    // What the first button says: 复制, or on a pin 完成.
    Q_PROPERTY(QString copyText READ copyText NOTIFY openChanged FINAL)
    Q_PROPERTY(bool ocrAvailable READ ocrAvailable CONSTANT FINAL)
    // The window's layout, logical px: the title bar and the toolbar above
    // the picture, the panel's row below it.
    Q_PROPERTY(int topHeight READ topHeight CONSTANT FINAL)
    Q_PROPERTY(int toolbarRow READ toolbarRow CONSTANT FINAL)
    Q_PROPERTY(int bottomHeight READ bottomHeight CONSTANT FINAL)
    // The picture in the window: logical px per image px (the zoom, at the
    // window's real buffer scale), one device pixel in logical px, the
    // monitor's scale; where the picture's top left is (window coordinates,
    // on a device pixel) and how large it is; the part in view (image px).
    Q_PROPERTY(qreal scaleX READ scaleX NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal scaleY READ scaleY NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal unitX READ unitX NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal unitY READ unitY NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal dpr READ dpr NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom NOTIFY viewChanged FINAL)
    Q_PROPERTY(QPointF contentOrigin READ contentOrigin NOTIFY viewChanged FINAL)
    Q_PROPERTY(QSizeF contentSize READ contentSize NOTIFY viewChanged FINAL)
    Q_PROPERTY(QRectF visible READ visible NOTIFY viewChanged FINAL)
    // The whole picture's size (image px): the crop is within it.
    Q_PROPERTY(QSize imageSize READ imageSize NOTIFY viewChanged FINAL)
    // Smaller than it is: shown smoothed; at 100 % and more, pixel for pixel.
    Q_PROPERTY(bool linear READ linear NOTIFY viewChanged FINAL)
    Q_PROPERTY(QString zoomText READ zoomText NOTIFY viewChanged FINAL)
    // What would be exported: the crop's size.
    Q_PROPERTY(QString sizeText READ sizeText NOTIFY sizeTextChanged FINAL)
    Q_PROPERTY(int cursorShape READ cursorShape NOTIFY cursorChanged FINAL)
    // A word for the user (the Esc that gave up the changes); empty when none.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged FINAL)

public:
    // The pin being edited, as the app lends it: the picture it shows and
    // its project, where its picture is on the screen (physical, virtual
    // desktop) and at what zoom; and what to do with it. `alive` says it is
    // still there (it may have been closed meanwhile).
    struct PinTarget {
        QImage image;
        QByteArray project;
        QRect picture;
        double zoom = 1.0;
        std::function<bool()> alive;
        std::function<void(bool shown)> setShown;
        std::function<void(const QImage&, const QByteArray& project)> replace;
    };

    struct Hooks {
        // The history entry's project (empty: none), its picture, whether it
        // is still in the history.
        std::function<QByteArray(qint64 id)> project;
        std::function<QImage(qint64 id)> picture;
        std::function<bool(qint64 id)> exists;
        // Enter on a changed picture: a new entry and the clipboard; the same
        // pixels as they were: the original moves to the top.
        std::function<void(const QImage&, const QByteArray& project)> copy;
        std::function<void(qint64 id)> copyOriginal;
        // Ctrl+S, F3 and 完成 on a pin also put the picture in the history.
        std::function<void(const QImage&, const QByteArray& project)> record;
        std::function<void(const QImage&, const QString& path)> save;
        std::function<QString(QQuickWindow*)> chooseSavePath; // empty path: keep editing
        // F3: a pin of its own, its picture at `origin` (physical).
        std::function<void(const QImage&, const QByteArray& project, QPoint origin)> pin;
        std::function<void(const QImage&, double dpr, const QRect& where)> ocr;
        // 智能打码: how the text is read and what is hidden (SmartRedact.h).
        SmartHooks smart;
        // What the tools remember ([Capture] AnnotateStyles, JSON).
        std::function<QString()> styles;
        std::function<void(const QString&)> stylesChanged;
        // The window is hidden and its memory let go of.
        std::function<void()> closed;
        // The window's own: shown above the rest with the keyboard; cloaked
        // (gone from the screen) or not; its native rectangle (physical); put
        // at a physical place.
        struct Window {
            std::function<void(QQuickWindow*)> show;
            std::function<void(QQuickWindow*, bool cloaked)> cloak;
            std::function<QRect(QQuickWindow*)> nativeRect;
            std::function<void(QQuickWindow*, QPoint topLeft)> moveTo;
        } window;
    };

    // How much of the window is not the picture, logical px.
    static constexpr int kTitleHeight = 48;
    static constexpr int kToolbarRow = 52;
    static constexpr int kBottomHeight = 56;
    static constexpr int kMinWindowWidth = 640;
    static constexpr int kMinWindowHeight = 360;
    // Around a picture that fits, logical px, and the share of the work area a window may take.
    static constexpr int kPadding = 16;
    static constexpr double kWorkShare = 0.92;
    static constexpr int kNoticeMs = 4000;

    explicit CaptureEditor(QObject* parent = nullptr);
    ~CaptureEditor() override;

    void setHooks(Hooks hooks);
    void setOcrAvailable(bool available) { m_ocrAvailable = available; }
    // The window EditWindow.qml made; its size, scale and screen are followed.
    void attach(QQuickWindow* window);
    QQuickWindow* window() const { return m_window; }

    // ---- opening ----
    // A history entry: its project, else its picture as the base of an empty
    // document. False when there is nothing to open (no such entry, a
    // picture that cannot be read).
    bool openHistory(qint64 clipId);
    // A pin: its project, else its picture.
    bool openPin(PinTarget pin);
    // A picture with nothing behind it (a long capture, the arrangement of
    // several selections).
    bool openImage(const QImage& picture);
    bool isOpen() const { return m_open; }
    bool fromPin() const { return m_open && m_origin == editview::Origin::Pin; }
    editview::Origin origin() const { return m_origin; }
    qint64 clipId() const { return m_clipId; }
    // Hides the window and lets go of the picture, the document and the tiles
    // (the changes are given up).
    Q_INVOKABLE void close();

    // ---- the window's measures ----
    // The window's logical size and the monitor's scale (attach() follows them
    // itself; the tests set them). The picture's part of the window is what
    // the title bar, the toolbar and the panel's row leave.
    void setMetrics(QSize windowLogical, qreal dpr);

    // ---- what QML reads ----
    AnnotationModel* model() { return &m_model; }
    const AnnotationModel& model() const { return m_model; }
    QString copyText() const;
    bool ocrAvailable() const { return m_ocrAvailable; }
    int topHeight() const { return kTitleHeight + kToolbarRow; }
    int toolbarRow() const { return kToolbarRow; }
    int bottomHeight() const { return kBottomHeight; }
    qreal scaleX() const { return m_zoom / m_bufferX; }
    qreal scaleY() const { return m_zoom / m_bufferY; }
    qreal unitX() const { return 1.0 / m_bufferX; }
    qreal unitY() const { return 1.0 / m_bufferY; }
    qreal dpr() const { return m_dpr; }
    qreal zoom() const { return m_zoom; }
    QPointF contentOrigin() const;
    QSizeF contentSize() const;
    QRectF visible() const;
    QSize imageSize() const { return m_model.document().canvas; }
    bool linear() const { return m_zoom < 1.0 - 1e-9; }
    QString zoomText() const;
    QString sizeText() const;
    int cursorShape() const { return m_cursor; }
    QString notice() const { return m_notice; }
    // The viewport in device px (window coordinates) and the picture's top
    // left in it: what the tests and the placing of the window read.
    QRect viewportDevice() const { return m_viewDev; }
    QPoint originDevice() const { return m_originDev; }
    bool fitted() const { return m_fit; }
    // The picture's top left in the window, device px.
    QPoint imageInWindow() const { return m_viewDev.topLeft() + m_originDev; }

    // ---- input, from QML (window logical coordinates) and the tests ----
    Q_INVOKABLE void mousePressed(qreal x, qreal y, int button, int modifiers);
    Q_INVOKABLE void mouseMoved(qreal x, qreal y, int modifiers);
    Q_INVOKABLE void mouseReleased(qreal x, qreal y, int button);
    Q_INVOKABLE void mouseDoubleClicked(qreal x, qreal y);
    Q_INVOKABLE void wheeled(qreal x, qreal y, int angleDeltaY, int angleDeltaX, int modifiers);
    Q_INVOKABLE bool keyPressed(int key, int modifiers);
    // The save and pin keys (#187); the text box asks the model, which gets them too.
    void setKeys(const CaptureKeys& keys)
    {
        m_keys = keys;
        m_model.setKeys(keys);
    }
    const CaptureKeys& keys() const { return m_keys; }
    Q_INVOKABLE void spaceHeld(bool held);
    // A save folder was chosen ([Capture] SaveFolder): Save goes straight into it (#194).
    void setSaveDirect(bool on) { m_model.setSaveDirect(on); }
    // The toolbar's buttons: a CaptureSession::Action; Save asks for a path
    // unless a save folder was chosen, SavePrevious uses the configured
    // folder without a picker, SaveAs always asks.
    Q_INVOKABLE void act(int action);

    // ---- the zoom ----
    // Zoom to `zoom` about the viewport point `anchor` (device px in the
    // viewport); none: its middle.
    void zoomTo(double zoom, std::optional<QPoint> anchor = std::nullopt);
    void zoomBySteps(int steps, std::optional<QPoint> anchor = std::nullopt);
    // Z: the whole picture, or 100 %.
    Q_INVOKABLE void toggleFit();
    Q_INVOKABLE void fit();
    // The picture moved by device px (kept inside the viewport).
    void scrollBy(QPoint device);

    // The Esc ladder after the model's own steps: give up the changes, else
    // close. True when it did one.
    bool escape();
    // Enter: copy / 完成. Save, pin and OCR are act()'s.
    void done();

signals:
    void openChanged();
    void viewChanged();
    void sizeTextChanged();
    void cursorChanged();
    void noticeChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    using Origin = editview::Origin;

    bool begin(annot::Project project, Origin origin);
    annot::Project projectOfPicture(const QImage& picture) const;
    void present();
    void settle();
    void relayout();
    void updateScale();
    void showNotice(const QString& text);
    QPoint deviceInViewport(qreal x, qreal y) const;
    QPoint imageAt(QPoint viewportDevice) const;
    void setCursor(int shape);
    void hoverAt(QPoint image);
    QByteArray packProject();
    QRect canvasOnScreen() const;
    void end();
    void hideWindow();
    void finishHide();
    void abortHide();
    // A line in the log (WINSHUN_LOG_INFO) with where the picture is: the live test reads it.
    void logView(const char* reason) const;
    // How smooth scrolling and zooming were: the time between the frames drawn while the view moved.
    void viewMoved();
    void frameDrawn();
    void flushMotion();
    void alignToPin();
    QScreen* targetScreen() const;

    AnnotationModel m_model;
    CaptureKeys m_keys;
    Hooks m_hooks;
    PinTarget m_pin;
    QPointer<QQuickWindow> m_window;
    bool m_open = false;
    bool m_choosingSave = false;
    bool m_ocrAvailable = false;
    Origin m_origin = Origin::Fresh;
    qint64 m_clipId = 0;
    // The window and the view
    QSize m_windowLogical {1000, 700};
    qreal m_dpr = 1.0;
    qreal m_bufferX = 1.0; // device px per logical px, per axis (Qt's real buffer)
    qreal m_bufferY = 1.0;
    QRectF m_viewport {0, 100, 1000, 544}; // logical, window coordinates (setMetrics)
    QRect m_viewDev;
    double m_zoom = 1.0;
    bool m_fit = true;
    QPoint m_originDev; // the picture's top left from the viewport's, device px
    // The mouse
    bool m_pressed = false;
    bool m_space = false;
    bool m_panning = false;
    QPoint m_panFrom;
    QPoint m_panOrigin;
    int m_wheelZoom = 0; // wheel not yet a whole notch (touchpads)
    int m_cursor = Qt::ArrowCursor;
    QElapsedTimer m_clock;
    QString m_notice;
    QTimer m_noticeTimer;
    // Closing: the window is drawn empty, then hidden
    int m_closingFrames = 0;
    QTimer m_hideTimeout;
    QMetaObject::Connection m_frameConnection;
    bool m_hiding = false;
    QTimer m_settle;
    QSize m_wantedSize; // what present() sized the window to
    QPoint m_wantedPosition;
    QElapsedTimer m_frameClock;
    QElapsedTimer m_stageClock; // the stages of a frame, for the slow ones' log
    std::atomic<qint64> m_stageBeforeSync {0}, m_stageAfterSync {0}, m_stageBeforeRender {0}, m_stageAfterRender {0},
        m_stageSwapped {0}, m_stagePrevSwapped {0};
    qint64 m_motionUntil = 0; // ms on m_clock: frames up to then are part of the movement
    bool m_frameOpen = false;
    std::vector<qint64> m_frames; // ns between frames
    qint64 m_wheelAt = 0; // m_clock ns of the notch not yet on the screen
    std::vector<qint64> m_latency; // ns from a notch to the frame that shows it
    std::vector<qint64> m_handled; // ns each wheel notch took to handle, the view's bindings included
    QTimer m_motionEnd;
    mutable QSize m_loggedWindow;
    mutable qreal m_loggedDpr = 0;
};

} // namespace ws::capture
