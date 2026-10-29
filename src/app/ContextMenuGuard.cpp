#include "ContextMenuGuard.h"

#include <QContextMenuEvent>
#include <QQuickWindow>

namespace ws {

bool ContextMenuGuard::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::ContextMenu) // first: every event of the GUI thread passes here
        return false;
    const auto* window = qobject_cast<QQuickWindow*>(watched);
    if (!window || window->activeFocusItem() || !static_cast<QContextMenuEvent*>(event)->pos().isNull())
        return false;
    event->ignore(); // taken by nobody: the mouse event it was made from stays as it was
    return true;
}

} // namespace ws
