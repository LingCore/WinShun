#pragma once

#include <QObject>

namespace ws {

// Since Qt 6.11, a right-click whose release lands on the top-left pixel of
// a Qt Quick window with no active focus item crashes the process
// (QTBUG-146671, still open). The context-menu event Qt makes from that
// release is at (0, 0); Qt takes (0, 0) for "from the keyboard", asks the
// active focus item where it is, and there is none. Every window of ours
// that never takes the focus has none: the menus (ContextMenu.qml), tips,
// the preview, the clipboard over another program, the capture's panels.
// A row's menu opens with that pixel under the pointer, so a second
// right-click on the same spot was enough.
//
// Installed on the application (main.cpp), it drops exactly the events Qt
// would crash on; nothing of ours wants a context-menu event there. It can
// go once Qt's QQuickDeliveryAgentPrivate::contextMenuTargets() checks
// activeFocusItem (it does not in 6.12, nor on dev as of 2026-10).
class ContextMenuGuard : public QObject {
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace ws
