import QtQuick

GlassSurface {
    property bool hovered: false
    property real corner: -1
    property bool inset: false
    property bool tag: false

    radius: corner >= 0 ? corner : look ? look.cardRadius : 0
    baseColor: !look || inset ? "transparent" : tag ? look.control : look.cardFill
    edgeColor: !look || inset ? "transparent" : look.cardBorder
}
