pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A plugin of the 插件库 (PluginGallery.list), drawn as a folded PluginCard:
// its icon, name and version; what it does; who made it, its keywords as
// keys to press and what has to be installed first; what it may do, the
// riskiest (going online, writing files, starting programs) marked with a
// warning. On the right what it is here: a button that installs or updates
// it; 已安装 with 查看 (its card on the 已安装 tab); 随 Win顺 自带 alone
// (it comes and goes with Win顺); or, when it asks for a newer Win顺, the
// button off and why under it. While it downloads: how far, and 取消 on
// the button.
Rectangle {
    id: entry

    required property PluginGallery gallery
    required property string pluginId
    required property string name
    required property string description
    required property string author
    required property string version
    required property string icon
    required property var keywords
    required property var permissions
    required property string needs
    required property bool needsMissing
    required property string stage // install | update | installed | bundled | app
    required property string installedVersion
    required property string minWinShun
    required property bool busy
    required property real progress
    required property string problem
    required property bool shown

    readonly property real dpr: Screen.devicePixelRatio
    readonly property bool installable: stage === "install" || stage === "update"

    signal showInstalled() // 查看

    visible: shown
    implicitHeight: Theme.snapUp(body.implicitHeight + 2 * body.y, dpr)
    radius: Theme.controlRadius
    color: "transparent"
    border.width: 1
    border.color: Theme.cardBorder

    // Offsets, gaps and the heights of the lines of text on whole device
    // pixels, so that the keycaps, the button and the progress line below
    // them are sharp at 125 %, 150 % and 175 %.
    Column {
        id: body
        x: Theme.snap(14, entry.dpr)
        y: Theme.snap(12, entry.dpr)
        width: parent.width - 2 * x
        spacing: Theme.snap(10, entry.dpr)

        Item { // the icon and the words; what it is here on the right
            width: parent.width
            height: Theme.snapUp(Math.max(texts.implicitHeight, action.implicitHeight), entry.dpr)

            Item {
                id: iconSlot
                width: 32
                height: 32
                y: Theme.snap(2, entry.dpr)

                Image { // whole device pixels, unsmoothed (see ResultRow)
                    readonly property int pixels: Math.round(32 * entry.dpr)

                    x: 1 / 64
                    y: 1 / 64
                    width: pixels / entry.dpr
                    height: pixels / entry.dpr
                    source: entry.icon
                    sourceSize.width: pixels / entry.dpr
                    sourceSize.height: pixels / entry.dpr
                    smooth: false
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                }
            }

            Column {
                id: texts
                anchors.left: iconSlot.right
                anchors.leftMargin: 12
                anchors.right: action.left
                anchors.rightMargin: 12
                spacing: 4

                Row {
                    width: parent.width
                    spacing: 8

                    Text {
                        id: nameText
                        text: entry.name
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Theme.fontStrong
                        elide: Text.ElideRight
                        width: Math.min(implicitWidth, parent.width - versionText.width - 8)
                        height: Theme.snapUp(implicitHeight, entry.dpr)
                    }
                    Text {
                        id: versionText
                        anchors.baseline: nameText.baseline
                        text: entry.version
                        textFormat: Text.PlainText
                        color: Theme.faint
                        font.pixelSize: Theme.fontCaption
                    }
                }
                Text { // what it does
                    width: parent.width
                    height: Theme.snapUp(implicitHeight, entry.dpr)
                    visible: entry.description.length > 0
                    text: entry.description
                    textFormat: Text.PlainText
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
                Flow { // who made it, its keywords, what it needs
                    width: parent.width
                    spacing: Theme.snap(6, entry.dpr)

                    Text {
                        visible: entry.author.length > 0
                        width: Theme.snapUp(implicitWidth, entry.dpr)
                        height: 24
                        verticalAlignment: Text.AlignVCenter
                        text: entry.author
                        textFormat: Text.PlainText
                        color: Theme.faint
                        font.pixelSize: Theme.fontCaption
                    }
                    Repeater {
                        model: entry.keywords

                        delegate: Rectangle { // as keys to press (PluginCard's)
                            id: keycap

                            required property string modelData

                            width: Theme.snapUp(Math.max(28, keycapText.implicitWidth + 14), entry.dpr)
                            height: 24
                            radius: Theme.controlRadius
                            color: Theme.keycap

                            Text {
                                id: keycapText
                                anchors.centerIn: parent
                                text: keycap.modelData
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                    }
                    Text { // the interpreter it runs on; not installed here, in the caution colour
                        visible: entry.needs.length > 0
                        height: 24
                        verticalAlignment: Text.AlignVCenter
                        text: entry.needsMissing ? qsTr("%1（这台电脑还没装）").arg(entry.needs) : entry.needs
                        textFormat: Text.PlainText
                        color: entry.needsMissing ? Theme.caution : Theme.subtext
                        font.pixelSize: Theme.fontCaption
                    }
                }

            }

            Row { // what it is here, and what can be done
                id: action
                anchors.right: parent.right
                spacing: 10

                Text {
                    y: Theme.snap((32 - height) / 2, entry.dpr) // across from the button, there or not
                    visible: text.length > 0
                    text: entry.busy ? "" : entry.stage === "installed" ? qsTr("已安装 %1").arg(entry.installedVersion)
                                        : entry.stage === "bundled" ? qsTr("随 Win顺 自带")
                                        : entry.stage === "update" ? qsTr("已装 %1").arg(entry.installedVersion)
                                        : ""
                    textFormat: Text.PlainText
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }
                FlatButton {
                    objectName: "installPlugin-" + entry.pluginId
                    visible: entry.busy || entry.stage !== "bundled" // nothing to do with one that comes with Win顺
                    implicitHeight: 32
                    text: entry.busy ? qsTr("取消")
                        : entry.stage === "install" ? qsTr("安装并启用")
                        : entry.stage === "update" ? qsTr("更新到 %1").arg(entry.version)
                        : entry.stage === "app" ? qsTr("需要更新 Win顺")
                        : qsTr("查看")
                    highlighted: entry.installable && !entry.busy
                    // One download at a time; a newer Win顺 first.
                    enabled: entry.busy || (entry.installable ? !entry.gallery.downloading : entry.stage !== "app")
                    onClicked: {
                        if (entry.busy)
                            entry.gallery.cancel(entry.pluginId)
                        else if (entry.stage === "install")
                            entry.gallery.installAndEnable(entry.pluginId)
                        else if (entry.installable)
                            entry.gallery.install(entry.pluginId)
                        else
                            entry.showInstalled()
                    }
                }
            }
        }

        Flow { // what it may do
            id: permissionsFlow
            x: texts.x
            width: body.width - x
            spacing: 12

            Text {
                width: Math.min(Theme.snapUp(implicitWidth, entry.dpr), permissionsFlow.width)
                height: Math.max(20, Theme.snapUp(implicitHeight, entry.dpr))
                verticalAlignment: Text.AlignVCenter
                text: entry.permissions.length > 0 ? qsTr("权限") : qsTr("不需要额外的权限")
                color: Theme.faint
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            Repeater {
                model: entry.permissions

                delegate: Row { // the riskiest with a warning, in the caution colour
                    id: permission

                    required property var modelData

                    height: Math.max(20, permissionText.implicitHeight)
                    spacing: 4

                    Glyph {
                        id: permissionWarning
                        anchors.verticalCenter: parent.verticalCenter
                        visible: permission.modelData.risky
                        glyph: "" // Warning
                        size: 12
                        color: Theme.caution
                    }
                    Text {
                        id: permissionText
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(Theme.snapUp(implicitWidth, entry.dpr),
                            Math.max(0, permissionsFlow.width - (permission.modelData.risky ? permissionWarning.width + permission.spacing : 0)))
                        text: permission.modelData.text
                        textFormat: Text.PlainText
                        color: permission.modelData.risky ? Theme.caution : Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        Column { // how far the download is
            visible: entry.busy
            width: parent.width
            spacing: Theme.snap(6, entry.dpr)

            Text {
                height: Theme.snapUp(implicitHeight, entry.dpr)
                text: qsTr("正在下载… %1%").arg(Math.round(entry.progress * 100))
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
            Rectangle {
                width: parent.width
                height: 4
                radius: Theme.progressRadius
                color: Theme.track

                Rectangle {
                    width: Theme.snap(parent.width * entry.progress, entry.dpr)
                    height: parent.height
                    radius: Theme.progressRadius
                    color: Theme.accent

                    Behavior on width { NumberAnimation { duration: 150 } }
                }
            }
        }

        Text { // why the last try did not install it, or why it cannot be installed yet
            visible: text.length > 0
            width: parent.width
            text: entry.problem.length > 0 ? entry.problem
                : entry.stage === "app" ? qsTr("要 Win顺 %1 或更新的版本，可以在“高级”里检查更新").arg(entry.minWinShun)
                : ""
            textFormat: Text.PlainText
            color: entry.problem.length > 0 ? Theme.danger : Theme.caution
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
    }
}
