pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import WinShun

Popup {
    id: popup

    required property ComboBox control

    y: control.height + 4
    width: control.width
    height: Math.min(280, contentItem.implicitHeight + topPadding + bottomPadding)
    topMargin: 6
    bottomMargin: 6
    padding: Theme.menuPadding
    font: control.font
    palette: control.palette

    contentItem: Item {
        implicitHeight: list.contentHeight
        ListView {
            id: list
            anchors.fill: parent
            clip: true
            model: popup.control.delegateModel
            currentIndex: popup.control.highlightedIndex
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
        }
        ListScrollBar {
            flickable: list
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            dpr: popup.control.Screen.devicePixelRatio
        }
    }

    background: Hook {
        part: Theme.menuFrame
        dpr: popup.control.Screen.devicePixelRatio
    }
}
