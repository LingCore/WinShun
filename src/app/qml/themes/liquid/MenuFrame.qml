import QtQuick

GlassSurface {
    radius: look ? look.menuRadius : 0
    baseColor: look ? look.menuBackground : "transparent"
    edgeColor: look ? look.menuEdge : "transparent"
}
