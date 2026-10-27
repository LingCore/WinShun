pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// "Software update", a Windows 11 style dialog over the settings window:
// checking, up to date, failed, or a new version with what changed in it.
// "Download and install" downloads, checks and installs it (see Updater),
// with the download's progress and a way to cancel it.
Item {
    id: dialog

    required property Updater updater
    property string icon // the program's icon (image://fileicon/...)
    property bool open: false

    function show() {
        open = true
        card.forceActiveFocus()
    }
    function close() { open = false }

    visible: opacity > 0
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

    Rectangle { // dims the window; clicks stay out of it
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, Theme.dark ? 0.5 : 0.3)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    Rectangle {
        id: card

        readonly property bool newer: dialog.updater.available
        readonly property string stage: dialog.updater.installStage // "" when nothing is under way
        readonly property bool cancellable: stage === "downloading"

        anchors.centerIn: parent
        width: Math.min(520, parent.width - 48)
        height: content.implicitHeight + 48
        radius: Theme.largeCardRadius
        color: Theme.menuBackground
        border.width: 1
        border.color: Theme.windowEdge
        scale: dialog.open ? 1 : 0.96
        Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

        Keys.onEscapePressed: dialog.close()
        Keys.onReturnPressed: primary.clicked()
        Keys.onEnterPressed: primary.clicked()

        MouseArea { anchors.fill: parent } // a click on the card does not reach the dimmer

        Column {
            id: content
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 16

            Row {
                width: parent.width
                spacing: 16

                Image {
                    id: appIcon

                    // Whole device pixels, unsmoothed: sharp (see TitleBar).
                    readonly property real dpr: Screen.devicePixelRatio
                    readonly property int pixels: Math.round(48 * dpr)

                    anchors.verticalCenter: parent.verticalCenter
                    width: pixels / dpr
                    height: pixels / dpr
                    source: dialog.icon
                    sourceSize.width: pixels / dpr
                    sourceSize.height: pixels / dpr
                    smooth: false
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - appIcon.width - parent.spacing - (busy.visible ? busy.width + parent.spacing : 0)
                    spacing: 4

                    Text {
                        width: parent.width
                        text: dialog.updater.checking ? qsTr("正在检查更新…")
                            : card.newer ? qsTr("Win顺 %1 可以更新了").arg(dialog.updater.availableVersion)
                            : dialog.updater.problem.length > 0 ? qsTr("检查更新失败")
                            : qsTr("已经是最新版本")
                        color: Theme.text
                        font.pixelSize: Theme.fontTitle
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                    }
                    Text {
                        width: parent.width
                        visible: text.length > 0
                        text: dialog.updater.checking ? ""
                            : card.newer && card.stage === "downloading" ? qsTr("正在下载… %1 / %2 MB")
                                .arg(dialog.updater.downloadedMB.toFixed(1)).arg(dialog.updater.packageMB.toFixed(1))
                            : card.newer && card.stage === "verifying" ? qsTr("正在校验安装包…")
                            : card.newer && card.stage === "installing" ? (dialog.updater.portable
                                ? qsTr("Win顺 马上关闭并替换程序文件，完成后自动重新打开。")
                                : qsTr("正在安装：Win顺 会先关闭，装好后自动重新打开。为所有用户安装的，系统会先确认管理员权限。"))
                            : card.newer && dialog.updater.installProblem.length > 0 ? dialog.updater.installProblem
                            : card.newer ? (dialog.updater.portable
                                ? qsTr("你现在用的是 %1。下载并校验后自动替换程序文件并重新打开，data 文件夹里的设置和索引原样保留。").arg(dialog.updater.currentVersion)
                                : qsTr("你现在用的是 %1。下载并校验后自动安装并重新打开，设置和索引都会保留。").arg(dialog.updater.currentVersion))
                            : dialog.updater.problem.length > 0 ? dialog.updater.problem
                            : qsTr("Win顺 %1 是目前最新的版本。").arg(dialog.updater.currentVersion)
                        color: (card.newer ? card.stage === "" && dialog.updater.installProblem.length > 0
                                           : dialog.updater.problem.length > 0) ? Theme.danger : Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                        lineHeight: 1.15
                    }
                }

                Item { // three dots pulsing while checking
                    id: busy
                    visible: dialog.updater.checking
                    anchors.verticalCenter: parent.verticalCenter
                    width: 30
                    height: 8

                    Repeater {
                        model: 3
                        delegate: Rectangle {
                            id: dot
                            required property int index
                            x: index * 11
                            width: 6
                            height: 6
                            radius: 3
                            color: Theme.accent

                            SequentialAnimation on opacity {
                                running: busy.visible && dialog.open
                                loops: Animation.Infinite
                                PauseAnimation { duration: dot.index * 160 }
                                NumberAnimation { from: 0.25; to: 1; duration: 320 }
                                NumberAnimation { from: 1; to: 0.25; duration: 320 }
                                PauseAnimation { duration: (2 - dot.index) * 160 }
                            }
                        }
                    }
                }
            }

            Rectangle { // how much is downloaded
                visible: card.newer && card.stage === "downloading"
                width: parent.width
                height: 4
                radius: Theme.progressRadius
                color: Theme.track

                Rectangle {
                    width: parent.width * dialog.updater.installProgress
                    height: parent.height
                    radius: Theme.progressRadius
                    color: Theme.accent

                    Behavior on width { NumberAnimation { duration: 150 } }
                }
            }

            Rectangle { // what is new
                id: notesBox
                visible: card.newer && (dialog.updater.summary.length > 0 || dialog.updater.highlights.length > 0)
                width: parent.width
                height: Math.min(notes.implicitHeight + 24, 240)
                radius: Theme.cardRadius
                color: Theme.hover
                border.width: 1
                border.color: Theme.cardBorder
                clip: true

                Flickable {
                    id: notesFlick
                    anchors.fill: parent
                    anchors.margins: 12
                    contentWidth: width
                    contentHeight: notes.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds

                    Column {
                        id: notes
                        width: notesFlick.width
                        spacing: 10
                        // On whole device pixels wherever it has scrolled to,
                        // a fiftieth of one lower (see SettingsWindow).
                        transform: Translate {
                            y: {
                                const dpr = Screen.devicePixelRatio
                                const deviceY = (dialog.y + card.y + content.y + notesBox.y + notesFlick.y
                                                 + notesFlick.contentItem.y + notes.y) * dpr
                                return (Math.round(deviceY) - deviceY + 0.02) / dpr
                            }
                        }

                        Text {
                            width: parent.width
                            visible: text.length > 0
                            text: dialog.updater.summary
                            textFormat: Text.MarkdownText // **bold**, `code`, links
                            color: Theme.text
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                            onLinkActivated: (link) => dialog.updater.openDownloadPage()
                        }
                        Repeater {
                            model: dialog.updater.highlights
                            delegate: Row {
                                id: item
                                required property var modelData
                                width: notes.width
                                spacing: 8

                                Text {
                                    width: 22
                                    text: item.modelData.symbol.length > 0 ? item.modelData.symbol : "•"
                                    color: Theme.text
                                    font.pixelSize: Theme.fontCaption
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                Text {
                                    width: parent.width - 30
                                    text: item.modelData.text
                                    textFormat: Text.MarkdownText
                                    color: Theme.text
                                    font.pixelSize: Theme.fontCaption
                                    wrapMode: Text.Wrap
                                    lineHeight: 1.1
                                }
                            }
                        }
                    }
                }
            }

            Text { // the full notes, on lcstp.com
                visible: card.newer
                text: qsTr("查看完整说明")
                color: Theme.accent
                font.pixelSize: Theme.fontCaption
                font.underline: linkArea.containsMouse

                MouseArea {
                    id: linkArea
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dialog.updater.openDownloadPage()
                }
            }

            Item {
                width: parent.width
                height: primary.implicitHeight

                FlatButton { // not reminded of this version again; still shown when asked
                    visible: card.newer && !dialog.updater.skipped && card.stage === ""
                    anchors.left: parent.left
                    flat: true
                    text: qsTr("跳过这个版本")
                    onClicked: {
                        dialog.updater.skip()
                        dialog.close()
                    }
                }

                Row {
                    anchors.right: parent.right
                    spacing: 8

                    FlatButton {
                        visible: !dialog.updater.checking && (card.stage === "" || card.cancellable)
                                 && (card.newer || dialog.updater.problem.length > 0)
                        text: card.cancellable ? qsTr("取消")
                            : card.newer ? qsTr("以后再说") : qsTr("打开下载页")
                        onClicked: {
                            if (card.cancellable) {
                                dialog.updater.cancelInstall()
                                return
                            }
                            if (!card.newer)
                                dialog.updater.openDownloadPage()
                            dialog.close()
                        }
                    }
                    FlatButton {
                        id: primary
                        highlighted: true
                        enabled: !card.newer || card.stage === ""
                        glyph: card.newer ? "" : "" // Download
                        text: card.newer ? (dialog.updater.portable ? qsTr("下载并更新") : qsTr("下载并安装"))
                            : dialog.updater.checking ? qsTr("关闭")
                            : dialog.updater.problem.length > 0 ? qsTr("重试")
                            : qsTr("好")
                        onClicked: {
                            if (card.newer) {
                                if (card.stage === "")
                                    dialog.updater.install()
                            } else if (!dialog.updater.checking && dialog.updater.problem.length > 0) {
                                dialog.updater.check(true)
                            } else {
                                dialog.close()
                            }
                        }
                    }
                }
            }
        }
    }
}
