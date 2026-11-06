pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import WinShun

// The recording panel's button: a flat key (Theme.keySurface) with a glyph
// and/or words; `highlighted` is the primary one, `checked` (checkable) a
// toggle that is on. One of a few choices (the quality, the format …) lies in a
// ChoiceGroup instead (an inline component of RecordingPanel and RecordingOptions): the group draws the style's segmented part behind them
// and the button only draws its words, in the colour the group gives.
Button {
    id: button
    property string glyph
    property string tip
    property bool tipsEnabled: false
    property bool selectionFill: true
    // The ChoiceGroup this button is one choice of (its Row's parent), or none.
    readonly property var choiceGroup: parent && parent.parent && parent.parent.isChoiceGroup === true ? parent.parent : null
    readonly property bool segment: choiceGroup !== null
    readonly property int segmentIndex: segment ? choiceGroup.indexOf(button) : -1
    // The words' and the glyph's colour, and how far they sit from the middle.
    readonly property color foreground: segment
        ? choiceGroup.hook.call("labelColor", [segmentIndex], !enabled ? Theme.faint : checked || hovered ? Theme.text : Theme.subtext)
        : face.out("foreground", !enabled ? Theme.faint : highlighted ? Theme.accentText : Theme.text)
    readonly property real markOffsetY: segment ? choiceGroup.hook.call("labelOffsetY", [segmentIndex], 0) : face.out("contentOffsetY", 0)

    // Room for the words in bold, when the group's style bolds the chosen one:
    // the buttons keep their places when the choice moves.
    TextMetrics { id: regular; text: button.text; font.pixelSize: button.font.pixelSize }
    TextMetrics { id: bold; text: button.text; font.pixelSize: button.font.pixelSize; font.weight: Theme.fontStrong }
    implicitWidth: Math.max(32, row.implicitWidth + (segment ? Math.ceil(Math.max(0, bold.advanceWidth - regular.advanceWidth)) : 0)
                               + (text.length > 0 ? 20 : 12))
    implicitHeight: 36
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    font.pixelSize: Theme.fontCaption
    Accessible.name: tip.length > 0 ? tip : text

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        Row {
            id: row
            spacing: 6
            anchors.centerIn: parent
            anchors.verticalCenterOffset: button.markOffsetY
            Glyph {
                visible: button.glyph.length > 0
                anchors.verticalCenter: parent.verticalCenter
                glyph: button.glyph
                size: 16
                color: button.foreground
            }
            Text {
                visible: button.text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: button.text
                font.pixelSize: button.font.pixelSize
                font.weight: button.segment ? button.choiceGroup.hook.call("labelWeight", [button.segmentIndex], Font.Normal)
                                            : face.out("fontWeight", Font.Normal)
                color: button.foreground
            }
        }
    }
    background: Item {
        Hook { // the style's key (Theme.keySurface); a choice in a group has the group's
            id: face
            anchors.fill: parent
            visible: !button.segment
            part: Theme.keySurface
            hovered: button.hovered
            pressed: button.down
            checked: button.checked && button.selectionFill
            highlighted: button.highlighted
            flat: !button.highlighted // the primary one is a filled key at rest
            role: "toolbar"
            dpr: Screen.devicePixelRatio
        }
        Rectangle { // keyboard focus
            anchors.fill: parent
            visible: button.visualFocus
            radius: Theme.controlRadius
            color: "transparent"
            border.width: 2
            border.color: Theme.text
        }
    }
    ToolTip {
        id: tooltip
        parent: button
        popupType: Popup.Window
        visible: button.tipsEnabled && button.Window.window && button.Window.window.visible
                 && button.tip.length > 0 && (button.hovered || button.visualFocus)
        delay: 450
        timeout: 5000
        width: Math.min(480, implicitWidth, Screen.desktopAvailableWidth - 16)
        text: button.tip
        font.pixelSize: Theme.fontCaption
        padding: 8
        contentItem: Text { text: button.tip; font: tooltip.font; color: Theme.text; wrapMode: Text.Wrap }
        background: Item {
            Hook { // the style's floating panel (Theme.menuFrame)
                anchors.fill: parent
                part: Theme.menuFrame
                dpr: Screen.devicePixelRatio
            }
        }
        enter: null
        exit: null
    }
}
