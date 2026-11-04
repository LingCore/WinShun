import QtQuick
import QtQuick.Effects
import WinShun

// The scene behind the whole settings window (sidebar included) while the
// 拾穗计划 page is open: sky, wheat field, motes of light and birds.
Item {
    id: backdrop

    property bool running: true // false while the window cannot be seen
    property real leadingInset: 0 // the sidebar's width
    // The window's corners as drawn (WindowEdge.cornerRadius): under a style's
    // own frame the scene is cut to them (the one place that fills the whole
    // window with something that is not a plain rectangle).
    property real cornerRadius: 0

    layer.enabled: cornerRadius > 0 && visible
    layer.effect: MultiEffect {
        maskEnabled: true
        maskSource: corners
    }

    Item { // the shape the scene is cut to
        id: corners
        anchors.fill: parent
        visible: false
        layer.enabled: backdrop.cornerRadius > 0 && backdrop.visible

        Rectangle {
            anchors.fill: parent
            radius: backdrop.cornerRadius
        }
    }

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

    Rectangle {
        anchors.fill: parent
        color: WarmPalette.dim
        visible: color.a > 0
    }
}
