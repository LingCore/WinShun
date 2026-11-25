pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// 更新内容 of a 插件库 entry (GitHub #337), laid out as Win顺's own update
// dialog (UpdateDialog): the plugin's icon and name, under it which version
// this is and the one installed; what changed on a plate of its own; then
// what can be done: 更新到 / 安装 right here, after reading, or close.
Item {
    id: dialog
    objectName: "pluginNotesDialog"

    required property PluginSettings plugins
    required property WindowFrame frame
    property bool open: false
    property var about: ({}) // id, name, icon, version, installedVersion, stage, staged (PluginGallery::openNotes)
    property string html
    property Item returnFocus: null
    readonly property real dpr: Screen.devicePixelRatio
    readonly property string stage: about.stage ?? ""
    readonly property bool waiting: stage === "update" && (about.staged ?? "").length > 0
    readonly property bool canAct: stage === "install" || stage === "update"

    function show(what, text) {
        returnFocus = Window.window.activeFocusItem
        about = what
        html = text
        scroll.contentY = 0
        open = true
        body.forceActiveFocus()
    }
    function close() {
        open = false
        html = ""
        returnFocus?.forceActiveFocus()
        returnFocus = null
    }
    function act() {
        const gallery = plugins.gallery
        if (!canAct || gallery.downloading)
            return
        if (waiting)
            gallery.applyNow(about.id)
        else if (stage === "install")
            gallery.installAndEnable(about.id)
        else
            gallery.install(about.id)
        close() // how it goes shows on the entry
    }

    visible: open
    Component.onCompleted: frame.addPopup(dialog)

    Connections {
        target: dialog.plugins
        function onNotesRequested(about, html) { dialog.show(about, html) }
    }

    Rectangle { // dims the window; clicks stay out of it
        anchors.fill: parent
        color: Theme.tok("scrim", Qt.rgba(0, 0, 0, Theme.dark ? 0.5 : 0.3))
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; hoverEnabled: true; onWheel: (wheel) => wheel.accepted = true }
    }

    Item {
        id: card
        width: Math.min(560, dialog.width - 48)
        height: Theme.snapUp(content.implicitHeight + 48, dialog.dpr)
        x: Theme.snap((dialog.width - width) / 2, dialog.dpr)
        y: Theme.snap((dialog.height - height) / 2, dialog.dpr)

        Hook { anchors.fill: parent; part: Theme.menuFrame; dpr: dialog.dpr }
        MouseArea { anchors.fill: parent }

        Column {
            id: content
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 16

            Row { // whose notes, and which version
                id: header
                width: parent.width
                spacing: 14

                Image { // whole device pixels, unsmoothed (see ResultRow)
                    id: icon
                    readonly property int pixels: Math.round(40 * dialog.dpr)
                    anchors.verticalCenter: parent.verticalCenter
                    width: pixels / dialog.dpr
                    height: pixels / dialog.dpr
                    source: dialog.about.icon ?? ""
                    sourceSize.width: pixels / dialog.dpr
                    sourceSize.height: pixels / dialog.dpr
                    smooth: false
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                    Accessible.ignored: true
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - icon.width - parent.spacing
                    spacing: 4

                    Text {
                        width: parent.width
                        text: dialog.about.name ?? ""
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text
                        font.pixelSize: Theme.fontTitle
                        font.weight: Theme.fontStrong
                    }
                    Text {
                        objectName: "pluginNotesVersion"
                        width: parent.width
                        text: dialog.waiting ? qsTr("%1 的更新内容 · 已经下载好，下次启动 Win顺 时换上").arg(dialog.about.version)
                            : dialog.stage === "update" ? qsTr("%1 的更新内容 · 现在装的是 %2").arg(dialog.about.version).arg(dialog.about.installedVersion)
                            : qsTr("%1 的更新内容").arg(dialog.about.version ?? "")
                        textFormat: Text.PlainText
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
            }

            Rectangle { // what changed, on a plate of its own, as Win顺's own update notes
                id: notesBox
                // As tall as the notes, up to what the window leaves.
                readonly property real room: dialog.height - 48 - 48 - header.height - footer.height - 2 * content.spacing
                width: parent.width
                height: Theme.snapUp(Math.min(body.contentHeight + 24, Math.max(96, room)), dialog.dpr)
                radius: Theme.cardRadius
                color: Theme.hover
                border.width: 1
                border.color: Theme.cardBorder

                Flickable {
                    id: scroll
                    objectName: "pluginNotesScroll"
                    anchors.fill: parent
                    anchors.margins: 12
                    contentWidth: width
                    contentHeight: body.contentHeight
                    clip: contentHeight > height
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.VerticalFlick
                    ListWheelScroll {
                        view: scroll
                        animate: SystemTheme.animations
                    }

                    TextEdit {
                        id: body
                        objectName: "pluginNotesText"
                        width: scroll.width // the scroll bar keeps to the plate's margin
                        text: dialog.html
                        textFormat: TextEdit.RichText
                        wrapMode: TextEdit.Wrap
                        readOnly: true
                        selectByMouse: true
                        persistentSelection: true
                        color: Theme.text
                        selectionColor: Theme.textSelection
                        selectedTextColor: Theme.text
                        font.pixelSize: Theme.fontCaption
                        // On whole device pixels wherever it has scrolled to, a
                        // fiftieth of one lower, as the 使用说明 (PluginReadmeDialog).
                        transform: ScrollPixelAlignment {
                            view: scroll
                            viewY: dialog.y + card.y + content.y + notesBox.y + scroll.y
                            itemY: body.y
                            pixelBias: 0.02
                        }
                        onLinkActivated: (link) => dialog.plugins.openReadmeLink(link)
                        ReadingStyle { // set as a page to read; all of it drawn
                            textDocument: body.textDocument
                            pixelSize: body.font.pixelSize
                            linkColor: Theme.accent
                            quietColor: Theme.subtext
                            ruleColor: Theme.controlBorder
                            codeColor: Theme.keycap
                        }
                        Keys.priority: Keys.BeforeItem
                        Keys.onPressed: (event) => {
                            if (event.key === Qt.Key_Escape || (event.key === Qt.Key_Q && event.modifiers === Qt.NoModifier)) {
                                dialog.close(); event.accepted = true
                            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                if (dialog.canAct) dialog.act(); else dialog.close()
                                event.accepted = true
                            } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
                                scroll.contentY = Math.max(0, Math.min(scroll.contentHeight - scroll.height,
                                    scroll.contentY + (event.key === Qt.Key_PageDown ? 1 : -1) * scroll.height * 0.85))
                                event.accepted = true
                            }
                        }
                    }
                }
                ListScrollBar {
                    visible: scroll.contentHeight > scroll.height
                    flickable: scroll
                    anchors.right: parent.right
                    anchors.rightMargin: 2
                    y: scroll.y
                    height: scroll.height
                    dpr: dialog.dpr
                }
            }

            Row {
                id: footer
                anchors.right: parent.right
                spacing: 8

                FlatButton {
                    objectName: "closePluginNotes"
                    dpr: dialog.dpr
                    role: "dialog"
                    highlighted: !dialog.canAct
                    text: qsTr("关闭")
                    onClicked: dialog.close()
                }
                FlatButton {
                    objectName: "pluginNotesAction"
                    dpr: dialog.dpr
                    role: "dialog"
                    visible: dialog.canAct
                    highlighted: true
                    enabled: !dialog.plugins.gallery.downloading // one download at a time
                    text: dialog.waiting ? qsTr("立即更新")
                        : dialog.stage === "install" ? qsTr("安装")
                        : qsTr("更新到 %1").arg(dialog.about.version ?? "")
                    onClicked: dialog.act()
                }
            }
        }
    }
}
