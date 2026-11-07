import QtQuick

GlassSurface {
    property bool focused: false
    property bool hovered: false
    property bool error: false
    readonly property color textColor: look ? look.text : "transparent"
    readonly property color placeholderColor: look ? look.subtext : "transparent"
    readonly property color caretColor: look ? look.accentLine : "transparent"

    baseColor: !look ? "transparent" : focused ? look.inputFocused : hovered ? look.controlHover : look.control
    edgeColor: !look ? "transparent" : error ? look.danger : focused ? look.accentLine : look.controlBorder
    border.width: focused || error ? 2 / Math.max(1, dpr) : hair
}
