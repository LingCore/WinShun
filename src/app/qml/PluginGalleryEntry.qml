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
    required property string reviewLabel
    required property bool unreviewed
    required property string author
    required property string homepage // an address of the web, or empty: PluginGallery looks at it (GitHub #248)
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
    required property string staged // a newer version kept aside for the next start
    required property bool hasReadme // the list names its README: 说明 shows it (GitHub #304)
    required property bool readmeBusy
    required property string morePermissions // what the list's version asks for besides, named

    readonly property real dpr: Screen.devicePixelRatio
    readonly property bool installable: stage === "install" || stage === "update"
    // Where 主页 leads, for its tip: "github.com" of "https://github.com/…".
    readonly property string homepageHost: homepage.replace(/^https?:\/\/([^\/?#]+).*$/i, "$1")
    readonly property bool waiting: stage === "update" && staged.length > 0 && !busy // put in place at the next start, or now
    // The tip of the "i" by the name: what need not be read to choose (it is asked again when installing).
    readonly property bool anyRisky: permissions.some(p => p.risky)
    readonly property string infoText: {
        const lines = []
        if (reviewLabel.length > 0)
            lines.push(reviewLabel)
        if (author.length > 0)
            lines.push(qsTr("作者：%1").arg(author))
        if (keywords.length > 0)
            lines.push(qsTr("关键字：%1").arg(keywords.join("  ")))
        if (needs.length > 0)
            lines.push(needs)
        if (stage === "update" || stage === "installed")
            lines.push(qsTr("已安装 %1").arg(installedVersion))
        if (permissions.length > 0) {
            lines.push(qsTr("权限："))
            for (const p of permissions)
                lines.push((p.risky ? "⚠ " : "• ") + p.text)
        } else {
            lines.push(qsTr("不需要额外的权限"))
        }
        return lines.join("\n")
    }

    signal showInstalled() // 查看

    visible: shown
    implicitHeight: Theme.snapUp(body.implicitHeight + 2 * body.y, dpr)
    radius: Theme.controlRadius
    color: "transparent"

    Hook { // the style's card (Theme.cardPart), a plate inside the section's
        anchors.fill: parent
        part: Theme.cardPart
        corner: Theme.controlRadius
        extra: ({ inset: true })
        dpr: entry.dpr
    }

    // Offsets, gaps and the heights of the lines of text on whole device
    // pixels, so that the keycaps, the button and the progress line below
    // them are sharp at 125 %, 150 % and 175 %.
    Column {
        id: body
        x: Theme.snap(14, entry.dpr)
        y: Theme.snap(12, entry.dpr)
        width: parent.width - 2 * x
        spacing: Theme.snap(8, entry.dpr)

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
                        width: Math.min(implicitWidth, parent.width - versionText.width - 8
                                                        - (homepageSlot.visible ? homepageSlot.width + 8 : 0)
                                                        - (readmeSlot.visible ? readmeSlot.width + 8 : 0)
                                                        - infoDot.width - 8)
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
                    // 主页 (GitHub #248): its own page, to look at before
                    // installing, beside what it is the page of. Among what
                    // can be done on the right it stood off from the button,
                    // further the longer the words between them. No taller
                    // than the name: the line, and all under it, as without.
                    Item {
                        id: homepageSlot
                        visible: entry.homepage.length > 0
                        width: 24
                        height: nameText.height

                        FlatButton {
                            id: homepageButton
                            dpr: entry.dpr
                            objectName: "pluginHomepage-" + entry.pluginId
                            y: Theme.snap((parent.height - height) / 2, entry.dpr) // on whole device pixels, as the rest
                            implicitWidth: 24
                            implicitHeight: 24
                            flat: true // there, not asking to be pressed
                            // Beside the name it is not to be read before it: as faint as
                            // the version until the pointer or the keyboard is on it.
                            opacity: homepageHover.hovered || activeFocus ? 1 : 0.55
                            glyph: "\uE71B" // Link, as 主页 in the installed card's menu
                            Accessible.name: qsTr("主页")
                            onClicked: entry.gallery.openHomepage(entry.pluginId)
                            HoverHandler { id: homepageHover }
                            HoverTip { // where it leads, before the click: the list's words are not Win顺's own
                                target: homepageHover.hovered ? homepageButton : null
                                text: qsTr("主页：%1").arg(entry.homepageHost)
                            }
                        }
                    }
                    Item { // 说明 (GitHub #304): its README, read here before installing
                        id: readmeSlot
                        visible: entry.hasReadme
                        width: 24
                        height: nameText.height

                        FlatButton {
                            id: readmeButton
                            dpr: entry.dpr
                            objectName: "pluginReadme-" + entry.pluginId
                            y: Theme.snap((parent.height - height) / 2, entry.dpr)
                            implicitWidth: 24
                            implicitHeight: 24
                            flat: true
                            enabled: !entry.readmeBusy
                            opacity: readmeHover.hovered || activeFocus ? 1 : 0.55
                            glyph: "\uE8A5" // Document, as 说明 in the installed card's menu
                            Accessible.name: qsTr("说明")
                            onClicked: entry.gallery.openReadme(entry.pluginId)
                            HoverHandler { id: readmeHover }
                            HoverTip {
                                target: readmeHover.hovered ? readmeButton : null
                                dpr: entry.dpr
                                text: entry.readmeBusy ? qsTr("正在取说明…") : qsTr("说明")
                            }
                        }
                    }
                    InfoDot { // who made it, its keywords, what it may do
                        id: infoDot
                        objectName: "pluginInfo-" + entry.pluginId
                        y: Theme.snap((nameText.height - height) / 2, entry.dpr)
                        dpr: entry.dpr
                        caution: entry.anyRisky
                        text: entry.infoText
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
                Text { // not reviewed, or its interpreter is not installed here: worth reading before installing
                    objectName: "pluginReviewLabel"
                    width: parent.width
                    visible: text.length > 0
                    text: [entry.unreviewed ? entry.reviewLabel : "",
                           entry.needsMissing ? qsTr("%1（这台电脑还没装）").arg(entry.needs) : ""].filter(t => t.length > 0).join(" · ")
                    textFormat: Text.PlainText
                    color: Theme.caution
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
            }

            Row { // what it is here, and what can be done
                id: action
                anchors.right: parent.right
                spacing: 10

                Text { // it comes and goes with Win顺: nothing to press
                    y: Theme.snap((32 - height) / 2, entry.dpr)
                    visible: !entry.busy && entry.stage === "bundled"
                    text: qsTr("自带")
                    textFormat: Text.PlainText
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }
                FlatButton {
                    id: actionButton
                    dpr: entry.dpr
                    objectName: "installPlugin-" + entry.pluginId
                    visible: entry.busy || entry.stage !== "bundled" // nothing to do with one that comes with Win顺
                    implicitHeight: 32
                    text: entry.busy ? qsTr("取消")
                        : entry.waiting ? qsTr("立即更新")
                        : entry.stage === "install" ? qsTr("安装")
                        : entry.stage === "update" ? qsTr("更新到 %1").arg(entry.version)
                        : entry.stage === "app" ? qsTr("需要更新 Win顺")
                        : qsTr("已安装")
                    flat: !entry.busy && entry.stage === "installed" // done: it only leads to its card
                    highlighted: entry.installable && !entry.busy && !entry.waiting
                    // One download at a time; a newer Win顺 first.
                    enabled: entry.busy || (entry.installable ? !entry.gallery.downloading : entry.stage !== "app")
                    onClicked: {
                        if (entry.busy)
                            entry.gallery.cancel(entry.pluginId)
                        else if (entry.waiting)
                            entry.gallery.applyNow(entry.pluginId)
                        else if (entry.stage === "install")
                            entry.gallery.installAndEnable(entry.pluginId)
                        else if (entry.installable)
                            entry.gallery.install(entry.pluginId)
                        else
                            entry.showInstalled()
                    }

                    HoverHandler { id: actionHover }
                    HoverTip {
                        target: actionHover.hovered && text.length > 0 ? actionButton : null
                        dpr: entry.dpr
                        maxWidth: 320
                        text: entry.busy ? ""
                            : entry.waiting ? qsTr("%1 已经下载好。现在更新会先停一下这个插件；不点的话，下次启动 Win顺 时自动换上").arg(entry.staged)
                            : entry.stage === "installed" ? qsTr("在“已安装”里查看")
                            : entry.stage === "update" ? qsTr("现在装的是 %1").arg(entry.installedVersion)
                            : ""
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
                : entry.stage === "update" && entry.morePermissions.length > 0 ? qsTr("新版本要更多权限：%1，更新时会问你").arg(entry.morePermissions)
                : ""
            textFormat: Text.PlainText
            color: entry.problem.length > 0 ? Theme.danger : Theme.caution
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
    }
}
