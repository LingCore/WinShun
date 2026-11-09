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

// A picture left on the screen ("贴图", docs/design-capture.md 4.3): a
// native layered window (WS_EX_LAYERED, one UpdateLayeredWindow of a 32-bit
// premultiplied DIB), not a QQuickWindow. Stage 0 measured a Qt Quick window
// per pin at +13 MB of video memory and +22–37 MB of commit each, the commit
// not given back when it closed (D3D's heaps); a DIB costs the picture's
// pixels and goes away with the window.
//
// It shows the picture where it was taken, at its size in screen pixels:
// 100% is the picture's pixels copied as they are, so a pin over the place
// it came from matches the screen pixel for pixel. A picture larger than the
// screen (a long screenshot) is shown fitted instead - its own pixels are
// kept, so zooming in shows detail rather than the blur of a stored fit.
// Around it a border of one
// logical pixel (neutral grey, the accent while it has the focus) and a soft
// shadow drawn into the DIB (DWM draws none round a layered window). On a
// monitor at another scale the picture keeps its physical size; only the
// border and shadow follow the DPI (WM_DPICHANGED, per-monitor v2, the
// suggested rectangle ignored).
//
// The mouse drags it; the wheel (and + / -) zooms about the cursor through
// zoomSteps() (no larger than kZoomBudget pixels of window); a corner dragged
// zooms freely, the aspect ratio kept; Ctrl+wheel changes the opacity; a
// double click goes back to 100%; the arrow keys move it a pixel (Shift: ten).
// R and Shift+R turn it, H and V flip it (these keys are the user's to change). T picks text out of it (the words
// are read once, then a drag takes the lines it touches and copies them).
// Space, or the middle button, collapses it to a small tile; the menu also
// collapses it to a tab on the top edge of its monitor's work area. While
// collapsed only the picture's PNG bytes and a thumbnail are kept; a click
// opens it again, to the pixel as it was. Esc closes it at once, Ctrl+C
// copies, Ctrl+S saves; the right button or the menu key asks the owner for
// the menu. Not in the taskbar or Alt+Tab (WS_EX_TOOLWINDOW).
class PinWindow {
public:
    struct Host {
        std::function<void(PinWindow*)> closed; // the window is gone: the owner deletes this later
        std::function<void(PinWindow*, POINT)> menu; // at that point (physical)
        std::function<void(PinWindow*)> copy; // Ctrl+C
        std::function<void(PinWindow*)> save; // Ctrl+S
        std::function<bool()> dark; // the border's grey
        std::function<QColor()> accent; // a fill that carries white words
        // The 界面风格's (Theme.windowEdge, Theme.controlRadius in logical px);
        // empty or invalid: the grey by `dark`, 4.
        std::function<QColor()> edge;
        std::function<qreal()> pillRadius;
        std::function<void(const QString&)> copyText; // the text picked
        // Reads the lines of the picture (their corners and characters' places,
        // in its pixels); `done` is called on the GUI thread, once, or never.
        // Empty: no text recognition.
        std::function<void(const QImage&, double dpr, std::function<void(QList<RedactLine>)> done)> readLines;
        // The edge tabs of the other pins on `monitorWork` (physical), so a new one goes beside them.
        std::function<std::vector<QRect>(const PinWindow* self)> takenTabs;
        std::function<void(PinWindow*)> released; // its picture was let go of (collapsed): memory can go back
    };

    // The largest window a zoom may make, in pixels (~96 MB of DIB).
    static constexpr qint64 kZoomBudget = 24'000'000;

    // Shown at once, its picture's top left at `origin` (physical, virtual
    // desktop), with the focus.
    PinWindow(const QImage& image, QPoint origin, Host host);
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

    // A new picture where this one is (the editing window's 完成): the top
    // left and the zoom stay, the window follows the new size. A null picture
    // changes nothing. A collapsed pin opens first.
    void setPicture(const QImage& image, QByteArray project);
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
    bool textMode() const { return m_text != Text::Off; }
    bool canPickText() const { return m_host.readLines != nullptr; }
    void beginTextPick(); // reads the picture's words, then a drag picks lines
    void endTextPick();
    QString pickedText() const; // the lines picked now

private:
    enum class Text { Off, Reading, Ready };
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
    // A picture larger than the screen keeps its own pixels and is shown
    // fitted to it (m_fitZoom): its middle stays where it was, and it comes
    // in from an edge it still hangs over.
    void fitToWorkArea();
    double minZoom() const; // the fitted zoom of a picture shown fitted, else the smallest step
    void drawTextLayer(QImage& canvas, const QRect& picture);
    void drawHint(QImage& canvas, const QRect& picture); // the zoom or opacity, a moment after it changes
    void showHint(const QString& text); // (re)starts the moment
    void hintTick(); // the moment's timer: fades the hint out
    void updatePicked(QPoint cursor);
    void finishEncode(const EncodeJob* job);
    QPoint toImage(QPoint screen) const; // a screen point in the picture's pixels

    HWND m_hwnd = nullptr;
    Host m_host;
    QImage m_image; // Format_RGB32, alpha forced opaque when drawn; null while collapsed and packed
    QImage m_scaled; // the picture at m_zoom when that is not 1
    QSize m_imageSize; // the picture's size, whether or not m_image is there
    QPoint m_origin; // the picture's top left, physical (the tile's while collapsed)
    double m_zoom = 1.0;
    double m_fitZoom = 1.0; // the zoom a picture larger than the screen was shown at
    double m_opacity = 1.0;
    UINT m_dpi = 96;
    PinFrame m_frame;
    bool m_topmost = true;
    bool m_clickThrough = false;
    bool m_reachable = false; // click-through, but Ctrl is held over it
    bool m_screenshot = true;
    bool m_shown = true;
    bool m_allHidden = false;
    QByteArray m_project;
    bool m_dragging = false;
    QPoint m_grab; // the cursor less m_origin at the press
    int m_wheel = 0; // wheel delta not yet a whole notch (touchpads)
    // "150%" in a pill on the picture for a moment after a zoom (#164).
    QString m_hint;
    qint64 m_hintSince = 0; // steady clock, ms
    int m_hintAlpha = 0; // 0: not drawn
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
    // Picking text.
    Text m_text = Text::Off;
    QList<RedactLine> m_lines;
    QList<PickedLine> m_picked;
    bool m_picking = false; // a drag under way
    QPoint m_pickStart; // screen, physical
    QRect m_pickBand; // the drag, in picture pixels
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // the reading's answer finds this pin gone
    // The DIB the window is drawn from, selected into m_dc.
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
    uchar* m_bits = nullptr;
    QSize m_dibSize;
};

} // namespace ws::capture
