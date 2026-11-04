pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// What a row cannot show of a long name: the whole name, the folder it is in,
// and for a file its size, when it was last written and when it was created.
// Offered by a row (ResultRow, PanelRow, DialogBarRow) while the mouse is over
// its text and its name or folder is cut (a row that shows everything has no
// tip); one per window, for all its rows. As Windows' own tips: it comes once
// the mouse has rested for the system's hover time; while it is up, or just
// after it went, another row's comes at once (moving down a list reads one
// name after another); it goes as the mouse leaves, and at once on a click, a
// key, the list scrolling (the window calls dismiss()), and comes back only
// once the mouse moves again. Under the row, its text level with the row's
// name; over the row where the screen ends below. A window of its own (as
// ShortcutTip) so it is not cut off by a launcher with a row or two; it never
// takes the focus, nor the mouse: clicks go through to the row. `host`: the
// window's view-model, for the screen area and the native styling.
Window {
    id: tip

    required property QtObject host

    // The row it is for (up, or waiting for the hover time), and what it says.
    property Item source: null
    property Item textItem: null // the row's text, which the tip's lines up with
    property string name
    property string folder
    property string path
    property string size // "1.2 MB"; empty for a folder or not known
    property string modified // "2026-10-15 14:32"
    property string created // ... once FileFacts has read it
    property double hiddenAt: 0 // when it last went as the mouse left: back at once for a moment

    readonly property int padX: 12
    readonly property int padY: 8
    property real maxTextWidth: 560 // set as it is placed

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
           | Qt.WindowTransparentForInput
    color: Theme.menuBackground

    // Dismissed: not back until the mouse really moves. A row that changes
    // under a still mouse (a key moves the current row onto it and its
    // buttons show, the list scrolled) gets hover moves from Qt all the same,
    // at the same place: where the mouse last was over a row (lastAt) when
    // the tip was dismissed, offers are not taken.
    property var lastAt: null
    property var quietAt: null

    // The row's facts: {name, folder, path, size, modified, onDisk (a file or
    // folder: when created is asked), at (the mouse, global)}.
    function offer(row: Item, text: Item, facts: var) {
        if (quietAt !== null && Math.abs(facts.at.x - quietAt.x) < 1 && Math.abs(facts.at.y - quietAt.y) < 1)
            return
        quietAt = null
        lastAt = facts.at
        if (row === source && (visible || delay.running)) {
            linger.stop() // back on it before it went
            return // still the same: nothing new (called as the mouse moves)
        }
        source = row
        textItem = text
        name = facts.name
        folder = facts.folder
        path = facts.path
        size = facts.size ?? ""
        modified = facts.modified ?? ""
        created = ""
        if (facts.onDisk)
            FileFacts.ask(path) // known by the time it shows, mostly
        linger.stop()
        if (visible || Date.now() - hiddenAt < 400)
            present()
        else
            delay.restart()
    }

    // The mouse left `row` (or it changed): the tip goes in a moment, unless another row takes it.
    function withdraw(row: Item) {
        if (row !== source)
            return
        delay.stop()
        if (visible)
            linger.restart()
        else
            source = null
    }

    // Gone at once, and not back until the mouse moves (a click, a key, the list scrolled).
    function dismiss() {
        delay.stop()
        linger.stop()
        hiddenAt = 0
        source = null
        visible = false
        quietAt = lastAt
    }

    function present() {
        if (!source || !textItem || !source.visible) {
            dismiss()
            return
        }
        // Its text at the row's name, under the row; over it where the screen ends.
        const at = textItem.mapToGlobal(0, 0)
        const top = source.mapToGlobal(0, 0).y
        const bottom = top + source.height
        const area = host.screenArea(Qt.point(at.x, at.y))
        // The name on one line where the screen has room (wrapped, a long
        // name without spaces breaks anywhere: "…FINAL_v3." and "docx" on
        // two lines); the folder wraps at that width, or at the row's text
        // and then some; never wider than the screen.
        maxTextWidth = Math.max(160, Math.min(Math.max(560, textItem.width + 160, Math.ceil(nameWidth.advanceWidth) + 1),
                                              (area.width > 0 ? area.width : 1e6) - 2 * padX - 16))
        lines.forceLayout() // its size now: placed by it
        const w = Math.ceil(lines.width) + 2 * padX
        const h = Math.ceil(lines.implicitHeight) + 2 * padY
        let x = Math.round(at.x - padX)
        let y = Math.round(bottom + 2)
        if (area.width > 0) {
            if (y + h > area.y + area.height)
                y = Math.round(top - h - 2)
            x = Math.max(area.x, Math.min(x, area.x + area.width - w))
            y = Math.max(area.y, Math.min(y, area.y + area.height - h))
        }
        if (!visible)
            host.prepareMenuWindow(tip)
        tip.setGeometry(x, y, w, h) // one move and resize: no frame of it half there
        if (!visible)
            show()
    }

    Timer {
        id: delay
        interval: SystemTheme.hoverTime
        onTriggered: tip.present()
    }
    Timer {
        id: linger // time to reach the next row before it goes
        interval: 100
        onTriggered: {
            tip.source = null
            tip.visible = false
            tip.hiddenAt = Date.now()
        }
    }

    Connections {
        target: FileFacts
        function onCreated(path, when) {
            if (path !== tip.path || tip.created === when)
                return
            tip.created = when
            if (tip.visible)
                tip.present() // a little wider, maybe
        }
    }

    // Folders broken after a backslash rather than inside a name: a zero-width space there.
    readonly property string folderShown: folder.split("\\").join("\\" + String.fromCharCode(0x200B))
    readonly property string details: {
        const parts = []
        if (size.length > 0)
            parts.push(size)
        if (modified.length > 0)
            parts.push(qsTr("修改于 %1").arg(modified))
        if (created.length > 0)
            parts.push(qsTr("创建于 %1").arg(created))
        return parts.join("  ·  ")
    }

    TextMetrics { id: nameWidth; font: nameText.font; text: tip.name }
    TextMetrics { id: folderWidth; font: folderText.font; text: tip.folder }
    TextMetrics { id: detailsWidth; font: detailsText.font; text: tip.details }

    Column {
        id: lines
        x: tip.padX
        y: tip.padY
        width: Math.min(tip.maxTextWidth, Math.ceil(Math.max(nameWidth.advanceWidth, folderWidth.advanceWidth,
                                                             detailsWidth.advanceWidth)) + 1)
        spacing: 3

        Text {
            id: nameText
            width: parent.width
            text: tip.name
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: Theme.uiBody
        }
        Text {
            id: folderText
            visible: tip.folder.length > 0
            width: parent.width
            text: tip.folderShown
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
        }
        Text {
            id: detailsText
            visible: tip.details.length > 0
            width: parent.width
            text: tip.details
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.faint
            font.pixelSize: Theme.uiCaption
        }
    }

    // The box: DWM's frame under fluent (the window's colour is the menu's);
    // an own frame has the menu's radius, the style's menuFrame draws the
    // body and the edge, and the window's shadow is OwnFrame's.
    Binding {
        target: tip
        property: "color"
        value: "transparent"
        when: Theme.ownFrame
    }
    OwnFrame {
        active: Theme.ownFrame
        radius: Theme.menuRadius
        shadow: Theme.tok("menuWindowShadow", Theme.frameShadow)
    }
    Hook {
        // The window's size, not its content item's (sized by a later resize
        // event): a body from the first frame (as ContextMenu).
        width: tip.width
        height: tip.height
        z: -1
        visible: Theme.ownFrame
        part: Theme.ownFrame ? Theme.menuFrame : null
        extra: ({ shadow: false })
        dpr: Screen.devicePixelRatio
    }
    Loader { // Windows 10 has no edge of DWM's
        anchors.fill: parent
        z: 1000
        active: !Theme.ownFrame
        sourceComponent: WindowEdge { fill: Theme.menuBackground }
    }
}
