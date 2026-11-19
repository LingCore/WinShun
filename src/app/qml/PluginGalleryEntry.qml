pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A plugin of the 插件库 (PluginGallery.list), a tile of the section's grid
// (TileFlow): its icon, name, version and who made it; what it does, three
// lines at most (the whole of it in the tip of the "i" when cut); what is
// worth reading before installing (not reviewed, its interpreter missing, a
// newer Win顺 needed, more permissions asked, why the last try failed); and
// along the bottom its 主页, 说明 and 更新内容, and on the right what it is
// here: a button that installs or updates it; 已安装, which leads to its
// card on the 已安装 tab; 自带 alone (it comes and goes with Win顺); or,
// when it asks for a newer Win顺, the button off. While it downloads: how
// far, and 取消 on the button. The grid sizes it (its row as tall as the
// tallest there); its own content sets `implicitHeight`.
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
    required property bool hasNotes // the list says what changed in its version: 更新内容 shows it (GitHub #337)
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
        if (descriptionText.truncated)
            lines.push(description, "")
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
    readonly property real pad: 14

    signal showInstalled() // 已安装

    visible: shown
    implicitHeight: Theme.snapUp(top.y + top.implicitHeight + 12 + footer.height + 12, dpr)
    // One element for UI Automation; one filtered away is skipped whole (see SettingsSection).
    Accessible.role: Accessible.Grouping
    Accessible.name: name
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
    // pixels, so that the buttons and the progress line are sharp at 125 %,
    // 150 % and 175 %.
    Column {
        id: top
        x: entry.pad
        y: Theme.snap(14, entry.dpr)
        width: parent.width - 2 * x
        spacing: Theme.snap(10, entry.dpr)

        Item { // the icon; the name and the "i"; the version and who made it
            width: parent.width
            height: Theme.snapUp(Math.max(36, names.implicitHeight), entry.dpr)

            Image { // whole device pixels, unsmoothed (see ResultRow)
                readonly property int pixels: Math.round(36 * entry.dpr)

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

            Column {
                id: names
                x: 36 + 12
                width: parent.width - x - infoDot.width - 8
                spacing: 2

                Text {
                    id: nameText
                    width: parent.width
                    height: Theme.snapUp(implicitHeight, entry.dpr)
                    text: entry.name
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Theme.fontStrong
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    height: Theme.snapUp(implicitHeight, entry.dpr)
                    text: [entry.version, entry.author].filter(t => t.length > 0).join(" · ")
                    textFormat: Text.PlainText
                    color: Theme.faint
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
            }
            InfoDot { // who made it, its keywords, what it may do; the whole description when it is cut
                id: infoDot
                objectName: "pluginInfo-" + entry.pluginId
                anchors.right: parent.right
                y: Theme.snap((nameText.height - height) / 2, entry.dpr)
                dpr: entry.dpr
                caution: entry.anyRisky
                text: entry.infoText
            }
        }

        Text { // what it does
            id: descriptionText
            width: parent.width
            height: Theme.snapUp(implicitHeight, entry.dpr)
            visible: entry.description.length > 0
            text: entry.description
            textFormat: Text.PlainText
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            lineHeight: 1.15
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
            maximumLineCount: 2
            elide: Text.ElideRight
        }

        Text { // why the last try did not install it, or why it cannot be installed yet
            visible: text.length > 0 && !entry.busy
            width: parent.width
            text: entry.problem.length > 0 ? entry.problem
                : entry.stage === "app" ? qsTr("要 Win顺 %1 或更新的版本，可以在“常规”里检查更新").arg(entry.minWinShun)
                : entry.stage === "update" && entry.morePermissions.length > 0 ? qsTr("新版本要更多权限：%1，更新时会问你").arg(entry.morePermissions)
                : ""
            textFormat: Text.PlainText
            color: entry.problem.length > 0 ? Theme.danger : Theme.caution
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
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
    }

    // Along the bottom, whatever the words above: 主页, 说明, 更新内容 on the
    // left (as faint as the version until the pointer or the keyboard is on
    // one: not to be read before the name), what it is here on the right.
    Item {
        id: footer
        x: entry.pad
        y: Theme.snap(entry.height - 12 - height, entry.dpr)
        width: parent.width - 2 * x
        height: 32

        Row {
            x: -7 // the first icon under the words above: a flat button's padding left out
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            FlatButton { // 主页 (GitHub #248): its own page, to look at before installing
                id: homepageButton
                dpr: entry.dpr
                objectName: "pluginHomepage-" + entry.pluginId
                visible: entry.homepage.length > 0
                implicitWidth: 28
                implicitHeight: 28
                flat: true
                opacity: homepageHover.hovered || activeFocus ? 1 : 0.55
                glyph: "" // Link, as 主页 in the installed card's menu
                Accessible.name: qsTr("主页")
                onClicked: entry.gallery.openHomepage(entry.pluginId)
                HoverHandler { id: homepageHover }
                HoverTip { // where it leads, before the click: the list's words are not Win顺's own
                    target: homepageHover.hovered ? homepageButton : null
                    dpr: entry.dpr
                    text: qsTr("主页：%1").arg(entry.homepageHost)
                }
            }
            FlatButton { // 说明 (GitHub #304): its README, read here before installing
                id: readmeButton
                dpr: entry.dpr
                objectName: "pluginReadme-" + entry.pluginId
                visible: entry.hasReadme
                implicitWidth: 28
                implicitHeight: 28
                flat: true
                enabled: !entry.readmeBusy
                opacity: readmeHover.hovered || activeFocus ? 1 : 0.55
                glyph: "" // Document, as 说明 in the installed card's menu
                Accessible.name: qsTr("说明")
                onClicked: entry.gallery.openReadme(entry.pluginId)
                HoverHandler { id: readmeHover }
                HoverTip {
                    target: readmeHover.hovered ? readmeButton : null
                    dpr: entry.dpr
                    text: entry.readmeBusy ? qsTr("正在取说明…") : qsTr("说明")
                }
            }
            FlatButton { // 更新内容 (GitHub #337): what changed in the list's version, before updating to it
                id: notesButton
                dpr: entry.dpr
                objectName: "pluginNotes-" + entry.pluginId
                visible: entry.hasNotes
                implicitWidth: 28
                implicitHeight: 28
                flat: true
                opacity: notesHover.hovered || activeFocus ? 1 : 0.55
                glyph: "" // History: what came with each version
                Accessible.name: qsTr("更新内容")
                onClicked: entry.gallery.openNotes(entry.pluginId)
                HoverHandler { id: notesHover }
                HoverTip {
                    target: notesHover.hovered ? notesButton : null
                    dpr: entry.dpr
                    text: qsTr("%1 更新内容").arg(entry.version)
                }
            }
        }

        Text { // it comes and goes with Win顺: nothing to press
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
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
            anchors.right: parent.right
            anchors.rightMargin: flat ? -12 : 0 // 已安装's words end where a button's edge would
            anchors.verticalCenter: parent.verticalCenter
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
