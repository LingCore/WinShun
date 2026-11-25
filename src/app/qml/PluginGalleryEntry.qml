pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A plugin of the 插件库 (PluginGallery.list), a tile of the section's grid
// (TileFlow): its icon, name, version and who made it (plus its installed
// state); what it does, three lines at most (the whole of it in the tip of
// the "i" when cut); what is worth reading before installing (not reviewed,
// its interpreter missing, a newer Win顺 needed, more permissions asked, why
// the last try failed); and along the bottom its 主页, 说明 and 更新内容, and
// on the right its action: 安装 / 更新到 / 立即更新 / 需要更新 Win顺, plus
// 设置 for any installed plugin (opening its own settings window when active,
// or its PluginCard right inside the 插件库). Clicking an installed card also
// opens its PluginCard. While it downloads: how far, and 取消 on the button.
// The grid sizes it (its row as tall as the tallest there); its own content
// sets `implicitHeight`.
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
    required property bool installed // installed locally (user copy or bundled)
    required property bool pluginEnabled // the installed copy is currently enabled
    required property bool settingsWindow // the installed copy opens its own standalone settings window
    required property string morePermissions // what the list's version asks for besides, named
    required property bool hidden // put away as 不感兴趣 (GitHub #442): on show while typed for, or in the view of those

    property real dpr: 0 // the window's, passed by a host that has it (GitHub #111)
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio
    readonly property bool installable: stage === "install" || stage === "update"
    readonly property bool hasPrimaryAction: busy || stage === "install" || stage === "update" || stage === "app"
    readonly property bool hasSettingsAction: installed && !busy
    readonly property string statusLabel: busy ? ""
        : installed && !pluginEnabled ? qsTr("已停用")
        : stage === "bundled" ? qsTr("自带")
        : stage === "installed" || (stage === "app" && installed) ? qsTr("已安装")
        : hidden ? qsTr("不感兴趣")
        : ""
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
        if ((stage === "update" || stage === "installed" || stage === "app") && installedVersion.length > 0)
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

    signal showInstalled() // open its PluginCard right in the 插件库

    visible: shown
    implicitHeight: Theme.snapUp(top.y + top.implicitHeight + 12 + footer.height + 12, pixelRatio)
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
        dpr: entry.pixelRatio
    }
    Rectangle { // under the pointer: an installed card opens its details on a click
        visible: cardArea.enabled && cardArea.containsMouse
        anchors.fill: parent
        anchors.margins: Theme.edge(entry.pixelRatio)
        radius: Math.max(0, Theme.controlRadius - Theme.edge(entry.pixelRatio))
        color: Theme.hover
    }
    MouseArea { // under the controls: a click anywhere else opens its PluginCard when installed
        id: cardArea
        anchors.fill: parent
        enabled: entry.installed && !entry.busy
        hoverEnabled: enabled
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: entry.showInstalled()
    }

    // Offsets, gaps and the heights of the lines of text on whole device
    // pixels, so that the buttons and the progress line are sharp at 125 %,
    // 150 % and 175 %.
    Column {
        id: top
        x: entry.pad
        y: Theme.snap(14, entry.pixelRatio)
        width: parent.width - 2 * x
        spacing: Theme.snap(10, entry.pixelRatio)

        Item { // the icon; the name and the "i"; the version and who made it
            width: parent.width
            height: Theme.snapUp(Math.max(36, names.implicitHeight), entry.pixelRatio)

            Image { // whole device pixels, unsmoothed (see ResultRow)
                readonly property int pixels: Math.round(36 * entry.pixelRatio)

                x: 1 / 64
                y: 1 / 64
                width: pixels / entry.pixelRatio
                height: pixels / entry.pixelRatio
                source: entry.icon
                sourceSize.width: pixels / entry.pixelRatio
                sourceSize.height: pixels / entry.pixelRatio
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
                    height: Theme.snapUp(implicitHeight, entry.pixelRatio)
                    text: entry.name
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Theme.fontStrong
                    elide: Text.ElideRight
                }
                Row {
                    width: parent.width
                    height: Theme.snapUp(Math.max(metaText.implicitHeight, statusText.visible ? statusText.implicitHeight : 0), entry.pixelRatio)

                    Text {
                        id: metaText
                        width: Math.min(implicitWidth, parent.width - (statusText.visible ? statusText.width : 0))
                        height: Theme.snapUp(implicitHeight, entry.pixelRatio)
                        text: [entry.version, entry.author].filter(t => t.length > 0).join(" · ")
                        textFormat: Text.PlainText
                        color: Theme.faint
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                    }
                    Text {
                        id: statusText
                        objectName: "pluginStatus-" + entry.pluginId
                        visible: entry.statusLabel.length > 0
                        height: Theme.snapUp(implicitHeight, entry.pixelRatio)
                        text: (metaText.text.length > 0 ? " · " : "") + entry.statusLabel
                        textFormat: Text.PlainText
                        color: entry.installed && !entry.pluginEnabled ? Theme.caution : Theme.faint
                        font.pixelSize: Theme.fontCaption
                    }
                }
            }
            InfoDot { // who made it, its keywords, what it may do; the whole description when it is cut
                id: infoDot
                objectName: "pluginInfo-" + entry.pluginId
                anchors.right: parent.right
                y: Theme.snap((nameText.height - height) / 2, entry.pixelRatio)
                dpr: entry.pixelRatio
                caution: entry.anyRisky
                text: entry.infoText
            }
        }

        Text { // what it does
            id: descriptionText
            width: parent.width
            height: Theme.snapUp(implicitHeight, entry.pixelRatio)
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
            spacing: Theme.snap(6, entry.pixelRatio)

            Text {
                height: Theme.snapUp(implicitHeight, entry.pixelRatio)
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
                    width: Theme.snap(parent.width * entry.progress, entry.pixelRatio)
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
    // one: not to be read before the name), 设置 and the install/update action
    // on the right; stacked onto two rows when a narrow tile would crowd them.
    Item {
        id: footer

        readonly property bool hasLinks: entry.homepage.length > 0 || entry.hasReadme || entry.hasNotes || hideButton.visible
        readonly property bool hasActions: entry.hasSettingsAction || entry.hasPrimaryAction
        readonly property bool stacked: hasLinks && hasActions
            && (linksRow.x + linksRow.implicitWidth + 8 + actionsRow.implicitWidth > width)

        x: entry.pad
        y: Theme.snap(entry.height - 12 - height, entry.pixelRatio)
        width: parent.width - 2 * x
        height: stacked ? 64 : 32

        Row {
            id: linksRow
            x: -7 // the first icon under the words above: a flat button's padding left out
            y: footer.stacked ? 0 : Theme.snap((32 - height) / 2, entry.pixelRatio)
            spacing: 2

            FlatButton { // 主页 (GitHub #248): its own page, to look at before installing
                id: homepageButton
                dpr: entry.pixelRatio
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
                    dpr: entry.pixelRatio
                    text: qsTr("主页：%1").arg(entry.homepageHost)
                }
            }
            FlatButton { // 说明 (GitHub #304): its README, read here before installing
                id: readmeButton
                dpr: entry.pixelRatio
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
                    dpr: entry.pixelRatio
                    text: entry.readmeBusy ? qsTr("正在取说明…") : qsTr("说明")
                }
            }
            FlatButton { // 更新内容 (GitHub #337): what changed in the list's version, before updating to it
                id: notesButton
                dpr: entry.pixelRatio
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
                    dpr: entry.pixelRatio
                    text: qsTr("%1 更新内容").arg(entry.version)
                }
            }
            FlatButton { // 不感兴趣 (GitHub #442): out of the list, one not installed; again: back in it
                id: hideButton
                dpr: entry.pixelRatio
                objectName: "pluginHide-" + entry.pluginId
                visible: !entry.installed && !entry.busy
                implicitWidth: 28
                implicitHeight: 28
                flat: true
                opacity: hideHover.hovered || activeFocus ? 1 : 0.55
                glyph: entry.hidden ? "ph:eye" : "ph:eye-slash"
                Accessible.name: entry.hidden ? qsTr("恢复显示") : qsTr("不感兴趣")
                onClicked: entry.gallery.setHidden(entry.pluginId, !entry.hidden)
                HoverHandler { id: hideHover }
                HoverTip {
                    target: hideHover.hovered ? hideButton : null
                    dpr: entry.pixelRatio
                    maxWidth: 320
                    text: entry.hidden ? qsTr("恢复显示在插件库里") : qsTr("不感兴趣：不再显示在插件库里，以后可以找回")
                }
            }
        }

        Row {
            id: actionsRow
            anchors.right: parent.right
            y: footer.stacked ? 32 : 0
            spacing: 6

            FlatButton { // 设置: opens its own settings window when active, or its PluginCard right here
                id: settingsButton
                dpr: entry.pixelRatio
                objectName: "gallerySettings-" + entry.pluginId
                visible: entry.hasSettingsAction
                implicitHeight: 32
                glyph: "" // Settings (Phosphor gear)
                text: entry.hasPrimaryAction ? "" : qsTr("设置")
                Accessible.name: qsTr("%1设置").arg(entry.name)
                onClicked: {
                    if (entry.settingsWindow)
                        entry.gallery.openSettings(entry.pluginId)
                    else
                        entry.showInstalled()
                }

                HoverHandler { id: settingsHover }
                HoverTip {
                    target: settingsHover.hovered ? settingsButton : null
                    dpr: entry.pixelRatio
                    maxWidth: 320
                    text: entry.settingsWindow
                        ? qsTr("打开设置窗口；点卡片可管理插件")
                        : (entry.hasPrimaryAction ? qsTr("设置") : qsTr("设置与管理"))
                }
            }
            FlatButton {
                id: actionButton
                dpr: entry.pixelRatio
                objectName: "installPlugin-" + entry.pluginId
                visible: entry.hasPrimaryAction
                implicitHeight: 32
                text: entry.busy ? qsTr("取消")
                    : entry.waiting ? qsTr("立即更新")
                    : entry.stage === "install" ? qsTr("安装")
                    : entry.stage === "update" ? qsTr("更新到 %1").arg(entry.version)
                    : qsTr("需要更新 Win顺")
                highlighted: entry.installable && !entry.busy && !entry.waiting
                // One download at a time; a newer Win顺 first.
                enabled: entry.busy || (entry.installable ? !entry.gallery.downloading : false)
                onClicked: {
                    if (entry.busy)
                        entry.gallery.cancel(entry.pluginId)
                    else if (entry.waiting)
                        entry.gallery.applyNow(entry.pluginId)
                    else if (entry.stage === "install")
                        entry.gallery.installAndEnable(entry.pluginId)
                    else if (entry.installable)
                        entry.gallery.install(entry.pluginId)
                }

                HoverHandler { id: actionHover }
                HoverTip {
                    target: actionHover.hovered && text.length > 0 ? actionButton : null
                    dpr: entry.pixelRatio
                    maxWidth: 320
                    text: entry.busy ? ""
                        : entry.waiting ? qsTr("%1 已经下载好。现在更新会先停一下这个插件；不点的话，下次启动 Win顺 时自动换上").arg(entry.staged)
                        : entry.stage === "update" ? qsTr("现在装的是 %1").arg(entry.installedVersion)
                        : ""
                }
            }
        }
    }
}
