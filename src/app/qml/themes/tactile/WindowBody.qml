pragma ComponentBehavior: Bound
import QtQuick

// The whole window as one solid panel of material: warm white (graphite on
// dark) with a slight top light and a fine grain. The host places it under
// everything (paint "back"; the body, in its `fill`: launcher background,
// settings page). Square: Windows rounds, edges and shadows the window (DWM).
Item {
    id: frame

    property string paint: "all"   // back | edge | all (no edge of its own)
    property color fill: "transparent" // the window's body colour (transparent: the panel's own)
    property bool active: true
    property bool maximized: false
    property real dpr: 1

    property QtObject look // injected by Look.qml
    // The panel: its own two tones, or the host's fill with the same top light.
    readonly property bool ownTone: !look || fill.a === 0 || Qt.colorEqual(fill, look.panelBottom)
    readonly property color bottomTone: !look ? "transparent" : ownTone ? look.panelBottom : fill
    readonly property color topTone: !look ? "transparent" : ownTone ? look.panelTop : Qt.tint(fill, look.dark ? look.white(0.033) : look.white(0.55))

    visible: paint !== "edge"

    // Built once `look` is set: a part's own bindings can run before Look's
    // `look: root` (Hook makes it with createObject).
    Loader {
        anchors.fill: parent
        active: frame.look !== null
        sourceComponent: Item {
            // ---- panel ----
            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0; color: frame.topTone }
                    GradientStop { position: frame.height > 0 ? Math.min(1, (frame.look?.panelLightHeight ?? 0) / frame.height) : 1; color: frame.bottomTone }
                    GradientStop { position: 1; color: frame.bottomTone }
                }
            }

            // ---- grain: a tiled 128 px noise ----
            Image {
                anchors.fill: parent
                source: (frame.look?.grainSource ?? "")
                fillMode: Image.Tile
                opacity: (frame.look?.grainOpacity ?? 0)
                smooth: false
            }
        }
    }
}
