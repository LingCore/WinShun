import QtQuick
import WinShun

// A clickable cross-reference row inside a SettingsSection card: lets a page
// point straight to a strongly associated setting that lives on another page
// or tab, showing its live summary and jumping right to that row (scrolling
// and flashing it) without duplicating the control or polluting search results.
Item {
    id: linkRow

    property string iconName
    property string glyph
    property string title
    property string description
    property string destination // short breadcrumb on the right, e.g. "系统整合 › 文件夹菜单"
    property bool sub: false
    property bool shown: true
    // Target passed to SettingsPage.openPart / openPage when clicked.
    // Leave both empty to handle `activated` manually.
    property var targetPage: ""
    property int targetTab: -1
    property string targetPart: ""

    signal activated()

    readonly property SettingsSection section: parent && parent.section !== undefined ? parent.section : null
    readonly property bool searching: section ? section.searching : false
    readonly property real dpr: section ? section.dpr : Screen.devicePixelRatio
    readonly property real edge: Theme.edge(dpr)
    readonly property int leftInset: sub ? 36 : 18
    readonly property bool hasIcon: iconName.length > 0 || glyph.length > 0
    readonly property bool interactive: area.containsMouse || activeFocus

    function trigger(): void {
        if (section && section.column) {
            if (targetPart.length > 0 && section.column.openPart)
                section.column.openPart(targetPage, targetPart)
            else if (String(targetPage).length > 0 && section.column.openPage)
                section.column.openPage(targetPage, targetTab >= 0 ? targetTab : undefined)
        }
        activated()
    }

    width: parent ? parent.width : 0
    implicitHeight: Theme.snap(content.implicitHeight + (sub ? 20 : 24), dpr)
    visible: shown && !searching
    activeFocusOnTab: visible

    Accessible.role: Accessible.Button
    Accessible.name: destination.length > 0 ? title + " · " + destination : title
    Accessible.description: description
    Accessible.ignored: !visible
    Accessible.onPressAction: linkRow.trigger()

    Keys.onSpacePressed: linkRow.trigger()
    Keys.onReturnPressed: linkRow.trigger()
    Keys.onEnterPressed: linkRow.trigger()

    Hook { // separates rows inside a card (Theme.separator); the first row has none above it
        x: linkRow.sub ? linkRow.leftInset : linkRow.edge
        width: parent.width - x - linkRow.edge
        height: linkRow.edge
        part: linkRow.y > 0 ? Theme.separator : null
        extra: ({ card: true })
        orientation: Qt.Horizontal
        dpr: linkRow.dpr
    }

    Hook { // hover highlight across the row, inside the card's border
        anchors.fill: parent
        anchors.margins: linkRow.edge
        part: area.containsMouse ? Theme.rowHighlight : null
        hovered: true
        role: "panel"
        dpr: linkRow.dpr
    }

    Rectangle { // sub-row accent bar, matching SettingRow.sub
        visible: linkRow.sub
        x: 20
        y: Theme.snap(8, linkRow.dpr)
        width: 2
        height: Theme.snap(Math.max(0, parent.height - 2 * y), linkRow.dpr)
        radius: Theme.lineRadius
        color: Theme.tok("subRowBar", Qt.alpha(Theme.accent, Theme.dark ? 0.40 : 0.32))
    }

    Rectangle { // keyboard focus
        visible: linkRow.activeFocus
        anchors.fill: parent
        anchors.margins: linkRow.edge + 1
        radius: Theme.insetCardRadius
        color: "transparent"
        border.width: 2
        border.color: Theme.text
    }

    Item {
        id: content
        x: linkRow.leftInset
        y: linkRow.sub ? 10 : 12
        width: parent.width - linkRow.leftInset - 18
        implicitHeight: Math.max(texts.implicitHeight, trailing.implicitHeight, linkRow.hasIcon ? 20 : 0)
        height: implicitHeight

        Glyph {
            id: leadingIcon
            visible: linkRow.hasIcon
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            iconName: linkRow.iconName
            glyph: linkRow.glyph
            size: 16
            color: linkRow.interactive ? Theme.accent : Theme.subtext
        }

        Column {
            id: texts
            anchors.left: linkRow.hasIcon ? leadingIcon.right : parent.left
            anchors.leftMargin: linkRow.hasIcon ? 12 : 0
            anchors.right: trailing.left
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            Text {
                width: parent.width
                text: linkRow.title
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontBody
                wrapMode: Text.Wrap
            }

            Text {
                width: parent.width
                visible: text.length > 0
                text: linkRow.description
                textFormat: Text.PlainText
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }
        }

        Row {
            id: trailing
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            Text {
                visible: linkRow.destination.length > 0
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, Math.max(90, content.width * 0.36))
                text: linkRow.destination
                textFormat: Text.PlainText
                color: linkRow.interactive ? Theme.accent : Theme.subtext
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }

            Glyph {
                anchors.verticalCenter: parent.verticalCenter
                iconName: "caret-right"
                size: 12
                color: linkRow.interactive ? Theme.accent : Theme.subtext
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: linkRow.trigger()
    }
}
