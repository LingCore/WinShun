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
    property list<string> keywords // other words people look for it by
    property list<string> values // what the user filled in, so that it can be found by that too
    // Rows that show only while this one is on: found while it is off, they
    // bring this one up instead.
    property list<Item> unlocks
    // Let nested settings reveal the matching group before search is cleared.
    signal revealRequested()

    // Its title, controls and body one element for UI Automation (see SettingsSection).
    Accessible.role: Accessible.Grouping
    Accessible.name: title

    readonly property SettingsSection section: parent && parent.section !== undefined ? parent.section : null
    readonly property SettingsSearch search: section ? section.search : null
    readonly property bool searching: search !== null && search.active
    // The labels on the controls: the choices of a ScopeTabs, the words on
    // buttons. Read only while searching ([] otherwise: not worked out for
    // every row as it is made).
    readonly property var options: {
        if (!searching)
            return []
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
    // Its page's title and other words for the page, its section's title (only while searching).
    readonly property var context: {
        if (!section || !searching)
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
    function valueFound(value: string): bool { return found && hit.values.indexOf(value) >= 0 }

    // Where it stands among the rows: by section, then within its section.
    function orderKey(): int {
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
        const made = needMarks()
        if (made)
            made.flash()
    }

    // The parts few rows ever show, made the first time one is needed and
    // then kept; by their files' names, as an inline Component would be one
    // more object in every row (see Glyph.qml).
    //   marks: the search's highlight and the flash (SettingRowMarks)
    //   tipBadge: the "i" after the title of a row with a tip (SettingRowTip)
    //   unlockedLine: what the search found among the rows this one keeps away
    // What makes one must not read `dpr` (nor anything else that asks the
    // section) from a handler that runs while the row is being made, `tip`
    // set where the row is declared, say: the row has no parent yet then, so
    // no section, and its `dpr` would be worked out from Screen, one attached
    // object per row (docs/pitfalls: QQuickScreenAttached).
    property Item marks: null
    property Item tipBadge: null
    property Item unlockedLine: null
    function madePart(file: string, parent: Item, properties: var): Item {
        const part = Qt.createComponent(file)
        const made = part.createObject(parent, properties) as Item
        if (!made)
            console.warn("SettingRow:", file, "could not be made:", part.errorString())
        return made
    }
    function needMarks(): Item {
        if (!marks)
            marks = madePart("SettingRowMarks.qml", row, { row: row })
        return marks
    }
    onTipChanged: {
        if (tip.length > 0 && !tipBadge)
            tipBadge = madePart("SettingRowTip.qml", titleRow, { row: row, title: titleText })
    }
    onUnlockedChanged: {
        if (unlocked.length === 0 || unlockedLine)
            return
        const line = Qt.createComponent("QtQuick", "Text").createObject(texts, {
            width: Qt.binding(() => texts.width),
            visible: Qt.binding(() => row.unlocked.length > 0),
            text: Qt.binding(() => qsTr("打开后可以设置：%1").arg(row.unlocked.join(qsTr("、")))),
            color: Qt.binding(() => Theme.accent),
            wrapMode: Text.Wrap
        })
        if (line)
            line.font.pixelSize = Qt.binding(() => Theme.fontCaption)
        unlockedLine = line as Item
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

    Hook { // separates rows inside a card (Theme.separator); the first row has none above it
        x: row.sub ? row.leftInset : row.edge
        width: parent.width - x - row.edge
        height: row.edge
        part: row.y > 0 ? Theme.separator : null
        extra: ({ card: true })
        orientation: Qt.Horizontal
        dpr: row.dpr
    }

    HoverHandler { id: rowHover }
    readonly property bool hovered: rowHover.hovered // for controls shown only then

    // The option Enter goes to while searching: the style's highlight
    // (Theme.rowHighlight), made on the first need only (marks).
    readonly property bool current: searching && search.current === row
    onCurrentChanged: if (current) needMarks()

    Rectangle {
        visible: row.sub
        z: 1 // over the marks, as the content is
        x: 20
        y: Theme.snap(8, row.dpr)
        width: 2
        height: Theme.snap(Math.max(0, parent.height - 2 * y), row.dpr)
        radius: Theme.lineRadius
        color: Theme.tok("subRowBar", Qt.alpha(Theme.accent, Theme.dark ? 0.40 : 0.32)) // a style may keep it neutral
    }

    Column {
        id: content
        z: 1 // over the marks (SettingRowMarks), which are made later and would else be drawn over it
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
                        width: Math.min(implicitWidth, parent.width - (row.tipBadge && row.tipBadge.visible ? row.tipBadge.width + parent.spacing : 0))
                        text: row.found ? row.hit.title : row.title
                        textFormat: row.found ? Text.StyledText : Text.AutoText
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }
                    // (a row with a tip: its "i" comes here, tipBadge)
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
                // (found among the rows this one keeps away: its line comes here, unlockedLine)
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
