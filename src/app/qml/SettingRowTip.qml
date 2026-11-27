import QtQuick
import WinShun

// The "i" after a SettingRow's title, whose tip says more (SettingRow.tip).
// Made by the row, in its title's Row, for the few rows that have one.
Item {
    id: badge

    required property SettingRow row
    required property Text title

    visible: row.tip.length > 0 && !row.searching
    width: 20
    height: 20
    anchors.verticalCenter: title.lineCount <= 1 ? title.verticalCenter : undefined
    y: title.lineCount > 1 ? 2 : 0

    Glyph {
        anchors.centerIn: parent
        glyph: ""
        size: 14
        color: tipArea.containsMouse ? Theme.accent : Theme.subtext
    }

    MouseArea {
        id: tipArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
    }

    HoverTip {
        essential: true // the badge says nothing else
        target: tipArea.containsMouse ? badge : null
        text: badge.row.tip
        delay: 150
        maxWidth: 420
    }
}
