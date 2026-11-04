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

    readonly property real padding: Theme.menuPadding
    readonly property int itemHeight: Theme.menuItemHeight
    readonly property real dpr: Screen.devicePixelRatio

    signal triggered(int row, int action)

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.menuBackground
    height: content.implicitHeight + 2 * padding

    // `pos`: global point (logical pixels) for the top-left corner (`upward`:
    // the bottom-left one); the menu flips left / up (down) when it would
    // leave the screen there.
    function popup(row, key, items, pos, fromKeyboard, upward) {
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
        content.forceLayout() // its height for these entries, now
        let x = pos.x
        let y = upward ? pos.y - height : pos.y
        if (area.width > 0) {
            if (x + width > area.x + area.width)
                x = x - width
            if (upward ? y < area.y : y + height > area.y + area.height)
                y = upward ? pos.y : y - height
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
        if (!entry || entry.separator || entry.enabled === false)
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
            if (!entries[i].separator && entries[i].enabled !== false)
                return i
        }
        return -1
    }

    // Returns true when the key was for the menu.
    function handleKey(event) {
        if (event.key === Qt.Key_O && (event.modifiers & ~Qt.KeypadModifier) === Qt.ControlModifier) {
            if (highlighted < 0)
                highlighted = step(-1, 1)
            return true
        }
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
        case Qt.Key_Left:
            dismiss()
            return true
        case Qt.Key_Right:
        case Qt.Key_PageUp:
        case Qt.Key_PageDown:
            return true
        default:
            dismiss() // typing goes on in the search box
            return false
        }
    }

    // The box. Under DWM's frame (fluent) Windows rounds, edges and shadows
    // the window, whose colour is the menu's; an own frame (Theme.ownFrame)
    // has the menu's radius, and the style's menuFrame draws the body and the
    // edge (the shadow is the window's, drawn by OwnFrame).
    Binding {
        target: menu
        property: "color"
        value: "transparent"
        when: Theme.ownFrame
    }
    OwnFrame {
        active: Theme.ownFrame
        radius: Theme.menuRadius
        shadow: Theme.tok("menuWindowShadow", Theme.frameShadow)
    }
    Hook {
        anchors.fill: parent
        visible: Theme.ownFrame
        part: Theme.ownFrame ? Theme.menuFrame : null
        extra: ({ shadow: false })
        dpr: menu.dpr
    }

    // The highlight under the item chosen or pointed at: one for the whole
    // menu (the style's rowHighlight, role "menu"), jumping from item to item
    // or sliding as the style says (Theme.rowSlide).
    readonly property Item current: highlighted >= 0 && highlighted < repeater.count ? repeater.itemAt(highlighted) : null
    Hook {
        id: highlight

        readonly property real wantedY: menu.current ? content.y + menu.current.y : 0
        property bool placed: false // it stood under an item before: slides from there
        property real shownY: 0

        visible: menu.current !== null
        x: content.x
        y: shownY
        width: menu.width - 2 * menu.padding
        height: menu.itemHeight
        // A part that knows the menu (declares `role`, or `inMenu` as brand's
        // does) draws the item's fill; one that does not (a list's highlight)
        // leaves it to the menu's own.
        readonly property bool inMenu: item !== null && ("role" in item || "inMenu" in item)
        part: Theme.rowHighlight
        hovered: inMenu
        position: shownY
        role: "menu"
        dpr: menu.dpr

        Rectangle {
            anchors.fill: parent
            visible: !highlight.inMenu
            radius: Theme.menuItemRadius
            color: Theme.menuHover
        }

        onWantedYChanged: {
            const had = placed
            placed = menu.current !== null
            if (had && placed && Theme.rowSlide > 0 && SystemTheme.animations) {
                slide.restart()
            } else {
                slide.stop()
                shownY = wantedY
            }
        }
        NumberAnimation {
            id: slide
            target: highlight
            property: "shownY"
            to: highlight.wantedY
            duration: Theme.rowSlide
            easing.type: Easing.OutCubic
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
                readonly property bool disabled: modelData.enabled === false // shown, cannot be picked

                width: menu.width - 2 * menu.padding
                height: separator ? 9 : menu.itemHeight
                implicitWidth: separator ? 0 : 44 + label.implicitWidth + 40 + shortcut.implicitWidth + 14

                Hook { // the style's seam, edge to edge
                    visible: entry.separator
                    anchors.verticalCenter: parent.verticalCenter
                    x: -menu.padding
                    width: menu.width
                    height: Theme.edge(menu.dpr)
                    part: Theme.separator
                    dpr: menu.dpr
                }

                Glyph {
                    visible: !entry.separator
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    // `checked: true` marks the entry as on (a column that is shown) in the icon's place
                    glyph: entry.modelData.checked === true ? String.fromCharCode(0xE73E) : (entry.modelData.glyph ?? "") // CheckMark
                    size: 16
                    color: entry.disabled ? Theme.faint : Theme.text
                }

                Text {
                    id: label
                    visible: !entry.separator
                    x: 44
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.modelData.text ?? ""
                    color: entry.disabled ? Theme.faint : Theme.text
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
                    enabled: !entry.separator && !entry.disabled
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onEntered: menu.highlighted = entry.index
                    onExited: if (menu.highlighted === entry.index) menu.highlighted = -1
                    onClicked: menu.activate(entry.index)
                }
            }
        }
    }

    Loader { // Windows 10 has no edge of DWM's: the window draws it (an own frame has its own, above)
        anchors.fill: parent
        z: 1000
        active: !Theme.ownFrame
        sourceComponent: WindowEdge { fill: Theme.menuBackground }
    }
}
