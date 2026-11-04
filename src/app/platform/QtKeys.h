#pragma once

#include <qnamespace.h>

#include <windows.h>

namespace ws {

// A key as Qt names it (Qt::Key), for the key events Win顺 makes for a window
// that does not have the keyboard: the keys KeyRouter routes, and those
// TypeToSearch held for the launcher and hands over to it (GitHub #203). 0
// for those named after the character they type.
inline int qtKeyOf(UINT vk, bool extended, bool shift)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return static_cast<int>(vk); // Qt::Key_A… and Qt::Key_0… have the same codes
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return Qt::Key_0 + static_cast<int>(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F24)
        return Qt::Key_F1 + static_cast<int>(vk - VK_F1);
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return Qt::Key_Shift;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return Qt::Key_Control;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return Qt::Key_Alt;
    case VK_BACK:
        return Qt::Key_Backspace;
    case VK_TAB:
        return shift ? Qt::Key_Backtab : Qt::Key_Tab; // as Qt spells Shift+Tab
    case VK_RETURN:
        return extended ? Qt::Key_Enter : Qt::Key_Return; // the number pad's is extended
    case VK_ESCAPE:
        return Qt::Key_Escape;
    case VK_SPACE:
        return Qt::Key_Space;
    case VK_PRIOR:
        return Qt::Key_PageUp;
    case VK_NEXT:
        return Qt::Key_PageDown;
    case VK_END:
        return Qt::Key_End;
    case VK_HOME:
        return Qt::Key_Home;
    case VK_LEFT:
        return Qt::Key_Left;
    case VK_UP:
        return Qt::Key_Up;
    case VK_RIGHT:
        return Qt::Key_Right;
    case VK_DOWN:
        return Qt::Key_Down;
    case VK_INSERT:
        return Qt::Key_Insert;
    case VK_DELETE:
        return Qt::Key_Delete;
    case VK_APPS:
        return Qt::Key_Menu;
    default:
        return 0;
    }
}

} // namespace ws
