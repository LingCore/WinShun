import QtQuick
import WinShun

// Typed in a folder's window, its own rows first (GitHub #110): the heading
// over the rows found elsewhere, as File Explorer heads a group (its name,
// then a line to the edge), with the way to the folder alone at the end of
// the line. The box's list makes it the heading of its sections (the rows'
// "elsewhere"); the other one, the folder's own rows, has none (no height).
// With no row of the folder's own above it, it says so, without the link:
// there would be nothing left.
//
// Shown or not by its parts, never by its own `visible`: the list sets that
// itself (true) when it takes a heading from its cache for another section,
// and a binding there would not be asked again. The folder's own section
// then drew a cut "其他位置" over the first row (seen 2026-10-27, #110 test).
//
// The line runs between the words as drawn, not their boxes: a long folder
// name is cut short of its box (by up to a glyph), so the link keeps to the
// right edge and the line goes up to its first glyph.
Item {
    id: caption

    required property string section
    required property string folder // Launcher.typingFolder: the folder's name
    required property bool alone

    signal onlyRequested()

    readonly property bool shown: section === "true"
    readonly property real dpr: Screen.devicePixelRatio

    // Main.qml's captionHeight (the same Theme.labelRowHeight): the window's
    // height and the rows under its number keys are worked out with it.
    height: shown ? Theme.labelRowHeight : 0

    Text {
        id: label
        visible: caption.shown
        x: 20 // as the box's other headings (常用应用, 最近)
        width: Math.min(implicitWidth, caption.width - x - (link.visible ? link.width + 18 + 32 : 40))
        height: caption.height
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideMiddle // a long folder name keeps its start and its end
        text: caption.alone ? qsTr("%1 里没有，以下在其他位置").arg(caption.folder) : qsTr("其他位置")
        textFormat: Text.PlainText
        color: Theme.subtext
        font.pixelSize: Theme.uiCaption
        // Read out as the heading it is; named here, as setting `ignored`
        // left it with no role or name (UIA found no "其他位置", 2026-10-28).
        // Not on show (the folder's own section's, or a heading the list
        // keeps for later): not there for a screen reader either.
        Accessible.role: Accessible.StaticText
        Accessible.name: text
        Accessible.ignored: !visible
    }

    Rectangle { // a line, as the divider under the search box: whole device pixels
        visible: caption.shown
        x: Theme.snap(label.x + Math.min(label.width, label.contentWidth) + 8, caption.dpr)
        width: Math.max(0, Theme.snap(link.visible ? link.x + link.width - Math.min(link.width, link.contentWidth) - 12
                                                   : caption.width - 20, caption.dpr) - x)
        height: Theme.edge(caption.dpr)
        y: Theme.snap((caption.height - height) / 2, caption.dpr)
        color: Theme.divider
    }

    Text { // accent words on the plain background, as a hint row's link
        id: link
        visible: caption.shown && !caption.alone && caption.folder.length > 0
        anchors.right: parent.right
        anchors.rightMargin: 18
        width: Math.min(implicitWidth, caption.width * 0.4)
        height: caption.height
        horizontalAlignment: Text.AlignRight // cut short: still 18 from the edge, as the hint rows' links
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideMiddle
        text: qsTr("只搜 %1").arg(caption.folder)
        textFormat: Text.PlainText
        color: Theme.accent
        font.pixelSize: Theme.uiCaption
        Accessible.role: Accessible.Link
        Accessible.name: text
        Accessible.ignored: !visible
        Accessible.onPressAction: caption.onlyRequested()

        MouseArea {
            anchors.fill: parent
            anchors.leftMargin: -6 // a little more than the words to hit
            anchors.rightMargin: -6
            cursorShape: Qt.PointingHandCursor
            onClicked: caption.onlyRequested()
        }
    }
}
