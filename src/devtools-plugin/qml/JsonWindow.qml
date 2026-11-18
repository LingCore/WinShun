pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

// The JSON tool: the text on the left (paste, type, drop a file), the tree
// it reads as on the right, what it is or what is wrong at the foot.
Window {
    id: window

    required property JsonTool tool
    required property WindowFrame frame
    property real split: 0.4 // the text's share of the width (kept by the plugin)
    property bool textShown: true
    readonly property real dpr: Screen.devicePixelRatio

    // For the plugin (QMetaObject::invokeMethod): the keys to the tree.
    function focusTree() { treePane.focusList() }
    function find(words) { toolbar.find(words) }
    function selectRow(row) { treePane.select(row) }

    objectName: "jsonWindow"
    width: 1100
    height: 720
    minimumWidth: 640
    minimumHeight: 420
    color: Theme.page
    flags: Qt.Window | Qt.FramelessWindowHint
    title: window.tool.source.length > 0 ? window.tool.source + " - " + qsTr("JSON 工具") : qsTr("JSON 工具")

    Connections {
        target: window.tool
        function onCopied(what) {
            footer.flash(what === "pretty" ? qsTr("已复制格式化的 JSON")
                       : what === "compact" ? qsTr("已复制压缩的 JSON")
                       : what === "escaped" ? qsTr("已复制转义后的 JSON")
                       : what === "key" ? qsTr("已复制键名")
                       : what === "path" ? qsTr("已复制路径")
                       : qsTr("已复制值"), false)
        }
        function onFailed(why) { footer.flash(why, true) }
    }

    Shortcut {
        sequences: [StandardKey.Find]
        onActivated: toolbar.focusSearch()
    }
    Shortcut {
        sequence: "F3"
        onActivated: toolbar.step(1)
    }
    Shortcut {
        sequence: "Shift+F3"
        onActivated: toolbar.step(-1)
    }

    TitleBar {
        id: titleBar
        width: parent.width
        frame: window.frame
        title: window.title + " - Win顺"
    }
    JsonToolbar {
        id: toolbar
        objectName: "jsonToolbar"
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        tool: window.tool
        dpr: window.dpr
        textShown: window.textShown
        onTextToggled: window.textShown = !window.textShown
        onFound: (row) => treePane.select(row)
        onSearchLeft: treePane.focusList()
    }
    Hook {
        id: toolbarSeam
        anchors.top: toolbar.bottom
        width: parent.width
        height: toolbarSeam.out("thickness", Theme.edge(window.dpr))
        part: Theme.separator
        dpr: window.dpr
    }

    Item {
        id: body
        anchors.top: toolbarSeam.bottom
        anchors.bottom: footer.top
        anchors.left: parent.left
        anchors.right: parent.right

        JsonTextPane {
            id: textPane
            objectName: "jsonText"
            visible: window.textShown
            width: window.textShown ? Math.round(Math.max(220, Math.min(body.width - 280, body.width * window.split))) : 0
            height: parent.height
            tool: window.tool
            dpr: window.dpr
        }
        Item { // the line between the two, dragged to share the width
            id: splitter
            visible: window.textShown
            x: textPane.width
            width: visible ? seam.width : 0
            height: parent.height
            Hook {
                id: seam
                width: seam.out("thickness", Theme.edge(window.dpr))
                height: parent.height
                part: Theme.separator
                orientation: Qt.Vertical
                dpr: window.dpr
            }
            MouseArea {
                anchors.fill: parent
                anchors.leftMargin: -4
                anchors.rightMargin: -4
                cursorShape: Qt.SplitHCursor
                onPositionChanged: (mouse) => {
                    if (pressed)
                        window.split = Math.max(0.15, Math.min(0.85, mapToItem(body, mouse.x, 0).x / body.width))
                }
            }
        }
        JsonTreePane {
            id: treePane
            objectName: "jsonTree"
            x: splitter.x + splitter.width
            width: body.width - x
            height: parent.height
            focus: true
            tool: window.tool
            dpr: window.dpr
            onLocateError: {
                window.textShown = true
                textPane.showError()
            }
            onCloseRequested: window.close()
        }
    }

    Rectangle {
        id: footer
        objectName: "jsonFooter"
        property string message
        property bool bad: false
        function flash(text, bad) {
            message = text
            footer.bad = bad
            flashTimer.restart()
        }
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 34
        color: Theme.background
        Timer { id: flashTimer; interval: 2500; onTriggered: footer.message = "" }
        Hook {
            id: footerSeam
            width: parent.width
            height: footerSeam.out("thickness", Theme.edge(window.dpr))
            part: Theme.separator
            dpr: window.dpr
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 8
            Glyph {
                readonly property bool flashing: footer.message.length > 0
                visible: flashing || window.tool.error.length > 0
                glyph: flashing ? (footer.bad ? "ph:warning-circle" : "ph:check-circle") : "ph:warning-circle"
                size: 14
                color: flashing && !footer.bad ? Theme.success : Theme.danger
            }
            Text {
                id: status
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.pixelSize: Theme.uiCaption
                color: footer.message.length > 0 ? Theme.text : window.tool.error.length > 0 ? Theme.danger : Theme.subtext
                text: footer.message.length > 0 ? footer.message
                    : window.tool.empty ? qsTr("Ctrl+V 粘贴 · Ctrl+F 搜索 · ← → 折叠和展开 · Ctrl+C 复制值 · Ctrl+Shift+C 复制路径")
                    : window.tool.error.length > 0
                      ? (window.tool.errorLine > 0 ? qsTr("第 %1 行第 %2 列：%3").arg(window.tool.errorLine).arg(window.tool.errorColumn).arg(window.tool.error)
                                                   : window.tool.error)
                    : [window.tool.summary, window.tool.inputSize, window.tool.notes].filter(s => s.length > 0).join(" · ")
                HoverHandler { id: statusHover }
                HoverTip { target: statusHover.hovered && status.truncated ? status : null; text: status.text; dpr: window.dpr; maxWidth: 560 }
            }
        }
    }

    DropArea {
        anchors.fill: parent
        onEntered: (drag) => drag.accepted = drag.hasUrls
        onDropped: (drop) => {
            if (drop.hasUrls && window.tool.openFile(drop.urls[0])) {
                drop.acceptProposedAction()
                treePane.focusList()
            }
        }
    }
    WindowEdge { anchors.fill: parent; fill: Theme.page }
}
