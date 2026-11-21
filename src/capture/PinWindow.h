#pragma once

#include "CaptureKeys.h"
#include "Geometry.h"
#include "PinTools.h"
#include "SmartRedact.h"

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ws::capture {

// What a pin tells its owner about the keyboard and the mouse, for its key
// card (GitHub #343, PinHintPolicy): a button down or let go, a key or the wheel.
enum class PinInput { Press, Release, Key };

// A picture left on the screen ("贴图", docs/design-capture.md 4.3): a
// native layered window (WS_EX_LAYERED, one UpdateLayeredWindow of a 32-bit
// premultiplied DIB), not a QQuickWindow. Stage 0 measured a Qt Quick window
// per pin at +13 MB of video memory and +22–37 MB of commit each, the commit
// not given back when it closed (D3D's heaps); a DIB costs the picture's
// pixels and goes away with the window.
//
// It shows the picture where it was taken, at its size in screen pixels:
// 100% is the picture's pixels copied as they are, so a pin over the place
// it came from matches the screen pixel for pixel. A pin made with
// `fitAbout` (a long screenshot, a file, the clipboard's picture) instead
// starts within the work area of the monitor at that point, and one larger
// than 90% of it is shown fitted: its own pixels are kept, so zooming in
// shows detail rather than the blur of a stored fit. Around it a border of one
// logical pixel (neutral grey, the accent while it has the focus) and a soft
// shadow drawn into the DIB (DWM draws none round a layered window). On a
// monitor at another scale the picture keeps its physical size; only the
// border and shadow follow the DPI (WM_DPICHANGED, per-monitor v2, the
// suggested rectangle ignored).
//
// The mouse drags it; the wheel (and + / -) zooms about the cursor through
// zoomSteps() (no larger than kZoomBudget pixels of window); a corner dragged
// zooms freely, the aspect ratio kept; Ctrl+wheel changes the opacity; a
// double click goes back to the size it was pinned at (100%, or the fit);
// the arrow keys move it a pixel (Shift: ten).
// R and Shift+R turn it, H and V flip it (these keys are the user's to change). T picks text out of it (the words
// are read once, then a drag takes the lines it touches and copies them).
// Space, or the middle button, collapses it to a small tile; the menu also
// collapses it to a tab on the top edge of its monitor's work area. While
// collapsed only the picture's PNG bytes and a thumbnail are kept; a click
// opens it again, to the pixel as it was. While the cursor is on it, a round
// close button waits in the corner on the side the cursor came in from (a
// click closes it). Esc closes it at once, Ctrl+C
// copies, Ctrl+S saves; the right button or the menu key asks the owner for
// the menu. Not in the taskbar or Alt+Tab (WS_EX_TOOLWINDOW).
class PinWindow {
public:
    struct Host {
        std::function<void(PinWindow*)> closed; // the window is gone: the owner deletes this later
        std::function<void(PinWindow*, POINT)> menu; // at that point (physical)
        std::function<void(PinWindow*)> copy; // Ctrl+C
        std::function<void(PinWindow*)> save; // Ctrl+S
        std::function<void(PinWindow*)> edit; // E: the editing window on this picture
        std::function<void(PinWindow*)> ocr; // O: the words in this picture
        std::function<void(PinWindow*)> ocrCopy; // Shift+C: read and copy all words in this picture (#423)
        std::function<bool()> dark; // the border's grey
        std::function<QColor()> accent; // a fill that carries white words
        // The 界面风格's (Theme.windowEdge, Theme.controlRadius in logical px);
        // empty or invalid: the grey by `dark`, 4.
        std::function<QColor()> edge;
        std::function<qreal()> pillRadius;
        std::function<void(const QString&)> copyText; // the text picked
        // Reads the lines of the picture (their corners and characters' places,
        // in its pixels); `done` is called on the GUI thread, once, or never
        // (skipped when `cancelled` turns true). Empty: no text recognition.
        std::function<void(const QImage&, double dpr, std::function<void(QList<RedactLine>)> done,
            std::shared_ptr<const std::atomic<bool>> cancelled)>
            readLines;
        // Whether the OCR reader is installed; empty: assumed available whenever readLines is set.
        std::function<bool()> ocrAvailable;
        // The edge tabs of the other pins on `monitorWork` (physical), so a new one goes beside them.
        std::function<std::vector<QRect>(const PinWindow* self)> takenTabs;
        std::function<void(PinWindow*)> released; // its picture was let go of (collapsed): memory can go back
        // The key card (#343): the keyboard focus came or went (`fromOwnWindow`:
        // it came back from another window of this program, its menu say);
        // the keys, buttons and wheel on it (moves are not told); ? or F1
        // (when F1 is not one of its keys) to show or hide the card.
        std::function<void(PinWindow*, bool focused, bool fromOwnWindow)> focus;
        std::function<void(PinWindow*, PinInput)> input;
        std::function<void(PinWindow*)> toggleHints;
    };

    // The largest window a zoom may make, in pixels (~96 MB of DIB).
    static constexpr qint64 kZoomBudget = 24'000'000;

    // Shown at once, its picture's top left at `origin` (physical, virtual
    // desktop), with the focus.
    // `fitAbout`: the point (physical) whose monitor the pin starts on, the
    // picture's pixel there kept in place when it is shown fitted; none: as
    // it was taken, at 100% where `origin` says.
    PinWindow(const QImage& image, QPoint origin, Host host, std::optional<QPoint> fitAbout = std::nullopt);
    ~PinWindow(); // destroys the window if it is still there
    PinWindow(const PinWindow&) = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    HWND hwnd() const { return m_hwnd; }
    // The whole picture (decoded from its PNG bytes while collapsed: a copy).
    QImage image() const;
    QRect pictureRect() const; // physical, virtual desktop, as shown (the tile's while collapsed)
    double dpr() const { return m_dpi / 96.0; }
    double zoom() const { return m_zoom; }
    double opacity() const { return m_opacity; }
    bool topmost() const { return m_topmost; }
    bool clickThrough() const { return m_clickThrough; }
    // Taken with the screen capture (copied, it is a screenshot in the
    // history), or a picture pinned from a file or the clipboard history.
    bool screenshot() const { return m_screenshot; }
    void setScreenshot(bool on) { m_screenshot = on; }
    // The annotated picture's editable form (opaque bytes, empty: none),
    // kept for the editing window of stage 4b.
    const QByteArray& project() const { return m_project; }
    void setProject(QByteArray project) { m_project = std::move(project); }
    // A pin of copied words (GitHub #388): what Ctrl+C copies instead of the
    // picture, until it is edited (setPicture); empty: a picture's pin.
    const QString& copiedText() const { return m_copiedText; }
    void setCopiedText(QString text) { m_copiedText = std::move(text); }

    // A new picture where this one is (the editing window's 完成): the top
    // left and the zoom stay, the window follows the new size. A null picture
    // changes nothing. A collapsed pin opens first.
    void setPicture(const QImage& image, QByteArray project);
    // 截图翻译 (GitHub #191). The picture with its words translated takes
    // the place of the one shown, which becomes the other side: Tab, or the
    // menu, turns the pin over (showOtherSide), as often as wanted. `words`:
    // the translation as text, for the menu's 复制译文. Only for a picture of
    // the size shown, in full and not changed since `revision` was read
    // (turned, flipped, edited: revision() counts those); else nothing
    // happens. Turning, flipping or editing afterwards leaves one side only.
    bool setTranslation(const QImage& translated, const QString& words, int revision);
    int revision() const { return m_revision; }
    bool hasOtherSide() const { return !m_other.isNull(); }
    bool translationShown() const { return m_translationShown; }
    const QString& translatedWords() const { return m_translatedWords; }
    void showOtherSide();
    // A word in the pill on the picture; `stay`: until the next one (正在翻译…), else for a moment.
    void note(const QString& text, bool stay = false);
    // Out of sight while it is edited, and back (it keeps its place).
    void setShown(bool shown);
    // Out of sight for 隐藏全部贴图, and back; independent of setShown.
    void setAllHidden(bool hidden);
    void restyle(); // PinManager::restyle
    bool visibleNow() const { return m_shown && !m_allHidden; }

    void setTopmost(bool on);
    // The mouse goes through it to what is under it (WS_EX_TRANSPARENT).
    // Back by turning it off in its menu, the tray's 取消所有贴图的穿透, the
    // restore hotkey, or by holding Ctrl with the cursor over it (updateReachable).
    void setClickThrough(bool on);
    // What a double click does while it is in full and no text is picked.
    void setDoubleClick(PinDoubleClick action) { m_doubleClick = action; }
    // The keys the window answers to (#187); the arrows and + / - stay fixed.
    void setKeys(const CaptureKeys& keys) { m_keys = keys; }
    // The owner's watcher calls this while any pin is click-through: a pin
    // set to click through takes the mouse while `ctrlDown` with `cursor`
    // (physical) over its picture, and lets it go again after (not while it
    // is dragged or `menuUp`). Returns true when that changed.
    bool updateReachable(bool ctrlDown, QPoint cursor, bool menuUp);
    bool reachable() const { return m_reachable; }
    bool dragging() const { return m_dragging; }
    void close(); // the window destroyed, its pixels let go of at once

    // ---- turning and flipping (the annotations of a turned picture are gone) ----
    void rotate(bool clockwise);
    void flip(bool horizontal);

    // ---- collapsing ----
    PinForm form() const { return m_form; }
    void collapse(PinForm to); // Tile or Edge
    void expand();
    // The PNG bytes kept while collapsed (empty while they are being made, or when in full).
    const QByteArray& packed() const { return m_png; }
    // The tab on the top edge, when it is one.
    std::optional<QRect> edgeTab() const;

    // ---- picking text ----
    bool autoOcr() const { return m_autoOcr; }
    void setAutoOcr(bool on); // quietly reads lines for direct mouse selection and instant copy-all
    // Supplies already-known lines (copied-text card layout or capture prescan) without re-running OCR.
    void setRecognizedLines(QList<RedactLine> lines, bool announce = false);
    bool hasRecognizedLines() const { return m_readState == ReadState::Ready; }
    int recognizedLineCount() const { return static_cast<int>(m_lines.size()); }
    bool textMode() const { return m_textMode; }
    bool canPickText() const { return m_host.readLines != nullptr; }
    void beginTextPick(); // explicit box-select mode (reuses cached lines if already read)
    void endTextPick();
    void clearSelection(); // clears direct text highlight without closing the pin
    void selectAllText(); // highlights all recognized lines (Ctrl+A)
    bool hasPickedText() const { return !m_picked.isEmpty(); }
    QString pickedText() const; // the lines picked now
    bool copyPickedText(); // copies pickedText() and shows confirmation pill
    // Copies all recognized text directly from cached lines (or runs/joins a scan);
    // returns false only when OCR is unavailable/unwired so the caller can show OCR setup.
    bool copyAllRecognizedText();
    void showCopiedAllPill();
    // The pill saying what the pick is doing: 255 while shown, 0 once it has
    // faded off the words (GitHub #258). For tests.
    int textHintAlpha() const { return m_pillAlpha; }
    QString textPillText() const;
    // The close button shown while the cursor is on it (physical, virtual desktop); empty: none now.
    QRect closeButton() const;

private:
    enum class ReadState { Unread, Reading, Ready };
    enum class PillKind { None, AutoReady, Reading, PickHint, SelectionHint, Copied, CopiedAll, NoText };
    struct EncodeJob {
        std::atomic<bool> cancelled {false};
        QByteArray bytes;
    };

    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void render(); // the whole window: shadow, border, picture
    void paintBorder(); // the border alone (the focus came or went)
    void present(); // the DIB onto the screen, at the opacity
    void setZoom(double zoom, QPoint cursor);
    void releasePixels();
    void applyClickThrough(); // WS_EX_TRANSPARENT = click-through and not reachable
    void applyVisibility();
    void moveTo(QPoint origin); // the picture's top left, window follows
    QRect windowRect() const;
    QSize fullSize() const;
    const QImage& shownImage() const; // what the window draws at the zoom shown: the picture, or the thumbnail
    double shownZoom() const { return m_form == PinForm::Full ? m_zoom : 1.0; }
    QRect workArea() const; // of the monitor nearest to the picture
    QRect workAreaAt(QPoint point) const; // of the monitor nearest to a point
    // Into the work area of the monitor at `anchor`, shown fitted
    // (m_fitZoom) when larger than 90% of it; before the window is made, so
    // it is made on that monitor.
    void fitToWorkArea(QPoint anchor);
    double minZoom() const; // the smallest step, or the fitted zoom below it
    void drawTextLayer(QImage& canvas, const QRect& picture);
    void drawHint(QImage& canvas, const QRect& picture); // the zoom or opacity, a moment after it changes
    void showHint(const QString& text); // (re)starts the moment
    void forgetOtherSide(); // the picture is another now: one side, the next revision
    void showPill(PillKind kind); // the text pill (drawTextLayer): (re)starts its moment
    void dismissAutoReadyPill();
    void hintTick(); // the moments' timer: fades the hint and the pill out
    void startReadLines(bool background);
    void maybeStartAutoOcr();
    void cancelPendingScan();
    void resetTextInteraction();
    void invalidateLines();
    bool ocrAvailable() const;
    bool canDirectSelect() const;
    double textHitPadding() const;
    bool hitText(QPoint screen) const;
    void refreshCursor();
    void updatePicked(QPoint cursor);
    void updatePickedFlow(QPoint cursor);
    void finishEncode(const EncodeJob* job);
    QPoint toImage(QPoint screen) const; // a screen point in the picture's pixels
    QPointF toImageF(QPoint screen) const;

    HWND m_hwnd = nullptr;
    Host m_host;
    QImage m_image; // Format_RGB32, alpha forced opaque when drawn; null while collapsed and packed
    QImage m_scaled; // the picture at m_zoom when that is not 1
    QSize m_imageSize; // the picture's size, whether or not m_image is there
    QPoint m_origin; // the picture's top left, physical (the tile's while collapsed)
    double m_zoom = 1.0;
    double m_fitZoom = 1.0; // the zoom it was pinned at: below 1 for a picture shown fitted (复位 goes back to it)
    double m_opacity = 1.0;
    UINT m_dpi = 96;
    PinFrame m_frame;
    bool m_topmost = true;
    bool m_clickThrough = false;
    bool m_reachable = false; // click-through, but Ctrl is held over it
    bool m_screenshot = true;
    QString m_copiedText; // copiedText()
    bool m_shown = true;
    bool m_allHidden = false;
    QByteArray m_project;
    QImage m_other; // the side not shown (the original under its translation, or the other way round); null: none
    QString m_translatedWords;
    bool m_translationShown = false;
    int m_revision = 0;
    bool m_hintStays = false;
    bool m_dragging = false;
    QPoint m_grab; // the cursor less m_origin at the press
    int m_wheel = 0; // wheel delta not yet a whole notch (touchpads)
    // "150%" in a pill on the picture for a moment after a zoom (#164).
    QString m_hint;
    qint64 m_hintSince = 0; // steady clock, ms
    int m_hintAlpha = 0; // 0: not drawn
    // The text pill (lower left): auto-ready, reading, pick/selection hint, or copy feedback.
    // Fades off the words after a moment (GitHub #258); never drawn while a drag is under way.
    PillKind m_pillKind = PillKind::None;
    qint64 m_pillSince = 0;
    int m_pillAlpha = 0;
    // The close button while the cursor is on the picture: in the corner on
    // the side it came in from (None: not shown), lit while the cursor is on
    // it, pressed while the left button went down on it.
    void drawCloseButton(QImage& canvas, const QRect& button);
    void updateCloseHover(QPoint cursor); // the cursor moved: which corner, lit or not
    bool m_tracking = false; // TrackMouseEvent asked for WM_MOUSELEAVE
    Corner m_closeCorner = Corner::None;
    bool m_closeHot = false;
    bool m_closePressed = false;
    // Dragging a corner: which, and the picture as it was at the press.
    Corner m_corner = Corner::None;
    QRect m_resizeStart;
    // Collapsed: its form, the thumbnail, where it was in full, the encode.
    PinForm m_form = PinForm::Full;
    PinDoubleClick m_doubleClick = PinDoubleClick::Reset;
    CaptureKeys m_keys;
    bool m_clickOpened = false; // this click opened a tile or tab: its double click does nothing more
    bool m_swallowUp = false; // a double click collapsed it: its button up must not open it again
    QImage m_thumb;
    QByteArray m_png;
    QPoint m_savedOrigin;
    double m_savedZoom = 1.0;
    QPoint m_tileAtCollapse;
    QPoint m_press; // where the left button went down (a click on a tile opens it)
    bool m_moved = false;
    std::shared_ptr<EncodeJob> m_encode;
    // Picking text & cached OCR layer.
    bool m_autoOcr = false;
    bool m_textMode = false; // explicit box-select mode (T)
    bool m_autoScan = false; // current scan started silently in background
    bool m_copyAllWhenReady = false; // Shift+C asked while scan was in flight
    ReadState m_readState = ReadState::Unread;
    QList<RedactLine> m_lines;
    ReadState m_otherReadState = ReadState::Unread;
    QList<RedactLine> m_otherLines;
    QList<PickedLine> m_picked;
    bool m_picking = false; // a drag under way
    bool m_pickDirect = false; // direct flowing drag on text (not explicit box mode)
    bool m_pickDragged = false; // direct drag moved past threshold
    QPoint m_pickStart; // screen, physical
    QRect m_pickBand; // the drag, in picture pixels (explicit box mode)
    std::shared_ptr<std::atomic<bool>> m_scanCancel;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // the reading's answer finds this pin gone
    // The DIB the window is drawn from, selected into m_dc.
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
    uchar* m_bits = nullptr;
    QSize m_dibSize;
};

} // namespace ws::capture
