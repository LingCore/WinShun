#pragma once

#include <QColor>
#include <QImage>
#include <QRect>

#include <windows.h>

#include <functional>

namespace ws::capture {

// The delayed capture's countdown (延时截图): a small native layered window
// in the top right corner of the monitor's work area, the seconds left in
// white on the accent fill. It takes no focus and no key; a click on it
// cancels. Destroyed before the capture is taken, so it is not in the
// picture. Drawn into a DIB like a pin (PinWindow): nothing of Qt Quick.
class CountdownWindow {
public:
    // `work`: the monitor's work area (physical, virtual desktop); `fill`:
    // the accent fill (white words stay readable on it); `radius`: its
    // corner, logical px.
    CountdownWindow(const QRect& work, const QColor& fill, qreal radius, std::function<void()> clicked);
    ~CountdownWindow();
    CountdownWindow(const CountdownWindow&) = delete;
    CountdownWindow& operator=(const CountdownWindow&) = delete;

    HWND hwnd() const { return m_hwnd; }
    void setSeconds(int seconds);
    QRect rect() const { return m_rect; } // physical, virtual desktop
    // What it shows, off screen (tests, previews); `dpr` device px per logical.
    static QImage picture(int seconds, const QColor& fill, qreal radius, double dpr);

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static void paint(QImage& canvas, int seconds, const QColor& fill, qreal radius, double dpr);
    void draw();

    HWND m_hwnd = nullptr;
    std::function<void()> m_clicked;
    QColor m_fill;
    qreal m_radius = 8;
    QRect m_work;
    QRect m_rect;
    double m_dpr = 1.0;
    int m_seconds = -1;
};

} // namespace ws::capture
