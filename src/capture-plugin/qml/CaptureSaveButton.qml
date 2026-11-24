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
    // Non-empty when editing a writable local image file (#427): primary button
    // saves back to the original file; the menu offers 另存为… and 保存到截图文件夹.
    property string sourceFileName: ""
    readonly property bool hasSource: sourceFileName.length > 0
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
        tip: split.hasSource
             ? (split.keyText.length > 0 ? qsTr("保存到原文件 %1（%2）").arg(split.sourceFileName).arg(split.keyText)
                                         : qsTr("保存到原文件 %1").arg(split.sourceFileName))
             : split.direct
               ? (split.keyText.length > 0 ? qsTr("保存（%1）").arg(split.keyText) : qsTr("保存"))
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
            width: Math.max(160, Math.max(previous.implicitWidth, saveFolderItem.visible ? saveFolderItem.implicitWidth : 0) + leftPadding + rightPadding)
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
            background: Item {
                Hook { // the style's menu box (Theme.menuFrame)
                    anchors.fill: parent
                    part: Theme.menuFrame
                    dpr: Screen.devicePixelRatio
                }
            }
            MenuItem {
                id: previous
                objectName: "captureSavePreviousItem"
                text: (split.hasSource || split.direct) ? qsTr("另存为…") : qsTr("保存到旧路径")
                implicitHeight: 34
                implicitWidth: label.implicitWidth + 32
                onTriggered: (split.hasSource || split.direct) ? split.saveAsRequested() : split.savePreviousRequested()
                contentItem: Text {
                    id: label
                    text: previous.text
                    leftPadding: 12
                    rightPadding: 12
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: Theme.fontCaption
                    color: Theme.text
                }
                background: Item {
                    Hook { // the style's row highlight (Theme.rowHighlight)
                        anchors.fill: parent
                        part: Theme.rowHighlight
                        selected: previous.highlighted
                        hovered: previous.hovered
                        role: "menu"
                        dpr: Screen.devicePixelRatio
                    }
                }
            }
            MenuItem {
                id: saveFolderItem
                objectName: "captureSaveFolderItem"
                visible: split.hasSource
                height: visible ? implicitHeight : 0
                text: qsTr("保存到截图文件夹")
                implicitHeight: 34
                implicitWidth: folderLabel.implicitWidth + 32
                onTriggered: split.savePreviousRequested()
                contentItem: Text {
                    id: folderLabel
                    text: saveFolderItem.text
                    leftPadding: 12
                    rightPadding: 12
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: Theme.fontCaption
                    color: Theme.text
                }
                background: Item {
                    Hook { // the style's row highlight (Theme.rowHighlight)
                        anchors.fill: parent
                        part: Theme.rowHighlight
                        selected: saveFolderItem.highlighted
                        hovered: saveFolderItem.hovered
                        role: "menu"
                        dpr: Screen.devicePixelRatio
                    }
                }
            }
        }
    }
}
