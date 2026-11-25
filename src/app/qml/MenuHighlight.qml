import QtQuick
import WinShun

// The highlight under a menu's row chosen or pointed at (the style's
// rowHighlight, role "menu"): FolderMenuColumn's, DropPieColumn's. A part
// that knows the menu (declares `role`, or `inMenu` as brand's does) draws
// the row's fill; one that does not (a list's highlight) leaves it to the
// menu's own.
Hook {
    id: highlight

    readonly property bool inMenu: item !== null && ("role" in item || "inMenu" in item)

    part: Theme.rowHighlight
    hovered: inMenu
    role: "menu"

    Rectangle {
        anchors.fill: parent
        visible: !highlight.inMenu
        radius: Theme.menuItemRadius
        color: Theme.menuHover
    }
}
