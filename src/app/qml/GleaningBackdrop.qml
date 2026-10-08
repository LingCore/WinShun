import QtQuick
import WinShun

// The scene behind the whole settings window (sidebar included) while the
// 拾穗计划 page is open: sky, wheat field, motes of light and birds.
Item {
    id: backdrop

    property bool running: true // false while the window cannot be seen
    property real leadingInset: 0 // the sidebar's width

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: WarmPalette.skyTop }
            GradientStop { position: 0.5; color: WarmPalette.skyMiddle }
            GradientStop { position: 1; color: WarmPalette.skyBottom }
        }
    }

    GleaningScene {
        anchors.fill: parent
        running: backdrop.running && backdrop.visible
        leadingInset: backdrop.leadingInset
        wheatBack: WarmPalette.wheatBack
        wheatFront: WarmPalette.wheatFront
        mote: WarmPalette.mote
        bird: WarmPalette.bird
    }
}
