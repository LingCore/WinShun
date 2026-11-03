#pragma once

#include "AnnotationModel.h"
#include "CaptureTools.h"
#include "Geometry.h"
#include "KeyHintPolicy.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <optional>
#include <vector>

namespace ws::capture {

// One monitor of a capture, as its overlay window (CaptureOverlay.qml)
// shows it: the frozen picture and what is drawn over it, all in the
// monitor's own physical pixels (0,0 is its top left). The window turns
// them into its logical units with scaleX and scaleY, Qt's real buffer
// scale (bufferScale), so every edge lands on a whole screen pixel.
class CaptureScreen : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the capture")

    Q_PROPERTY(int index READ index CONSTANT FINAL)
    Q_PROPERTY(QImage image READ image NOTIFY imageChanged FINAL)
    // QML cannot look into a QImage (image.width is undefined there): whether there is one.
    Q_PROPERTY(bool hasImage READ hasImage NOTIFY imageChanged FINAL)
    Q_PROPERTY(QSize pixels READ pixels NOTIFY geometryChanged FINAL)
    Q_PROPERTY(qreal scaleX READ scaleX NOTIFY geometryChanged FINAL) // logical units per physical pixel
    Q_PROPERTY(qreal scaleY READ scaleY NOTIFY geometryChanged FINAL)
    Q_PROPERTY(qreal dpr READ dpr NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QPoint origin READ origin NOTIFY geometryChanged FINAL) // its top left, physical, virtual desktop
    // Without the taskbar: physical, its own (the whole monitor when unknown).
    Q_PROPERTY(QRect workArea READ workArea NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QRect hover READ hover NOTIFY changed FINAL) // what a click would pick; null when none here
    Q_PROPERTY(QRect selection READ selection NOTIFY changed FINAL) // null when none here
    Q_PROPERTY(bool dimmed READ dimmed NOTIFY changed FINAL) // dim outside the hover/selection on every monitor
    Q_PROPERTY(bool adjusting READ adjusting NOTIFY changed FINAL) // being drawn, moved or resized: no toolbar
    // The magnifier by the cursor here: not beside a selection made, nor over the toolbar.
    Q_PROPERTY(bool magnifierShown READ magnifierShown NOTIFY changed FINAL)
    Q_PROPERTY(bool cursorHere READ cursorHere NOTIFY cursorChanged FINAL)
    Q_PROPERTY(QPoint cursor READ cursor NOTIFY cursorChanged FINAL)
    Q_PROPERTY(QImage loupe READ loupe NOTIFY cursorChanged FINAL) // the pixels around the cursor, kLoupeCells square
    Q_PROPERTY(QString color READ color NOTIFY cursorChanged FINAL) // "#RRGGBB" under the cursor
    Q_PROPERTY(QString colorText READ colorText NOTIFY cursorChanged FINAL) // the same in the chosen format (HEX, RGB, HSL)
    Q_PROPERTY(int cursorShape READ cursorShape NOTIFY cursorChanged FINAL)
    Q_PROPERTY(QString flash READ flash NOTIFY flashChanged FINAL) // a word by the magnifier ("已复制 #…")
    // A word for the user by the selection ("已放弃这次的标注 · Ctrl+Z 恢复"); empty when none.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged FINAL)

public:
    static constexpr int kLoupeCells = 15; // odd: the cursor's pixel in the middle

    CaptureScreen(int index, QObject* parent = nullptr);

    int index() const { return m_index; }
    QImage image() const { return m_image; }
    bool hasImage() const { return !m_image.isNull(); }
    QSize pixels() const { return m_rect.size(); }
    QRect rect() const { return m_rect; } // physical, virtual desktop
    QPoint origin() const { return m_rect.topLeft(); }
    QRect bounds() const { return QRect(QPoint(0, 0), m_rect.size()); } // physical, its own
    QRect workArea() const;
    qreal dpr() const { return m_dpr; }
    qreal scaleX() const { return m_scaleX; }
    qreal scaleY() const { return m_scaleY; }
    QRect hover() const { return m_hover; }
    QRect selection() const { return m_selection; }
    bool dimmed() const { return m_dimmed; }
    bool adjusting() const { return m_adjusting; }
    bool magnifierShown() const { return m_magnifierShown; }
    bool cursorHere() const { return m_cursorHere; }
    QPoint cursor() const { return m_cursor; }
    QImage loupe() const; // sampled lazily, once per pointer position and frozen image
    QString color() const { return m_color; }
    QString colorText() const { return m_colorText; }
    int cursorShape() const { return m_cursorShape; }
    QString flash() const { return m_flash; }
    QString notice() const { return m_notice; }

