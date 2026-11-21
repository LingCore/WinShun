pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Floating edge cue shown when the user scrolls past the top or bottom of a
// settings view (EdgeScrollGate): previews the adjacent view, fills a thin
// progress accent bar as the wheel moves further, and jumps right away if clicked.
Item {
    id: hint

    property int direction: 0 // +1 next (bottom), -1 previous (top), 0 hidden
    property real progress: 0 // 0..1
    property string targetLabel: ""
    property bool warm: false
    property real dpr: 1

    signal clicked()

    readonly property bool shown: direction !== 0 && targetLabel.length > 0
    // Keep the last shown content while fading out so the pill never flashes blank.
    property int shownDirection: 1
    property string shownLabel: ""
    property real shownProgress: 0

    onDirectionChanged: {
        if (direction !== 0)
            shownDirection = direction
    }
    onTargetLabelChanged: {
        if (targetLabel.length > 0)
            shownLabel = targetLabel
    }
    onProgressChanged: {
        if (shown)
            shownProgress = progress
    }

    readonly property real maxWidth: parent ? Math.max(120, parent.width - 32) : 360
    anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
    y: parent ? (shownDirection > 0 ? parent.height - height - 16 : 12) : 0
    width: Math.min(maxWidth, Math.ceil(row.implicitWidth) + 28)
    height: 36
    visible: opacity > 0.001
    opacity: shown ? 1 : 0

    Accessible.role: Accessible.Button
    Accessible.ignored: !visible
    Accessible.name: label.text
    Accessible.onPressAction: if (hint.shown) hint.clicked()

    Behavior on opacity {
        enabled: SystemTheme.animations
        NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
    }

    transform: Translate {
        y: hint.shown ? 0 : (hint.shownDirection > 0 ? 8 : -8)
        Behavior on y {
            enabled: SystemTheme.animations
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }
    }

    Hook { // the style's floating card (Theme.menuFrame)
        anchors.fill: parent
        visible: !hint.warm
        lazy: true
        part: hint.warm ? null : Theme.menuFrame
        dpr: hint.dpr
    }

    Rectangle { // warm-scene floating pill (拾穗计划)
        anchors.fill: parent
        visible: hint.warm
        radius: Theme.menuRadius
        color: area.containsMouse ? Qt.rgba(1, 0.99, 0.97, 0.98) : Qt.rgba(1, 0.98, 0.95, 0.95)
        border.width: Theme.edge(hint.dpr)
        border.color: WarmPalette.iconStroke
    }

    Row {
        id: row
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -1
        spacing: 8

        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            iconName: hint.shownDirection > 0 ? "arrow-down" : "arrow-up"
            size: 14
            color: hint.warm ? WarmPalette.accent : Theme.accent
        }

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(80, hint.maxWidth - 50))
            text: hint.shownDirection > 0
                  ? qsTr("继续向下滚动，切到「%1」").arg(hint.shownLabel)
                  : qsTr("继续向上滚动，切到「%1」").arg(hint.shownLabel)
            textFormat: Text.PlainText
            color: hint.warm ? WarmPalette.ink : Theme.text
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }
    }

    // Subtle progress bar along the pill's bottom edge showing how close the
    // boundary push is to switching.
    Rectangle {
        id: progressTrack
        x: 12
        width: parent.width - 24
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.snap(4, hint.dpr)
        height: Theme.snap(2, hint.dpr)
        radius: Theme.progressRadius
        color: hint.warm ? Qt.alpha(WarmPalette.ink, 0.12) : Theme.divider

        Rectangle {
            height: parent.height
            width: Math.max(0, Math.min(parent.width, parent.width * hint.shownProgress))
            radius: parent.radius
            color: hint.warm ? WarmPalette.accent : Theme.accent

            Behavior on width {
                enabled: SystemTheme.animations
                NumberAnimation { duration: 100; easing.type: Easing.OutCubic }
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        enabled: hint.shown
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: hint.clicked()
    }
}
