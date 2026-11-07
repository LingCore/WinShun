import QtQuick

Rectangle {
    property QtObject look: null
    property string paint: "back"
    property color fill: "transparent"
    property bool active: true
    property bool maximized: false
    property real dpr: 1

    // DWM alone owns the outside edge, shadow and corner clipping.
    visible: paint === "back" && look !== null
    color: look && look.backdrop ? look.windowTint : "transparent"
}