    // Where the monitor is (physical, virtual desktop), its DPR and its
    // window's logical size, which the scale follows.
    void setGeometry(const QRect& rect, qreal dpr, QSize logicalWindow);
    void setWorkArea(const QRect& rect); // physical, virtual desktop (the monitor's rcWork)
    void setImage(const QImage& image);
    // The format the colour is written in; the magnifier shows it at once.
    void setColorFormat(ColorFormat format);

    // Kept for QML's sake: the selection's toolbar, size label and the
    // magnifier, placed by Geometry.h from sizes QML measured (logical),
    // returned in logical units.
    Q_INVOKABLE QPointF placeToolbar(const QRect& selection, qreal width, qreal height) const;
    Q_INVOKABLE QPointF placeSizeLabel(const QRect& selection, qreal width, qreal height) const;
    Q_INVOKABLE QPointF placeMagnifier(const QPoint& cursor, qreal width, qreal height) const;

signals:
    void imageChanged();
    void geometryChanged();
    void changed();
    void cursorChanged();
    void flashChanged();
    void noticeChanged();

private:
    friend class CaptureSession;
    void setState(const QRect& hover, const QRect& selection, bool dimmed, bool adjusting, bool magnifierShown);
    void setCursor(bool here, QPoint at, int shape);
    void setFlash(const QString& text);
    void setNotice(const QString& text);
    QSize toPhysical(qreal width, qreal height) const;
    QPointF toLogical(QPoint p) const;

    int m_index = 0;
    QImage m_image;
    QRect m_rect;
    QRect m_work; // physical, virtual desktop; null: the whole monitor
    qreal m_dpr = 1;
    qreal m_scaleX = 1;
    qreal m_scaleY = 1;
    QRect m_hover;
    QRect m_selection;
    bool m_dimmed = false;
    bool m_adjusting = false;
    bool m_magnifierShown = false;
    bool m_cursorHere = false;
    QPoint m_cursor {-1, -1};
    mutable QImage m_loupe;
    mutable QPoint m_loupeAt {-1, -1};
    mutable qint64 m_loupeImageKey = 0;
    QString m_color;
    QString m_colorText;
    QRgb m_rgb = 0;
    ColorFormat m_colorFormat = ColorFormat::Hex;
    int m_cursorShape = Qt::CrossCursor;
    QString m_flash;
    QString m_notice;
};

