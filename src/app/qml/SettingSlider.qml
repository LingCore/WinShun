import QtQuick
import QtQuick.Controls.Basic as Controls
import WinShun

Item {
    id: root
    property int value: 50
    property string label
    signal previewed(int value)
    signal committed(int value)
    implicitWidth: 240
    implicitHeight: 36
    opacity: enabled ? 1 : 0.4

    Controls.Slider {
        id: slider
        objectName: "settingSlider"
        anchors.left: parent.left
        anchors.right: readout.left
        anchors.rightMargin: 12
        height: parent.height
        from: 0
        to: 100
        stepSize: 1
        value: root.value
        Accessible.name: root.label
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                const limit = event.key === Qt.Key_Home ? slider.from : slider.to
                root.previewed(limit)
                root.committed(limit)
                event.accepted = true
            }
        }
        onMoved: {
            root.previewed(Math.round(slider.value))
            if (!slider.pressed) root.committed(Math.round(slider.value))
        }
        onPressedChanged: if (!slider.pressed) root.committed(Math.round(slider.value))
        background: Rectangle {
            x: slider.leftPadding + slider.handle.width / 2
            y: (slider.height - height) / 2
            width: slider.availableWidth - slider.handle.width
            height: 4
            radius: Theme.pillRadius(height)
            color: Theme.track
            Rectangle {
                width: slider.visualPosition * parent.width
                height: parent.height
                radius: parent.radius
                color: Theme.accentLine
            }
        }
        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: (slider.height - height) / 2
            width: 20
            height: 20
            radius: Theme.pillRadius(height)
            color: Theme.control
            border.width: slider.activeFocus ? 2 : Theme.edge(Screen.devicePixelRatio)
            border.color: slider.activeFocus ? Theme.accentLine : Theme.controlBorder
            Rectangle {
                anchors.centerIn: parent
                width: slider.pressed ? 8 : slider.hovered ? 12 : 10
                height: width
                radius: Theme.pillRadius(height)
                color: Theme.accentLine
            }
        }
    }
    Text {
        id: readout
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: 36
        horizontalAlignment: Text.AlignRight
        text: Math.round(slider.value) + "%"
        font.pixelSize: Theme.fontBody
        color: Theme.subtext
    }
}
