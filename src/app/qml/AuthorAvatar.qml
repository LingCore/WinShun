import QtQuick
import WinShun

// The author LingCore's avatar: a chibi holding a cursor. It pops up and
// tilts side to side like waving hello whenever AuthorClock plays (each time
// the page opens), then keeps swaying gently while the page is open. Under
// the pointer it grows a little and holds still. Pressed, it plays a short
// sound (resources/gleaning/ikun.mp3) and beats along: it swells at each
// loud moment and settles between them. Every press bumps it at once; one
// while the sound plays lets it play on, so a quick series of presses
// neither piles up sounds nor stutters on the first moment of it. Without
// the sound (no Media Foundation), a press replays the hello. With Windows'
// animation effects off it stays still. The picture comes with its ring and
// shadow (resources/gleaning/avatar.png: a 56 px circle in 96 px).
Item {
    id: avatar

    property real size: 64
    readonly property real t: AuthorClock.elapsed
    readonly property bool lively: SystemTheme.animations && visible
    property real beat: 1 // the scale the sound gives it; exactly 1 at rest
    // A tenth larger under the pointer.
    property real lift: area.containsMouse && SystemTheme.animations ? 1.1 : 1
    Behavior on lift {
        NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
    }
    // The sway: 6° each way, there and back in 2.4 s, around the bottom.
    // Its amount fades in over 1.2 s when the page opens or the pointer
    // leaves, and out in a quarter second under the pointer, while the
    // phase runs on, so it never jumps.
    property real swayPhase: 0 // radians
    property real swayAmount: 0 // 0 to 1

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
    readonly property bool posing: t < 0.45 || 9 * Math.exp(-2.6 * (t - 0.35)) > 0.15
    readonly property bool moving: posing || beat !== 1 || lift !== 1 || swayAmount !== 0

    implicitWidth: size
    implicitHeight: size

    SoundClip {
        id: clip
        source: ":/qt/qml/WinShun/gleaning/ikun.mp3"
    }

    // Decoded when the page opens, so that the first press plays at once.
    // Silent as soon as the page is left; closing the window deletes it all.
    onVisibleChanged: {
        if (visible) {
            swayPhase = 0
            swayAmount = 0
            clip.prepare()
        } else {
            clip.stop()
        }
    }

    // The sound: up to a quarter larger at its loudest, quiet moments
    // counting for little. It swells within a few frames and shrinks at a
    // steady pace (from the largest in a sixth of a second), so it never
    // jumps and lands on exactly 1 when the sound ends.
    FrameAnimation {
        running: avatar.lively || clip.playing || avatar.beat !== 1 || avatar.swayAmount !== 0
        onTriggered: {
            const dt = frameTime
            const sway = avatar.lively && !area.containsMouse ? 1 : 0
            avatar.swayAmount = sway > avatar.swayAmount ? Math.min(sway, avatar.swayAmount + dt / 1.2)
                                                         : Math.max(sway, avatar.swayAmount - dt / 0.25)
            avatar.swayPhase = (avatar.swayPhase + dt * 2 * Math.PI / 2.4) % (2 * Math.PI)

            const target = clip.playing && SystemTheme.animations ? 1 + 0.25 * Math.pow(clip.level(), 1.5) : 1
            const b = avatar.beat
            avatar.beat = target > b ? b + (target - b) * Math.min(1, dt * 30)
                                     : Math.max(target, b - dt * 1.5)
        }
    }

    Item {
        anchors.fill: parent
        transform: [
            Scale {
                origin.x: avatar.width / 2
                origin.y: avatar.height / 2
                xScale: avatar.moving ? avatar.popScale(avatar.t) * avatar.beat * avatar.lift : 1
                yScale: xScale
            },
            Rotation { // around the bottom, like a head tilting
                origin.x: avatar.width / 2
                origin.y: avatar.height
                angle: (avatar.posing ? avatar.tilt(avatar.t) : 0)
                       + (avatar.swayAmount !== 0 ? 6 * avatar.swayAmount * Math.sin(avatar.swayPhase) : 0)
            }
        ]

        // Still (animation effects off), it is loaded at exactly the whole
        // device pixels it covers: for a PNG, sourceSize is in image pixels
        // (only image providers, SVG and PDF take it in logical pixels), and
        // it is sampled unsmoothed: smoothly it would blur wherever it sits
        // on a half pixel. Nudged off the exact half pixel, where the nearest
        // sample is a tie (see ResultRow). Lively, it is nearly always turned
        // or scaled: loaded at 1.4 times that, enough for the largest it gets
        // (1.1 × 1.25), and drawn smoothly from mipmaps.
        Image {
            readonly property real dpr: Screen.devicePixelRatio
            readonly property int pixels: Math.round(avatar.size * 96 / 56 * dpr)
            readonly property int loaded: SystemTheme.animations ? Math.round(pixels * 1.4) : pixels

            anchors.centerIn: parent
            anchors.horizontalCenterOffset: 1 / 64
            anchors.verticalCenterOffset: 1 / 64
            width: pixels / dpr
            height: width
            source: "qrc:/qt/qml/WinShun/gleaning/avatar.png"
            sourceSize.width: loaded
            sourceSize.height: loaded
            smooth: avatar.moving
            mipmap: SystemTheme.animations
        }
    }

    // On press, not click: of quick presses, a double click's second one
    // never sends clicked.
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: {
            if (!clip.available) {
                AuthorClock.play()
                return
            }
            if (SystemTheme.animations)
                avatar.beat = Math.max(avatar.beat, 1.1) // felt before the sound is heard
            clip.play()
        }
    }
}
