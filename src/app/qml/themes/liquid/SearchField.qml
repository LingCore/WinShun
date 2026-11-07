import QtQuick

InputField {
    property bool flat: false
    readonly property color iconColor: look ? look.subtext : "transparent"

    radius: look ? (flat ? look.fieldRadius : look.controlRadius) : 0
    // The launcher header is a single surface; settings search stays an input.
    baseColor: !look || flat ? "transparent" : focused ? look.inputFocused : hovered ? look.controlHover : look.control
    edgeColor: !look || flat ? "transparent" : error ? look.danger : focused ? look.accentLine : look.controlBorder
    border.width: flat ? 0 : focused || error ? 2 / Math.max(1, dpr) : hair
}
