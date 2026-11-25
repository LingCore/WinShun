pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A row of the JSON tree: the name (or the index), then the value, or what
// an object or array holds while it is closed. Colours as VS Code's.
Item {
    id: row

    required property int index
    required property int depth
    required property string key
    required property bool keyIsIndex
    required property bool hasKey
    required property int kind // JsonTree's Kind: 0 object, 1 array, 2 string, 3 number, 4 bool, 5 null
    required property int shape
    required property string value
    required property string preview
    required property int count
    required property bool expandable
    required property bool expanded
    required property bool embedded
    required property int match // 1 the name, 2 the value
    property bool selected: false
    property real dpr: 1 // the list's (no Screen.* in a row: GitHub #111)
    property int codeSize: 14
    readonly property real indentX: 12 + depth * 18
    readonly property bool hovered: mouse.containsMouse
    readonly property bool cut: valueLabel.truncated
    readonly property color keyColor: Theme.dark ? "#9cdcfe" : "#0451a5"
    readonly property color stringColor: Theme.dark ? "#ce9178" : "#a31515"
    readonly property color numberColor: Theme.dark ? "#b5cea8" : "#098658"
    readonly property color wordColor: Theme.dark ? "#569cd6" : "#0000ff"

    signal clicked()
    signal toggled()
    signal menuRequested(real x, real y)

    Accessible.role: Accessible.TreeItem
    Accessible.name: (hasKey ? key + ": " : "") + (expandable ? preview : value)
    Accessible.selected: selected

    Loader { // the style's row highlight, made only while wanted
        anchors.fill: parent
        anchors.leftMargin: Theme.rowInset
        anchors.rightMargin: Theme.rowInset
        active: row.selected || row.hovered
        sourceComponent: Hook {
            part: Theme.rowHighlight
            selected: row.selected
            hovered: row.hovered
            role: "list"
            dpr: row.dpr
        }
    }
    Glyph {
        x: row.indentX
        anchors.verticalCenter: parent.verticalCenter
        visible: row.expandable
        glyph: row.expanded ? "ph:caret-down" : "ph:caret-right"
        size: 14
        color: Theme.subtext
    }
    Text {
        id: keyLabel
        x: row.indentX + 18
        width: row.hasKey ? Math.min(implicitWidth, Math.max(40, (row.width - x) * 0.6)) : 0
        anchors.verticalCenter: parent.verticalCenter
        visible: row.hasKey
        text: row.key
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: (row.match & 1) ? Theme.matchColor : row.keyIsIndex ? Theme.faint : row.keyColor
        font.family: "Consolas"
        font.pixelSize: row.codeSize
        font.weight: (row.match & 1) ? Theme.matchWeight : Font.Normal
    }
    Text {
        id: colon
        anchors.left: keyLabel.right
        anchors.verticalCenter: parent.verticalCenter
        width: row.hasKey ? implicitWidth : 0
        visible: row.hasKey
        text: ": "
        color: Theme.subtext
        font.family: "Consolas"
        font.pixelSize: row.codeSize
    }
    Rectangle { // a string that holds JSON, opened like an object
        id: chip
        anchors.left: colon.right
        anchors.verticalCenter: parent.verticalCenter
        visible: row.embedded
        width: visible ? chipLabel.implicitWidth + 10 : 0
        height: row.codeSize + 2
        radius: Theme.smallRadius
        color: Theme.accentTint
        Text {
            id: chipLabel
            anchors.centerIn: parent
            text: "JSON"
            color: Theme.accent
            font.pixelSize: Math.max(10, row.codeSize - 4)
            font.weight: Theme.fontStrong
        }
    }
    Text {
        id: valueLabel
        anchors.left: chip.right
        anchors.leftMargin: row.embedded ? 6 : 0
        anchors.right: countLabel.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: row.expandable ? (row.expanded ? (row.shape === 0 ? "{" : "[") : row.preview) : row.value
        textFormat: Text.PlainText
        elide: Text.ElideRight
        maximumLineCount: 1
        color: (row.match & 2) ? Theme.matchColor
             : row.expandable || row.kind < 2 ? Theme.subtext
             : row.kind === 2 ? row.stringColor
             : row.kind === 3 ? row.numberColor
             : row.wordColor
        font.family: "Consolas"
        font.pixelSize: row.codeSize
        font.weight: (row.match & 2) ? Theme.matchWeight : Font.Normal
    }
    Text {
        id: countLabel
        anchors.right: parent.right
        anchors.rightMargin: 12 + Theme.rowInset
        anchors.verticalCenter: parent.verticalCenter
        visible: row.expandable
        width: visible ? implicitWidth : 0
        text: row.shape === 0 ? qsTr("%n 个键", "", row.count) : qsTr("%n 项", "", row.count)
        color: Theme.faint
        font.pixelSize: Theme.uiCaption
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: (event) => {
            row.clicked()
            if (event.button === Qt.RightButton)
                row.menuRequested(event.x, event.y)
            else if (row.expandable && event.x >= row.indentX - 6 && event.x < row.indentX + 18)
                row.toggled() // the chevron
        }
        onDoubleClicked: (event) => {
            if (event.button === Qt.LeftButton && row.expandable && event.x >= row.indentX + 18)
                row.toggled()
        }
    }
}
