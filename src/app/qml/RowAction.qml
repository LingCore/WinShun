import QtQuick
import WinShun

// A small icon button at the end of a result row (reveal, copy, delete).
// `danger` paints it red, with an optional label: the armed delete button.
// `checked`: what it opens is open (a clipboard row's preview). `tip` (and
// `tipShortcut`) are for the HoverTip the row shares among its buttons;
// `tipWanted` says when the mouse rests on this one.
Rectangle {
    id: button

    property string glyph
    property string glyphOverlay // drawn over `glyph` (see Glyph.overlay)
    property string text
    property bool danger: false
    property bool checked: false
    property string tip
    property string tipShortcut
    readonly property bool tipWanted: area.containsMouse && !area.used && tip.length > 0
    readonly property color foreground: surface.out("foreground", danger ? Theme.accentText : checked ? Theme.accent
                                                    : area.containsMouse ? Theme.text : Theme.subtext)
    property string role: "rowAction" // Hook.role
    readonly property real dpr: Screen.devicePixelRatio

    // A handle to drag (a clipboard row in its group's order): held, every
    // move of the mouse is told, in this button's coordinates.
    property bool draggable: false
    property bool small: false // a smaller box and icon (the clipboard's 密集 rows)
    readonly property bool held: area.pressed

    signal clicked()
    signal dragMoved(point position)

    implicitWidth: text.length > 0 ? content.implicitWidth + 20 : small ? 24 : 32
    implicitHeight: small ? 24 : 32
    radius: Theme.controlRadius
    color: "transparent"
    opacity: enabled ? 1 : surface.out("disabledOpacity", 0.45)

    Hook { // the style's key (Theme.keySurface): flat until hovered
        id: surface
        anchors.fill: parent
        part: Theme.keySurface
        hovered: area.containsMouse
        pressed: area.pressed
        current: button.checked // what it opens is open: marked, not a filled toggle
        danger: button.danger
        flat: true
        role: button.role
        extra: ({ corner: Theme.capsuleKeyRadius })
        dpr: button.dpr
    }

    Row {
        id: content
        anchors.centerIn: parent
        anchors.verticalCenterOffset: surface.out("contentOffsetY", 0)
        spacing: 6

        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            glyph: button.glyph
            overlay: button.glyphOverlay
            size: button.small ? 16 : 64 / 3 // 32 device pixels at 150%
            color: button.foreground
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.text.length > 0
            text: button.text
            color: button.foreground
            font.pixelSize: Theme.fontCaption
        }
    }

    MouseArea {
        id: area

        property bool used: false // clicked since the mouse came: no tooltip until it leaves

        anchors.fill: parent
        hoverEnabled: true
        preventStealing: button.draggable // the list does not scroll under a drag
        onPressed: used = true
        onContainsMouseChanged: if (!containsMouse) used = false
        onClicked: button.clicked()
        onPositionChanged: (mouse) => {
            if (button.draggable && pressed)
                button.dragMoved(Qt.point(mouse.x, mouse.y))
        }

        Binding on cursorShape { // the other buttons keep the cursor they have
            when: button.draggable
            value: area.pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        }
    }
}
