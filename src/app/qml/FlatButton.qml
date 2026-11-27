import QtQuick
import WinShun

// A Windows 11 style push button, optionally with an icon glyph.
Rectangle {
    id: button

    property string text
    property string glyph
    property string trailingGlyph // after the text (a drop-down's chevron)
    property int fontSize: Theme.fontBody
    property int glyphSize: 16
    property real contentSpacing: 6 // icon to text; a round icon (arrow-clockwise) fills its box and wants a bit more
    property bool highlighted: false
    property bool flat: false // no background until hovered
    // false: Tab skips it and Space and Enter do nothing; only a click works
    // (for a button that a key meant for another window must not press).
    property bool keyboardActivation: true
    property real corner: -1 // a radius of the host's (-1: the style's)
    property string role // where it stands (Hook.role: "footer", "toolbar", "dialog" …)
    property real dpr: 0 // the window's, passed by a host that has it (GitHub #111)
    // Screen.* only when no host passed it: a default binding to it would make a
    // Screen object even where the host's value replaces it.
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio
    readonly property color foreground: surface.out("foreground", highlighted ? Theme.accentText : Theme.text)

    signal clicked()

    implicitWidth: Math.max(32, content.implicitWidth + (text.length > 0 ? 24 : 16))
    implicitHeight: [32, 36, 40][Theme.step]
    radius: corner >= 0 ? corner : Theme.controlRadius
    opacity: enabled ? 1 : surface.out("disabledOpacity", 0.45)
    activeFocusOnTab: keyboardActivation
    color: "transparent"

    Hook { // the style's key (Theme.keySurface)
        id: surface
        anchors.fill: parent
        part: Theme.keySurface
        hovered: area.containsMouse
        pressed: area.pressed
        highlighted: button.highlighted
        flat: button.flat
        corner: button.corner
        role: button.role
        dpr: button.pixelRatio
    }

    Keys.onSpacePressed: if (keyboardActivation) button.clicked()
    Keys.onReturnPressed: if (keyboardActivation) button.clicked()
    Keys.onEnterPressed: if (keyboardActivation) button.clicked()

    Row {
        id: content
        anchors.centerIn: parent
        anchors.verticalCenterOffset: surface.out("contentOffsetY", 0)
        spacing: button.contentSpacing

        Glyph {
            inkScale: surface.out("contentScale", 1)
            anchors.verticalCenter: parent.verticalCenter
            visible: button.glyph.length > 0
            glyph: button.glyph
            size: button.glyphSize
            color: button.foreground
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.text.length > 0
            text: button.text
            color: button.foreground
            font.pixelSize: button.fontSize
            font.weight: surface.out("fontWeight", Font.Normal)
        }
        // (trailingGlyph: its icon comes here, trailingIcon)
    }

    // The icon after the words: made for the few buttons that have one (a
    // drop-down's chevron), after the words in the Row.
    property Item trailingIcon: null
    onTrailingGlyphChanged: {
        if (trailingGlyph.length === 0 || trailingIcon)
            return
        trailingIcon = Qt.createComponent("Glyph.qml").createObject(content, {
            glyph: Qt.binding(() => button.trailingGlyph),
            visible: Qt.binding(() => button.trailingGlyph.length > 0),
            size: 12,
            color: Qt.binding(() => button.foreground),
            inkScale: Qt.binding(() => surface.out("contentScale", 1))
        }) as Item
        if (trailingIcon)
            trailingIcon.anchors.verticalCenter = content.verticalCenter
    }

    // Keyboard focus: its ring, made the first time the button has it (FocusRing).
    property Item focusRing: null
    onActiveFocusChanged: {
        if (activeFocus && !focusRing)
            focusRing = Qt.createComponent("FocusRing.qml").createObject(button, { host: button }) as Item
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
