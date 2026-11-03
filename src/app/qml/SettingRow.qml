import QtQuick
import WinShun

// One option: title and explanation on the left, its control on the right,
// optional extra content (lists, inputs) underneath.
//
// While the settings are searched (SettingsSearch, in the section's page
// column), a row shows only when the words typed fit it, with them marked,
// and reports itself so that the categories can count it.
Item {
    id: row

    property string title
    property string description
    property bool sub: false
    property string tip: ""
    // Long segmented choices go below the explanation when they would squeeze it.
    property bool stackControl: trailingRow.implicitWidth > content.width * 0.66
    default property alias trailing: trailingRow.data
    property alias body: bodyColumn.data
    property bool bodyShown: true // false: no room is kept for a body with nothing to show now
    // Whether the option is there now: false while a switch above keeps it
    // away, or on a Windows without it. Not `visible`, which the row decides
    // while searching.
    property bool shown: true
    property var keywords: [] // other words people look for it by
    property var values: [] // what the user filled in, so that it can be found by that too
    // Rows that show only while this one is on: found while it is off, they
    // bring this one up instead.
    property list<Item> unlocks
    // Let nested settings reveal the matching group before search is cleared.
    signal revealRequested()

    readonly property SettingsSection section: parent && parent.section !== undefined ? parent.section : null
    readonly property SettingsSearch search: section ? section.search : null
    readonly property bool searching: search !== null && search.active
    // The labels on the controls: the choices of a ScopeTabs, the words on
    // buttons.
    readonly property var options: {
        const labels = []
        for (let i = 0; i < trailingRow.children.length; ++i) {
            const control = trailingRow.children[i]
            if (Array.isArray(control.labels))
                labels.push(...control.labels)
            else if (control.glyph !== undefined && typeof control.text === "string")
                labels.push(control.text)
        }
        return labels
    }
    // Its page's title and other words for the page, its section's title.
    readonly property var context: {
        if (!section)
            return []
        const page = section.pageInfo
        const words = page ? [page.title].concat(section.pageKeywords) : []
        if (section.title.length > 0)
            words.push(section.title)
        return words
    }
    // How the words typed fit (SettingsSearch.match); undefined when they don't.
    readonly property var hit: searching
                               ? search.match(search.revision, {
                                     title: row.title,
                                     description: row.tip.length > 0 ? (row.description.length > 0 ? row.description + " · " + row.tip : row.tip) : row.description,
                                     keywords: row.keywords,
                                     options: row.options,
                                     context: row.context,
                                     values: row.values
                                 })
                               : undefined
    readonly property bool found: !!hit
    // The titles of the rows this one keeps away that the words fit.
    readonly property var unlocked: {
        const titles = []
        if (!searching)
            return titles
        for (let i = 0; i < unlocks.length; ++i) {
            const other = unlocks[i]
            if (other && other.found && !other.shown)
                titles.push(other.title)
        }
        return titles
    }
    // How it ranks while searching; -1: not on show.
    readonly property int score: !searching || !shown ? -1 : found ? hit.score : unlocked.length > 0 ? 1 : -1

    // Whether `value` (one of `values`) has a word typed in it.
    function valueFound(value) { return found && hit.values.indexOf(value) >= 0 }

    // Where it stands among the rows: by section, then within its section.
    function orderKey() {
        // A list property is no array: Array.prototype.indexOf finds nothing in it.
        const indexIn = (list, item) => {
            for (let i = 0; i < list.length; ++i) {
                if (list[i] === item)
                    return i
            }
            return -1
        }
        if (!section)
            return 0
        return indexIn(section.parent.children, section) * 1000 + indexIn(row.parent.children, row)
    }

    // Gives the keyboard to the switch on the right, if there is one: Space
    // then turns it on or off, and once more back. Not to a button, where a
    // stray Enter would clear or reset something, nor to a hotkey recorder,
    // which starts recording once it has the focus.
    function focusControl() {
        for (let i = 0; i < trailingRow.children.length; ++i) {
            const control = trailingRow.children[i]
            if (control.visible && control.enabled && control.activeFocusOnTab && control.checked !== undefined) {
                control.forceActiveFocus(Qt.TabFocusReason)
                return true
            }
        }
        return false
    }

    // Draws the eye to the row once it has been gone to.
    function flash() {
        if (SystemTheme.animations) {
            flashAnimation.restart()
        } else {
            flashMark.opacity = 0.14
            flashTimer.restart()
        }
    }

    onScoreChanged: if (search) search.report(row, section, section.page, score)

    width: parent ? parent.width : 0
    // The section's (the window's, SettingsWindow.dpr), not a Screen attached
    // object per item: each one is work on every move to another monitor
    // (GitHub #111; docs/pitfalls.md).
    readonly property real dpr: section ? section.dpr : Screen.devicePixelRatio
    // On whole device pixels, so that the rows below, and the card's bottom edge, stay on them.
    readonly property real edge: Theme.edge(row.dpr)
    readonly property int leftInset: row.sub ? 36 : 18
    implicitHeight: Theme.snap(content.implicitHeight + (row.sub ? 22 : 28), row.dpr)
    visible: shown && (!searching || score >= 0)

    Rectangle { // separates rows inside a card
        visible: row.y > 0
        x: row.sub ? row.leftInset : row.edge
        width: parent.width - x - row.edge
        height: row.edge
        color: Theme.cardBorder
    }

    HoverHandler { id: rowHover }
    readonly property bool hovered: rowHover.hovered // for controls shown only then

    Rectangle {
        visible: rowHover.hovered && !(row.searching && row.search.current === row)
        anchors.fill: parent
        anchors.margins: row.edge
        radius: Theme.insetCardRadius
        color: Theme.hover
    }

    Rectangle {
        visible: row.sub
        x: 20
        y: Theme.snap(8, row.dpr)
        width: 2
        height: Theme.snap(Math.max(0, parent.height - 2 * y), row.dpr)
        radius: Theme.lineRadius
        color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, Theme.dark ? 0.40 : 0.32)
    }

    Rectangle { // the option Enter goes to, while searching
        visible: row.searching && row.search.current === row
        anchors.fill: parent
        anchors.margins: row.edge
        radius: Theme.insetCardRadius
        color: Theme.selection

        Rectangle {
            x: 3
            y: content.y + texts.y + (titleText.height - height) / 2 // by the title
            width: 3
            height: 16
            radius: Theme.smallRadius
            color: Theme.accent
        }
    }

    Rectangle { // see flash()
        id: flashMark
        anchors.fill: parent
        anchors.margins: row.edge
        radius: Theme.insetCardRadius
        color: Theme.accent
        opacity: 0
        visible: opacity > 0

        SequentialAnimation {
            id: flashAnimation
            NumberAnimation { target: flashMark; property: "opacity"; to: 0.16; duration: 160; easing.type: Easing.OutQuad }
            NumberAnimation { target: flashMark; property: "opacity"; to: 0.04; duration: 360; easing.type: Easing.InOutQuad }
            NumberAnimation { target: flashMark; property: "opacity"; to: 0.16; duration: 200; easing.type: Easing.InOutQuad }
            NumberAnimation { target: flashMark; property: "opacity"; to: 0; duration: 700; easing.type: Easing.InQuad }
        }
        Timer {
            id: flashTimer
            interval: 1600
            onTriggered: flashMark.opacity = 0
        }
    }

    Column {
        id: content
        x: row.leftInset
        y: row.sub ? 11 : 14
        width: parent.width - row.leftInset - 18
        spacing: 12

        Item {
            id: header
            width: parent.width
            height: Theme.snapUp(row.stackControl
                    ? texts.implicitHeight + (trailingRow.implicitHeight > 0 ? trailingRow.implicitHeight + 12 : 0)
                    : Math.max(texts.implicitHeight, trailingRow.implicitHeight), row.dpr)

            Column {
                id: texts
                anchors.left: parent.left
                anchors.right: row.stackControl ? parent.right : trailingRow.left
                anchors.rightMargin: row.stackControl || trailingRow.width === 0 ? 0 : Theme.snap(Math.max(24, 88 - trailingRow.width), row.dpr)
                y: row.stackControl ? 0 : Theme.snap((parent.height - height) / 2, row.dpr)
                spacing: 3

                Row {
                    id: titleRow
                    width: parent.width
                    spacing: 6

                    Text {
                        id: titleText
                        width: Math.min(implicitWidth, parent.width - (tipBadge.visible ? tipBadge.width + parent.spacing : 0))
                        text: row.found ? row.hit.title : row.title
                        textFormat: row.found ? Text.StyledText : Text.AutoText
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }

                    Item {
                        id: tipBadge
                        visible: row.tip.length > 0 && !row.searching
                        width: 20
                        height: 20
                        anchors.verticalCenter: titleText.lineCount <= 1 ? titleText.verticalCenter : undefined
                        y: titleText.lineCount > 1 ? 2 : 0

                        Glyph {
                            anchors.centerIn: parent
                            glyph: "\uE946"
                            size: 13
                            color: tipArea.containsMouse ? Theme.accent : Theme.subtext
                        }

                        MouseArea {
                            id: tipArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                        }

                        HoverTip {
                            target: tipArea.containsMouse ? tipBadge : null
                            text: row.tip
                            delay: 150
                        }
                    }
                }

                Text {
                    id: descriptionText
                    width: parent.width
                    visible: text.length > 0
                    text: row.found ? row.hit.description : row.description
                    textFormat: row.found ? Text.StyledText : Text.AutoText
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                    lineHeight: 1.15
                }
                Text { // found among the rows this one keeps away
                    width: parent.width
                    visible: row.unlocked.length > 0
                    text: qsTr("打开后可以设置：%1").arg(row.unlocked.join(qsTr("、")))
                    color: Theme.accent
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
            }

            Row {
                id: trailingRow
                // Placed by x, not by switching anchors: for a moment both
                // anchors would hold, which sets the Row's width for good
                // (then wider than its controls, squeezing the texts).
                x: row.stackControl ? 0 : parent.width - width
                y: row.stackControl ? texts.implicitHeight + 12 : Theme.snap((parent.height - height) / 2, row.dpr)
                spacing: 8
            }
        }

        Column {
            id: bodyColumn
            width: parent.width
            spacing: 10
            visible: row.bodyShown && children.length > 0
        }
    }
}
