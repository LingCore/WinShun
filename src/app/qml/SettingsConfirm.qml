pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// change_settings (MCP, App::confirmSettingsChange): an AI client wants to
// change Win顺's settings. Shown in the middle of the monitor the mouse is
// on, above the rest. Only a click on 允许
// changes anything; 拒绝, Esc or no answer in time refuse. No key says yes:
// 允许 is out of the Tab order and Space and Enter do nothing on it, and
// for the first half second nothing but Esc and 拒绝 answers, so that keys
// typed into the AI client as the card comes up land nowhere.
//
// `items` are aisettings::itemJson of each change (title, old_text, new_text,
// diff_text, is_list, ask_always); `reason` is the AI's own words, shown as
// such and never as markup.
Window {
    id: window

    required property WindowFrame frame
    required property string client // who asks
    required property string reason // what the AI says it is for
    required property var items
    required property bool askAlways // one of them asks even when the user said "from now on": no such box
    property int secondsLeft: 60

    // 1: allowed; 0: refused; -1: no answer in time. `always`: "from now on" was ticked.
    signal answered(int how, bool always)

    readonly property int padding: 20
    readonly property real dpr: Screen.devicePixelRatio
    // Only what changes; a no-op in the batch is not worth the user's eye.
    readonly property var shown: items.filter(item => item.changed !== false)
    readonly property var securityTitles: items.filter(item => item.ask_always === true).map(item => item.title)
    property bool done: false
    property bool armed: false // a click on 允许 counts (after the card has been up a moment)

    function answer(how) {
        if (done)
            return
        done = true
        countdown.stop()
        answered(how, how === 1 && !askAlways && always.checked)
    }

    function onPixel(v) { return Math.round(v * dpr) / dpr }

    // The style's line between two parts (Theme.separator), as thick as the style wants it.
    component Divider: Hook {
        id: seam
        height: seam.out("thickness", Theme.edge(seam.dpr))
        part: Theme.separator
    }

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: Theme.backdrop && SystemTheme.materials ? "transparent" : Theme.background
    title: qsTr("AI 请求修改设置") // Qt adds " - Win顺"
    width: 440 + 20 * Theme.step
    height: header.height + body.implicitHeight + 2 * window.padding

    Timer {
        id: arm
        interval: 500
        running: true
        onTriggered: window.armed = true
    }

    Timer {
        id: countdown
        interval: 1000
        repeat: true
        running: true
        onTriggered: {
            window.secondsLeft -= 1
            if (window.secondsLeft <= 0)
                window.answer(-1)
        }
    }

    FocusScope {
        id: root
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: window.answer(0)

        Item {
            id: header
            width: parent.width
            height: Theme.headerHeight
            Component.onCompleted: window.frame.addDragArea(header)

            Glyph {
                id: headerIcon
                x: window.padding
                anchors.verticalCenter: parent.verticalCenter
                width: 20
                height: 20
                glyph: "" // Settings
                size: 16
                color: Theme.text
            }
            Text {
                anchors.left: headerIcon.right
                anchors.leftMargin: 8
                anchors.right: parent.right
                anchors.rightMargin: window.padding
                y: window.onPixel((header.height - height) / 2)
                text: qsTr("%1 想修改 Win顺 的设置").arg(window.client)
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: Theme.uiTitle
                font.weight: Theme.fontStrong
            }
            Divider {
                anchors.bottom: parent.bottom
                width: parent.width
                dpr: window.dpr
            }
        }

        Column {
            id: body
            x: window.padding
            anchors.top: header.bottom
            anchors.topMargin: window.padding
            width: parent.width - 2 * window.padding
            spacing: 14

            // What the AI says it is for: quoted, secondary, capped.
            Column {
                width: parent.width
                spacing: 6
                visible: window.reason.length > 0

                Text {
                    text: qsTr("AI 的说明")
                    color: Theme.subtext
                    font.pixelSize: Theme.uiCaption
                }
                Row {
                    width: parent.width
                    spacing: 10

                    Rectangle { // the quote's bar
                        width: 3
                        height: quote.height
                        radius: Theme.lineRadius
                        color: Theme.windowEdge
                    }
                    Text {
                        id: quote
                        width: parent.width - 13
                        text: window.reason
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        color: Theme.text
                        font.pixelSize: Theme.uiBody
                    }
                }
            }

            // What would change: as tall as it is, up to a limit, then it scrolls.
            Column {
                width: parent.width
                spacing: 6

                Text {
                    text: qsTr("将修改 %Ln 项设置", "", window.shown.length)
                    color: Theme.subtext
                    font.pixelSize: Theme.uiCaption
                }
                Item {
                    width: parent.width
                    height: Math.min(list.contentHeight, 4.5 * Theme.rowHeight) + 2 * Theme.edge(window.dpr)

                    Hook { // the style's card (Theme.cardPart)
                        anchors.fill: parent
                        part: Theme.cardPart
                        dpr: window.dpr
                    }

                    Flickable {
                        id: list
                        anchors.fill: parent
                        anchors.margins: Theme.edge(window.dpr) // inside the card's border
                        clip: contentHeight > height
                        contentWidth: width
                        contentHeight: rows.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds

                        Column {
                            id: rows
                            width: list.width

                            Repeater {
                                model: window.shown

                                delegate: Item {
                                    id: change

                                    required property var modelData
                                    required property int index

                                    width: rows.width
                                    height: text.implicitHeight + 20

                                    Column {
                                        id: text
                                        x: 12
                                        y: 10
                                        width: parent.width - 24 - (scrollBar.visible ? scrollBar.width : 0)
                                        spacing: 3

                                        Text {
                                            width: parent.width
                                            text: change.modelData.title
                                            textFormat: Text.PlainText
                                            elide: Text.ElideRight
                                            color: Theme.text
                                            font.pixelSize: Theme.uiBody
                                            font.weight: Theme.fontStrong
                                        }
                                        // A list: what goes out, then what comes in (each line is short,
                                        // "还有 N 项" for the rest); the rest: before → after.
                                        Text {
                                            width: parent.width
                                            visible: change.modelData.is_list === true && text.length > 0
                                            text: change.modelData.removed_text ?? ""
                                            textFormat: Text.PlainText
                                            wrapMode: Text.Wrap
                                            maximumLineCount: 4
                                            elide: Text.ElideRight
                                            color: Theme.danger
                                            font.pixelSize: Theme.uiCaption
                                        }
                                        Text {
                                            width: parent.width
                                            visible: change.modelData.is_list === true && text.length > 0
                                            text: change.modelData.added_text ?? ""
                                            textFormat: Text.PlainText
                                            wrapMode: Text.Wrap
                                            maximumLineCount: 4
                                            elide: Text.ElideRight
                                            color: Theme.text
                                            font.pixelSize: Theme.uiCaption
                                        }
                                        Flow {
                                            width: parent.width
                                            visible: change.modelData.is_list !== true
                                            spacing: 6

                                            Text {
                                                width: Math.min(implicitWidth, parent.width)
                                                text: change.modelData.old_text
                                                textFormat: Text.PlainText
                                                wrapMode: Text.Wrap
                                                color: Theme.subtext
                                                font.pixelSize: Theme.uiCaption
                                            }
                                            Text {
                                                text: "→"
                                                color: Theme.subtext
                                                font.pixelSize: Theme.uiCaption
                                            }
                                            Text {
                                                width: Math.min(implicitWidth, parent.width)
                                                text: change.modelData.new_text
                                                textFormat: Text.PlainText
                                                wrapMode: Text.Wrap
                                                color: Theme.text
                                                font.pixelSize: Theme.uiCaption
                                                font.weight: Theme.fontStrong
                                            }
                                        }
                                    }
                                    Divider { // between two changes
                                        visible: change.index < window.shown.length - 1
                                        anchors.bottom: parent.bottom
                                        x: 12
                                        width: parent.width - 24
                                        dpr: window.dpr
                                    }
                                }
                            }
                        }
                    }
                    ListScrollBar {
                        dpr: window.dpr
                        id: scrollBar
                        flickable: list
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                    }
                }
            }

            // Why the box is not there: these ask every time.
            Row {
                width: parent.width
                spacing: 8
                visible: window.securityTitles.length > 0

                Glyph {
                    width: 16
                    height: warning.height
                    glyph: "" // Warning
                    size: 14
                    color: Theme.caution
                }
                Text {
                    id: warning
                    width: parent.width - 24
                    text: qsTr("“%1”涉及安全，每次都会问你。").arg(window.securityTitles.join(qsTr("”“")))
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.subtext
                    font.pixelSize: Theme.uiCaption
                }
            }

            // Only the user's own click turns this on.
            Item {
                id: always
                readonly property alias checked: box.checked

                visible: !window.askAlways
                width: parent.width
                height: visible ? 28 : 0

                Hook { // the style's check box (Theme.toggle, kind "check")
                    id: mark
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.tok("checkSize", 18)
                    height: width
                    part: Theme.toggle
                    kind: "check"
                    checked: box.checked
                    hovered: box.containsMouse
                    pressed: box.pressed
                    dpr: window.dpr
                }
                Text {
                    anchors.left: mark.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("以后直接修改，不再询问")
                    color: Theme.text
                    font.pixelSize: Theme.uiBody
                }
                MouseArea {
                    id: box
                    property bool checked: false
                    anchors.fill: parent
                    enabled: window.armed
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: checked = !checked
                }
                Accessible.role: Accessible.CheckBox
                Accessible.name: qsTr("以后直接修改，不再询问")
                Accessible.checked: box.checked
            }

            Item {
                width: parent.width
                height: denyButton.implicitHeight

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("%1 秒后自动拒绝").arg(window.secondsLeft)
                    color: Theme.faint
                    font.pixelSize: Theme.uiCaption
                }
                Row {
                    anchors.right: parent.right
                    spacing: 8

                    FlatButton {
                        dpr: window.dpr
                        id: denyButton
                        role: "dialog"
                        text: qsTr("拒绝")
                        onClicked: window.answer(0)
                    }
                    FlatButton {
                        dpr: window.dpr
                        role: "dialog"
                        highlighted: true
                        enabled: window.armed
                        keyboardActivation: false
                        text: qsTr("允许")
                        onClicked: window.answer(1)
                    }
                }
            }
        }
    }

    WindowEdge { id: windowEdge; fill: Theme.background }
}
