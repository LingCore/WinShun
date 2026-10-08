import QtQuick
import WinShun

// Search icon, the input field and the scope switcher. The rest of it drags
// the window (see WindowFrame).
Item {
    id: bar

    required property Launcher launcher
    required property Placement placement
    required property WindowFrame frame
    property alias text: input.text
    readonly property bool hasSelection: input.selectedText.length > 0
    // The pointer over the header: Windows takes its drag area as a title bar,
    // so QML hears of the pointer there only from the frame.
    readonly property bool hovered: hover.hovered || contains(mapFromItem(null, frame.pointer))

    signal keyPressed(var event)

    function focusAndSelect() {
        input.forceActiveFocus()
        input.selectAll()
    }

    implicitHeight: 64

    Component.onCompleted: {
        frame.addDragArea(bar)
        frame.addControl(input)
        frame.addControl(tabs)
    }

    HoverHandler { id: hover } // over the field and the tabs

    // Grip: shows that the header moves the window. Faint while the pointer is
    // over the header; pointed at, it grows and brightens; while the window
    // moves, it takes the accent colour.
    Item {
        id: grip

        readonly property bool pointed: contains(mapFromItem(null, bar.frame.pointer))
        readonly property bool held: bar.placement.moving

        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        width: 72 // easier to find than the pill itself
        height: 14

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 6 // even: on whole device pixels at 150 %
            width: grip.held || grip.pointed ? 48 : 36
            height: 4
            radius: 2
            color: grip.held ? Theme.accent : grip.pointed ? Theme.subtext : Theme.faint
            opacity: grip.held || grip.pointed ? 1 : bar.hovered ? 0.7 : 0
            Behavior on width { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
            Behavior on color { ColorAnimation { duration: 150 } }
            Behavior on opacity { NumberAnimation { duration: 150 } }
        }
    }

    Glyph {
        id: icon
        anchors.left: parent.left
        anchors.leftMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        glyph: bar.launcher.scope === Launcher.Content ? "" // Document
             : "" // Search
        size: 22
    }

    TextInput {
        id: input
        anchors.left: icon.right
        anchors.leftMargin: 14
        anchors.right: tabs.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        focus: true
        clip: true
        color: Theme.text
        font.pixelSize: Theme.fontSearch
        selectByMouse: true
        selectionColor: Theme.textSelection
        selectedTextColor: Theme.text
        onTextChanged: bar.launcher.query = text
        Keys.onPressed: (event) => bar.keyPressed(event)

        Text {
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            text: bar.launcher.placeholder
            color: Theme.faint
            font: input.font
            elide: Text.ElideRight
            visible: input.text.length === 0 && input.preeditText.length === 0
        }
    }

    ScopeTabs {
        id: tabs
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        current: bar.launcher.scope
        onActivated: (scope) => bar.launcher.scope = scope
    }
}
