import QtQuick
import WinShun

// Left shortcut gutter slot shared by ResultRow, ClipRow and DialogBarRow:
//   - Number shortcut (`1`–`9`, plain text, struck through if taken by a hotkey)
//   - Passive letter preview (`A`–`Z` when number keys are off and letter jump is
//     enabled: quiet plain letter, clearly still in typing mode)
//   - Armed letter jump (`armed: true` after tapping the jump key: solid accent
//     keycap so typing mode vs jump mode is unmistakable at a glance)
Item {
    id: slot

    property int hint: 0
    property string hintLetter
    property bool armed: false
    property bool taken: false
    property bool current: false
    property bool compact: false
    property UiScale metrics: Theme.metrics
    property real dpr: 1
    property alias glyphObjectName: glyph.objectName

    readonly property bool letterShown: hintLetter.length > 0
    readonly property bool armedLetter: armed && letterShown
    // Even sizes land on whole device pixels at 150 % scaling (Theme.qml).
    readonly property int capSize: compact || metrics.dense ? 18 : Math.min(22, metrics.keycapHeight)

    function onPixel(v) { return Math.round(v * dpr) / dpr }

    width: 24

    Rectangle {
        id: cap
        objectName: "hintKeyCap"
        visible: slot.armedLetter
        x: slot.onPixel((slot.width - width) / 2)
        y: slot.onPixel((slot.height - height) / 2)
        width: slot.capSize
        height: slot.capSize
        radius: Theme.smallControlRadius
        color: Theme.accentFill
    }

    CenteredNumber {
        id: glyph
        text: slot.letterShown ? slot.hintLetter : slot.hint > 0 ? String(slot.hint) : ""
        font.pixelSize: slot.armedLetter ? (slot.capSize >= 22 ? 14 : 12)
                      : slot.letterShown ? (slot.compact ? 12 : slot.metrics.uiBody)
                      : slot.compact ? 14 : slot.metrics.uiTitle
        font.weight: slot.armedLetter ? Theme.fontStrong : Font.Medium
        font.strikeout: slot.taken && !slot.letterShown
        color: slot.armedLetter ? Theme.accentText
             : slot.letterShown ? (slot.current ? Theme.subtext : Theme.faint)
             : slot.taken ? Theme.faint
             : slot.current ? Theme.accent
             : Theme.subtext
        Accessible.ignored: true
    }
}