// A capture from the frozen screens to the picture taken (or none): what
// the mouse and keys do. Moving over the screens highlights the window or
// control a click picks (the wheel steps out to the window and the whole
// screen and back); a drag draws a selection; with one, its eight handles
// resize it and a drag inside moves it; the arrow keys move it a physical
// pixel (Shift: resize), and WASD moves the mouse pointer a physical pixel
// (Shift: ten) in every phase, a held-button drag included. R recalls
// recent regions on this picture; ? (or F1) shows or hides the key hints.
// Enter or a double click copies, Ctrl+S saves, F3 pins it
// to the screen; Esc or a right click drops the selection, or with none ends
// the capture; C copies the colour under the cursor. A selection stays on
// one monitor. With a selection, the annotation tools (AnnotationModel) draw
// on the frozen picture: the mouse and keys go to the model first (docs/
// design-capture-roadmap.md, 3.6), and Esc takes a text, a stroke, then the
// selection with its annotations off the screen in steps.
//
// Input comes in physical pixels of the monitor it is on (the overlays
// convert; the tests call these directly). No window is needed here.
class CaptureSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the capture")

    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged FINAL)
    // The OCR models are there: 识别文字 can read; else it says why not.
    Q_PROPERTY(bool ocrAvailable READ ocrAvailable NOTIFY ocrAvailableChanged FINAL)
    // The annotations of this capture (always there; active() once a selection is).
    Q_PROPERTY(AnnotationModel* annotation READ annotation CONSTANT FINAL)
    // H hides the toolbar (the selection may sit under it).
    Q_PROPERTY(bool toolbarHidden READ toolbarHidden NOTIFY toolbarHiddenChanged FINAL)
    // The action buttons left off the toolbar (settings; ids as Settings::captureToolbarButtonIds):
    // their keys still work.
    Q_PROPERTY(QStringList hiddenButtons READ hiddenButtons NOTIFY hiddenButtonsChanged FINAL)
    // The hint about the tool keys has been shown (once per run).
    Q_PROPERTY(bool toolHintSeen READ toolHintSeen WRITE setToolHintSeen NOTIFY toolHintSeenChanged FINAL)
    Q_PROPERTY(int selectionHistoryCount READ selectionHistoryCount NOTIFY selectionHistoryChanged FINAL)
    // Zero-based, newest first; -1 until a remembered region is restored.
    Q_PROPERTY(int recalledSelection READ recalledSelection NOTIFY selectionChanged FINAL)
    // The shortcut hints in a corner (CaptureKeyHints.qml). keyHintsEnabled:
    // the setting ([Capture] KeyHints); keyHintsShown: whether they are up
    // now, by KeyHintPolicy's rules (timers, presses, ? and F1); keyHints:
    // the ids of the rows for what can be done now (keyHintRows), none while
    // a drag, a stroke or a text is under way.
    Q_PROPERTY(bool keyHintsEnabled READ keyHintsEnabled NOTIFY keyHintsChanged FINAL)
    Q_PROPERTY(bool keyHintsShown READ keyHintsShown NOTIFY keyHintsChanged FINAL)
    Q_PROPERTY(QStringList keyHints READ keyHints NOTIFY keyHintsChanged FINAL)

