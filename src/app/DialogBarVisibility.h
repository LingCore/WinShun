#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>

#include <atomic>
#include <functional>

#include <windows.h>

namespace ws {

// The passive bar waits for a painted frame before it and its separate logo
// appear. Kept apart from searching so the native visibility can be tested.
class DialogBarVisibility : public QObject {
public:
    void setWindow(QQuickWindow* window, std::function<void(bool)> showLogo);
    void show(HWND dialog = nullptr, bool elevated = false); // elevated: of higher integrity than ours
    void hide();
    bool isRevealed() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void reveal(quint64 generation);
    void cancel();

    QPointer<QQuickWindow> m_window;
    std::function<void(bool)> m_showLogo;
    std::atomic<quint64> m_pending {0}; // cleared only on the GUI thread, after revealing
    std::atomic<quint64> m_synchronized {0}; // the opening this frame was painted for
    quint64 m_generation = 0;
    QTimer m_timeout;
};

} // namespace ws
