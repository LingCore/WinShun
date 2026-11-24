#pragma once

#include <QColor>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <windows.h>

namespace ws::capture {

// Preferred edge of a pin's picture for a floating status pill.
enum class PinPillEdge { Top, Bottom };

// Where a pin's floating pill goes (physical, virtual desktop).
struct PinPillPlace {
    QRect rect; // the pill body without its shadow
    bool inside = false; // fell back inside the picture (no room on any edge of the work area)
};

// Pure geometry solver for placing a floating status pill around a pin's
// `picture` (with `border` px frame) inside `bounds` (the monitor's work area):
//  - Top prefers: above -> below -> right -> left -> inside top-left.
//  - Bottom prefers: below -> above -> right -> left -> inside bottom-left.
// On top/bottom edges it aligns with the pin's left edge (clamped horizontally
// into `bounds`) and shifts horizontally around `avoid` (another pill) if
// needed. Only when all four outer edges are blocked by `bounds` does it fall
// back inside `picture` (intersected with `bounds`), avoiding `closeButton`
// and `avoid`.
PinPillPlace placePinPill(const QRect& picture, int border, QSize pill, const QRect& bounds, PinPillEdge preferred,
    int gap, int inset, const QRect& avoid = {}, const QRect& closeButton = {});

// A lightweight native layered pill owned by a PinWindow: floats outside the
// picture so translation, zoom, opacity and text-pick hints never cover small
// pins (and stay legible on tiny or low-opacity pins). Takes neither focus nor
// mouse input (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
// WS_EX_NOACTIVATE).
class PinPill {
public:
    struct Look {
        QColor fill {0x0F, 0x6C, 0xBD};
        qreal radius = 4.0; // logical px

        bool operator==(const Look&) const = default;
    };

    PinPill() = default;
    ~PinPill();
    PinPill(const PinPill&) = delete;
    PinPill& operator=(const PinPill&) = delete;

    // Shows or updates the pill at `body` (physical px, without shadow), owned
    // by `owner`, with `alpha` (0..255). Redraws the DIB only when the text,
    // scale, look or window size changes; simple moves only reposition.
    void show(HWND owner, bool topmost, const QString& text, const QRect& body, double dpr, const Look& look, int alpha);
    void setAlpha(int alpha); // fast-path fade without re-rendering the DIB
    void setTopmost(HWND owner, bool topmost);
    void hide();
    void destroy(); // called when the owner HWND is going away

    bool visible() const;
    HWND hwnd() const { return m_hwnd; }
    QRect rect() const { return visible() ? m_body : QRect(); } // body without shadow (physical)

    // Pill body size without shadow (physical px), shadow margin, and offscreen
    // picture with shadow (for tests and visual previews).
    static QSize pillSize(const QString& text, double dpr);
    static int shadowMargin(double dpr);
    static QImage picture(const QString& text, double dpr, const Look& look);

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static void paint(QImage& canvas, const QString& text, double dpr, const Look& look);
    bool ensureWindow(HWND owner, bool topmost);
    bool ensureSurface(QSize windowSize);
    void releaseSurface();
    void syncZOrder(HWND owner, bool topmost);
    void present(int alpha);

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    QRect m_body; // physical, without shadow
    QRect m_windowRect; // physical, with shadow
    QString m_text;
    double m_dpr = 0.0;
    Look m_look;
    int m_alpha = 0;
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
    uchar* m_bits = nullptr;
    QSize m_dibSize;
};

} // namespace ws::capture
