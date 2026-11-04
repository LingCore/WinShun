pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import WinShun

ListView {
    id: view
    required property ContentPane pane
    property bool textView: false
    readonly property real dpr: Screen.devicePixelRatio
    objectName: textView ? "contentTextLines" : "contentHitLines"
    model: textView ? pane.textLines : pane.hits
    clip: true
    reuseItems: true
    spacing: 2
    leftMargin: 10; rightMargin: 10
    boundsBehavior: Flickable.StopAtBounds
    activeFocusOnTab: true
    Keys.onReturnPressed: pane.openCurrent()
    Keys.onEnterPressed: pane.openCurrent()
    Keys.onDownPressed: pane.nextMatch(1)
    Keys.onUpPressed: pane.nextMatch(-1)
    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
    Connections {
        target: view.pane
        function onPositionRequested(hitRow, textRow) {
            const row = view.textView ? textRow : hitRow
            if (row >= 0) Qt.callLater(() => view.positionViewAtIndex(row, ListView.Contain))
        }
    }
    delegate: Item {
        id: row
        required property int index
        required property string html
        required property string location
        required property string heading
        required property bool hit
        required property bool current
        required property string number
        width: Math.max(0, view.width - 20)
        height: headingBox.height + Math.max(30, editor.contentHeight + 12)
        ListView.onReused: editor.deselect()
        Item {
            id: headingBox
            width: parent.width
            height: visible ? headingText.implicitHeight + 10 : 0
            visible: row.heading.length > 0
            Text {
                id: headingText
                width: parent.width
                text: row.heading
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.subtext
                font.pixelSize: Theme.uiCaption
            }
        }
        Hook { // the style's highlight: the current line chosen, the others that matched lightly
            anchors.top: headingBox.bottom
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            part: Theme.rowHighlight
            selected: row.current
            hovered: row.hit && !row.current
            current: row.current
            role: "list"
            dpr: view.dpr
        }
        Text {
            x: 6; y: headingBox.height + 6
            width: 36
            text: row.number
            color: Theme.subtext
            horizontalAlignment: Text.AlignRight
            font.pixelSize: Theme.uiCaption
        }
        TextEdit {
            id: editor
            x: 50; y: headingBox.height + 6
            width: parent.width - 60
            readOnly: true
            selectByMouse: true
            activeFocusOnTab: false
            textFormat: TextEdit.RichText
            text: row.html.length > 0 ? row.html : " "
            wrapMode: TextEdit.Wrap
            color: row.hit || view.textView ? Theme.text : Theme.subtext
            selectionColor: Theme.textSelection
            selectedTextColor: Theme.text
            font.pixelSize: Theme.uiBody
            Accessible.name: row.location
            TapHandler {
                acceptedButtons: Qt.LeftButton
                onTapped: view.pane.selectLine(row.index, view.textView)
                onDoubleTapped: view.pane.activateLine(row.index, view.textView)
            }
        }
    }
    Text {
        anchors.centerIn: parent
        width: parent.width - 32
        visible: view.count === 0 && !view.pane.busy
        text: view.pane.path.length > 0 ? qsTr("没有可显示的内容，请检查筛选条件或读取提示") : qsTr("选择左侧文件查看详情")
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        color: Theme.subtext
        font.pixelSize: Theme.uiBody
    }
}
