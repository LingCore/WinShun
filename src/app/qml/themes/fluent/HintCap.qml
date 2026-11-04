import QtQuick

// keyHint: a key cap that only tells (KeyHint "Ctrl+Enter 打开位置", the
// footer's): never pressable, so flat — a thin neutral outline, no fill.
Rectangle {
    property QtObject look: null
    property real dpr: 1

    radius: look ? look.controlRadius : 0
    color: "transparent"
    border.width: Math.max(1, Math.round(dpr)) / dpr
    border.color: look ? look.keyEdge : "transparent"
}
