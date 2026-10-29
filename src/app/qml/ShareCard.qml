import QtQuick
import WinShun

// A network share in the settings (SettingsEditor.shares): its name, where it
// stands, and what can be done about it; under it, whether its files'
// contents are searched too, and how far they are read (GitHub #114). Also a
// folder chosen for the index (kind "folder", SettingsEditor.folders, GitHub
// #52): no signing in, no contents of its own, and 立即核对 only where it
// means something (`verifiable`).
Rectangle {
    id: card

    property string kind: "share" // share | folder
    property var share: blank // {path, name, tone, status, signIn, suspect, busy[, verifiable][, content, contentStatus]}
    readonly property var blank: ({ path: "", name: "", tone: "idle", status: "", signIn: false, suspect: false, busy: false, verifiable: false,
                                    content: false, contentStatus: "" })
    property bool marked: false // what the settings search found

    signal verifyClicked()
    signal startClicked()
    signal signInClicked()
    signal acceptClicked()
    signal removeClicked()
    signal contentToggled(bool on)

    readonly property color toneColor: share.tone === "ok" ? Theme.success
                                     : share.tone === "busy" ? Theme.accent
                                     : share.tone === "warn" ? Theme.caution
                                     : share.tone === "error" ? Theme.danger
                                     : Theme.faint

    readonly property real headHeight: Math.max(texts.implicitHeight, buttons.implicitHeight)
    // Whole device pixels: the cards below it in the column, and its switch, stay sharp.
    implicitHeight: Theme.snap(headHeight + 24 + (card.kind === "share" ? contents.height + 10 : 0), Screen.devicePixelRatio)
    radius: Theme.controlRadius
    color: "transparent"
    border.width: 1
    border.color: marked ? Theme.accent : Theme.cardBorder

    Glyph {
        id: icon
        x: 14
        y: 14
        glyph: card.kind === "folder" ? "\uE8B7" : card.share.wsl === true ? "\uE756" : "\uE8CE"
        size: 20
        color: Theme.subtext
    }

    Column {
        id: texts
        anchors.left: icon.right
        anchors.leftMargin: 12
        anchors.right: buttons.left
        anchors.rightMargin: 12
        y: 12
        spacing: 4

        Text {
            width: parent.width
            text: card.share.name
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.fontBody
            elide: Text.ElideMiddle
        }
        Text {
            visible: card.share.name !== card.share.path
            width: parent.width
            text: card.share.path
            textFormat: Text.PlainText
            color: Theme.faint
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideMiddle
        }
        Item {
            visible: card.share.status.length > 0 // a folder while the disks are indexed whole: nothing to say
            width: parent.width
            height: status.implicitHeight

            Rectangle { // where it stands, at a glance; the words say it
                id: dot
                y: Math.round(status.fontInfo.pixelSize * 0.62 - height / 2)
                width: 8
                height: 8
                radius: 4
                color: card.toneColor

                SequentialAnimation on opacity { // read just now
                    running: card.share.busy && SystemTheme.animations
                    loops: Animation.Infinite
                    alwaysRunToEnd: true
                    NumberAnimation { to: 0.3; duration: 600; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 1; duration: 600; easing.type: Easing.InOutSine }
                }
            }
            Text {
                id: status
                anchors.left: dot.right
                anchors.leftMargin: 8
                anchors.right: parent.right
                text: card.share.status
                textFormat: Text.PlainText
                color: card.share.tone === "error" ? Theme.danger : Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }
        }
    }

    Row {
        id: buttons
        anchors.right: parent.right
        anchors.rightMargin: 8
        y: 8
        spacing: 4

        FlatButton {
            visible: card.share.startable === true
            implicitHeight: 32
            highlighted: true
            text: qsTr("启动并索引")
            onClicked: card.startClicked()
        }
        FlatButton {
            visible: card.kind === "share" && card.share.signIn
            implicitHeight: 32
            highlighted: true
            text: qsTr("登录")
            onClicked: card.signInClicked()
        }
        FlatButton {
            visible: card.kind === "share" && card.share.suspect
            implicitHeight: 32
            text: qsTr("按现在的内容更新")
            onClicked: card.acceptClicked()
        }
        FlatButton {
            visible: card.kind === "share" ? !card.share.signIn && card.share.startable !== true : card.share.verifiable === true
            enabled: !card.share.busy
            implicitHeight: 32
            flat: true
            text: qsTr("立即核对")
            onClicked: card.verifyClicked()
        }
        FlatButton {
            implicitHeight: 32
            flat: true
            text: qsTr("移除")
            onClicked: card.removeClicked()
        }
    }

    // After the buttons, so that Tab comes to it after them, as it is below them.
    Item { // its files' contents, searched too (GitHub #114)
        id: contents
        visible: card.kind === "share"
        anchors.left: texts.left
        anchors.right: parent.right
        anchors.rightMargin: 20 // the switch's edge under 移除's text: the buttons' 8 and a FlatButton's 12 of padding
        y: Theme.snap(12 + card.headHeight + 10, Screen.devicePixelRatio)
        height: Theme.snapUp(Math.max(contentTexts.implicitHeight, contentSwitch.implicitHeight), Screen.devicePixelRatio)

        Column {
            id: contentTexts
            anchors.left: parent.left
            anchors.right: contentSwitch.left
            anchors.rightMargin: 12
            y: Theme.snap((parent.height - height) / 2, Screen.devicePixelRatio)
            spacing: 2

            Text {
                width: parent.width
                text: qsTr("搜索文件内容")
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: text.length > 0
                text: card.share.contentStatus ?? ""
                textFormat: Text.PlainText
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }
        ToggleSwitch {
            id: contentSwitch
            anchors.right: parent.right
            y: Theme.snap((parent.height - height) / 2, Screen.devicePixelRatio) // not verticalCenter: the track's edges on whole pixels
            checked: card.share.content === true
            Accessible.name: qsTr("搜索文件内容")
            onToggled: (on) => card.contentToggled(on)
        }
    }
}
