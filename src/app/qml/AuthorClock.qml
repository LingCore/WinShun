pragma Singleton
import QtQuick
import WinShun

// When the author's mark and avatar last started their animation, shared by
// all of them (MacShun: AuthorMarkClock). Opening the 拾穗计划 page plays it.
QtObject {
    id: clock

    readonly property real duration: 3.2 // everything has settled by then
    // Seconds since play(); a large value outside an animation, which both
    // the mark and the avatar draw as their resting state.
    property real elapsed: 10
    readonly property bool running: sweep.running

    function play() {
        if (SystemTheme.animations) // Windows' "Animation effects" setting
            sweep.restart()
    }

    property NumberAnimation sweep: NumberAnimation {
        target: clock
        property: "elapsed"
        from: 0
        to: clock.duration
        duration: clock.duration * 1000
        onFinished: clock.elapsed = 10 // stop redrawing every frame
    }
}
