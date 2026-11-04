import QtQuick
import QtQuick.Shapes

// A physical toggle: a round groove with one round key as the knob, 2 px
// from the groove all round; on, the knob slides right and the lamp on its
// face lights orange; off, the knob is plain (no dark dot to misread);
// mixed, the knob rests in the middle, unlit. Not read yet (`active` false):
// the groove alone.
// As a check box (kind "check"): a small key that stays pressed down while
// checked, with an ink check mark on the sunk face (a dash when mixed).
// Sized by the host (toggleWidth x toggleHeight, or checkSize square).
Item {
    id: toggle

    property bool checked: false
    property bool hovered: false
    property bool pressed: false
    property bool mixed: false
    property bool active: true
    property string kind           // "check": a check box
    property real dpr: 1

    property QtObject look // injected by Look.qml
    readonly property bool isCheck: kind === "check"
    readonly property bool lit: checked && !mixed && active
    readonly property real pad: look ? look.switchPadding : 2
    readonly property real knobSize: height - 2 * pad
    readonly property real disabledOpacity: 1

    implicitWidth: !look ? 0 : isCheck ? look.checkSize : look.toggleWidth
    implicitHeight: !look ? 0 : isCheck ? look.checkSize : look.toggleHeight

    // The host's switch was taken for another (a recycled row) or shown
    // again: no slide is left half way.
    function snap(v: real): real { return look ? look.snap(v, dpr) : v }
    function pill(size: real): real { return look ? look.pill(size) : 0 }

    function settle() {
        if (slide.running)
            slide.complete()
    }

    // ---- switch ----
    Groove {
        visible: !toggle.isCheck
        anchors.fill: parent
        radius: toggle.pill(height)
        dpr: toggle.dpr
        look: toggle.look
    }
    KeySurface {
        id: knob
        visible: !toggle.isCheck && toggle.active
        x: toggle.mixed ? toggle.snap((toggle.width - width) / 2)
           : toggle.checked ? toggle.width - toggle.pad - width : toggle.pad
        y: toggle.pad
        width: toggle.knobSize
        height: toggle.knobSize
        corner: toggle.pill(width)
        hovered: toggle.hovered
        pressed: toggle.pressed
        enabled: toggle.enabled
        dpr: toggle.dpr
        look: toggle.look
        Behavior on x { NumberAnimation { id: slide; duration: toggle.look ? (toggle.look?.animSlide ?? 0) : 0; easing.type: Easing.OutCubic } }

        Lamp {
            visible: knob.enabled && toggle.lit // off: a plain knob; on: the lamp lights on it
            x: toggle.snap((knob.width - width) / 2)
            y: toggle.snap((knob.height - height) / 2 + knob.contentOffsetY)
            lit: toggle.lit
            dpr: toggle.dpr
            look: toggle.look
        }
    }

    // ---- check box ----
    KeySurface {
        id: box
        visible: toggle.isCheck
        anchors.fill: parent
        corner: toggle.look ? (toggle.look?.smallControlRadius ?? 0) : 0
        hovered: toggle.hovered
        pressed: toggle.pressed
        checked: toggle.active && (toggle.checked || toggle.mixed)
        enabled: toggle.enabled
        dpr: toggle.dpr
        look: toggle.look

        Shape {
            visible: box.checked && toggle.look !== null
            anchors.centerIn: parent
            anchors.verticalCenterOffset: box.contentOffsetY
            width: 12
            height: 12
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: !toggle.look ? "transparent" : box.enabled ? (toggle.look?.ink ?? Qt.rgba(0, 0, 0, 0)) : (toggle.look?.ink3 ?? Qt.rgba(0, 0, 0, 0))
                strokeWidth: 1.75
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: toggle.mixed ? "M2.5 6L9.5 6" : "M1.8 6.4L4.8 9.3L10.2 3.2" }
            }
        }
    }
}
