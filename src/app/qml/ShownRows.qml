import QtQuick

// The rows the number keys pick in a list (Alt+1… or Ctrl+1…): the first one
// at least half in view and the ones after it, nine at most, each at least
// half in view; as the list scrolls, the numbers go with it, as the search
// list's do (Main.qml). Told by the rows themselves, whatever their heights,
// and upside down too (ListView.BottomToTop: the first row at the bottom).
// The hints and the keys both ask this, so a number always pastes the row
// that shows it.
QtObject {
    id: rows

    required property ListView view
    // -1 for none
    readonly property int first: {
        // The rows are asked directly (indexAt, itemAtIndex), which tells
        // nothing when they move: these do, the rows' own box too (laid out
        // after a change of the model, which may leave the rest as it was).
        view.contentY; view.height; view.contentHeight; view.originY; view.count; view.verticalLayoutDirection
        view.contentItem.childrenRect
        return firstShown()
    }
    readonly property int last: {
        if (first < 0)
            return -1
        view.contentY; view.height; view.contentHeight; view.originY; view.count; view.contentItem.childrenRect
        let row = first
        while (row + 1 < view.count && row + 1 <= first + 8 && halfShown(row + 1))
            ++row
        return row
    }

    // The number that row shows (1…9), else 0.
    function numberOf(index: int): int {
        return first >= 0 && index >= first && index <= last ? index - first + 1 : 0
    }
    // The row that number picks, else -1.
    function rowOf(number: int): int {
        const row = first + number - 1
        return first >= 0 && number >= 1 && row <= last ? row : -1
    }

    function halfShown(row: int): bool {
        const item = view.itemAtIndex(row)
        if (!item || item.height <= 0)
            return false
        const top = Math.max(item.y, view.contentY)
        const bottom = Math.min(item.y + item.height, view.contentY + view.height)
        return bottom - top >= item.height / 2
    }
    function firstShown(): int {
        if (view.count === 0 || view.height <= 0)
            return -1
        // The row at the edge the list begins at (its top; upside down, its
        // bottom), or past the margin there.
        const flipped = view.verticalLayoutDirection === ListView.BottomToTop
        const edge = flipped ? view.contentY + view.height - 1 : view.contentY
        const inward = flipped ? -view.bottomMargin : view.topMargin
        let row = view.indexAt(1, edge)
        if (row < 0)
            row = view.indexAt(1, edge + inward)
        if (row < 0)
            row = 0
        while (row < view.count - 1 && !halfShown(row))
            ++row
        return halfShown(row) ? row : -1
    }
}
