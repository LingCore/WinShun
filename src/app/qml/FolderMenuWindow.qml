pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// One non-activating window preserves the source folder's focus across pages.
Window {
    id: menu
    required property FolderMenu folderMenu
    property int highlighted: -1
    property var page: null
    property int parentHighlight: -1
    property Item hoveredEntry: null
    property string hoveredText
    property point popupAnchor
    property rect availableArea
    readonly property var entries: page === null ? folderMenu.entries : page.children
    readonly property real padding: Theme.menuPadding
    readonly property int itemHeight: Theme.menuItemHeight
    readonly property real dpr: Screen.devicePixelRatio
    readonly property real listHeight: entries.reduce((height, entry) => height + (entry.separator === true ? 9 : itemHeight), 0)
    readonly property int headingHeight: page === null ? 0 : itemHeight + 5
    readonly property real maxHeight: Math.min(600, availableArea.height > 0 ? availableArea.height - 16 : 600)

    objectName: "folderMenuWindow"
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.menuBackground
    width: availableArea.width > 0 ? Math.min(360, availableArea.width - 16) : 360
    height: Math.min(listHeight + 2 * padding + headingHeight, maxHeight)
    onHeightChanged: if (visible) place()
    onHighlightedChanged: {
        flick.currentIndex = highlighted // the list's highlight follows it
        Qt.callLater(ensureVisible)
    }
    onEntriesChanged: {
        hoveredEntry = null
        highlighted = -1
        flick.currentIndex = -1
        flick.contentY = 0
    }

    // The highlight under an item chosen or pointed at (the style's
    // rowHighlight, role "menu"). A part that knows the menu (declares
    // `role`, or `inMenu` as brand's does) draws the item's fill; one that
    // does not (a list's highlight) leaves it to the menu's own.
    component MenuHighlight: Hook {
        id: highlightPart
        readonly property bool inMenu: item !== null && ("role" in item || "inMenu" in item)
        part: Theme.rowHighlight
        hovered: inMenu
        role: "menu"
        dpr: menu.dpr

        Rectangle {
            anchors.fill: parent
            visible: !highlightPart.inMenu
            radius: Theme.menuItemRadius
            color: Theme.menuHover
        }
    }

    function place() {
        let x = popupAnchor.x
        let y = popupAnchor.y
        const area = availableArea
        if (area.width > 0) {
            if (x + width > area.x + area.width) x -= width
            if (y + height > area.y + area.height) y -= height
            x = Math.max(area.x + 8, Math.min(x, area.x + area.width - width - 8))
            y = Math.max(area.y + 8, Math.min(y, area.y + area.height - height - 8))
        }
        menu.x = x
        menu.y = y
    }
    function popup(pos) {
        page = null
        parentHighlight = -1
        highlighted = -1
        hoveredEntry = null
        flick.contentY = 0
        popupAnchor = pos
        availableArea = folderMenu.screenArea(pos)
        flick.forceLayout()
        place()
        folderMenu.prepareMenuWindow(menu)
        show()
        raise()
    }
    function selectable(i) {
        const entry = entries[i]
        return entry !== undefined && entry.separator !== true && entry.disabled !== true
    }
    function step(from, delta) {
        const n = entries.length
        let i = from
        for (let k = 0; k < n; ++k) {
            i = ((i + delta) % n + n) % n
            if (selectable(i)) return i
        }
        return -1
    }
    function ensureVisible() {
        if (highlighted >= 0)
            flick.positionViewAtIndex(highlighted, ListView.Contain)
    }
    function activate(i) {
        if (!selectable(i)) return
        const entry = entries[i]
        if (entry.children !== undefined) {
            parentHighlight = i
            page = entry
            highlighted = step(-1, 1)
        } else {
            folderMenu.trigger(entry.action)
        }
    }
    function back() {
        if (page === null) return
        page = null
        highlighted = parentHighlight
    }
    function jumpTo(text) {
        const wanted = text.toLowerCase()
        const n = entries.length
        for (let k = 1; k <= n; ++k) {
            const i = ((highlighted < 0 ? -1 : highlighted) + k) % n
            if (selectable(i) && (entries[i].text ?? "").toLowerCase().startsWith(wanted)) {
                highlighted = i
                return
            }
        }
    }
    function handleKey(key, text) {
        switch (key) {
        case Qt.Key_Down: highlighted = step(highlighted, 1); return
        case Qt.Key_Up: highlighted = step(highlighted < 0 ? 0 : highlighted, -1); return
        case Qt.Key_Home:
        case Qt.Key_PageUp: highlighted = step(-1, 1); return
        case Qt.Key_End:
        case Qt.Key_PageDown: highlighted = step(0, -1); return
        case Qt.Key_Right:
            if (highlighted >= 0 && entries[highlighted].children !== undefined) activate(highlighted)
            return
        case Qt.Key_Left:
        case Qt.Key_Backspace: back(); return
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (highlighted >= 0) activate(highlighted)
            return
        case Qt.Key_Escape:
            if (page !== null) back()
            else folderMenu.dismiss()
            return
        case Qt.Key_Menu: folderMenu.dismiss(); return
        default:
            if (text.length > 0 && text.trim().length > 0) jumpTo(text)
        }
    }
    Connections {
        target: menu.folderMenu
        function onPopupRequested(pos) { menu.popup(pos) }
        function onHideRequested() {
            menu.visible = false
            menu.page = null
            menu.highlighted = -1
            menu.hoveredEntry = null
        }
        function onKeyPressed(key, text) { menu.handleKey(key, text) }
    }

    Item {
        id: heading
        visible: menu.page !== null
        x: menu.padding
        y: menu.padding
        width: menu.width - 2 * menu.padding
        height: menu.headingHeight
        Accessible.role: Accessible.MenuItem
        Accessible.name: qsTr("返回")
        Accessible.onPressAction: menu.back()
        MenuHighlight {
            visible: backMouse.containsMouse
            width: heading.width
            height: menu.itemHeight
        }
        Glyph {
            x: 12
            y: (menu.itemHeight - height) / 2
            glyph: "\uE72B"
            size: 16
            color: Theme.text
        }
        Text {
            x: 42
            y: (menu.itemHeight - height) / 2
            width: heading.width - x - 12
            text: menu.page === null ? "" : menu.page.text
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
            color: Theme.text
            elide: Text.ElideRight
        }
        MouseArea {
            id: backMouse
            width: heading.width
            height: menu.itemHeight
            hoverEnabled: true
            onClicked: menu.back()
        }
        Hook { // the style's seam, edge to edge
            anchors.bottom: parent.bottom
            x: -menu.padding
            width: menu.width
            height: Theme.edge(menu.dpr)
            part: Theme.separator
            dpr: menu.dpr
        }
    }
    ListView {
        id: flick
        objectName: "folderMenuScroll"
        x: menu.padding
        y: menu.padding + menu.headingHeight
        width: menu.width - 2 * menu.padding
        height: menu.height - 2 * menu.padding - menu.headingHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        reuseItems: true
        model: menu.entries
        // One highlight for the list, under the item chosen or pointed at:
        // it jumps, or slides as the style says (Theme.rowSlide). Not
        // managed by the view's own current item: menu.highlighted is.
        currentIndex: -1
        highlightMoveDuration: Theme.rowSlide
        highlightMoveVelocity: -1
        highlightResizeDuration: 0
        highlightResizeVelocity: -1
        highlight: Item {
            width: flick.width
            visible: flick.currentItem !== null && menu.highlighted >= 0
            MenuHighlight { anchors.fill: parent }
        }
        delegate: Item {
            id: entry
            required property var modelData
            required property int index
            readonly property bool separator: modelData.separator === true
            readonly property bool disabled: modelData.disabled === true
            readonly property bool branch: modelData.children !== undefined
            readonly property string tooltip: modelData.tooltip ?? ""
            readonly property real trailingWidth: Math.min(detail.implicitWidth, 96) + (branch ? 20 : 0)
            objectName: "folderMenuEntry" + index
            width: flick.width
            height: separator ? 9 : menu.itemHeight
            enabled: !disabled
            ListView.onPooled: {
                if (menu.hoveredEntry === entry)
                    menu.hoveredEntry = null
            }
            Accessible.role: Accessible.MenuItem
            Accessible.name: modelData.text ?? ""
            Accessible.description: tooltip
            Accessible.ignored: separator
            Accessible.onPressAction: menu.activate(entry.index)
            Hook { // the style's seam, edge to edge
                visible: entry.separator
                anchors.verticalCenter: parent.verticalCenter
                x: -menu.padding
                width: menu.width
                height: Theme.edge(menu.dpr)
                part: Theme.separator
                dpr: menu.dpr
            }
            Image {
                id: icon
                visible: !entry.separator && status === Image.Ready
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                width: 18
                height: 18
                source: entry.modelData.icon ?? ""
                sourceSize: Qt.size(18 * Screen.devicePixelRatio, 18 * Screen.devicePixelRatio)
                asynchronous: true
                fillMode: Image.PreserveAspectFit
            }
            Glyph {
                visible: !entry.separator && !icon.visible
                x: 13
                anchors.verticalCenter: parent.verticalCenter
                glyph: entry.modelData.glyph ?? ""
                size: 16
                color: entry.disabled ? Theme.faint : Theme.text
            }
            Text {
                x: 42
                width: Math.max(0, entry.width - x - 14 - entry.trailingWidth - 12)
                anchors.verticalCenter: parent.verticalCenter
                text: entry.modelData.text ?? ""
                elide: Text.ElideRight
                color: entry.disabled ? Theme.faint : Theme.text
                font.pixelSize: Theme.fontBody
            }
            Text {
                id: detail
                anchors.right: parent.right
                anchors.rightMargin: entry.branch ? 34 : 14
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, 96)
                text: entry.modelData.detail ?? ""
                elide: Text.ElideRight
                color: Theme.faint
                font.pixelSize: Theme.fontCaption
            }
            Glyph {
                visible: entry.branch
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                glyph: "\uE76C"
                size: 12
                color: entry.disabled ? Theme.faint : Theme.text
            }
            MouseArea {
                anchors.fill: parent
                enabled: !entry.separator && !entry.disabled
                hoverEnabled: true
                onEntered: {
                    menu.highlighted = entry.index
                    menu.hoveredEntry = entry
                    menu.hoveredText = entry.tooltip
                }
                onExited: {
                    if (menu.highlighted === entry.index) menu.highlighted = -1
                    if (menu.hoveredEntry === entry) menu.hoveredEntry = null
                }
                onClicked: menu.activate(entry.index)
            }
        }
    }
    HoverTip {
        target: menu.hoveredText.length > 0 ? menu.hoveredEntry : null
        text: menu.hoveredText
    }
    ListScrollBar {
        dpr: menu.dpr
        visible: flick.contentHeight > flick.height
        flickable: flick
        x: menu.width - width
        y: flick.y
        height: flick.height
    }

    // The box: DWM's frame under fluent (the window's colour is the menu's);
    // an own frame has the menu's radius, the style's menuFrame draws the
    // body and the edge, and the window's shadow is OwnFrame's.
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
        z: -1
        visible: Theme.ownFrame
        part: Theme.ownFrame ? Theme.menuFrame : null
        extra: ({ shadow: false })
        dpr: menu.dpr
    }
    Loader { // Windows 10 has no edge of DWM's
        anchors.fill: parent
        z: 1000
        active: !Theme.ownFrame
        sourceComponent: WindowEdge { fill: Theme.menuBackground }
    }
}
