pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The monitor's corners and edges: a drawing of a monitor with a spot at each
// corner and in the middle of each side, each a button that shows what it
// does (or that it is not set) and opens the picker. The column its section
// reads (see SettingsSection.qml).
Column {
    id: page

    required property GestureEditor editor
    required property ActionPicker picker
    property real dpr: Screen.devicePixelRatio
    property Flickable flick: null

    readonly property int currentPage: -1
    readonly property int currentTab: 0

    // The spots: the rule's trigger, where it stands (0 start, 1 middle, 2 end) across and down, and what hovering says.
    readonly property var spots: [
        { on: "corner:tl", cx: 0, cy: 0, tip: qsTr("左上角：把鼠标推到这个角，停一下") },
        { on: "edge:top", cx: 1, cy: 0, tip: qsTr("上边：在这条边上来回蹭几下") },
        { on: "corner:tr", cx: 2, cy: 0, tip: qsTr("右上角：把鼠标推到这个角，停一下") },
        { on: "edge:left", cx: 0, cy: 1, tip: qsTr("左边：在这条边上来回蹭几下") },
        { on: "edge:right", cx: 2, cy: 1, tip: qsTr("右边：在这条边上来回蹭几下") },
        { on: "corner:bl", cx: 0, cy: 2, tip: qsTr("左下角：把鼠标推到这个角，停一下") },
        { on: "edge:bottom", cx: 1, cy: 2, tip: qsTr("下边：在这条边上来回蹭几下") },
        { on: "corner:br", cx: 2, cy: 2, tip: qsTr("右下角：把鼠标推到这个角，停一下") }
    ]

    width: parent ? parent.width : 0
    spacing: 20

    SettingsSection {
        width: parent.width
        title: qsTr("屏幕的角和边")
        glyph: "ph:monitor"
        note: qsTr("把鼠标推到屏幕角落停一下，或在屏幕边上来回蹭几下，就做设好的事。只在屏幕的外边缘有效（两块屏幕相接的地方不算）")

        Item {
            id: diagram
            width: parent.width
            height: monitor.height + 48

            Rectangle { // the monitor
                id: monitor
                readonly property real spotWidth: Math.min(180, Math.floor((width - 24 - 24) / 3))
                readonly property real spotHeight: 36
                anchors.horizontalCenter: parent.horizontalCenter
                y: 24
                width: Math.min(parent.width - 48, 580)
                height: Math.round(width * 0.58)
                radius: Theme.controlRadius
                color: Theme.page

                Hook {
                    anchors.fill: parent
                    part: Theme.cardPart
                    corner: Theme.controlRadius
                    extra: ({ inset: true })
                    dpr: page.dpr
                }

                Column {
                    anchors.centerIn: parent
                    spacing: 6

                    Glyph {
                        anchors.horizontalCenter: parent.horizontalCenter
                        glyph: "ph:monitor"
                        size: 40
                        color: Theme.faint
                    }
                }

                Repeater {
                    model: page.spots

                    delegate: Item {
                        id: spot

                        required property var modelData
                        readonly property var info: {
                            page.editor.revision
                            return page.editor.slot(modelData.on, "")
                        }

                        x: modelData.cx === 0 ? 12 : modelData.cx === 1 ? Math.round((monitor.width - width) / 2) : monitor.width - width - 12
                        y: modelData.cy === 0 ? 12 : modelData.cy === 1 ? Math.round((monitor.height - height) / 2) : monitor.height - height - 12
                        width: monitor.spotWidth
                        height: monitor.spotHeight

                        ActionButton {
                            id: button
                            objectName: "spotButton"
                            anchors.fill: parent
                            dpr: page.dpr
                            compact: true
                            trailingSpace: spot.info.has ? 24 : 0
                            open: page.picker.open && page.picker.anchor === button
                            unset: !spot.info.has
                            invalid: spot.info.kind === "invalid"
                            icon: spot.info.icon
                            text: spot.info.has ? spot.info.title : qsTr("未设置")
                            onClicked: page.picker.ask(button, spot.info,
                                (kind, value) => page.editor.setSlot(spot.modelData.on, "", kind, value), null)
                        }
                        Item { // clear: on a spot that is set, while the pointer is on it
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            width: 24
                            height: 24
                            visible: spot.info.has && (button.hovered || clearArea.containsMouse)

                            Hook {
                                id: clearPart
                                anchors.fill: parent
                                part: Theme.keySurface
                                hovered: clearArea.containsMouse
                                pressed: clearArea.pressed
                                flat: true
                                corner: Theme.smallControlRadius
                                role: "toolbar"
                                dpr: page.dpr
                            }
                            Glyph {
                                anchors.centerIn: parent
                                anchors.verticalCenterOffset: clearPart.out("contentOffsetY", 0)
                                glyph: "ph:x"
                                size: 14
                                color: clearArea.containsMouse ? Theme.text : Theme.subtext
                            }
                            MouseArea {
                                id: clearArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: page.editor.clearSlot(spot.modelData.on, "")
                            }
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("清除")
                        }
                        HoverTip {
                            target: button.hovered ? button : null
                            text: spot.modelData.tip
                            dpr: page.dpr
                            maxWidth: 320
                        }
                    }
                }
            }
        }
    }
}
