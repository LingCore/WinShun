#pragma once

#include "AnnotationModel.h"
#include "CaptureTools.h"
#include "Geometry.h"
#include "KeyHintPolicy.h"
#include "SizePresets.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QTimer>
#include <QVariantList>
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
    // What a click would pick, and the selection; null when none here. One over
    // several monitors (GitHub #409) is given whole, in this monitor's own
    // pixels, beyond its edges: what lies elsewhere is cut by the window.
    Q_PROPERTY(QRect hover READ hover NOTIFY changed FINAL)
    Q_PROPERTY(QRect selection READ selection NOTIFY changed FINAL)
    // The size, the toolbar and the words of what is picked or selected are on
    // this monitor: its own, or for one over several monitors the monitor of
    // its bottom right corner (the pointer's while picking).
    Q_PROPERTY(bool home READ home NOTIFY changed FINAL)
    // The annotations' picture in this monitor's own physical pixels: the
    // monitor itself, or for a selection over several monitors the picture of
    // the whole desktop, which reaches beyond it.
    Q_PROPERTY(QRect annotationArea READ annotationArea NOTIFY changed FINAL)
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
    // The round tip of the tool under the pointer, screen pixels across (0: none), and its colour:
    // the pointer shows it in place of the cross (CapturePointer, GitHub #461).
    Q_PROPERTY(qreal brushSize READ brushSize NOTIFY cursorChanged FINAL)
    Q_PROPERTY(QColor brushColor READ brushColor NOTIFY cursorChanged FINAL)
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
    bool home() const { return m_home; }
    QRect annotationArea() const { return m_annotationArea.isNull() ? bounds() : m_annotationArea; }
    bool dimmed() const { return m_dimmed; }
    bool adjusting() const { return m_adjusting; }
    bool magnifierShown() const { return m_magnifierShown; }
    bool cursorHere() const { return m_cursorHere; }
    QPoint cursor() const { return m_cursor; }
    QImage loupe() const; // sampled lazily, once per pointer position and frozen image
    QString color() const { return m_color; }
    QString colorText() const { return m_colorText; }
    int cursorShape() const { return m_cursorShape; }
    qreal brushSize() const { return m_brushSize; }
    QColor brushColor() const { return QColor::fromRgba(m_brushColor); }
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
    void setState(const QRect& hover, const QRect& selection, bool dimmed, bool adjusting, bool magnifierShown,
        bool home = false, const QRect& annotationArea = {});
    void setCursor(bool here, QPoint at, int shape, qreal brushSize = 0, QRgb brushColor = 0);
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
    bool m_home = false;
    QRect m_annotationArea; // null: the monitor itself
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
    qreal m_brushSize = 0;
    QRgb m_brushColor = 0;
    QString m_flash;
    QString m_notice;
};