public:
    // Pin: left on the screen where it was (PinWindow); Ocr: its words read (CaptureOcr);
    // OcrCopy, OcrSearch and OcrWeb read them and go on at once: to the clipboard,
    // the launcher's search box, the default web search (the mouse buttons' choices).
    // Long: the selection scrolls into a long screenshot (Ctrl+L; LongCapture),
    // one at least kLongMinimum px each way. Qr: the QR codes in it are read
    // (Q); Delay: this capture ends and the next begins after the countdown (Ctrl+Shift+D).
    enum class Action { Copy, Save, Cancel, Pin, Ocr, OcrCopy, OcrSearch, OcrWeb, Long, Qr, Delay, CopyPin, Record, SavePrevious };
    Q_ENUM(Action)
    static constexpr int kLongMinimum = 48;
    static constexpr int kSelectionHistoryLimit = 10;

    // The windows at the freeze: top-level ones from the top of the Z order
    // down, physical, virtual desktop; children(i) gives window i's controls.
    struct Windows {
        std::vector<QRect> tops;
        std::function<std::vector<QRect>(std::size_t)> children;
        // The mouse is at `point` (physical, virtual desktop) over top-level
        // window i: UI Automation may be asked about the controls there
        // (UiaProbe); what it finds is joined to children(i) and refreshHover()
        // called. Optional.
        std::function<void(std::size_t i, QPoint point)> probe;
    };


    explicit CaptureSession(QObject* parent = nullptr);

    void begin(std::vector<CaptureScreen*> screens, Windows windows);
    // The screens show another capture's pictures now (, and . step through
    // the history; the caller set them): a click picks from `windows`, and
    // the selection is `rect` on screen `screen` (an index, as finished()
    // gives it) with `doc`'s annotations over it, or (screen -1) there is
    // none. What there was goes; kept() first keeps it.
    void replaceShot(Windows windows, int screen, const QRect& rect, const std::optional<annot::Document>& doc);
    struct Kept {
        int screen = -1;
        QRect rect;
        std::optional<annot::Document> doc; // annotations, on the screen's whole picture
    };
    // The selection and its annotations now (put down with Esc too).
    Kept kept() const;
    void notify(const QString& text) { showNotice(text); } // by the selection, for a few seconds
    // The Start menu (or a shell flyout) in the picture: over every window
    // listed, without controls (physical, virtual desktop). EnumWindows
    // does not list it (another band of the Z order); found from the
    // picture once it closed (changedRegion).
    void setFlyout(const QRect& rect);
    // The windows' controls changed (UI Automation answered): the hover is
    // worked out again at the cursor.
    void refreshHover();
    void end();
    bool ocrAvailable() const { return m_ocrAvailable; }
    void setSmartHooks(SmartHooks hooks) { m_annot.setSmartHooks(std::move(hooks)); }
    void setOcrAvailable(bool on);
    // The colour's format (C copies it, Shift alone changes it): kept between captures.
    void setColorFormat(ColorFormat format);
    ColorFormat colorFormat() const { return m_colorFormat; }
    // Completed regions only (virtual desktop, physical), newest first,
    // deduplicated and kept for this run. R cycles them on the frozen picture.
    void rememberSelection(const QRect& rect);
    int selectionHistoryCount() const { return static_cast<int>(m_selectionHistory.size()); }
    int recalledSelection() const { return m_recalledSelection; }
    QStringList hiddenButtons() const { return m_hiddenButtons; }
    void setHiddenButtons(const QStringList& ids);
    bool active() const { return m_active; }
    bool hasSelection() const { return m_selection.screen >= 0; }
    int selectionScreen() const { return m_selection.screen; }
    QRect selectionRect() const { return m_selection.rect; } // physical, that monitor's own
    // For the diagnostics: where the gesture is (hover, dragging, selected,
    // adjusting, annotating), and the time since the left button was last let
    // go of in this capture (-1: not yet).
    QString phaseName() const
    {
        switch (m_phase) {
        case Phase::Hover: return QStringLiteral("hover");
        case Phase::Pressed:
        case Phase::Dragging: return QStringLiteral("dragging");
        case Phase::Selected: return QStringLiteral("selected");
        case Phase::Adjusting: return QStringLiteral("adjusting");
        case Phase::Annotating: return QStringLiteral("annotating");
        }
        return QStringLiteral("other");
    }
    qint64 msSinceRelease() const { return m_releaseClock.isValid() ? m_releaseClock.elapsed() : -1; }
    QRect hoverRect() const;
    void setCopyOnSelect(bool on) { m_copyOnSelect = on; } // a drag or a click on a window copies at once
    void setMagnifier(bool on) { m_magnifier = on; } // takes effect with the next publish()
    void setPinOnCopy(bool on) { m_pinOnCopy = on; }

    // What the mouse buttons do once a selection is there (issue #46), as
    // Settings keeps them: none | copy | save | pin | ocr | ocrcopy | ocrsearch |
    // ocrweb | cancel, the right button also back. Only with no annotation tool on and no stroke or text
    // under way; then the right button steps back like Esc, the rest do nothing.
    struct MouseActions {
        QString doubleLeft = QStringLiteral("copy");
        QString right = QStringLiteral("back");
        QString middle = QStringLiteral("none");
        QString back = QStringLiteral("none"); // X1
        QString forward = QStringLiteral("none"); // X2
    };
    void setMouseActions(const MouseActions& actions);

    AnnotationModel* annotation() { return &m_annot; }
    // The monitor the annotations are on (the selection's, even while it is
    // put down with Esc), -1 when there are none.
    int annotationScreen() const { return m_annot.active() ? m_annotScreen : -1; }
    bool hasAnnotations() const { return m_annot.active() && m_annot.hasItems(); }
    // What the tools remember (annot::StyleMemory): set before a capture,
    // read after it (stylesChanged: a tool's style changed meanwhile).
    void setStyles(const annot::StyleMemory& styles);
    annot::StyleMemory styles() const;
    bool stylesChanged() const;
    bool toolbarHidden() const { return m_toolbarHidden; }
    bool toolHintSeen() const { return m_toolHintSeen; }
    void setToolHintSeen(bool seen);

    // The key hints. Rows, in order (the panel words them):
    //   choosing: wheel, wasd, recall (some remembered), history (setHistoryAvailable),
    //             ocr (ocrAvailable), quit
    //   selected: copy, save, pin, arrows, ocr, long, recalled (R brought it back)
    //   tool:     shift (shapes, lines and strokes), width, undo, delete, toolOff, hide
    // and none while busy.
    QStringList keyHintRows() const;
    QStringList keyHints() const { return m_keyHints; }
    bool keyHintsEnabled() const { return m_hintPolicy.enabled(); }
    bool keyHintsShown() const { return m_hintsShown; }
    void setKeyHints(bool on); // the setting
    // This capture was begun by the delay's countdown: no hints by themselves (after begin()).
    void quietKeyHints();
    // , and . have captures to bring back (Capture's history, at the start).
    void setHistoryAvailable(bool on);
    // The panel's button: 不再显示 (false), or 一直显示 (true) when ? showed it
    // with the setting off. keyHintsKept tells the settings; this capture
    // follows them at once.
    Q_INVOKABLE void keepKeyHints(bool on);

    void pointerMove(int screen, QPoint p, Qt::KeyboardModifiers modifiers = {});
    void pointerPress(int screen, QPoint p, Qt::MouseButton button, Qt::KeyboardModifiers modifiers = {});
    void pointerRelease(int screen, QPoint p, Qt::MouseButton button);
    void doubleClick(int screen, QPoint p);
    void wheel(int screen, QPoint p, int steps); // steps > 0: out (bigger)
    // The wheel over a selection with a tool on or an object selected sizes
    // the tool; otherwise it is wheel()'s. `angleDelta` is Qt's (120 a notch).
    void wheelAngle(int screen, QPoint p, int angleDelta, qint64 nowMs);
    bool key(int key, Qt::KeyboardModifiers modifiers);

    // From QML, in the window's logical units (Qt's mouse positions).
    Q_INVOKABLE void mouseMoved(int screen, qreal x, qreal y);
    Q_INVOKABLE void mousePressed(int screen, qreal x, qreal y, int button);
    Q_INVOKABLE void mouseReleased(int screen, qreal x, qreal y, int button);
    Q_INVOKABLE void mouseDoubleClicked(int screen, qreal x, qreal y);
    // The picture lost the mouse: the toolbar above it took it, or another monitor.
    Q_INVOKABLE void mouseLeft(int screen);
    Q_INVOKABLE void wheeled(int screen, qreal x, qreal y, int angleDeltaY);
    Q_INVOKABLE bool keyPressed(int key, int modifiers, bool autoRepeat = false);
    Q_INVOKABLE void keyReleased(int key); // a Shift alone, let go of: the colour's next format
    Q_INVOKABLE void act(int action); // the toolbar's buttons: an Action

