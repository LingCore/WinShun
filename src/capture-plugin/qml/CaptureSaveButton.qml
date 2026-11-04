pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import WinShun

Row {
    id: split

    property bool tipsEnabled: true
    property string keyText: "Ctrl+S" // the save key; empty: none (#187)
    // A save folder was chosen: the button saves into it and the menu offers
    // 另存为… (#194); none: the button is 另存为… and the menu 保存到旧路径.
    property bool direct: false
    signal saveRequested()
    signal savePreviousRequested()
    signal saveAsRequested()
    signal keyForwarded(var event)
    signal keyReleased(var event)

    onVisibleChanged: if (!visible) saveMenu.close()

    RecordingButton {
        id: save
        objectName: "captureSaveButton"
        implicitWidth: 32
        glyph: "\uE74E"
        tip: split.direct ? (split.keyText.length > 0 ? qsTr("保存（%1）").arg(split.keyText) : qsTr("保存"))
                          : (split.keyText.length > 0 ? qsTr("另存为…（%1）").arg(split.keyText) : qsTr("另存为…"))
        tipsEnabled: split.tipsEnabled && !saveMenu.visible
        onClicked: split.saveRequested()
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                split.saveRequested()
                event.accepted = true
            } else if (event.key !== Qt.Key_Space) {
                split.keyForwarded(event)
            }
        }
        Keys.onReleased: (event) => { if (event.key !== Qt.Key_Space) split.keyReleased(event) }
    }

    RecordingButton {
        id: arrow
        objectName: "captureSaveMenuButton"
        implicitWidth: 24
        glyph: "\uE70D"
        tip: qsTr("更多保存选项")
        tipsEnabled: split.tipsEnabled && !saveMenu.visible
        onClicked: saveMenu.visible = !saveMenu.visible
        Accessible.role: Accessible.ButtonMenu
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Down) {
                saveMenu.visible = !saveMenu.visible
                event.accepted = true
            } else if (event.key !== Qt.Key_Space) {
                split.keyForwarded(event)
            }
        }
        Keys.onReleased: (event) => { if (event.key !== Qt.Key_Space) split.keyReleased(event) }

        Menu {
            id: saveMenu
            objectName: "captureSaveMenu"
            parent: arrow
            width: Math.max(160, previous.implicitWidth + leftPadding + rightPadding)
            y: arrow.height + 4
            margins: 8
            padding: 4
            popupType: Popup.Item
            modal: true
            dim: false
            // Modal dismissal consumes the press, including on the arrow,
            // so the same click cannot reopen the menu or activate Copy.
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            onAboutToShow: {
                const window = split.Window.window
                const frame = window ? (window.frame ?? null) : null
                if (frame)
                    frame.addPopup(saveMenu.background)
            }
            enter: null
            exit: null
            background: Rectangle {
                color: Theme.menuBackground
                radius: Theme.cardRadius
                border.width: 1
                border.color: Theme.windowEdge
            }
            MenuItem {
                id: previous
                objectName: "captureSavePreviousItem"
                text: split.direct ? qsTr("另存为…") : qsTr("保存到旧路径")
                implicitHeight: 34
                implicitWidth: label.implicitWidth + 32
                onTriggered: split.direct ? split.saveAsRequested() : split.savePreviousRequested()
                contentItem: Text {
                    id: label
                    text: previous.text
                    leftPadding: 12
                    rightPadding: 12
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: Theme.fontCaption
                    color: Theme.text
                }
                background: Rectangle {
                    color: previous.highlighted ? Theme.menuHover : "transparent"
                    radius: Theme.controlRadius
                }
            }
        }
    }
}
