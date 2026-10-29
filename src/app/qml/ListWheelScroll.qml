import QtQuick

// Plan an aligned destination before moving. Flickable's wheel animation
// rounds contentY again in its final fixup; at fractional DPI that can move
// a row backwards after it has visibly stopped. Never feed rounded frames
// back into a native flick: only discrete wheel events use this animation.
Item {
    id: scroll

    required property Flickable view
    property bool animate: true
    property int acceptedModifiers: Qt.KeyboardModifierMask
    property int scrollLines: Application.styleHints.wheelScrollLines
    // Variable-height ListView reflows round contentY during layout. Giving
    // those views integral coordinates leaves no layout fixup to interrupt
    // the glide; ScrollPixelAlignment still aligns the rendered rows.
    property bool logicalPixelSteps: false
    // A ListView's wheel stops at row edges: the top of the rows (inside the
    // top margin) comes to rest on the edge of a row, never in the middle of
    // one. Rows are of any height; the goal is the next edge along the way,
    // taken once the distance the wheel asked for has covered half of it, and
    // always one row at least for a whole notch. Smaller deltas (free-spinning
    // and high-resolution wheels) pile up in `carry` until they do. At the end
    // it stops at the end (the bottom edge of the last row), never back for
    // the top's sake.
    property bool snapRows: false
    // A heading between the rows (a section's: the launcher's caption over
    // the rows found elsewhere, GitHub #110), over row `headingRow`, as tall
    // as `headingHeight`; -1: none. It goes with the row under it: no edge
    // between them, the edge above it is that row's top.
    property int headingRow: -1
    property real headingHeight: 0
    readonly property bool running: glide.running
    // A list laid out bottom to top (ListView.BottomToTop: the clipboard
    // turned over) has its first row at the bottom and rests there; content
    // shorter than the view sits there too. Else all of it is at the top.
    readonly property bool bottomUp: view instanceof ListView
                                     && (view as ListView).verticalLayoutDirection === ListView.BottomToTop
    readonly property real topY: view.originY - view.topMargin
    readonly property real bottomY: view.originY + view.contentHeight + view.bottomMargin - view.height
    readonly property real minimumY: bottomUp ? Math.min(topY, bottomY) : topY
    readonly property real maximumY: bottomUp ? bottomY : Math.max(topY, bottomY)
    // Where it rests: the device-pixel grid is counted from there, as
    // ScrollPixelAlignment counts the rows' displacement.
    readonly property real restY: bottomUp ? maximumY : minimumY
    readonly property real dpr: view.Screen.devicePixelRatio
    property real requestedY: 0
    property real position: 0
    property int direction: 0
    property int writeDepth: 0
    readonly property bool writing: writeDepth > 0
    property bool boundsMoved: false
    // Distance the wheel asked for that no row edge has taken yet, signed as
    // contentY (negative against the way: an edge taken a little early).
    property real carry: 0
    // The goal had to be a plain distance, for want of rows made to read edges
    // from: look for the nearest edge once it has arrived.
    property bool unsnapped: false

    function cancel() {
        glide.stop()
        direction = 0
    }

    function bounded(y) {
        return Math.max(minimumY, Math.min(maximumY, y))
    }

    // The row edge next to where the rows' top is, along `dir`: the contentY
    // that puts the top of the rows (below the top margin) on it. NaN where
    // there is no row to read it from (no row there, or not made: the ListView
    // keeps the ones in view and its cacheBuffer only).
    function rowEdge(y, dir) {
        const list = view as ListView
        const probe = y + list.topMargin + dir * 0.5
        let index = list.indexAt(list.contentX + 1, probe)
        if (index < 0 && headingRow >= 0) { // on the heading: the row it goes with
            const under = list.itemAtIndex(headingRow)
            if (under && probe >= under.y - headingHeight && probe < under.y)
                index = headingRow
        }
        const row = index >= 0 ? list.itemAtIndex(index) : null
        if (!row)
            return NaN
        const top = index === headingRow ? row.y - headingHeight : row.y
        return (dir > 0 ? row.y + row.height : top) - list.topMargin
    }

    function aligned(y) {
        if (snapRows && isFinite(y))
            return bounded(y) // an edge already (or a distance for want of one: settled later)
        if (logicalPixelSteps)
            return bounded(Math.round(y))
        return bounded(restY + Math.round((y - restY) * dpr) / dpr)
    }

    // A goal at or past the end it is heading for is that end, whatever the
    // rows' heights turn out to be (a goal that stays at the end it came
    // from, as a delta too small for a row, is not the other end).
    function endGoal(goal, dir) {
        return dir < 0 && goal <= minimumY ? -Infinity : dir > 0 && goal >= maximumY ? Infinity : goal
    }

    function moveTo(y) {
        glide.stop()
        const destination = aligned(y)
        if (Math.abs(destination - view.contentY) < 0.000001)
            return
        if (!animate) {
            writePosition(destination)
            // No glide follows an end the write moved: the rows it laid out
            // change the estimate. Go to the end again until it stays put.
            for (let i = 0; i < 8 && !isFinite(y); ++i) {
                const settled = aligned(y)
                if (Math.abs(settled - view.contentY) < 0.000001)
                    break
                writePosition(settled)
            }
            return
        }
        glide.from = view.contentY
        glide.to = destination
        glide.start()
    }

    function wheel(angle) {
        const lines = scrollLines
        if (lines === 0) {
            cancel()
            return
        }
        const step = -angle / 120 * (lines > 0 ? lines * 24 : view.height)
        const nextDirection = Math.sign(step)
        const whole = Math.abs(angle) >= 120
        // Accumulate a burst, but discard its untravelled distance on reversal.
        // Keep the fractional remainder after arrival too: tiny deltas must
        // accumulate even when animations are disabled or events are sparse.
        const continuing = direction === nextDirection
            && (running || Math.abs(view.contentY - aligned(requestedY)) < 0.000001)
        const base = continuing ? requestedY : view.contentY
        cancel()
        view.cancelFlick()
        if (!continuing) {
            carry = 0
            unsnapped = false
        }
        direction = nextDirection
        const goal = snapRows ? rowGoal(base, step, whole) : base + step
        // Keep an endpoint's intent when variable-height rows change the
        // view's estimate of the range during the glide.
        requestedY = endGoal(goal, nextDirection)
        moveTo(requestedY)
    }

    // Where `step` more takes the goal from `from`, on the row edges: see snapRows.
    function rowGoal(from, step, whole) {
        if (!isFinite(from))
            return from // at an end, and meant to stay there
        const dir = Math.sign(step)
        let y = from
        carry += step
        for (let i = 0; i < 400 && carry * dir > 0; ++i) {
            const edge = rowEdge(y, dir)
            if (isNaN(edge)) {
                y += carry
                carry = 0
                unsnapped = true
                break
            }
            const distance = edge - y
            if (carry * dir < Math.abs(distance) / 2)
                break
            y = edge
            carry -= distance
        }
        if (y === from && whole) { // a notch never goes unanswered
            const edge = rowEdge(y, dir)
            y = isNaN(edge) ? y + step : edge
            unsnapped = unsnapped || isNaN(edge)
            carry = 0
        }
        // Past an end is the end, and stays so while the rows' heights are re-estimated.
        return y
    }

    // The glide has come to a goal that is a distance (rows it could not read
    // were not made): now they are, so rest on the nearest edge.
    function settleOnEdge() {
        if (!snapRows || !unsnapped || Math.abs(view.contentY - glide.to) > 0.000001)
            return
        unsnapped = false
        const y = view.contentY
        const up = rowEdge(y, -1)
        const down = rowEdge(y, 1)
        if (isNaN(up) || isNaN(down))
            return
        const edge = y - up <= down - y ? up : down
        if (Math.abs(edge - y) > 0.000001) {
            requestedY = endGoal(edge, Math.sign(edge - y))
            moveTo(requestedY)
        }
    }

    function writePosition(y) {
        // Layout answers this assignment and can change the range several
        // times inside it: variable-height rows are released before new ones
        // are made, so an end can dip past the glide and come straight back.
        // Judge the range once it has settled, after the outermost write.
        writeDepth++
        view.contentY = logicalPixelSteps ? bounded(Math.round(y)) : y
        writeDepth--
        if (!writing && boundsMoved) {
            boundsMoved = false
            boundsChanged()
        }
    }
    onPositionChanged: if (glide.running) writePosition(position)
    // A resized view can move the endpoint while a wheel glide is running.
    // Retarget that glide now, never correct the position after it finishes.
    function boundsChanged() {
        if (!running)
            return
        if (writing) {
            boundsMoved = true
            return
        }
        const destination = aligned(requestedY)
        if (Math.abs(destination - glide.to) < 0.000001)
            return
        // A newly estimated end must not pull a running glide backwards.
        if ((destination - view.contentY) * direction < 0)
            cancel()
        else
            moveTo(requestedY)
    }
    // Rows added or removed above (bottom up: below) shift the content under
    // a glide, and its target no longer means anything. An end is still an end.
    function restMoved() {
        if (running && !writing && isFinite(requestedY))
            cancel()
        else
            boundsChanged()
    }
    onMinimumYChanged: bottomUp ? boundsChanged() : restMoved()
    onMaximumYChanged: bottomUp ? restMoved() : boundsChanged()
    onDprChanged: cancel()

    WheelHandler {
        parent: scroll.view
        target: null
        // Qt also rejects synthesized touchpad events before emitting wheel.
        // Setting WheelEvent.accepted in this signal does not forward events.
        // angleDelta already includes the OS's natural-scrolling direction.
        acceptedDevices: PointerDevice.Mouse
        acceptedModifiers: scroll.acceptedModifiers
        enabled: scroll.view.interactive
        onWheel: event => {
            if (event.angleDelta.y)
                scroll.wheel(event.angleDelta.y)
        }
    }

    NumberAnimation {
        id: glide
        target: scroll
        property: "position"
        duration: 200
        easing.type: Easing.OutCubic
        onFinished: scroll.settleOnEdge()
    }

    Connections {
        target: scroll.view
        function onContentYChanged() { if (!scroll.writing) scroll.cancel() }
        function onDraggingChanged() { if (scroll.view.dragging) scroll.cancel() }
        function onMovementStarted() { scroll.cancel() }
        function onVisibleChanged() { if (!scroll.view.visible) scroll.cancel() }
        function onInteractiveChanged() { if (!scroll.view.interactive) scroll.cancel() }
    }
    Connections {
        target: scroll.view instanceof ListView ? scroll.view : null
        function onCurrentIndexChanged() { scroll.cancel() }
    }
    Connections {
        target: scroll.view.Window.window
        function onVisibleChanged() { if (!target.visible) scroll.cancel() }
    }
}
