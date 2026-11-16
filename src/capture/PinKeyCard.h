#pragma once

#include "CaptureKeys.h"

#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <windows.h>

#include <vector>

namespace ws::capture {

// The keys of a pin in a card at the corner of its monitor (GitHub #343):
// what the capture's corner list (CaptureKeyHints.qml) is to a capture.

// When it shows by itself. Times are the caller's clock, ms; PinManager
// feeds it and wakes it at nextDeadline(). The rules:
//  1. A pin taking the keyboard focus begins a round: kRestMs after the
//     last key, wheel notch or button let go, with no button held, the card
//     shows. Moving the mouse leaves that alone.
//  2. Shown, a button pressed, a key or the wheel takes it away, and so does
//     kShownMs with none of them. Once per round, so it never nags.
//  3. The focus going hides it. The same pin taking it back from one of
//     the app's own windows (its menu, the editing window), or told again
//     that it has it, goes on with the round rather than beginning one: a
//     card that was showing shows again for the rest of its time. A key, a
//     button, the wheel, kShownMs or stop() end a showing; the focus coming
//     and going by itself never does.
//  4. ? or F1: shown until pressed again or a button pressed, with no
//     timer (hidden while the focus is away, rule 3); hidden so, nothing
//     more by itself this round.
//  5. Off in the settings ([Capture] KeyHints): nothing by itself; ? still
//     shows it.
//  6. stop(): collapsed, edited, closed: gone for this round.
class PinHintPolicy {
public:
    static constexpr qint64 kRestMs = 400;
    static constexpr qint64 kShownMs = 8000;

    void setEnabled(bool on)
    {
        m_enabled = on;
        if (!on)
            m_auto = false;
    }
    bool enabled() const { return m_enabled; }
    void focus(qint64 now, bool resume) // rules 1 and 3
    {
        m_focused = true;
        m_down = false;
        m_since = now;
        if (!resume) {
            m_auto = false;
            m_done = false;
            m_manual = Manual::None;
        }
    }
    void blur() // rule 3: hidden (shown() needs the focus), what showed kept for the same pin's return
    {
        m_focused = false;
        m_down = false;
    }
    void press(qint64 now) // a button down: rules 2 and 4
    {
        if (shown()) {
            m_auto = false;
            m_manual = Manual::None;
            m_done = true;
        }
        m_down = true;
        m_since = now;
    }
    void release(qint64 now)
    {
        m_down = false;
        m_since = now;
    }
    void input(qint64 now) // a key, the wheel: rule 2 (what ? showed stays)
    {
        if (m_auto) {
            m_auto = false;
            m_done = true;
        }
        m_since = now;
    }
    void toggle() // rule 4
    {
        if (shown()) {
            m_auto = false;
            m_manual = Manual::Hidden;
            m_done = true;
        } else {
            m_manual = Manual::Shown;
        }
    }
    void stop() // rule 6
    {
        m_auto = false;
        m_manual = Manual::None;
        m_done = true;
    }
    // The timers due by now (rules 1 and 2).
    void tick(qint64 now)
    {
        if (m_auto && now - m_shownAt >= kShownMs)
            m_auto = false;
        if (m_auto || !m_focused || !m_enabled || m_done || m_down || m_manual != Manual::None)
            return;
        if (now - m_since >= kRestMs) {
            m_auto = true;
            m_done = true;
            m_shownAt = now;
        }
    }
    // When tick() has something to do next; -1: nothing to wait for.
    qint64 nextDeadline() const
    {
        if (m_auto)
            return m_shownAt + kShownMs;
        if (!m_focused || !m_enabled || m_done || m_down || m_manual != Manual::None)
            return -1;
        return m_since + kRestMs;
    }
    bool shown() const { return m_focused && (m_manual == Manual::None ? m_auto : m_manual == Manual::Shown); }

private:
    enum class Manual { None, Shown, Hidden };

    bool m_enabled = true; // the setting
    bool m_focused = false;
    bool m_down = false; // a button held
    bool m_auto = false; // shown by the timer
    bool m_done = false; // this round's showing is over (or was given up)
    qint64 m_since = 0; // the round's start, or the last input
    qint64 m_shownAt = 0;
    Manual m_manual = Manual::None;
};

// A row of the card: its keycaps (alternatives, "/" between) and what they do.
struct PinKeyRow {
    QStringList keys;
    QString text;
};

// The card's rows for a pin: the user's keys, in the menu's words, and the
// wheel's; a row whose keys are all unset is left out. `ocr`: text
// recognition is there (识别文字, 选择文字); `otherSide`: a translated pin
// (Tab turns it over), `translationShown`: the side it shows.
std::vector<PinKeyRow> pinKeyRows(const CaptureKeys& keys, bool ocr, bool otherSide, bool translationShown);

// Where the card goes (physical): the bottom-left corner of `work`, or the
// bottom-right one when `avoid` (the pin) comes within `gap` of it there;
// empty when it does at both, as the capture's does.
QRect pinKeyCardPlace(QSize card, const QRect& work, const QRect& avoid, int margin, int gap);

// The card itself: a small native layered window, drawn into a DIB like a
// pin (nothing of Qt Quick), that takes neither the focus nor a click: the
// pointer goes through it to what is under it.
class PinKeyCard {
    Q_DECLARE_TR_FUNCTIONS(ws::capture::PinKeyCard)

public:
    // Its colours (the 界面风格's, as the capture's card: the menu's body and
    // edge, the words, the line above the footer, a keycap's outline and
    // words) and corners, logical px.
    struct Look {
        QColor background;
        QColor edge;
        QColor text;
        QColor subtext;
        QColor faint;
        QColor divider;
        QColor keyEdge;
        QColor keyText;
        QColor shadow;
        qreal radius = 8;
        qreal keyRadius = 4;
        static Look fallback(bool dark);
    };

    PinKeyCard() = default;
    ~PinKeyCard();
    PinKeyCard(const PinKeyCard&) = delete;
    PinKeyCard& operator=(const PinKeyCard&) = delete;

    // Shown (or drawn again where it is) in a corner of `work` (physical,
    // virtual desktop) at `dpr`, by pinKeyCardPlace(); false, and hidden,
    // when the pin is in the way at both.
    bool show(const std::vector<PinKeyRow>& rows, const QRect& work, const QRect& avoid, double dpr, const Look& look);
    void hide();
    bool visible() const;
    HWND hwnd() const { return m_hwnd; }
    QRect rect() const { return m_rect; } // the card without its shadow, physical

    // The card's size without its shadow (device px), and what it shows,
    // shadow round it, off screen (tests, previews).
    static QSize cardSize(const std::vector<PinKeyRow>& rows, double dpr);
    static QImage picture(const std::vector<PinKeyRow>& rows, double dpr, const Look& look);
    static int shadowMargin(double dpr);

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static void paint(QImage& canvas, const std::vector<PinKeyRow>& rows, double dpr, const Look& look);
    bool create();
    void present(BYTE alpha);

    HWND m_hwnd = nullptr;
    QRect m_rect;
    QRect m_windowRect;
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
    uchar* m_bits = nullptr;
    int m_alpha = 0; // the fade in
};

} // namespace ws::capture
