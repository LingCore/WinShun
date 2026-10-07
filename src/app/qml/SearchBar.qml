import QtQuick
import QuickFind

// Search icon, the input field and the scope switcher.
Item {
    id: bar

    required property Launcher launcher
    property alias text: input.text
    readonly property bool hasSelection: input.selectedText.length > 0

    signal keyPressed(var event)

    function focusAndSelect() {
        input.forceActiveFocus()
        input.selectAll()
    }

    implicitHeight: 64

    Glyph {
        id: icon
        anchors.left: parent.left
        anchors.leftMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        glyph: bar.launcher.scope === Launcher.Content ? "" // Document
             : bar.launcher.scope === Launcher.Apps ? "" // AllApps
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
