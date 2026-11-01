#pragma once

#include "platform/Shell.h"

class QObject;

namespace ws::traymenu {

// A native menu loop with Win顺's Theme, UI font and Phosphor icons. Native
// command ids, checked/disabled states and plugin submenus stay intact.
// `shown` observes the root popup once, after Windows has created its HWND.
int popup(HWND owner, const std::vector<shell::MenuItem>& items, POINT screenPos, QObject* theme,
    std::function<void(HWND, HMENU)> shown = {});

} // namespace ws::traymenu
