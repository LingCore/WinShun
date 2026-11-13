pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A small "i" beside a name: what need not be read every time (who made it,
// what it may do, how it stands) is said in a tip while the pointer rests on
// it. As SettingRow's own tip badge, for the cards.
Item {
    id: dot

    property string text
    property real dpr: 0
    property bool caution: false // something in the tip is worth a look

    implicitWidth: 20
    implicitHeight: 20
    visible: text.length > 0
    Accessible.role: Accessible.StaticText
    Accessible.name: text

    Glyph {
        anchors.centerIn: parent
        glyph: String.fromCharCode(0xE946) // Info
        size: 13
        color: area.containsMouse ? Theme.accent : dot.caution ? Theme.caution : Theme.faint
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
    }
    HoverTip {
        target: area.containsMouse ? dot : null
        text: dot.text
        delay: 150
        maxWidth: 360
        dpr: dot.dpr
    }
}
