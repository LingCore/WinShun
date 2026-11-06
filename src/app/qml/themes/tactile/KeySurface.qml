pragma ComponentBehavior: Bound
import QtQuick

// The one physical part: a key. The box IS the visible key: a 1 px rim
// (darker along the bottom), a face with a vertical gradient and a 1 px top
// highlight, and a 2 px side drawn inside the bottom; a thin contact shadow
// tucked under it. Down (pressed or checked) the face sinks 2 px, a 2 px wall
// shows above it and the side goes; the outline never moves.
// The host centres its label on the box and adds contentOffsetY.
Item {
    id: key

    // state (Hook)
    property bool hovered: false
    property bool pressed: false
    property bool checked: false
    property bool current: false // the preview/menu opened by this key remains open
    property bool highlighted: false // a primary button: an orange key, white words
    property bool danger: false      // an armed delete: a red key, white words
    property bool flat: false        // a toolbar icon button: nothing at rest, a key under the pointer
    property real corner: -1         // the host's radius (-1: by role)
    property string role             // "" | rowAction | fieldKey (in the search groove: radius 2) | toolbar | footer | dialog
    property real dpr: 1
    // tactile inputs (Hook.extra): a lamp at the left (scope keys, 不限) lit
    // while checked, the contact shadow (off for menu items), and a deep
    // colour the face takes when down (a pressed coloured type key).
    property bool indicator: false
    property bool drop: true
    property color downFill: "transparent"
    readonly property real radius: corner >= 0 ? corner : !look ? 0 : role === "fieldKey" ? look.grooveKeyRadius : look.keyRadius

    // for the host
    readonly property bool down: pressed || checked || current
    readonly property bool bare: flat && !hovered && !down // a disabled one too: faint words on nothing, as the other styles' flat keys
    readonly property real contentOffsetY: bare || !look ? 0 : down ? look.keyTravel / 2 : -look.keyTravel / 2
    // A solid colour key: primary, armed delete, or a coloured key held down.
    readonly property color solid: !look ? "transparent" : danger ? look.dangerFillColor : highlighted ? look.accentFillColor
                                   : downFill.a > 0 && down ? downFill : "transparent"
    readonly property bool tinted: solid.a > 0 && enabled
    readonly property color foreground: !look ? "transparent" : !enabled ? look.ink3 : tinted ? "#FFFFFF" : current ? look.accent : (down || hovered) ? look.ink : look.keyInk
    readonly property color iconColor: foreground
    readonly property int fontWeight: checked && !tinted ? Font.Bold : Font.Normal
    readonly property real leadingInset: indicator && look ? look.ledInset + look.ledSize + look.ledGap : 0
    // Disabled keys draw themselves flat with faint words: no fading by the host.
    readonly property real disabledOpacity: 1

    property QtObject look // injected by Look.qml
    // A key shows as it is when made: its state can still change in the same
    // turn (TypeStrip's chips, made anew for 文件 or 内容, are checked just
    // after), which must not play as a press. Changes after that animate.
    property bool settled: false
    Component.onCompleted: Qt.callLater(() => key.settled = true)
    readonly property real hair: look ? look.hair(dpr) : 1
    readonly property real innerRadius: Math.max(0, radius - hair)
    readonly property real bodyHeight: height - 2 * hair

    // Face, rim and side colours. A solid key at rest keeps the material's
    // build (top light, side, darker bottom rim) in its own colour.
    readonly property color faceTop: !look ? "transparent" : !enabled ? look.keyDisabledFace : tinted ? (down ? solid : Qt.lighter(solid, 1.05))
                                     : down ? look.keyDownTop : look.keyFaceTop
    readonly property color faceBottom: !look ? "transparent" : !enabled ? look.keyDisabledFace : tinted ? solid : down ? look.keyDownBottom : look.keyFaceBottom
    readonly property color rimColor: !look ? "transparent" : tinted ? look.chipDownRim : look.keyRim
    readonly property color rimBottom: !look ? "transparent" : tinted ? look.chipDownWall : look.keyRimBottom
    readonly property color sideColor: !look ? "transparent" : !enabled ? look.keyDisabledFace : tinted ? Qt.darker(solid, 1.45) : look.keySideColor
    readonly property color capColor: !look ? "transparent" : tinted ? (down ? Qt.tint(solid, look.chipDownWall) : Qt.tint(faceTop, look.white(0.22)))
                                      : down ? Qt.tint(look.keyDownTop, look.keyWall)
                                      : Qt.tint(look.keyFaceTop, look.keyHighlight)

    // Built once `look` is set (a part's own bindings can run before Look's
    // `look: root`), and not at all for a flat key at rest.
    Loader {
        anchors.fill: parent
        active: key.look !== null && !key.bare
        sourceComponent: Item {

            // contact shadow: one soft device-pixel line under the bottom edge
            Rectangle {
                visible: key.drop && key.enabled && !key.down
                x: key.radius * 0.6
                y: key.height
                width: key.width - 2 * x
                height: key.hair
                color: (key.look?.keyDrop ?? Qt.rgba(0, 0, 0, 0))
                opacity: 0.1 // CSS 0 1px 1px -1px: barely there, it only seats the key
            }

            // rim, darker along the bottom; laid over the face colour, as a CSS
            // border lies over the element's own background
            Rectangle {
                anchors.fill: parent
                radius: key.radius
                gradient: Gradient {
                    GradientStop { position: 0; color: Qt.tint(key.faceTop, key.rimColor) }
                    GradientStop { position: key.height > 0 ? Math.max(0, (key.height - key.radius - 1) / key.height) : 0.5; color: Qt.tint(key.faceBottom, key.rimColor) }
                    GradientStop { position: 1; color: Qt.tint(key.faceBottom, key.down || !key.enabled ? key.rimColor : key.rimBottom) }
                }
            }

            // padding box: the side at rest
            Rectangle {
                x: key.hair
                y: key.hair
                width: key.width - 2 * key.hair
                height: key.bodyHeight
                radius: key.innerRadius
                color: key.sideColor
            }

            // top light at rest, the wall when down: the face below leaves a crescent of it
            Rectangle {
                visible: key.enabled
                x: key.hair
                y: key.hair
                width: key.width - 2 * key.hair
                height: key.down ? key.bodyHeight : key.bodyHeight - (key.look?.keyTravel ?? 0)
                radius: key.innerRadius
                color: key.capColor
            }

            // face
            Rectangle {
                id: face
                x: key.hair
                width: key.width - 2 * key.hair
                y: !key.enabled ? key.hair : key.down ? key.hair + (key.look?.keyTravel ?? 0) : 2 * key.hair
                height: !key.enabled ? key.bodyHeight : key.bodyHeight - (key.look?.keyTravel ?? 0) - (key.down ? 0 : key.hair)
                radius: key.innerRadius
                gradient: Gradient {
                    GradientStop { position: 0; color: key.faceTop }
                    GradientStop { position: 1; color: key.faceBottom }
                }
                Behavior on y { enabled: key.settled; NumberAnimation { duration: (key.look?.animFast ?? 0); easing.type: Easing.OutCubic } }
                Behavior on height { enabled: key.settled; NumberAnimation { duration: (key.look?.animFast ?? 0); easing.type: Easing.OutCubic } }
            }

            // indicator lamp, on the face (it travels with it)
            Loader {
                active: key.indicator
                x: (key.look?.snap(key.look.ledInset, key.dpr) ?? 0)
                y: (key.look?.snap(face.y + (face.height - key.look.ledSize) / 2, key.dpr) ?? 0)
                sourceComponent: Lamp {
                    lit: key.checked
                    animated: key.settled
                    dpr: key.dpr
                    look: key.look
                }
            }
        }
    }
}
