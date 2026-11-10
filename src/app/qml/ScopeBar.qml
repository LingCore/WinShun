pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Typed in a folder's window with that folder alone searched (GitHub #110):
// where the search looked, and the ways to widen or narrow it, held in one
// line under the rows. As two hints after the last row, a long list took
// them out of sight until scrolled to its end (GitHub #246). The words of
// Launcher.leftOutHints' "everywhere" hint, then the "level" one's link
// (含子文件夹 / 只搜这一层) and the "everywhere" one's (也搜其他位置);
// Ctrl+Shift+D goes round the same.
Item {
    id: bar

    required property var launcher
    required property var hints // Launcher.leftOutHints' "everywhere" and "level" ones
    property bool compact: false // as the rows of a folder's window
    property real dpr: Screen.devicePixelRatio // the window's, passed by the launcher

    readonly property var everywhere: hints.find(h => h.kind === "everywhere") ?? null
    readonly property var links: ["level", "everywhere"].map(kind => hints.find(h => h.kind === kind)).filter(h => !!h)
    readonly property int textSize: compact ? 12 : Theme.uiBody

    implicitHeight: Theme.hintRowHeight

    Rectangle { // where it meets the rows, as the divider under the search box
        width: parent.width
        height: Theme.edge(bar.dpr)
        color: Theme.layerEdge
    }

    Text {
        x: bar.compact ? 16 : Theme.textStart // under the rows' text
        width: Math.max(0, linkRow.x - 16 - x)
        anchors.verticalCenter: parent.verticalCenter
        text: bar.everywhere ? bar.everywhere.text : ""
        textFormat: Text.PlainText // holds a folder's name
        elide: Text.ElideMiddle // a long folder name keeps its start and its end
        color: Theme.subtext
        font.pixelSize: bar.textSize
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    Row {
        id: linkRow
        anchors.right: parent.right
        anchors.rightMargin: 18
        anchors.verticalCenter: parent.verticalCenter
        spacing: 16

        Repeater {
            model: bar.links

            delegate: Text { // accent words on the plain background, as the hints' links
                id: link

                required property var modelData

                objectName: "scopeLink-" + modelData.kind // the --folder-typing probe clicks it
                text: modelData.action
                color: Theme.accent
                font.pixelSize: bar.textSize
                font.underline: linkArea.containsMouse
                Accessible.role: Accessible.Link
                Accessible.name: text
                Accessible.onPressAction: bar.launcher.searchLeftOut(link.modelData.kind)

                MouseArea {
                    id: linkArea
                    // The bar's height, and half the gap either side: easy to hit.
                    x: -linkRow.spacing / 2
                    y: (link.height - bar.height) / 2
                    width: link.width + linkRow.spacing
                    height: bar.height
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bar.launcher.searchLeftOut(link.modelData.kind)
                }
            }
        }
    }
}
