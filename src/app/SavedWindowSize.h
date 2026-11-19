#pragma once

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QtGui/qwindowdefs.h>

class QScreen;
class QWindow;

namespace ws {

// The size its user gives one of our windows by its edges, kept between runs
// (GitHub #401): an ini file's [<group>] Width and Height (logical pixels, the
// same on every monitor) and Maximized.
//
// Only a size dragged out at an edge is kept (the system's sizing loop, by
// mouse or keyboard). One the window gets otherwise (snapped to half the
// screen, fitted to a smaller one as it opens) is not: the wish outlives a
// small screen. A double click on an edge puts it back to the size it was made
// with (its QML width and height), as the clipboard's does.
//
// Opened (placeOn), it is centred on that screen's work area at that size, as
// much of it as fits there; maximized again the first time if it was left so.
// All of it only while remembering is on (setRemembering).
class SavedWindowSize : public QObject {
    Q_OBJECT

public:
    // `file`: the ini file (state.ini); `group`: its section.
    SavedWindowSize(QString file, QString group, QObject* parent = nullptr);
    ~SavedWindowSize() override;

    // The window, its native handle made, still at the size it was made with
    // (the default).
    void setWindow(QWindow* window);
    // Before showing it, on `screen` (none: the one under the mouse).
    // Minimized, it is restored; maximized while it was up before, it stays
    // so, where it is, as the system's own windows reopen. Left maximized the
    // last run, the first show() maximizes it there.
    void placeOn(QScreen* screen = nullptr);
    QSize wanted() const { return m_wanted; } // its user's size, else the default
    bool maximizedWanted() const { return m_maximized; }

    // 外观 › 记住窗口大小 (Settings::rememberWindowSize), for each of these
    // windows in this process. Off: each run opens them at the size they were
    // made with (one dragged out still lasts while it runs, as before #401),
    // and what was saved is forgotten. On again: what its user has given each
    // window by then is saved at once.
    static void setRemembering(bool on);
    static bool remembering();

    // `wanted` on a work area `area` large: kMargin of it left over across
    // and down, and no smaller than `minimum` where that fits.
    static QSize fit(QSize wanted, QSize minimum, QSize area);
    static constexpr int kMargin = 40; // logical pixels, both sides together

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Hook; // the window procedure subclass
    friend struct Hook;

    void hook();
    void forget(); // remembering turned off
    void remember(); // ... and on again
    void sized(QSize from, QSize to); // a sizing loop ended (logical pixels)
    void reset(); // an edge double clicked
    void stateChanged(Qt::WindowState state);

    QString m_file;
    QString m_group;
    QPointer<QWindow> m_window;
    WId m_hwnd = 0;
    QSize m_default; // as it was made
    QSize m_saved; // as read; empty: none
    QSize m_wanted;
    bool m_maximized = false; // left maximized
    bool m_shown = false; // placed once already
    bool m_maximizeOnShow = false; // placed the first time, left maximized
    bool m_sizing = false; // in the sizing loop, an edge moved
    QSize m_loopStart; // the size as that loop began
};

} // namespace ws