// A capture from the frozen screens to the picture taken (or none): what
// the mouse and keys do. Moving over the screens highlights the window or
// control a click picks (the wheel steps out to the window and the whole
// screen and back; Tab before a selection switches to whole windows only and
// back, GitHub #360); a drag draws a selection; with one, its eight handles
// resize it and a drag inside moves it; the arrow keys move it a physical
// pixel (Shift: resize), and WASD (the arrows too, before a selection) moves
// the mouse pointer a physical pixel (Shift: ten) in every phase, a held-button drag included. R recalls
// recent regions on this picture; ? (or F1) shows or hides the key hints.
// Enter or a double click copies, Ctrl+S saves, F3 pins it
// to the screen; Esc or a right click drops the selection, or with none ends
// the capture; C copies the colour under the cursor. A selection is on one
// monitor until a drag (drawing it, an edge, moving it) or the wheel (past the
// whole screen) takes it over more than one (GitHub #409): it is then on the
// whole desktop's picture (kDesktop), annotations and all. F opens the size panel (CaptureSizeBox.qml, GitHub #308): a
// width and a height typed in, a shape the selection is then held to
// (ratioLocked: drags, handles and Shift with the arrows keep it), and the
// sizes and shapes kept to pick from, before a selection too. With a selection, the annotation tools (AnnotationModel) draw
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
    // 识别屏幕文字 (#198): this capture only picks a region for its words; no
    // toolbar, no annotations, no size label.
    Q_PROPERTY(bool ocrOnly READ ocrOnly NOTIFY ocrOnlyChanged FINAL)
    // One-shot quick picking (OCR, translate, or capture-and-pin #450): drag or
    // click finishes at once with no toolbar.
    Q_PROPERTY(bool quickPicking READ quickPicking NOTIFY quickModeChanged FINAL)
    // The size panel is open (F, or a click on the size by the selection):
    // no magnifier and no key hints meanwhile; a press on the picture closes it.
    Q_PROPERTY(bool sizeEditing READ sizeEditing WRITE setSizeEditing NOTIFY sizeEditingChanged FINAL)
    // The selection keeps the shape `ratio` (width : height) however it is
    // drawn or resized; for this capture only.
    Q_PROPERTY(bool ratioLocked READ ratioLocked NOTIFY ratioChanged FINAL)
    Q_PROPERTY(QSize ratio READ ratio NOTIFY ratioChanged FINAL)
    // The sizes and shapes to pick from, in order: {label, ratio, width, height}.
    Q_PROPERTY(QVariantList sizePresets READ sizePresetList NOTIFY sizePresetsChanged FINAL)
    Q_PROPERTY(bool sizePresetsFull READ sizePresetsFull NOTIFY sizePresetsChanged FINAL)
    // 只检测窗口 (GitHub #360): the hover picks whole top-level windows (and the
    // screen), not the controls in them. Tab switches it before a selection;
    // kept between captures ([Capture] WindowsOnly).
    Q_PROPERTY(bool windowsOnly READ windowsOnly NOTIFY windowsOnlyChanged FINAL)

