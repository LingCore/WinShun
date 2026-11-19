import QtQuick
import WinShun

// One entry of a picker panel, drawn as DropMenu's: the style's highlight
// under the entry the pointer is on (or the one that is open), an icon, the
// words, a note, a mark for the one now chosen or a chevron where it opens more.
Item {
    id: row

    property string icon // a Phosphor icon ("ph:globe")
    property string text
    property string note // small, at the right
    property bool chevron: false
    property bool marked: false // the one in use now
    property bool active: false // the one whose more is open
    property bool danger: false
    readonly property bool hovered: area.containsMouse
    readonly property real dpr: Screen.devicePixelRatio

    signal clicked()
    signal entered()

    height: Theme.menuItemHeight

    Hook { // the style's highlight (Theme.rowHighlight), as the drop-down menu's
        id: highlight
        // A part that knows the menu (declares `role`, or `inMenu`) draws the fill; one that does not leaves it to the row's own.
        readonly property bool inMenu: item !== null && ("role" in item || "inMenu" in item)
        anchors.fill: parent
        visible: row.hovered || row.active
        lazy: true
        part: Theme.rowHighlight
        hovered: inMenu
        role: "menu"
        dpr: row.dpr

        Rectangle {
            anchors.fill: parent
            visible: !highlight.inMenu
            radius: Theme.menuItemRadius
            color: Theme.menuHover
        }
    }

    Glyph {
        x: 10
        anchors.verticalCenter: parent.verticalCenter
        visible: row.icon.length > 0 || row.marked
        glyph: row.marked && row.icon.length === 0 ? "ph:check" : row.icon
        size: 14
        color: row.danger ? Theme.danger : row.marked && row.icon.length === 0 ? Theme.accent : Theme.text
    }
    Text {
        id: label
        x: 36
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, parent.width - 36 - (noteText.visible ? noteText.width + 16 : 12) - (marker.visible ? 24 : 0))
        text: row.text
        elide: Text.ElideRight
        color: row.danger ? Theme.danger : Theme.text
        font.pixelSize: Theme.menuCaptionSize
    }
    Text {
        id: noteText
        anchors.right: marker.visible ? marker.left : parent.right
        anchors.rightMargin: marker.visible ? 6 : 12
        anchors.verticalCenter: parent.verticalCenter
        visible: row.note.length > 0
        text: row.note
        color: Theme.subtext
        font.pixelSize: Theme.fontCaption - 2
    }
    Glyph {
        id: marker
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        visible: row.chevron || (row.marked && row.icon.length > 0)
        glyph: row.chevron ? "ph:caret-right" : "ph:check"
        size: 12
        color: row.chevron ? Theme.subtext : Theme.accent
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onEntered: row.entered()
        onClicked: row.clicked()
    }
}
