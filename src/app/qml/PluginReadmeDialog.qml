pragma ComponentBehavior: Bound
import QtQuick
import WinShun

Item {
    id: dialog
    objectName: "pluginReadmeDialog"
    required property PluginSettings plugins
    required property WindowFrame frame
    property bool open: false
    property string title
    property string html
    property Item returnFocus: null
    readonly property real dpr: Screen.devicePixelRatio
    visible: open

    function show(name, text) {
        returnFocus = Window.window.activeFocusItem
        title = name
        html = text
        scroll.contentY = 0
        open = true
        body.forceActiveFocus()
    }
    function close() {
        open = false
        html = ""
        returnFocus?.forceActiveFocus()
        returnFocus = null
    }
    Component.onCompleted: frame.addPopup(dialog)
    Connections {
        target: dialog.plugins
        function onReadmeRequested(title, html) { dialog.show(title, html) }
    }
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Escape || (event.key === Qt.Key_Q && event.modifiers === Qt.NoModifier)) {
            dialog.close(); event.accepted = true
        }
    }
    Rectangle {
        anchors.fill: parent
        color: Theme.tok("scrim", Qt.rgba(0, 0, 0, Theme.dark ? 0.5 : 0.3))
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; hoverEnabled: true; onWheel: (wheel) => wheel.accepted = true }
    }
    Item {
        id: card
        anchors.centerIn: parent
        width: Math.min(800, parent.width - 40)
        height: parent.height - 40
        Hook { anchors.fill: parent; part: Theme.menuFrame; dpr: dialog.dpr }
        MouseArea { anchors.fill: parent }
        Text {
            x: 20; y: 17
            width: parent.width - 128
            text: qsTr("%1 · 使用说明").arg(dialog.title)
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: Theme.fontTitle
            font.weight: Theme.fontStrong
        }
        RowAction {
            objectName: "closePluginReadme"
            anchors.right: parent.right; anchors.rightMargin: 14; y: 10
            glyph: "\uE711"; text: "Esc"; tip: qsTr("关闭说明（Esc / Q）"); dpr: dialog.dpr
            onClicked: dialog.close()
        }
        Flickable {
            id: scroll
            objectName: "pluginReadmeScroll"
            x: 20; y: 56; width: parent.width - 40; height: parent.height - 72
            contentWidth: width
            contentHeight: body.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            TextEdit {
                id: body
                objectName: "pluginReadmeText"
                width: scroll.width - 14
                height: Math.max(scroll.height, contentHeight)
                text: dialog.html
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                color: Theme.text
                selectionColor: Theme.accent
                selectedTextColor: Theme.accentText
                font.pixelSize: Theme.fontBody
                onLinkActivated: (link) => dialog.plugins.openReadmeLink(link)
                Keys.priority: Keys.BeforeItem
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Escape || (event.key === Qt.Key_Q && event.modifiers === Qt.NoModifier)) {
                        dialog.close(); event.accepted = true
                    } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
                        scroll.contentY = Math.max(0, Math.min(scroll.contentHeight - scroll.height,
                            scroll.contentY + (event.key === Qt.Key_PageDown ? 1 : -1) * scroll.height * 0.85))
                        event.accepted = true
                    }
                }
            }
        }
        ListScrollBar { flickable: scroll; anchors.right: scroll.right; y: scroll.y; height: scroll.height; dpr: dialog.dpr }
    }
}
