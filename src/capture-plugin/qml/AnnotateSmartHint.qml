pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// 智能打码's offer (docs/design-capture-roadmap.md, 4.1): once the scan of the
// selection's text found places to hide, "发现 N 处 · 点此打码" stands by the
// toolbar; one click hides all of them (one undo step). Nothing shows while
// the scan runs or when it found nothing. Fades in; goes at once.
Rectangle {
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
    radius: Theme.controlRadius
    color: area.pressed ? Theme.controlPressed : area.containsMouse ? Theme.controlHover : Theme.background
    border.width: 1
    border.color: Theme.windowEdge
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

    Text {
        id: label
        anchors.centerIn: parent
        text: qsTr("发现 %1 处 · 点此打码").arg(hint.found)
        color: Theme.text
        font.pixelSize: Theme.fontCaption
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: hint.annot.smartRedact()
    }
}
