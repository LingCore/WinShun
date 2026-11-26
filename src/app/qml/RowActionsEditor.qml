pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Which buttons a result's row shows, and in which order (设置 › 结果列表,
// SettingsEditor.rowActions): the ones shown as keys in their order, left to
// right as on the row, dragged into another place, clicked to take one off;
// under them the ones not shown, clicked to put one on (at the end).
Column {
    id: editor

    required property SettingsEditor settings
    property real dpr: 1

    readonly property var shown: settings.rowActions
    readonly property var hidden: settings.rowActionChoices.slice(shown.length)

    // What each is on the row (ResultActions.qml): its icon, a short name for
    // the key, and what it does in full for the tip.
    readonly property var kinds: ({
        "admin": { glyph: "ph:shield-check", name: qsTr("管理员运行"), tip: qsTr("以管理员身份运行（只在程序上出现）") },
        "reveal": { glyph: "ph:folder-open", name: qsTr("打开位置"), tip: qsTr("打开所在位置") },
        "terminal": { glyph: "ph:terminal-window", name: qsTr("终端"), tip: qsTr("在终端中打开，Shift+点击以管理员身份") },
        "copy": { glyph: "ph:copy", name: qsTr("复制"), tip: qsTr("复制文件或文件夹，可在资源管理器中粘贴") },
        "copyPath": { glyph: "copyPath", name: qsTr("复制路径"), tip: qsTr("复制完整路径") },
        "delete": { glyph: "ph:trash", name: qsTr("删除"), tip: qsTr("删除（点两次，移到回收站）") },
        "preview": { glyph: "ph:eye", name: qsTr("预览"), tip: qsTr("不打开文件，在旁边预览内容") },
        "copyName": { glyph: "ph:textbox", name: qsTr("复制名称"), tip: qsTr("复制文件名") },
        "favorite": { glyph: "ph:star", name: qsTr("收藏"), tip: qsTr("加入收藏夹，已收藏的再点一次移除") }
    })

    // A key dragged among those shown: from where, and before which of the
    // others it would go now (their count: after the last).
    property int dragFrom: -1
    property int dragTo: -1
    property Item hoveredKey: null

    function onPixel(v) { return Math.round(v * dpr) / dpr }

    // Where the key dragged would go: before the first of the others that is
    // on a later line than its middle, or on its line and right of it.
    function placeFor(key: Item, dx: real, dy: real): int {
        const cx = key.x + key.width / 2 + dx
        const cy = key.y + key.height / 2 + dy
        let place = 0
        for (let i = 0; i < shownKeys.count; ++i) {
            const other = shownKeys.itemAt(i)
            if (!other || other === key)
                continue
            const above = other.y + other.height <= cy
            const sameLine = other.y <= cy && cy < other.y + other.height
            if (above || (sameLine && other.x + other.width / 2 < cx))
                ++place
            else if (!sameLine && other.y > cy)
                break
        }
        return place
    }
    // The other key the dragged one would go before (null: after the last).
    function keyAt(place: int): Item {
        let n = 0
        for (let i = 0; i < shownKeys.count; ++i) {
            const other = shownKeys.itemAt(i)
            if (!other || i === dragFrom)
                continue
            if (n === place)
                return other
            ++n
        }
        return null
    }
    function lastOther(): Item {
        for (let i = shownKeys.count - 1; i >= 0; --i) {
            if (i !== dragFrom && shownKeys.itemAt(i))
                return shownKeys.itemAt(i)
        }
        return null
    }

    spacing: 8

    component ActionKey: Item {
        id: key

        required property string action
        property bool on: true // shown on the row
        property bool dragged: false
        readonly property var kind: editor.kinds[action] ?? ({ glyph: "", name: action, tip: "" })
        readonly property alias area: area

        signal clicked()

        implicitWidth: Theme.snapUp(10 + 16 + 6 + label.implicitWidth + 12, editor.dpr)
        implicitHeight: [32, 36, 40][Theme.step]
        width: implicitWidth
        height: implicitHeight
        Accessible.role: Accessible.CheckBox
        Accessible.name: kind.name
        Accessible.description: kind.tip
        Accessible.checked: on

        Hook { // the style's key (Theme.keySurface): flat while not shown on the row
            id: surface
            anchors.fill: parent
            part: Theme.keySurface
            hovered: area.containsMouse || key.dragged
            pressed: area.pressed || key.dragged
            flat: !key.on
            role: "toolbar"
            dpr: editor.dpr
        }
        Glyph {
            x: 10
            y: editor.onPixel((key.height - height) / 2 + surface.out("contentOffsetY", 0))
            glyph: key.kind.glyph
            size: 16
            color: surface.out("foreground", key.on ? Theme.text : Theme.subtext)
        }
        Text {
            id: label
            x: 10 + 16 + 6
            y: editor.onPixel((key.height - height) / 2 + surface.out("contentOffsetY", 0))
            text: key.kind.name
            color: surface.out("foreground", key.on ? Theme.text : Theme.subtext)
            font.pixelSize: Theme.fontBody
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: key.clicked()
            onContainsMouseChanged: {
                if (containsMouse)
                    editor.hoveredKey = key
                else if (editor.hoveredKey === key)
                    editor.hoveredKey = null
            }
        }
    }

    Text {
        text: editor.shown.length > 0 ? qsTr("显示的按钮，从左到右；拖动调整顺序，点一下取消显示")
                                      : qsTr("现在结果行上没有按钮；点下面的按钮把它加上")
        color: Theme.subtext
        font.pixelSize: Theme.fontCaption
    }

    Item {
        width: parent.width
        height: shownFlow.height
        visible: editor.shown.length > 0

        Flow {
            id: shownFlow
            objectName: "rowActionsShown"
            width: parent.width
            spacing: 6

            Repeater {
                id: shownKeys
                model: editor.shown

                delegate: ActionKey {
                    id: shownKey
                    required property int index
                    required property string modelData

                    objectName: "rowActionKey_" + modelData
                    action: modelData
                    on: true
                    dragged: editor.dragFrom === index
                    z: dragged ? 2 : 0
                    transform: Translate {
                        x: shownKey.dragged ? dragger.activeTranslation.x : 0
                        y: shownKey.dragged ? dragger.activeTranslation.y : 0
                    }
                    onClicked: editor.settings.toggleRowAction(modelData)

                    // Held and moved past the drag distance: the key goes with
                    // the pointer and the click is called off. In the key's own
                    // mouse area, so it sees the press before the area takes it.
                    DragHandler {
                        id: dragger
                        objectName: "rowActionDrag_" + shownKey.modelData
                        parent: shownKey.area
                        target: null
                        enabled: editor.shown.length > 1
                        dragThreshold: Math.max(8, Qt.styleHints.startDragDistance)
                        cursorShape: Qt.ClosedHandCursor
                        onActiveChanged: {
                            if (active) {
                                editor.dragFrom = shownKey.index
                                editor.dragTo = shownKey.index
                                return
                            }
                            const from = editor.dragFrom
                            const to = editor.dragTo
                            editor.dragFrom = -1
                            editor.dragTo = -1
                            if (from >= 0 && to >= 0 && to !== from)
                                editor.settings.moveRowAction(shownKey.modelData, to - from)
                        }
                        onActiveTranslationChanged: {
                            if (active)
                                editor.dragTo = editor.placeFor(shownKey, activeTranslation.x, activeTranslation.y)
                        }
                        onCanceled: editor.dragTo = editor.dragFrom // put back where it was
                    }
                }
            }
        }

        Rectangle { // where the key dragged would go
            id: marker
            objectName: "rowActionsMarker"
            readonly property Item before: visible ? editor.keyAt(editor.dragTo) : null
            readonly property Item after: visible && !before ? editor.lastOther() : null
            visible: editor.dragFrom >= 0 && editor.dragTo !== editor.dragFrom
            x: editor.onPixel(before ? before.x - shownFlow.spacing / 2 - width / 2
                                     : after ? after.x + after.width + shownFlow.spacing / 2 - width / 2 : 0)
            y: before ? before.y + 4 : after ? after.y + 4 : 0
            width: 2
            height: (before ? before.height : after ? after.height : 0) - 8
            radius: 1
            color: Theme.accent
        }
    }

    Text {
        visible: editor.hidden.length > 0
        topPadding: 2
        text: qsTr("没有显示的，点一下加到最后")
        color: Theme.subtext
        font.pixelSize: Theme.fontCaption
    }

    Flow {
        objectName: "rowActionsHidden"
        width: parent.width
        spacing: 6
        visible: editor.hidden.length > 0

        Repeater {
            model: editor.hidden

            delegate: ActionKey {
                required property string modelData

                objectName: "rowActionKey_" + modelData
                action: modelData
                on: false
                onClicked: editor.settings.toggleRowAction(modelData)
            }
        }
    }

    HoverTip {
        target: editor.dragFrom < 0 ? editor.hoveredKey : null
        text: editor.hoveredKey ? editor.hoveredKey.kind.tip : ""
        dpr: editor.dpr
    }
}
