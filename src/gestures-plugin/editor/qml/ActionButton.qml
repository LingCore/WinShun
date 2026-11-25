import QtQuick
import WinShun

// What a rule does, as the control that changes it: the style's key (as
// FlatButton's) with the action's icon, its name and a chevron. An action
// not set shows "未设置" in faint words with a plus; a gesture just drawn,
// not yet given an action, asks for one in the accent.
Rectangle {
    id: button

    property string icon: "ph:plus"
    property string text
    property bool unset: false
    property bool invalid: false
    property bool asking: false // just drawn: choose what it does
    property bool compact: false // a spot on the screen diagram: no chevron
    property bool open: false // its picker is open
    property real trailingSpace: 0 // room kept at the right for what the host puts there
    property real dpr: 0
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio
    readonly property bool hovered: area.containsMouse
    readonly property color ink: invalid ? Theme.danger : asking ? Theme.accent : unset ? Theme.faint : Theme.text

    signal clicked()

    implicitWidth: 220
    implicitHeight: 32
    radius: Theme.controlRadius
    color: "transparent"
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: text

    Hook { // the style's key (Theme.keySurface)
        id: surface
        anchors.fill: parent
        part: Theme.keySurface
        hovered: area.containsMouse
        pressed: area.pressed || button.open
        dpr: button.pixelRatio
    }

    Keys.onSpacePressed: button.clicked()
    Keys.onReturnPressed: button.clicked()

    Glyph {
        id: leading
        x: 10
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: surface.out("contentOffsetY", 0)
        glyph: button.icon
        size: 18
        color: button.ink
    }
    Text {
        id: label
        anchors.left: leading.right
        anchors.leftMargin: 8
        anchors.right: chevron.visible ? chevron.left : parent.right
        anchors.rightMargin: (chevron.visible ? 6 : 10) + button.trailingSpace
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: surface.out("contentOffsetY", 0)
        text: button.text
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: button.ink
        font.pixelSize: Theme.fontBody
    }
    Glyph {
        id: chevron
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: surface.out("contentOffsetY", 0)
        visible: !button.compact
        glyph: "ph:caret-down"
        size: 14
        color: Theme.subtext
    }

    Rectangle { // keyboard focus
        visible: button.activeFocus
        anchors.fill: parent
        anchors.margins: -3
        radius: Theme.largeControlRadius
        color: "transparent"
        border.width: 2
        border.color: Theme.text
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
