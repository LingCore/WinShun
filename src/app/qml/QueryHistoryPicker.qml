pragma ComponentBehavior: Bound
import QtQuick
import WinShun

Item {
    id: picker
    objectName: "queryHistoryPicker"
    required property Launcher launcher
    required property WindowFrame frame
    property string popupName: "queryHistoryPopup"
    // Its two windows and the list are made the first time it opens, then
    // kept: most searches never open it, and the box and the panel each have one.
    readonly property var popup: popupLoader.item
    readonly property bool opened: popup !== null && popup.window.visible
    signal chosen()

    // Made now if not yet (a probe cloaks the window before it first opens).
    function prepare() {
        popupLoader.active = true
        return popup.window
    }
    function close() {
        if (popup)
            popup.close()
    }
    function toggle(anchor) {
        if (opened) { close(); return }
        prepare()
        popup.open(anchor)
    }
    function handleKey(event) {
        if (!opened) return false
        return popup.handleKey(event)
    }
    Component.onCompleted: frame.addPopup(outside)
    Connections {
        target: picker.launcher
        function onHistoryChanged() { if (!picker.launcher.queryHistoryEnabled) picker.close() }
    }
    Connections {
        target: picker.Window.window
        function onVisibleChanged() { if (!picker.Window.window.visible) picker.close() }
        function onActiveChanged() { if (!picker.Window.window.active) picker.close() }
    }
    Item {
        id: outside
        parent: picker.Window.window ? picker.Window.window.contentItem : picker
        anchors.fill: parent
        z: 10000
        visible: picker.opened
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onPressed: picker.close() }
    }
    Loader {
        id: popupLoader
        active: false
        sourceComponent: Item {
            id: content
            readonly property Window window: menu

            function close() { menu.hide(); tip.target = null }
            function open(anchor) {
                const point = anchor.mapToGlobal(0, anchor.height + 6)
                menu.anchor = anchor
                menu.origin = point
                menu.area = picker.launcher.screenArea(point)
                list.currentIndex = list.count > 0 ? 0 : -1
                picker.launcher.prepareMenuWindow(menu)
                menu.show()
            }
            function activate(index) {
                if (index < 0 || index >= list.count) return
                picker.launcher.recallQuery(index)
                picker.close()
                picker.chosen()
            }
            function handleKey(event) {
                switch (event.key) {
                case Qt.Key_Escape: close(); return true
                case Qt.Key_Up: list.currentIndex = Math.max(0, list.currentIndex - 1); break
                case Qt.Key_Down: list.currentIndex = Math.min(list.count - 1, list.currentIndex + 1); break
                case Qt.Key_PageUp: list.currentIndex = Math.max(0, list.currentIndex - 8); break
                case Qt.Key_PageDown: list.currentIndex = Math.min(list.count - 1, list.currentIndex + 8); break
                case Qt.Key_Home: list.currentIndex = list.count > 0 ? 0 : -1; break
                case Qt.Key_End: list.currentIndex = list.count - 1; break
                case Qt.Key_Return:
                case Qt.Key_Enter: activate(list.currentIndex); return true
                case Qt.Key_Delete: picker.launcher.removeQuery(list.currentIndex); break
                default: close(); return false
                }
                if (list.currentIndex >= 0) list.positionViewAtIndex(list.currentIndex, ListView.Contain)
                return true
            }

            Window {
                id: menu
                objectName: picker.popupName
                transientParent: picker.Window.window
                flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
                color: Theme.menuBackground
                property Item anchor: null
                property point origin
                property rect area
                readonly property real dpr: Screen.devicePixelRatio
                readonly property real naturalWidth: Math.max(240, measure.implicitWidth + 68,
                    heading.implicitWidth + 28, list.count === 0 ? emptyLabel.implicitWidth + 28 : 0)
                width: Math.max(1, Math.floor(Math.min(Math.ceil(naturalWidth), 440,
                    anchor ? anchor.width : 440, area.width > 0 ? area.width - 16 : 440)))
                height: Math.min(440, 48 + Math.max(1, list.count) * 38, area.height > 0 ? area.height - 16 : 440)
                x: Math.max(area.x + 8, Math.min(origin.x, area.x + area.width - width - 8))
                y: Math.max(area.y + 8, Math.min(origin.y, area.y + area.height - height - 8))
                Text {
                    id: measure
                    visible: false
                    text: picker.launcher.queryHistory.join("\n")
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.fontBody
                }
                Text {
                    id: heading
                    x: 14; y: 12
                    text: qsTr("搜索历史 · %1").arg(list.count)
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }
                ListView {
                    id: list
                    objectName: "queryHistoryList"
                    x: 6; y: 40
                    width: parent.width - 12; height: parent.height - y - 6
                    clip: true
                    reuseItems: true
                    model: picker.launcher.queryHistory
                    boundsBehavior: Flickable.StopAtBounds
                    delegate: Item {
                        id: row
                        required property string modelData
                        required property int index
                        width: list.width; height: 38
                        Accessible.role: Accessible.ListItem
                        Accessible.name: modelData
                        Accessible.onPressAction: content.activate(index)
                        Hook { anchors.fill: parent; part: Theme.rowHighlight; selected: list.currentIndex === row.index; hovered: mouse.containsMouse; role: "menu"; dpr: menu.dpr }
                        Text {
                            x: 10; anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 56
                            text: row.modelData; textFormat: Text.PlainText
                            color: Theme.text; font.pixelSize: Theme.fontBody; elide: Text.ElideRight
                        }
                        MouseArea {
                            id: mouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: content.activate(row.index)
                            onContainsMouseChanged: {
                                if (containsMouse) { tip.target = row; tip.text = row.modelData }
                                else if (tip.target === row) tip.target = null
                            }
                        }
                        RowAction {
                            anchors.right: parent.right; anchors.rightMargin: 4; anchors.verticalCenter: parent.verticalCenter
                            small: true; glyph: ""; tip: qsTr("移除这条搜索记录")
                            dpr: menu.dpr
                            onClicked: picker.launcher.removeQuery(row.index)
                        }
                    }
                    ListScrollBar { flickable: list; anchors.right: parent.right; height: parent.height; dpr: menu.dpr }
                }
                Text {
                    id: emptyLabel
                    anchors.centerIn: list; visible: list.count === 0
                    text: qsTr("还没有搜索记录")
                    color: Theme.faint; font.pixelSize: Theme.fontBody
                }
            }
            MenuPathTip { id: tip; host: menu; availableArea: menu.area }
        }
    }
}
