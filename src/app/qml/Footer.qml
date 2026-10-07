import QtQuick
import QuickFind

// Status on the left, keyboard hints on the right.
Item {
    id: footer

    required property Launcher launcher

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
    Component.onCompleted: showBusy = busy
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
        anchors.right: hints.left
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
            color: Theme.subtext
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
    }

    Row {
        id: hints
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 14

        // Plain words, not "↵": a symbol missing from the UI font triggers
        // system-wide font fallback, which costs tens of MB.
        KeyHint { keys: "Enter"; label: qsTr("打开") }
        KeyHint { keys: "Ctrl+Enter"; label: qsTr("打开位置") }
        KeyHint { keys: "Tab"; label: qsTr("切换范围") }
    }
}
