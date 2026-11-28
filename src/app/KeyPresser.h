#pragma once

#include "KeyChord.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <windows.h>

namespace ws {

// Presses a favorite's shortcut (KeyChord.h) where its user is working: in
// the window that was in front before Win顺's own came up (the launcher,
// the folder menu), once that window is in front again and the hand is off
// the keyboard (Enter, or the Ctrl of Ctrl+Enter, still down would join the
// chord). By SendInput, as a hand would press it. Windows drops such keys
// in a program run as administrator (UIPI) without saying so: then nothing
// is pressed, and its user is told why (failed). On the GUI thread.
class KeyPresser : public QObject {
    Q_OBJECT

public:
    using Sender = decltype(&::SendInput);
    explicit KeyPresser(QObject* parent = nullptr, Sender send = &::SendInput);

    // `target`: the window to press it in, brought to the front (it may be
    // nullptr, or one no longer there: then whichever comes to the front
    // once Win顺's own is gone). One still waiting is dropped. `name`: the
    // favorite's, for what failed() says.
    void press(const keychord::Chord& chord, HWND target, const QString& name);
    bool pending() const noexcept { return m_timer.isActive(); }

    // How long it waits, at most, for the window to come to the front and for
    // the keys to be let go of; how long the window is in front before.
    static constexpr int kFrontWaitMs = 1000;
    static constexpr int kKeysWaitMs = 1500;
    static constexpr int kSettleMs = 60;

signals:
    // Nothing pressed: a title and why, for a notification.
    void failed(const QString& title, const QString& why);

private:
    void step();
    void fail(const QString& why, const char* reason);

    Sender m_send;
    QTimer m_timer;
    QElapsedTimer m_clock; // since press()
    QElapsedTimer m_frontClock; // since it was seen in front with the keys up
    keychord::Chord m_chord;
    HWND m_target = nullptr;
    QString m_name;
};

} // namespace ws