public:
    // Pin: left on the screen where it was (PinWindow); Ocr: its words read (CaptureOcr);
    // OcrCopy, OcrSearch and OcrWeb read them and go on at once: to the clipboard,
    // the launcher's search box, the default web search (the mouse buttons' choices).
    // Long: the selection scrolls into a long screenshot (Ctrl+L; LongCapture),
    // one at least kLongMinimum px each way. Qr: the QR codes in it are read
    // (Q); Delay: this capture ends and the next begins after the countdown (Ctrl+Shift+D).
    // Save: into the chosen save folder, or Save As when none was chosen (#194);
    // SavePrevious: into the folder without a picker; SaveAs: always the picker.
    // OcrTranslate: pinned where it is, its words translated in place (GitHub #191).
    enum class Action { Copy, Save, Cancel, Pin, Ocr, OcrCopy, OcrSearch, OcrWeb, Long, Qr, Delay, CopyPin, Record, SavePrevious, SaveAs, OcrTranslate };
    Q_ENUM(Action)
    // One-shot picking modes: a drag or click finishes immediately without the toolbar.
    // Ocr: 识别屏幕文字 / 截图翻译 (#198, #356); Pin: 截图并直接贴图 (#450).
    // Not a Q_ENUM: QML reads enum keys unscoped (CaptureSession.Pin), so its
    // Ocr and Pin took over the toolbar's Action ones (1.1.33: 识别文字 saved,
    // 贴图 cancelled). Every Q_ENUM key here must be unique (tst_capture).
    enum class QuickMode { None, Ocr, Pin };
    static constexpr int kLongMinimum = 48;
    static constexpr int kSelectionHistoryLimit = 10;
    // The "screen" of a selection over more than one monitor (GitHub #409):
    // its rectangle is in the pixels of the whole desktop's picture
    // (desktopRect(), 0,0 at its top left). Only with two monitors or more.
    static constexpr int kDesktop = 1000;

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
    int selectionScreen() const { return m_selection.screen; } // a monitor's index, or kDesktop
    QRect selectionRect() const { return m_selection.rect; } // physical, that monitor's own (or the desktop picture's)
    // The monitors' bounding box (physical, virtual desktop) and its picture:
    // each monitor's frozen picture in its place, black where none is. Made
    // the first time a capture asks for it; null with one monitor.
    QRect desktopRect() const { return m_desktop.rect(); }
    QImage desktopPicture() const;
    // `rect` on `screen` (a monitor's index or kDesktop), physical, virtual
    // desktop; null for no such screen.
    QRect virtualRect(int screen, const QRect& rect) const;
    // The monitor where the size, the toolbar and the words of `rect` on
    // `screen` go (CaptureScreen::home); -1 when it is on none.
    int homeOf(int screen, const QRect& rect) const;
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
    // One-shot picking (set before begin(), cleared on end()): a drag let go
    // of, or a click on a window or control, finishes at once (Ocr -> OcrCopy,
    // Pin -> Pin); Esc and the right button end the capture; the keys that
    // would annotate, save or bring back a picture do nothing.
    void setQuickMode(QuickMode mode);
    QuickMode quickMode() const { return m_quickMode; }
    bool quickPicking() const { return m_quickMode != QuickMode::None; }
    void setOcrOnly(bool on) { setQuickMode(on ? QuickMode::Ocr : QuickMode::None); }
    bool ocrOnly() const { return m_quickMode == QuickMode::Ocr; }
    // 只检测窗口 (#360): the setting, or Tab (which also says so by the pointer's
    // monitor). The hover is picked afresh at once, from the most specific of
    // the new kind; a capture beginning or ending leaves it as it is.
    void setWindowsOnly(bool on);
    bool windowsOnly() const { return m_windowsOnly; }

    // ---- the size panel (#308) ----
    bool sizeEditing() const { return m_sizeEditing; }
    void setSizeEditing(bool on);
    bool ratioLocked() const { return !m_ratio.isEmpty(); }
    QSize ratio() const { return m_ratio; }
    // The selection made `width` x `height` (its top left stays where the
    // monitor has room); with none, one of that size about the pointer. Held
    // to the shape when locked: the side along `lead` (a Qt::Orientation, the
    // number typed) is kept and the other follows.
    Q_INVOKABLE void setSize(int width, int height, int lead = Qt::Horizontal);
    // The shape `width` : `height` held from now on (the panel's lock; the
    // numbers it shows); 0 either way lets go of it.
    Q_INVOKABLE void lockRatio(int width, int height);
    // One of sizePresets: a size is given to the selection (or makes one
    // about the pointer), a shape is held and the selection made to fit it.
    Q_INVOKABLE void applySizePreset(int index);
    // The list itself: `width` x `height` at its end, or one taken out
    // (sizePresetsEdited tells the settings).
    Q_INVOKABLE void addSizePreset(int width, int height);
    Q_INVOKABLE void removeSizePreset(int index);
    void setSizePresets(std::vector<SizePreset> presets);
    const std::vector<SizePreset>& sizePresets() const { return m_sizePresets; }
    QVariantList sizePresetList() const;
    bool sizePresetsFull() const { return m_sizePresets.size() >= kSizePresetLimit; }

    // What the mouse buttons do once a selection is there (issue #46), as
    // Settings keeps them: none | copy | save | pin | ocr | ocrcopy | ocrsearch |
    // ocrweb | cancel, the right button also back. With an annotation tool on too (no stepping out of
    // it first); only in the middle of drawing (a stroke, an open line of several points) the right
    // button steps back like Esc and the rest do nothing.
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
    // put down with Esc; kDesktop over several), -1 when there are none.
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
    //   choosing: wheel, tab (windowsOnly), wasd, size, recall (some remembered),
    //             history (setHistoryAvailable), ocr (ocrAvailable), quit
    //             (识别屏幕文字: wheel, tab, wasd, quit)
    //   selected: copy, save, pin, arrows, ocr, long, recalled (R brought it back)
    //   tool:     shift (shapes, lines and strokes), width, undo, delete, toolOff, hide
    // and none while busy.
    QStringList keyHintRows() const;
    QStringList keyHints() const { return m_keyHints; }
    bool keyHintsEnabled() const { return m_hintPolicy.enabled(); }
    bool keyHintsShown() const { return m_hintsShown; }
    void setKeyHints(bool on); // the setting
    // The pin and save keys the user chose (#187), for the text box too.
    void setKeys(const CaptureKeys& keys);
    const CaptureKeys& keys() const { return m_annot.keys(); }
    // A save folder was chosen ([Capture] SaveFolder): Save needs no picker (#194).
    void setSaveDirect(bool on) { m_annot.setSaveDirect(on); }
    bool saveDirect() const { return m_annot.saveDirect(); }
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
    // coordinates; for kDesktop the desktop picture's); nothing for Cancel.
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
    void ocrOnlyChanged();
    void quickModeChanged();
    void sizeEditingChanged();
    void ratioChanged();
    void sizePresetsChanged();
    void sizePresetsEdited(); // by the user, in the panel: [Capture] SizePresets
    void windowsOnlyChanged(bool on); // Tab, or the setting: [Capture] WindowsOnly

