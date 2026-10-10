import QtQuick
import WinShun

// Status on the left, keyboard hints on the right, and at the very right a
// button that clears the recent items (while there are some to clear). In
// 文件 and 文件夹, before the hints, how the rows are sorted: click to switch
// between best match and last written. Dragging it moves the window.
Item {
    id: footer

    required property Launcher launcher
    required property WindowFrame frame

    implicitHeight: 42

    // The activity dot appears only once the work has lasted a moment, so
    // brief work does not make it (and the text beside it) blink.
    property bool showBusy: false
    readonly property bool busy: launcher.busy
    onBusyChanged: {
        if (busy) {
            busyDelay.start()
        } else {
            busyDelay.stop()
            showBusy = false
        }
    }
    Component.onCompleted: {
        showBusy = busy
        frame.addDragArea(footer)
    }
    Timer {
        id: busyDelay
        interval: 400
        onTriggered: footer.showBusy = footer.busy
    }

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.divider
    }

    Row {
        id: status
        anchors.left: parent.left
        anchors.leftMargin: 18
        anchors.right: sort.visible ? sort.left : hints.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        Rectangle { // activity dot while indexing or scanning file contents
            visible: footer.showBusy
            anchors.verticalCenter: parent.verticalCenter
            width: 6
            height: 6
            radius: 3
            color: Theme.accent

            SequentialAnimation on opacity {
                running: footer.showBusy && footer.Window.active
                loops: Animation.Infinite
                NumberAnimation { to: 0.25; duration: 600 }
                NumberAnimation { to: 1; duration: 600 }
            }
        }

        Text {
            width: status.width - (footer.showBusy ? 14 : 0)
            text: footer.launcher.statusText
            textFormat: Text.PlainText // may name a file
            color: Theme.subtext
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
    }

    RowAction {
        id: sort
        visible: footer.launcher.scope === Launcher.Files || footer.launcher.scope === Launcher.Folders
        anchors.right: hints.left
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        glyph: "\uE8CB" // Sort
        text: footer.launcher.rankByTime ? qsTr("最近修改的在前") : qsTr("最匹配的在前")
        tip: footer.launcher.rankByTime ? qsTr("点一下改为最匹配的在前") : qsTr("点一下改为最近修改的在前")
        onClicked: footer.launcher.rankByTime = !footer.launcher.rankByTime
        Component.onCompleted: footer.frame.addControl(sort) // a button, not part of the drag area
    }

    Row {
        id: hints
        anchors.right: clearHistory.visible ? clearHistory.left : parent.right
        anchors.rightMargin: clearHistory.visible ? 10 : 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 14

        // Plain words, not "↵": a symbol missing from the UI font triggers
        // system-wide font fallback, which costs tens of MB.
        KeyHint { keys: "Enter"; label: qsTr("打开") }
        KeyHint { keys: "Ctrl+Enter"; label: qsTr("打开位置") }
        KeyHint { keys: "Tab"; label: qsTr("切换范围") }
    }

    // Two clicks, as a row's delete button: the first arms it.
    RowAction {
        id: clearHistory

        property bool armed: false

        visible: footer.launcher.canClearHistory
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        glyph: "\uE74D" // Delete
        tip: armed ? "" : qsTr("清除最近使用记录")
        text: armed ? qsTr("确认清除") : ""
        danger: armed
        onClicked: {
            if (armed) {
                armed = false
                footer.launcher.clearHistory()
            } else {
                armed = true
                disarm.restart()
            }
        }
        onVisibleChanged: armed = false
        Component.onCompleted: footer.frame.addControl(clearHistory) // a button, not part of the drag area

        Timer {
            id: disarm
            interval: 3000
            onTriggered: clearHistory.armed = false
        }
    }

    HoverTip {
        target: clearHistory.tipWanted ? clearHistory : sort.tipWanted ? sort : null
        text: clearHistory.tipWanted ? clearHistory.tip : sort.tip
    }
}
