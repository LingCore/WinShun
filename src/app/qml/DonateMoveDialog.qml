pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Asked when the × beside the settings' 捐赠 button is clicked, before the
// button moves to the 拾穗计划 page (GitHub #402), over the window as
// UpdateDialog is: a word from the author beside the avatar, the website
// to drop by, and two buttons. 捐赠 (the button itself, as in the title bar)
// opens the donation page and leaves the question up, thanking, for when the
// user comes back; 确认 (or Enter) moves the button. Esc or a click beside
// the card leave it where it is.
Item {
    id: dialog

    property bool open: false
    property bool thanked: false // 捐赠 was clicked while it was up
    readonly property real dpr: Screen.devicePixelRatio

    signal donate()
    signal visitSite()
    signal confirmed()

    function ask() {
        thanked = false
        open = true
        card.forceActiveFocus()
    }
    function close() { open = false }

    visible: opacity > 0
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

    Rectangle { // dims the window; a click there leaves the button where it is
        anchors.fill: parent
        color: Theme.tok("scrim", Qt.rgba(0, 0, 0, Theme.dark ? 0.5 : 0.3))

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onWheel: (wheel) => wheel.accepted = true
            onClicked: dialog.close()
        }
    }

    Item {
        id: card
        objectName: "donateMoveCard"

        anchors.centerIn: parent
        width: Math.min(460, parent.width - 48)
        height: content.implicitHeight + 48
        scale: dialog.open ? 1 : 0.96
        Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

        Keys.onEscapePressed: dialog.close()
        Keys.onReturnPressed: confirm.clicked()
        Keys.onEnterPressed: confirm.clicked()

        Hook { // the style's floating box (Theme.menuFrame)
            anchors.fill: parent
            part: Theme.menuFrame
            dpr: dialog.dpr
        }

        MouseArea { anchors.fill: parent } // a click on the card does not reach the dimmer

        Column {
            id: content
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 18

            Row {
                width: parent.width
                spacing: 16

                Item { // the author's avatar, still; its picture's ring and shadow reach past this box
                    id: face
                    width: 48
                    height: 48

                    // Loaded at the whole device pixels it covers and sampled
                    // unsmoothed, a hair off the half pixel (see AuthorAvatar).
                    Image {
                        readonly property int pixels: Math.round(face.width * 96 / 56 * dialog.dpr)

                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: 1 / 64
                        anchors.verticalCenterOffset: 1 / 64
                        width: pixels / dialog.dpr
                        height: width
                        source: "qrc:/qt/qml/WinShun/gleaning/avatar.png"
                        sourceSize.width: pixels
                        sourceSize.height: pixels
                        smooth: false
                    }
                }

                Column {
                    width: parent.width - face.width - parent.spacing
                    spacing: 6

                    Text {
                        width: parent.width
                        text: qsTr("把捐赠按钮挪到%1页？").arg(Gleaning.title)
                        color: Theme.text
                        font.pixelSize: Theme.fontTitle
                        font.weight: Theme.fontStrong
                        wrapMode: Text.Wrap
                    }
                    Text {
                        // The rose of the 捐赠 key for the slogan's two words (用心, 好用), bold;
                        // lighter on a dark card. Either reads at 4.6:1 or more on the style's cards.
                        readonly property color mark: Theme.dark ? "#FF7A9C" : "#D1245A"

                        objectName: "donateMoveWord"
                        width: parent.width
                        textFormat: Text.StyledText
                        // A sentence a line, as below: wrapped as one, a word broke over the lines.
                        text: dialog.thanked ? qsTr("谢谢你的心意！") + "<br>" + qsTr("Win顺 会继续用心做下去。")
                                             : qsTr("用最<font color=\"%1\"><b>用心</b></font>的态度，做最<font color=\"%1\"><b>好用</b></font>的产品。").arg(mark)
                                               + "<br>" + qsTr("如果 Win顺 帮到了你，欢迎请我喝瓶快乐水～")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        lineHeight: 1.15
                        wrapMode: Text.Wrap
                    }
                }
            }

            Text { // what 确认 does, centred on the card (the arrow back, on the page, tells itself)
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("点“确认”，它就挪到%1页，标题栏清爽一些。").arg(Gleaning.title)
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                lineHeight: 1.15
                wrapMode: Text.Wrap
            }

            Item {
                width: parent.width
                height: confirm.height

                Item { // the website, a link: underlined under the pointer
                    objectName: "donateMoveSite"
                    visible: Gleaning.siteUrl.length > 0
                    anchors.verticalCenter: parent.verticalCenter
                    width: siteWords.implicitWidth + 4 + siteArrow.width
                    height: siteWords.implicitHeight

                    Accessible.role: Accessible.Link
                    Accessible.name: siteWords.text
                    Accessible.onPressAction: dialog.visitSite()

                    Text {
                        id: siteWords
                        text: qsTr("来官网坐坐，那里很有趣")
                        color: Theme.accent
                        font.pixelSize: Theme.fontCaption
                        font.underline: siteArea.containsMouse
                    }
                    Glyph {
                        id: siteArrow
                        anchors.left: siteWords.right
                        anchors.leftMargin: 4
                        anchors.verticalCenter: siteWords.verticalCenter
                        iconName: "arrow-up-right"
                        size: 12
                        color: Theme.accent
                    }
                    MouseArea {
                        id: siteArea
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dialog.visitSite()
                    }
                }

                Row {
                    anchors.right: parent.right
                    spacing: 8

                    DonateButton {
                        objectName: "donateMoveDonate"
                        width: Math.max(implicitWidth, confirm.width)
                        height: confirm.height
                        dpr: dialog.dpr
                        onClicked: {
                            dialog.thanked = true
                            dialog.donate()
                        }
                    }
                    FlatButton {
                        id: confirm
                        objectName: "donateMoveConfirm"
                        width: Math.max(implicitWidth, 80)
                        dpr: dialog.dpr
                        role: "dialog"
                        text: qsTr("确认")
                        onClicked: {
                            dialog.close()
                            dialog.confirmed()
                        }
                    }
                }
            }
        }
    }
}
