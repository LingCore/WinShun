#pragma once

#include "platform/Shell.h"

class QImage;
class QObject;

namespace ws::traymenu {

// Shared by the tray and capture pin menus: a native menu loop with
// Win顺's Theme, UI font and Phosphor icons. Native command ids,
// checked/disabled states and plugin submenus stay intact.
// `shown` observes the root popup once, after Windows has created its HWND.
int popup(HWND owner, const std::vector<shell::MenuItem>& items, POINT screenPos, QObject* theme,
    std::function<void(HWND, HMENU)> shown = {});

// The top level's rows as popup() draws them, stacked, without Windows'
// frame round them, at `scale` (device pixels per logical): for tests and
// previews, no window shown. `highlighted`: the item under the pointer.
QImage render(const std::vector<shell::MenuItem>& items, QObject* theme, qreal scale, int highlighted = -1);

} // namespace ws::traymenu
