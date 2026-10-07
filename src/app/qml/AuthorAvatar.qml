import QtQuick
import QuickFind

// The author LingCore's avatar: a chibi holding a cursor. It pops up and
// tilts side to side like waving hello whenever AuthorClock plays (each time
// the page opens); a click plays it again. The picture comes with its ring
// and shadow (resources/gleaning/avatar.png: a 56 px circle in 96 px).
Item {
    id: avatar

    property real size: 64
    readonly property real t: AuthorClock.elapsed

    // 0.6 to full size in 0.45 s, overshooting a little.
    function popScale(t) {
        const p = Math.min(Math.max(t / 0.45, 0), 1), s = 1.9
        return 0.6 + 0.4 * (1 + (s + 1) * Math.pow(p - 1, 3) + s * Math.pow(p - 1, 2))
    }

    // Then two tilts, each lighter than the last.
    function tilt(t) {
        const u = t - 0.35
        return u > 0 ? 9 * Math.sin(2 * Math.PI * 1.5 * u) * Math.exp(-2.6 * u) : 0
    }

    // Until the pop is over and the tilts have died down below 0.15° (under
    // a fifth of a pixel at the top): after that the picture snaps to exactly
    // upright and full size, drawn pixel for pixel, rather than staying soft
    // through the clock's last, motionless second.
    readonly property bool moving: t < 0.45 || 9 * Math.exp(-2.6 * (t - 0.35)) > 0.15

    implicitWidth: size
    implicitHeight: size

    Item {
        anchors.fill: parent
        transform: [
            Scale {
                origin.x: avatar.width / 2
                origin.y: avatar.height / 2
                xScale: avatar.moving ? avatar.popScale(avatar.t) : 1
                yScale: xScale
            },
            Rotation { // around the bottom, like a head tilting
                origin.x: avatar.width / 2
                origin.y: avatar.height
                angle: avatar.moving ? avatar.tilt(avatar.t) : 0
            }
        ]

        // Loaded at exactly the whole device pixels it covers: for a PNG,
        // sourceSize is in image pixels (only image providers, SVG and PDF
        // take it in logical pixels). At rest it is sampled unsmoothed:
        // smoothly it would blur wherever it sits on a half pixel. Nudged off
        // the exact half pixel, where the nearest sample is a tie (see
        // ResultRow). Smooth while it visibly scales and tilts.
        Image {
            readonly property real dpr: Screen.devicePixelRatio
            readonly property int pixels: Math.round(avatar.size * 96 / 56 * dpr)

            anchors.centerIn: parent
            anchors.horizontalCenterOffset: 1 / 64
            anchors.verticalCenterOffset: 1 / 64
            width: pixels / dpr
            height: width
            source: "qrc:/qt/qml/QuickFind/gleaning/avatar.png"
            sourceSize.width: pixels
            sourceSize.height: pixels
            smooth: avatar.moving
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: AuthorClock.play()
    }
}
