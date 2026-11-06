pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// 智能打码's offer (docs/design-capture-roadmap.md, 4.1): once the scan of the
// selection's text found places to hide, "发现 N 处 · 点此打码" stands by the
// toolbar; one click hides all of them (one undo step). Nothing shows while
// the scan runs or when it found nothing. Fades in; goes at once.
Item {
    id: hint

    required property AnnotationModel annot
    property real dpr: 1

    readonly property int found: annot ? annot.smartFound : -1
    readonly property int fadeMs: 120
    readonly property int padX: 12

    function snap(v) { return Math.round(v * dpr) / dpr }

    visible: found > 0
    width: snap(label.implicitWidth + 2 * padX)
    height: snap(32)
    Accessible.role: Accessible.Button
    Accessible.name: label.text

    onVisibleChanged: {
        fade.stop()
        if (visible)
            fade.restart()
        else
            opacity = 1
    }

    NumberAnimation {
        id: fade
        target: hint
        property: "opacity"
        from: 0
        to: 1
        duration: hint.fadeMs
        easing.type: Easing.OutCubic
    }

    Hook { // the style's key (Theme.keySurface): a floating button by the toolbar
        id: face
        anchors.fill: parent
        part: Theme.keySurface
        hovered: area.containsMouse
        pressed: area.pressed
        role: "toolbar"
        dpr: hint.dpr
    }

    Text {
        id: label
        anchors.centerIn: parent
        anchors.verticalCenterOffset: face.out("contentOffsetY", 0)
        text: qsTr("发现 %1 处 · 点此打码").arg(hint.found)
        color: face.out("foreground", Theme.text)
        font.pixelSize: Theme.fontCaption
        font.weight: face.out("fontWeight", Font.Normal)
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: hint.annot.smartRedact()
    }
}