private:
    // Annotating: a drawing, moving or reshaping gesture of the model's is
    // under way. Not Adjusting: the toolbar stays (it steps aside by itself).
    enum class Phase { Hover, Pressed, Dragging, Selected, Adjusting, Annotating };
    struct Pick {
        int screen = -1;
        QRect rect;
    };

    CaptureScreen* screenAt(int index) const; // kDesktop: the desktop's (with two monitors or more)
    QPoint clampTo(int screen, QPoint p) const;
    // ---- over several monitors (GitHub #409) ----
    // Input on monitor `screen` reaches what is in `space` (that monitor, or the desktop).
    bool inSpace(int screen, int space) const;
    // `p` on monitor `screen` in the pixels of `space` (a monitor or kDesktop).
    QPoint toSpace(int screen, QPoint p, int space) const;
    bool onMonitor(QPoint global) const; // physical, virtual desktop
    // A rectangle of the desktop's picture: on the monitor that holds it
    // whole, else on the desktop.
    Pick fromDesktop(const QRect& rect) const;
    // `pick` on its own monitor when one holds it whole: a selection with no
    // annotations, after it was sized or moved.
    Pick settled(const Pick& pick) const;
    // A selection drawn on monitor `screen` from `from` to `to` (its pixels,
    // `to` beyond its edges too): on it while the pointer is, else over the
    // desktop when the pointer is on another monitor.
    Pick drawn(int screen, QPoint from, QPoint to) const;
    QImage pictureOf(int screen) const; // a monitor's frozen picture, or the desktop's
    void updateHover(int screen, QPoint p);
    void updateCursor(int screen, QPoint p);
    void refreshCursor();
    void publish(); // the state onto the screens
    void finish(Action action);
    void recallSelection(bool previous); // R: older; Shift+R: newer
    void nudgeSelection(int dx, int dy, bool shift); // Shift: resize a selection, ten pixels for the pointer
    void movePointer(int dx, int dy); // WASD: the mouse pointer, whatever the phase
    // Tab or Shift+Tab: true when it is the picking's (no selection, no size
    // panel); then, when resting, windowsOnly switched and the switch said.
    bool switchPicking(bool plain);
    void placeSize(QSize size); // setSize's, the shape already seen to
    void setRatio(QSize ratio); // empty: none held
    bool resting() const { return m_phase == Phase::Hover || m_phase == Phase::Selected; } // no button held
    void back(); // Esc, right click
    enum class MouseAct { None, Copy, Save, Pin, Ocr, Cancel, Back, OcrCopy, OcrSearch, OcrWeb, CopyPin, OcrTranslate };
    static MouseAct parseMouseAct(const QString& name, MouseAct fallback);
    bool buttonMayAct() const; // a mouse button's action from the settings may run now
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
    // `tool`: whether the shape is the annotations' (their tool's, an object's), not the selection's.
    int cursorShapeAt(int screen, QPoint p, bool* tool = nullptr) const;
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
    QuickMode m_quickMode = QuickMode::None;
    bool m_windowsOnly = false;
    bool m_switchNotice = false; // the notice up is Tab's (switchPicking): a press that picks puts it away
    bool m_sizeEditing = false;
    QSize m_ratio; // the shape held; empty: none
    std::vector<SizePreset> m_sizePresets;
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
    int m_startScreen = -1; // m_startRect's (the selection's when the press began)
    // The whole desktop (GitHub #409): its rectangle, set at begin(), and its
    // picture, made when first asked for.
    mutable CaptureScreen m_desktop {kDesktop};
    bool m_hoverAll = false; // the wheel went past the whole screen: every monitor at once
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
