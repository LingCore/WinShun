pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A plugin's 使用说明 (its README), headed as its 更新内容 are
// (PluginNotesDialog): the icon and name, under it what this is and which
// version. The text is set as a page to read (ReadingStyle) in a column as
// wide as reads well (about 44 characters a line), as tall as it is up to what
// the window leaves; on whole device pixels wherever it has scrolled to, a
// fiftieth of one lower (see SettingsWindow), and the wheel glides as the
// lists' does (ListWheelScroll).
Item {
    id: dialog
    objectName: "pluginReadmeDialog"
    required property PluginSettings plugins
    required property WindowFrame frame
    property bool open: false
    property var about: ({}) // name, icon, version (PluginSettings::readmeRequested)
    property string html
    property Item returnFocus: null
    readonly property real dpr: Screen.devicePixelRatio
    readonly property int padding: 28
    readonly property string version: about.version ?? ""
    visible: open

    function show(what, text) {
        returnFocus = Window.window.activeFocusItem
        about = what
        html = text
        wheelScroll.cancel()
        scroll.contentY = wheelScroll.minimumY
        open = true
        body.forceActiveFocus()
    }
    function close() {
        open = false
        html = ""
        returnFocus?.forceActiveFocus()
        returnFocus = null
    }
    // A key's move (PageDown, End, ↓…): the wheel's glide, to whole device
    // pixels; from where one already under way is going, as the wheel's are.
    function scrollTo(y) {
        const target = wheelScroll.bounded(y)
        wheelScroll.direction = Math.sign(target - scroll.contentY)
        wheelScroll.requestedY = target
        wheelScroll.moveTo(target)
    }
    function scrollBy(distance) {
        scrollTo((wheelScroll.running ? wheelScroll.aligned(wheelScroll.requestedY) : scroll.contentY) + distance)
    }

    Component.onCompleted: frame.addPopup(dialog)
    Connections {
        target: dialog.plugins
        function onReadmeRequested(about, html) { dialog.show(about, html) }
    }
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Escape || (event.key === Qt.Key_Q && event.modifiers === Qt.NoModifier)) {
            dialog.close(); event.accepted = true
        }
    }
    Rectangle { // dims the window; clicks stay out of it
        anchors.fill: parent
        color: Theme.tok("scrim", Qt.rgba(0, 0, 0, Theme.dark ? 0.5 : 0.3))
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; hoverEnabled: true; onWheel: (wheel) => wheel.accepted = true }
    }
    Item {
        id: card
        width: Math.min(680, dialog.width - 48)
        // A short one in a card of its own size, not on a page.
        height: Theme.snapUp(Math.min(dialog.height - 48, Math.max(200, scroll.y + body.contentHeight + 2 * scroll.bottomMargin)), dialog.dpr)
        x: Theme.snap((dialog.width - width) / 2, dialog.dpr)
        y: Theme.snap((dialog.height - height) / 2, dialog.dpr)
        Hook { anchors.fill: parent; part: Theme.menuFrame; dpr: dialog.dpr }
        MouseArea { anchors.fill: parent }

        Row { // whose it is, then what and which version
            id: header
            x: dialog.padding
            y: 22
            width: closeButton.x - x - 12
            spacing: 12

            Image { // whole device pixels, unsmoothed (see ResultRow)
                id: icon
                readonly property int pixels: Math.round(32 * dialog.dpr)
                anchors.verticalCenter: parent.verticalCenter
                visible: (dialog.about.icon ?? "").length > 0
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
                width: parent.width - (icon.visible ? icon.width + parent.spacing : 0)
                spacing: 2

                Text {
                    objectName: "pluginReadmeTitle"
                    width: parent.width
                    text: dialog.about.name ?? ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                    font.weight: Theme.fontStrong
                }
                Text {
                    width: parent.width
                    text: dialog.version.length > 0 ? qsTr("使用说明 · %1").arg(dialog.version) : qsTr("使用说明")
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }
            }
        }
        RowAction {
            id: closeButton
            objectName: "closePluginReadme"
            anchors.right: parent.right; anchors.rightMargin: 14
            y: Theme.snap(header.y + (header.height - height) / 2, dialog.dpr)
            glyph: ""; text: "Esc"; tip: qsTr("关闭（Esc / Q）"); dpr: dialog.dpr
            onClicked: dialog.close()
        }
        Rectangle { // the header's edge, once the text has gone under it
            x: dialog.padding
            y: scroll.y - height
            width: parent.width - 2 * dialog.padding
            height: Theme.edge(dialog.dpr)
            color: Theme.controlBorder
            opacity: scroll.contentY > wheelScroll.minimumY + 0.5 ? 1 : 0
            Behavior on opacity {
                enabled: SystemTheme.animations
                NumberAnimation { duration: 120 }
            }
        }
        Flickable {
            id: scroll
            objectName: "pluginReadmeScroll"
            x: dialog.padding
            y: Theme.snap(header.y + header.height + 14, dialog.dpr)
            width: parent.width - 2 * dialog.padding
            height: parent.height - y - bottomMargin
            bottomMargin: 12 // the last line clear of the card's edge
            contentWidth: width
            contentHeight: body.contentHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            ListWheelScroll {
                id: wheelScroll
                view: scroll
                animate: SystemTheme.animations
            }

            TextEdit {
                id: body
                objectName: "pluginReadmeText"
                width: scroll.width
                text: dialog.html
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                color: Theme.text
                selectionColor: Theme.textSelection
                selectedTextColor: Theme.text
                font.pixelSize: Theme.fontBody
                // The lines of a long text sit at all sorts of fractions: on
                // whole device pixels as one, a fiftieth lower, those on a half
                // pixel always round the same way and nothing shakes in a glide.
                transform: ScrollPixelAlignment {
                    view: scroll
                    viewY: dialog.y + card.y + scroll.y
                    itemY: body.y
                    pixelBias: 0.02
                }
                onLinkActivated: (link) => dialog.plugins.openReadmeLink(link)
                ReadingStyle {
                    textDocument: body.textDocument
                    pixelSize: body.font.pixelSize
                    linkColor: Theme.accent
                    quietColor: Theme.subtext
                    ruleColor: Theme.controlBorder
                    codeColor: Theme.keycap
                }
                Keys.priority: Keys.BeforeItem
                Keys.onPressed: (event) => {
                    const page = scroll.height * 0.85
                    const plain = event.modifiers === Qt.NoModifier
                    if (event.key === Qt.Key_Escape || (event.key === Qt.Key_Q && plain)) {
                        dialog.close()
                    } else if (event.key === Qt.Key_PageDown || (event.key === Qt.Key_Space && plain)) {
                        dialog.scrollBy(page)
                    } else if (event.key === Qt.Key_PageUp || (event.key === Qt.Key_Space && event.modifiers === Qt.ShiftModifier)) {
                        dialog.scrollBy(-page)
                    } else if ((event.key === Qt.Key_Down || event.key === Qt.Key_Up) && plain) {
                        dialog.scrollBy((event.key === Qt.Key_Down ? 1 : -1) * 3 * wheelScroll.lineHeight)
                    } else if ((event.key === Qt.Key_Home || event.key === Qt.Key_End)
                               && (plain || event.modifiers === Qt.ControlModifier)) {
                        dialog.scrollTo(event.key === Qt.Key_End ? wheelScroll.maximumY : wheelScroll.minimumY)
                    } else {
                        return
                    }
                    event.accepted = true
                }
            }
        }
        ListScrollBar {
            objectName: "pluginReadmeScrollBar"
            flickable: scroll
            anchors.right: parent.right
            anchors.rightMargin: 4
            y: scroll.y
            height: scroll.height
            dpr: dialog.dpr
        }
    }
}
