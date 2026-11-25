pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Docked edge overscroll cue shown when the user scrolls past the top or bottom
// of a settings view (EdgeScrollGate): an Android-inspired edge glow and curved
// stretch emerge flush from the pane edge together with a docked tab that fills
// with progress as the wheel pushes further, and jumps right away if clicked.
Item {
    id: hint

    property int direction: 0 // +1 next (bottom), -1 previous (top), 0 hidden
    property real progress: 0 // 0..1
    property bool ready: false // threshold reached; waiting for deliberate push or minArmMs
    property string targetLabel: ""
    property bool warm: false
    property real dpr: 1

    signal clicked()

    readonly property bool shown: direction !== 0 && targetLabel.length > 0
    // Keep the last shown content while retracting so the docked tab never flashes blank.
    property int shownDirection: 1
    property string shownLabel: ""
    property real shownProgress: 0
    property bool shownReady: false

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
    onReadyChanged: {
        if (shown)
            shownReady = ready
        else if (!shown)
            shownReady = false
    }

    readonly property bool atBottom: shownDirection > 0
    readonly property color accentColor: warm ? WarmPalette.accent : Theme.accent
    readonly property real easedProgress: 1 - Math.pow(1 - Math.max(0, Math.min(1, shownProgress)), 2)
    readonly property real maxTabWidth: Math.max(140, width - 48)
    readonly property real dockRadius: Math.max(8, Theme.menuRadius)
    readonly property real tabHeight: Theme.snap(34 + (shown ? 3 * easedProgress : 0), dpr)

    clip: true
    visible: opacity > 0.001
    opacity: shown ? 1 : 0

    Behavior on opacity {
        enabled: SystemTheme.animations
        NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
    }

    // Layer 1: Soft edge glow anchored flush against the top or bottom border.
    Rectangle {
        id: edgeGlow
        anchors.left: parent.left
        anchors.right: parent.right
        y: hint.atBottom ? parent.height - height : 0
        height: hint.shown ? Theme.snap(16 + 28 * hint.easedProgress, hint.dpr) : 0
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: hint.atBottom
                       ? "transparent"
                       : Qt.alpha(hint.accentColor, hint.shownReady ? 0.20 : 0.07 + 0.10 * hint.easedProgress)
            }
            GradientStop {
                position: 1.0
                color: hint.atBottom
                       ? Qt.alpha(hint.accentColor, hint.shownReady ? 0.20 : 0.07 + 0.10 * hint.easedProgress)
                       : "transparent"
            }
        }

        Behavior on height {
            enabled: SystemTheme.animations
            NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
        }
    }

    // Layer 2: Shallow curved overscroll dome peeking smoothly from the edge.
    Rectangle {
        id: edgeArc
        property real reveal: hint.shown ? Theme.snap(10 + 18 * hint.easedProgress, hint.dpr) : 0
        width: Math.min(parent.width * 0.72, 460)
        height: Math.max(64, width * 0.38)
        radius: height / 2
        anchors.horizontalCenter: parent.horizontalCenter
        y: hint.atBottom ? parent.height - reveal : reveal - height
        color: Qt.alpha(hint.accentColor, hint.shownReady ? 0.14 : 0.05 + 0.07 * hint.easedProgress)

        Behavior on reveal {
            enabled: SystemTheme.animations
            NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
        }
    }

    // Layer 3: Docked edge pull tab — flush against the pane edge, rounded on
    // its inner face, emerging smoothly out of the border as the user scrolls.
    Item {
        id: dockSlot
        anchors.horizontalCenter: parent.horizontalCenter
        y: hint.atBottom ? parent.height - height : 0
        width: Math.min(hint.maxTabWidth, Math.ceil(row.implicitWidth) + 32)
        height: hint.tabHeight
        clip: true

        Accessible.role: Accessible.Button
        Accessible.ignored: !hint.visible
        Accessible.name: label.text
        Accessible.onPressAction: if (hint.shown) hint.clicked()

        transform: Translate {
            y: hint.shown ? 0 : (hint.atBottom ? dockSlot.height + 2 : -(dockSlot.height + 2))
            Behavior on y {
                enabled: SystemTheme.animations
                NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
            }
        }

        // Oversized by `dockRadius` on the outer edge and clipped by `dockSlot`
        // so the outer edge sits completely flush with the pane border.
        Rectangle {
            id: surface
            x: 0
            y: hint.atBottom ? 0 : -hint.dockRadius
            width: parent.width
            height: parent.height + hint.dockRadius
            radius: hint.dockRadius
            clip: true
            color: hint.warm
                   ? (area.containsMouse ? Qt.rgba(1, 0.99, 0.97, 0.98) : Qt.rgba(1, 0.98, 0.95, 0.96))
                   : (area.containsMouse ? Theme.menuHover : Theme.menuBackground)
            border.width: Theme.edge(hint.dpr)
            border.color: hint.shownReady
                          ? Qt.alpha(hint.accentColor, 0.72)
                          : (hint.warm ? WarmPalette.iconStroke : Theme.menuEdge)

            Behavior on border.color {
                enabled: SystemTheme.animations
                ColorAnimation { duration: 120 }
            }

            // Progress fill wash sweeping smoothly across the docked tab.
            Rectangle {
                x: surface.border.width
                y: hint.atBottom ? surface.border.width : hint.dockRadius
                height: dockSlot.height - surface.border.width
                width: Math.max(0, (surface.width - 2 * surface.border.width) * hint.shownProgress)
                color: Qt.alpha(hint.accentColor, hint.shownReady ? 0.16 : 0.09)

                Behavior on width {
                    enabled: SystemTheme.animations
                    NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                }
            }
        }

        // Crisp accent progress bar hugging the outer viewport edge inside the docked tab.
        Rectangle {
            id: progressTrack
            x: surface.border.width
            width: Math.max(0, parent.width - 2 * surface.border.width)
            y: hint.atBottom ? parent.height - height : 0
            height: Theme.snap(2.5, hint.dpr)
            color: hint.warm ? Qt.alpha(WarmPalette.ink, 0.10) : Qt.alpha(Theme.divider, 0.7)

            Rectangle {
                height: parent.height
                width: Math.max(0, Math.min(parent.width, parent.width * hint.shownProgress))
                color: hint.accentColor

                Behavior on width {
                    enabled: SystemTheme.animations
                    NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                }
            }
        }

        Row {
            id: row
            anchors.centerIn: parent
            anchors.verticalCenterOffset: hint.atBottom ? -1 : 1
            spacing: 8

            Glyph {
                anchors.verticalCenter: parent.verticalCenter
                iconName: hint.atBottom ? "arrow-down" : "arrow-up"
                size: 16
                color: hint.accentColor
                transform: Translate {
                    y: hint.shown ? hint.shownDirection * (1.5 * hint.easedProgress) : 0
                    Behavior on y {
                        enabled: SystemTheme.animations
                        NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                    }
                }
            }

            Text {
                id: label
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, Math.max(80, hint.maxTabWidth - 54))
                text: hint.atBottom
                      ? qsTr("继续向下滚动，切到「%1」").arg(hint.shownLabel)
                      : qsTr("继续向上滚动，切到「%1」").arg(hint.shownLabel)
                textFormat: Text.PlainText
                color: hint.warm ? WarmPalette.ink : Theme.text
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
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
}
