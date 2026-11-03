#pragma once

#include "Presenter.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QString>
#include <QTimer>

#include <memory>
#include <vector>

class QQmlEngine;

namespace ws {

// The presenter's windows (GitHub #189, docs/design-presenter.md 4.2): a
// transparent, topmost ink window on each monitor (PresenterOverlay.qml)
// and the toolbar (PresenterToolbar.qml), a window of its own that never
// takes the focus, so it can be clicked while the ink lets the mouse
// through. Made the first time the presenter opens, hidden after; made
// again when the monitors change. The native side: click-through
// (Qt::WindowTransparentForInput), the foreground given back, the windows
// kept above the others while a program comes to the front (a WinEvent
// hook on EVENT_SYSTEM_FOREGROUND), the remembered state (presenter.ini).
class PresenterHost : public QObject {
    Q_OBJECT

public:
    PresenterHost(QQmlEngine& engine, QString stateFile, QObject* parent = nullptr);
    ~PresenterHost() override;

    capture::Presenter& presenter() { return m_presenter; }
    void hotkey() { m_presenter.hotkey(); }
    void setHotkeyText(const QString& text) { m_presenter.setHotkeyText(text); }
    bool isOpen() const { return m_presenter.isOpen(); }

    // The hook's: a program came to the front; ours go above it again.
    void raiseAll();
    // The other hook's: windows changed places (a program made its window
    // topmost without coming to the front); ours go above it again, a moment later.
    void reordered();

private:
    void modeChanged();
    void makeWindows();
    void dropWindows();
    void monitorsChanged();
    void placeToolbar();
    void rememberToolbar();
    void watchForeground(bool on);
    bool covered() const;
    void raiseIfCovered();
    Q_SLOT void updateToolbarMask();
    QString screenKey(int index) const;

    QQmlEngine& m_engine;
    const QString m_stateFile;
    capture::Presenter m_presenter;
    std::vector<QPointer<QQuickWindow>> m_inks; // one per presenter screen, in its order
    QPointer<QQuickWindow> m_toolbar;
    bool m_shownOnce = false;
    void* m_hook = nullptr; // HWINEVENTHOOK: the foreground
    void* m_reorderHook = nullptr; // HWINEVENTHOOK: the windows' order
    QTimer* m_reorderTimer = nullptr;
    std::vector<qint64> m_raises; // when reordered() last raised ours (ms): a fight is given up
    qint64 m_quietUntil = 0;
};

} // namespace ws
