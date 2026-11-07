import QtQuick

// A few sets of a list's row buttons (`bar`), lent to the rows that show
// them. At most the current row and the one under the mouse do (and, for a
// moment, the row the mouse just left: a hover enters before the old one
// leaves), so three sets are made, all at once when the first row asks (the
// first results), instead of one in every row. A row borrows one as it shows
// its buttons and gives it back after: nothing is made then, which in a
// row's first showing took longer than a frame at 240 Hz (docs/pitfalls.md).
// A set is an Item with a `host` property, the row it serves; the row has
// `showActions` and `actionsArea`, the item a lent set is put in. A set put
// away waits in the pool, which is hidden.
Item {
    id: pool

    property Component bar
    property int ready: 3 // made together; more only if ever all are out
    property var made: []

    visible: false

    // Its set for `row`, lent to it now: its own if it has one, else a free
    // one, or one whose row no longer shows it and has not given it back yet.
    function take(row: Item): Item {
        fill()
        let set = made.find(s => s.host === row)
            ?? made.find(s => s.host === null)
            ?? made.find(s => !s.host.showActions)
            ?? make()
        if (!set)
            return null
        set.host = row
        set.parent = row.actionsArea
        return set
    }

    function give(row: Item) {
        for (const set of made) {
            if (set.host === row) {
                set.host = null
                set.parent = pool
            }
        }
    }

    function make(): Item {
        const set = bar.createObject(pool)
        if (!set) {
            console.warn("RowActionsPool: a set could not be made:", bar.errorString())
            return null
        }
        made = made.concat([set])
        return set
    }

    function fill() {
        while (made.length < ready && make()) {}
    }
}
