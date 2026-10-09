pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The context menu of a row (a search result, a clipboard entry), drawn to
// match the launcher (Windows 11 style). A window of its own, so it can
// extend past the launcher's edges; it never takes the focus, so the page
// forwards the keys to handleKey(). `host` is the page's view-model: it
// knows the screen area (screenArea) and styles the window (prepareMenuWindow).
Window {
    id: menu

    required property QtObject host
    property int row: -1
    property var key // what the row was (a path): the page closes the menu if a refresh puts another there
    property var entries: []
    property int highlighted: -1

    readonly property int padding: 4
    readonly property int itemHeight: 36

    signal triggered(int row, int action)

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.menuBackground
    height: content.implicitHeight + 2 * padding

    // `pos`: global point (logical pixels) for the top-left corner; the menu
    // flips left / up when it would leave the screen there.
    function popup(row, key, items, pos, fromKeyboard) {
        menu.row = row
        menu.key = key
        entries = items
        if (entries.length === 0) {
            dismiss()
            return
        }
        let w = 0
        for (let i = 0; i < repeater.count; ++i)
            w = Math.max(w, repeater.itemAt(i).implicitWidth)
        width = Math.max(240, w + 2 * padding)
        highlighted = fromKeyboard ? step(-1, 1) : -1

        const area = host.screenArea(pos)
        let x = pos.x
        let y = pos.y
        if (area.width > 0) {
            if (x + width > area.x + area.width)
                x = x - width
            if (y + height > area.y + area.height)
                y = y - height
            x = Math.max(area.x, Math.min(x, area.x + area.width - width))
            y = Math.max(area.y, Math.min(y, area.y + area.height - height))
        }
        menu.x = x
        menu.y = y
        host.prepareMenuWindow(menu)
        show()
        raise()
    }

    function dismiss() {
        visible = false
        highlighted = -1
    }

    function activate(i) {
        const entry = entries[i]
        if (!entry || entry.separator)
            return
        const r = row
        dismiss()
        triggered(r, entry.action)
    }

    // Next selectable entry from `from` in direction `delta`, wrapping around.
    function step(from, delta) {
        const n = entries.length
        let i = from
        for (let k = 0; k < n; ++k) {
            i = ((i + delta) % n + n) % n
            if (!entries[i].separator)
                return i
        }
        return -1
    }

    // Returns true when the key was for the menu.
    function handleKey(event) {
        switch (event.key) {
        case Qt.Key_Down: highlighted = step(highlighted, 1); return true
        case Qt.Key_Up: highlighted = step(highlighted < 0 ? 0 : highlighted, -1); return true
        case Qt.Key_Home: highlighted = step(-1, 1); return true
        case Qt.Key_End: highlighted = step(0, -1); return true
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Space:
            if (highlighted >= 0)
                activate(highlighted)
            else
                dismiss()
            return true
        case Qt.Key_Escape:
        case Qt.Key_Menu:
            dismiss()
            return true
        case Qt.Key_Left:
        case Qt.Key_Right:
        case Qt.Key_PageUp:
        case Qt.Key_PageDown:
            return true
        default:
            dismiss() // typing goes on in the search box
            return false
        }
    }

    Column {
        id: content
        x: menu.padding
        y: menu.padding

        Repeater {
            id: repeater
            model: menu.entries

            delegate: Item {
                id: entry

                required property var modelData
                required property int index
                readonly property bool separator: modelData.separator === true

                width: menu.width - 2 * menu.padding
                height: separator ? 9 : menu.itemHeight
                implicitWidth: separator ? 0 : 44 + label.implicitWidth + 40 + shortcut.implicitWidth + 14

                Rectangle {
                    visible: entry.separator
                    anchors.verticalCenter: parent.verticalCenter
                    x: -menu.padding
                    width: menu.width
                    height: 1
                    color: Theme.divider
                }

                Rectangle {
                    visible: !entry.separator && menu.highlighted === entry.index
                    anchors.fill: parent
                    radius: 4
                    color: Theme.menuHover
                }

                Glyph {
                    visible: !entry.separator
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: entry.modelData.glyph ?? ""
                    size: 16
                    color: Theme.text
                }

                Text {
                    id: label
                    visible: !entry.separator
                    x: 44
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.modelData.text ?? ""
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }

                Text {
                    id: shortcut
                    visible: !entry.separator
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.modelData.shortcut ?? ""
                    color: Theme.faint
                    font.pixelSize: Theme.fontCaption
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: !entry.separator
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onEntered: menu.highlighted = entry.index
                    onExited: if (menu.highlighted === entry.index) menu.highlighted = -1
                    onClicked: menu.activate(entry.index)
                }
            }
        }
    }
}
