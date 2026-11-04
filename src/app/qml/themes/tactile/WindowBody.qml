pragma ComponentBehavior: Bound
import QtQuick

// The whole window as one solid panel of material: warm white (graphite on
// dark) with a slight top light and a fine grain, radius 18 and a 1 px
// neutral grey edge. The host places it twice: paint "back" under
// everything (the body, in its `fill`: launcher background, settings page)
// and paint "edge" over everything (the edge); "all" both. The shadow is
// drawn by C++ from Look.windowShadow. Maximized: square, no edge.
Item {
    id: frame

    property string paint: "all"   // back | edge | all
    property color fill: "transparent" // the window's body colour (transparent: the panel's own)
    property bool active: true
    property bool maximized: false
    property real dpr: 1

    property QtObject look // injected by Look.qml
    readonly property bool fillsScreen: maximized || (Window.window !== null && Window.window.visibility === Window.FullScreen)
    readonly property bool edged: !fillsScreen && !!look && look.drawsWindowEdge
    readonly property real radius: fillsScreen || !look ? 0 : look.windowRadius
    readonly property real edgeWidth: edged ? look.edge(dpr) : 0
    readonly property real contentInset: edgeWidth
    // The panel: its own two tones, or the host's fill with the same top light.
    readonly property bool ownTone: !look || fill.a === 0 || Qt.colorEqual(fill, look.panelBottom)
    readonly property color bottomTone: !look ? "transparent" : ownTone ? look.panelBottom : fill
    readonly property color topTone: !look ? "transparent" : ownTone ? look.panelTop : Qt.tint(fill, look.dark ? look.white(0.033) : look.white(0.55))
    readonly property bool drawsBack: paint !== "edge"

    // Built once `look` is set: a part's own bindings can run before Look's
    // `look: root` (Hook makes it with createObject).
    Loader {
        anchors.fill: parent
        active: frame.look !== null
        sourceComponent: Item {
            // ---- panel ----
            Rectangle {
                visible: frame.drawsBack
                anchors.fill: parent
                radius: frame.radius
                gradient: Gradient {
                    GradientStop { position: 0; color: frame.topTone }
                    GradientStop { position: frame.height > 0 ? Math.min(1, (frame.look?.panelLightHeight ?? 0) / frame.height) : 1; color: frame.bottomTone }
                    GradientStop { position: 1; color: frame.bottomTone }
                }
            }

            // ---- grain: a tiled 128 px noise, kept off the four corner squares
            // (outside the arc it would show; inside it nobody misses 4 %) ----
            component Grain: Image {
                visible: frame.drawsBack
                source: (frame.look?.grainSource ?? "")
                fillMode: Image.Tile
                opacity: (frame.look?.grainOpacity ?? 0)
                smooth: false
            }
            Grain {
                y: frame.radius
                width: frame.width
                height: Math.max(0, frame.height - 2 * frame.radius)
            }
            Grain {
                x: frame.radius
                width: Math.max(0, frame.width - 2 * frame.radius)
                height: frame.radius
            }
            Grain {
                x: frame.radius
                y: frame.height - frame.radius
                width: Math.max(0, frame.width - 2 * frame.radius)
                height: frame.radius
            }

            // ---- edge (over everything; the host keeps its content inside it) ----
            Rectangle {
                visible: frame.edged && frame.paint !== "back"
                anchors.fill: parent
                radius: frame.radius
                color: "transparent"
                border.width: frame.edgeWidth
                border.color: (frame.look?.edgeColor ?? Qt.rgba(0, 0, 0, 0))
            }
        }
    }
}