signals:
    // What to take: that monitor's pixels in `rect` (its own physical
    // coordinates); nothing for Cancel.
    void finished(ws::capture::CaptureSession::Action action, int screen, const QRect& rect);
    void colorCopied(const QString& color);
    // , (or <): the capture before the one shown, 1; . (or >): the one after, -1.
    void historyStep(int step);
    void colorFormatChanged(ws::capture::ColorFormat format);
    void cursorMoveRequested(const QPoint& global); // WASD, and the arrow keys with no selection: the pointer
    void selectionChanged();
    void activeChanged();
    void ocrAvailableChanged();
    void hiddenButtonsChanged();
    void toolbarHiddenChanged();
    void toolHintSeenChanged();
    void selectionHistoryChanged();
    void keyHintsChanged();
    void keyHintsKept(bool on); // the panel's button: [Capture] KeyHints

private:
    // Annotating: a drawing, moving or reshaping gesture of the model's is
    // under way. Not Adjusting: the toolbar stays (it steps aside by itself).
    enum class Phase { Hover, Pressed, Dragging, Selected, Adjusting, Annotating };
    struct Pick {
        int screen = -1;
        QRect rect;
    };

    CaptureScreen* screenAt(int index) const;
    QPoint clampTo(int screen, QPoint p) const;
    void updateHover(int screen, QPoint p);
    void updateCursor(int screen, QPoint p);
    void publish(); // the state onto the screens
    void finish(Action action);
    void recallSelection(bool previous); // R: older; Shift+R: newer
    void nudgeSelection(int dx, int dy, bool resize);
    void movePointer(int dx, int dy); // WASD: the mouse pointer, whatever the phase
    void back(); // Esc, right click
    enum class MouseAct { None, Copy, Save, Pin, Ocr, Cancel, Back, OcrCopy, OcrSearch, OcrWeb, CopyPin };
    static MouseAct parseMouseAct(const QString& name, MouseAct fallback);
    bool annotationBusy() const; // a tool on, a gesture or a text under way
    void mouseButton(Qt::MouseButton button, int screen, QPoint p); // right, middle, X1, X2
    void perform(MouseAct act);
    void syncAnnotation(); // the model follows the selection
    void retireAnnotation(); // the model ended, its styles kept
    void dropParked(); // a new selection: the annotations put down with Esc are gone
    void showNotice(const QString& text);
    // keyHints follows keyHintRows, keyHintsShown the policy (its stage, its
    // timers); told only when either changed.
    void refreshKeyHints();
    void hintInput(); // a move, a key, the wheel, a tool: KeyHintPolicy::input
    qint64 hintNow() const { return m_hintClock.elapsed(); }
    int cursorShapeAt(int screen, QPoint p) const;
    Pick hovered() const; // what a click would pick now: the hover, else the screen under the cursor
    int reach(int screen) const; // how near a handle a press takes it, physical
    int dragStart(int screen) const; // how far a press moves before it is a drag

    std::vector<CaptureScreen*> m_screens;
    Windows m_windows;
    QRect m_flyout; // over all of m_windows.tops
    bool m_active = false;
    bool m_ocrAvailable = false;
    QStringList m_hiddenButtons;
    bool m_copyOnSelect = false;
    bool m_magnifier = true; // off: no magnifier, so no colour picking either
    bool m_pinOnCopy = false;
    MouseAct m_actDoubleLeft = MouseAct::Copy;
    MouseAct m_actRight = MouseAct::Back;
    MouseAct m_actMiddle = MouseAct::None;
    MouseAct m_actBack = MouseAct::None;
    MouseAct m_actForward = MouseAct::None;
    // A button pressed on a selection at rest, its action waiting for the release.
    Qt::MouseButton m_pendingButton = Qt::NoButton;
    MouseAct m_pendingAct = MouseAct::None;
    int m_pendingScreen = -1;
    QPoint m_pendingAt;
    Phase m_phase = Phase::Hover;
    Pick m_selection;
    Pick m_previous; // kept while a press outside it may still be a click
    int m_pressScreen = -1;
    QPoint m_pressAt;
    Handle m_handle = Handle::None;
    QRect m_startRect;
    int m_cursorScreen = -1;
    QPoint m_cursorAt;
    bool m_cursorCovered = false; // over the toolbar, not the picture, since the last move
    bool m_shiftAlone = false; // Shift pressed, and no other key, button or wheel since
    // The hover: its monitor, the picks under the cursor smallest first
    // (hitChain), and how far out the wheel went.
    int m_hoverScreen = -1;
    std::vector<QRect> m_chain;
    std::size_t m_level = 0;

    AnnotationModel m_annot;
    int m_annotScreen = -1;
    annot::StyleMemory m_styles; // before the model began, and after it ended
    bool m_stylesChanged = false;
    // Esc put the selection and its annotations down (Ctrl+Z picks them up
    // again, Esc once more ends the capture): the selection as it was.
    bool m_parked = false;
    Pick m_parkedSelection;
    bool m_toolbarHidden = false;
    bool m_toolHintSeen = false;
    KeyHintPolicy m_hintPolicy;
    QElapsedTimer m_hintClock; // the policy's clock
    QTimer m_hintTimer; // wakes it when a hint is due
    bool m_hintsShown = false;
    bool m_historyAvailable = false;
    QStringList m_keyHints;
    ColorFormat m_colorFormat = ColorFormat::Hex;
    std::vector<QRect> m_selectionHistory;
    int m_recalledSelection = -1;
    QTimer m_noticeTimer;
    QElapsedTimer m_releaseClock; // the left button's last release (diagnostics)
};

} // namespace ws::capture
