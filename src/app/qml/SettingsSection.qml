import QtQuick
import WinShun

// A card holding SettingRows, with an optional title and note above it.
//
// It shows while its page and its tab on that page are the current ones;
// while the settings are searched, while some of its rows fit the words,
// whatever its tab, the first such section of a page under the page's
// name. Both come from the column of pages in the settings window: search,
// currentPage, currentTab, pages, pageKeywords, openPage().
Column {
    id: section

    property string title
    property string note
    property int page: -1 // the index into the window's pages
    property int tab: 0 // the index into its page's tabs, on a page that has them
    default property alias rows: card.data

    // The column of pages in the settings window (see above).
    readonly property var column: parent
    readonly property SettingsSearch search: column && column.search !== undefined ? column.search : null
    readonly property bool searching: search !== null && search.active
    readonly property var pageInfo: column && column.pages !== undefined ? column.pages[page] ?? null : null
    // Other words people look for the page by.
    readonly property var pageKeywords: column && column.pageKeywords !== undefined
                                        ? (column.pageKeywords[page] ?? "").split(";").filter(word => word.length > 0)
                                        : []
    // Its rows on show while searching.
    readonly property int found: searching && search.total >= 0 ? search.sectionCount(section) : 0
    // While searching: the first section of its page on show, which carries the page's name.
    readonly property bool leads: {
        if (!searching || !parent)
            return false
        const siblings = parent.children
        for (let i = 0; i < siblings.length; ++i) {
            const other = siblings[i]
            if (other === section)
                return true
            if (other.page === page && other.visible)
                return false
        }
        return true
    }

    // Lays out the rows now rather than before the next frame, for a
    // position to be read right after they changed.
    function layoutNow() {
        card.forceLayout()
        section.forceLayout()
    }

    visible: searching ? found > 0 : !!column && column.currentPage === page && column.currentTab === tab
    spacing: 8

    Item { // the page's name over its results
        visible: section.leads
        width: parent.width
        height: 32

        Glyph {
            id: pageGlyph
            anchors.verticalCenter: parent.verticalCenter
            glyph: section.pageInfo?.glyph ?? ""
            size: 16
            color: Theme.text
        }
        Text {
            anchors.left: pageGlyph.right
            anchors.leftMargin: 10
            anchors.right: pageLink.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: section.pageInfo?.title ?? ""
            color: Theme.text
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Text { // leaves the search for the whole page
            id: pageLink
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("在“%1”中查看").arg(section.pageInfo?.title ?? "") + "  ›"
            color: linkArea.pressed ? Theme.subtext : Theme.accent
            font.pixelSize: Theme.fontCaption
            font.underline: linkArea.containsMouse

            MouseArea {
                id: linkArea
                anchors.fill: parent
                anchors.margins: -4
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: section.column.openPage(section.page, section.tab)
            }
        }
    }

    Text {
        visible: section.title.length > 0
        text: section.title
        color: Theme.text
        font.pixelSize: Theme.fontTitle
        font.weight: Font.DemiBold
    }

    Text {
        width: parent.width
        visible: section.note.length > 0
        text: section.note
        color: Theme.subtext
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
    }

    Rectangle {
        width: parent.width
        height: card.implicitHeight
        radius: 6
        color: Theme.card
        border.width: 1
        border.color: Theme.cardBorder

        Column {
            id: card

            readonly property Item section: parent.parent // what its rows read: this section

            width: parent.width
        }
    }
}
