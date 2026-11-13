pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The size of what is picked, by the selection (GitHub #308). At rest it is
// the label "1280 × 720"; with a selection a click on it, or F, opens the
// panel in its place: the width and the height to type in (Enter gives them
// to the selection, Tab goes to the other one, the arrows and the wheel step
// a pixel, ten with Shift), a lock that holds the shape (the other number
// follows the one typed, and so do drags, handles and Shift with the arrows),
// and the list of sizes and shapes kept to pick from, with the size now to
// add to it. With no selection yet, F opens it by the pointer with the list
// out: a size makes a selection there, a shape is held for the drag to come.
// All numbers are screen pixels. CaptureSession keeps the state (sizeEditing,
// ratio, sizePresets); nothing here outlives the capture.
Item {
    id: box
    objectName: "captureSizeBox"

    required property CaptureSession session
    required property CaptureScreen screenModel
    // The overlay's key handler: the keys are its again once the panel closes.
    required property Item keyTarget
    property real dpr: 1
    property rect shown // the selection here, else what a click would pick; physical px
    property bool selectedHere: false
    property bool hoverHere: false
    // The window's size, logical px: the list opens where there is room.
    property real areaWidth: 0
    property real areaHeight: 0

    // ---- geometry, logical px ----
    readonly property int pad: 4 // round the panel's row, and the list's rows
    readonly property int rowHeight: 28
    readonly property int fieldWidth: 58
    readonly property int gap: 4 // between the parts of the row; the panel to its list
    readonly property int listRow: 30
    readonly property int listWidth: 200
    readonly property real edge: Theme.edge(dpr)

    function snap(v) { return Theme.snap(v, dpr) }

    // ---- state ----
    // Opened with no selection: on the monitor the pointer was on, where it was.
    property bool openedHere: false
    property point openedAt: Qt.point(0, 0)
    readonly property bool editing: session.sizeEditing && (selectedHere || (!session.hasSelection && openedHere))
    property bool listOpen: false
    property int current: -1 // the list's row the arrows are on; presets.length: the one that adds
    property int lead: Qt.Horizontal // the number typed last: the other follows it when locked
    readonly property var presets: session.sizePresets
    readonly property int typedWidth: number(wField.input.text)
    readonly property int typedHeight: number(hField.input.text)
    readonly property bool typed: typedWidth > 0 && typedHeight > 0
    readonly property bool canAdd: typed && !session.sizePresetsFull
                                   && !presets.some((p) => !p.ratio && p.width === typedWidth && p.height === typedHeight)

    readonly property Item face: editing ? panel : chip
    readonly property point place: editing && !selectedHere
                                   ? screenModel.placeMagnifier(openedAt, face.width, face.height)
                                   : screenModel.placeSizeLabel(shown, face.width, face.height)

    visible: (selectedHere || hoverHere || editing) && !session.ocrOnly // 识别屏幕文字: only the shade
    x: snap(place.x)
    y: snap(place.y)
    width: face.width
    height: face.height

    Connections {
        target: box.session
        function onSizeEditingChanged() {
            if (!box.session.sizeEditing)
                return
            box.openedHere = !box.session.hasSelection && box.screenModel.cursorHere
            box.openedAt = box.screenModel.cursor
        }
    }
    onEditingChanged: {
        if (editing) {
            load()
            lead = Qt.Horizontal
            current = -1
            listOpen = !selectedHere
            Qt.callLater(focusInput)
        } else if (wField.input.activeFocus || hField.input.activeFocus) {
            keyTarget.forceActiveFocus()
        }
    }
    // A step, a preset or a number entered moved the selection: the numbers follow.
    onShownChanged: if (editing && selectedHere) load()

    // The whole pixels typed (the fields take digits only); 0: none, or not a size.
    function number(text) {
        const n = parseInt(text, 10)
        return n >= 1 && n <= 99999 ? n : 0
    }
    function load() {
        wField.input.text = shown.width > 0 ? String(shown.width) : ""
        hField.input.text = shown.height > 0 ? String(shown.height) : ""
    }
    function focusInput() {
        if (!editing)
            return
        const w = box.Window.window
        if (w)
            w.requestActivate()
        wField.input.forceActiveFocus()
        wField.input.selectAll()
    }
    function close() { session.sizeEditing = false }
    function commit() {
        if (typed)
            session.setSize(typedWidth, typedHeight, lead)
    }
    // The other number, by the shape held, as one is typed.
    function follow(orientation) {
        lead = orientation
        const r = session.ratio
        if (!session.ratioLocked || r.width <= 0 || r.height <= 0)
            return
        // From the field's own text: textEdited comes before the properties bound to it follow.
        const typedNow = number((orientation === Qt.Horizontal ? wField : hField).input.text)
        if (typedNow <= 0)
            return
        if (orientation === Qt.Horizontal)
            hField.input.text = String(Math.max(1, Math.round(typedNow * r.height / r.width)))
        else
            wField.input.text = String(Math.max(1, Math.round(typedNow * r.width / r.height)))
    }
    function step(orientation, by) {
        const field = orientation === Qt.Horizontal ? wField : hField
        field.input.text = String(Math.max(1, Math.min(99999, number(field.input.text) + by)))
        follow(orientation)
        if (session.hasSelection)
            commit()
    }
    function toggleLock() {
        if (session.ratioLocked)
            session.lockRatio(0, 0)
        else if (typed)
            session.lockRatio(typedWidth, typedHeight)
    }
    function choose(index) {
        if (index === presets.length) { // the row that adds: the list stays, with the new one in it
            if (canAdd)
                session.addSizePreset(typedWidth, typedHeight)
            return
        }
        if (index < 0 || index > presets.length)
            return
        session.applySizePreset(index)
        close()
    }
    function moveCurrent(by) {
        const count = presets.length + 1
        current = current < 0 ? (by > 0 ? 0 : count - 1) : (current + by + count) % count
    }
    function handleKey(event, orientation) {
        const tens = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
        switch (event.key) {
        case Qt.Key_Escape:
            close()
            break
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (listOpen && current >= 0) {
                choose(current)
            } else {
                commit()
                close()
            }
            break
        case Qt.Key_Tab:
        case Qt.Key_Backtab: {
            if (session.hasSelection)
                commit()
            const other = orientation === Qt.Horizontal ? hField : wField
            other.input.forceActiveFocus()
            other.input.selectAll()
            break
        }
        case Qt.Key_Up:
        case Qt.Key_Down:
            if (event.key === Qt.Key_Down && (event.modifiers & Qt.AltModifier))
                listOpen = true
            else if (listOpen)
                moveCurrent(event.key === Qt.Key_Down ? 1 : -1)
            else
                step(orientation, event.key === Qt.Key_Up ? tens : -tens)
            break
        case Qt.Key_F4:
            listOpen = !listOpen
            current = -1
            break
        default:
            return // the digits and the caret keys are the field's
        }
        event.accepted = true
    }

    // ---- the label ----
    Rectangle {
        id: chip
        objectName: "captureSizeLabel"

        readonly property bool clickable: box.selectedHere && !box.screenModel.adjusting

        visible: !box.editing
        width: chipRow.implicitWidth + 12
        height: chipRow.implicitHeight + 4
        radius: Theme.smallControlRadius
        color: Theme.accentFill

        Row {
            id: chipRow
            anchors.centerIn: parent
            spacing: 4

            Glyph {
                anchors.verticalCenter: parent.verticalCenter
                visible: box.session.ratioLocked && box.selectedHere
                iconName: "lock-simple"
                size: 12
                color: Theme.accentText
            }
            Text {
                text: qsTr("%1 × %2").arg(box.shown.width).arg(box.shown.height)
                color: Theme.accentText
                font.pixelSize: Theme.fontCaption
            }
            Glyph { // it opens
                anchors.verticalCenter: parent.verticalCenter
                visible: box.selectedHere
                iconName: "caret-down"
                size: 10
                color: Theme.accentText
                opacity: chipArea.containsMouse ? 1 : 0.7
            }
        }

        MouseArea {
            id: chipArea
            anchors.fill: parent
            enabled: chip.clickable
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton // a right press here is not Esc's step back
            cursorShape: Qt.PointingHandCursor
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("输入尺寸或选预设（F）")
            onClicked: (mouse) => {
                if (mouse.button === Qt.LeftButton)
                    box.session.sizeEditing = true
            }
        }
    }

    // ---- the panel ----
    Item {
        id: panel
        objectName: "captureSizePanel"

        visible: box.editing
        width: Theme.snapUp(fields.width + 2 * box.pad, box.dpr)
        height: box.rowHeight + 2 * box.pad
        // Whatever the fields leave (a letter, a key let go of) ends here, not in the capture.
        Keys.onPressed: (event) => { event.accepted = true }
        Keys.onReleased: (event) => { event.accepted = true }

        Hook { // the style's floating panel (Theme.menuFrame)
            anchors.fill: parent
            part: Theme.menuFrame
            dpr: box.dpr
        }
        MouseArea { // presses on the panel stay with it
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            onWheel: (wheel) => { wheel.accepted = true }
        }

        Row {
            id: fields
            x: box.pad
            y: box.pad
            spacing: box.gap

            NumberField {
                id: wField
                objectName: "captureSizeWidth"
                orientation: Qt.Horizontal
                name: qsTr("宽度（像素）")
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "×"
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
            NumberField {
                id: hField
                objectName: "captureSizeHeight"
                orientation: Qt.Vertical
                name: qsTr("高度（像素）")
            }
            KeyButton {
                objectName: "captureSizeLock"
                iconName: box.session.ratioLocked ? "lock-simple" : "lock-simple-open"
                checked: box.session.ratioLocked
                enabled: box.session.ratioLocked || box.typed
                name: box.session.ratioLocked ? qsTr("取消锁定比例") : qsTr("锁定比例")
                onClicked: box.toggleLock()
            }
            KeyButton {
                objectName: "captureSizePresets"
                iconName: "caret-down"
                name: qsTr("预设尺寸")
                onClicked: {
                    box.listOpen = !box.listOpen
                    box.current = -1
                }
            }
        }
    }

    // ---- the list of sizes and shapes ----
    Item {
        id: list
        objectName: "captureSizeList"

        // Under the panel where the screen has room, else over it; kept on the screen across.
        readonly property bool below: box.y + panel.height + box.gap + height <= box.areaHeight

        visible: box.editing && box.listOpen
        x: box.snap(Math.max(-box.x, Math.min(0, box.areaWidth - box.x - width)))
        y: box.snap(below ? panel.height + box.gap : -box.gap - height)
        width: box.listWidth
        height: Theme.snapUp(rows.height + 2 * box.pad, box.dpr)

        Hook { // the style's menu box (Theme.menuFrame)
            anchors.fill: parent
            part: Theme.menuFrame
            dpr: box.dpr
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            onWheel: (wheel) => { wheel.accepted = true }
        }

        Column {
            id: rows
            x: box.pad
            y: box.pad
            width: list.width - 2 * box.pad

            Repeater {
                model: box.presets

                delegate: ListRow {
                    required property var modelData
                    required property int index

                    objectName: "captureSizePreset" + index
                    row: index
                    label: modelData.label
                    note: modelData.ratio ? qsTr("比例") : ""
                    removable: true
                }
            }

            Item { // the line over the row that adds
                width: rows.width
                height: 2 * box.pad + box.edge
                visible: box.presets.length > 0

                Hook {
                    y: box.pad
                    width: parent.width
                    height: box.edge
                    part: Theme.separator
                    orientation: Qt.Horizontal
                    dpr: box.dpr
                }
            }

            ListRow {
                objectName: "captureSizeAdd"
                row: box.presets.length
                iconName: "plus"
                enabled: box.canAdd
                label: box.typed ? qsTr("%1 × %2").arg(box.typedWidth).arg(box.typedHeight) : qsTr("存为预设")
                note: !box.typed ? "" : box.canAdd ? qsTr("存为预设") : box.session.sizePresetsFull ? qsTr("预设已满") : qsTr("已在预设里")
            }
        }
    }

    // A whole number of pixels to type in.
    component NumberField: Item {
        id: field

        required property int orientation
        property string name
        readonly property alias input: input

        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        width: box.fieldWidth
        height: box.rowHeight

        Hook { // the style's input field (Theme.inputField)
            id: fieldFace
            anchors.fill: parent
            part: Theme.inputField
            focused: input.activeFocus
            hovered: fieldArea.containsMouse
            dpr: box.dpr
        }
        MouseArea {
            id: fieldArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.IBeamCursor
            onPressed: input.forceActiveFocus()
            onWheel: (wheel) => {
                if (wheel.angleDelta.y !== 0)
                    box.step(field.orientation, (wheel.angleDelta.y > 0 ? 1 : -1) * ((wheel.modifiers & Qt.ShiftModifier) ? 10 : 1))
            }
        }
        TextInput {
            id: input
            anchors.fill: parent
            leftPadding: 6
            rightPadding: 6
            horizontalAlignment: TextInput.AlignHCenter
            verticalAlignment: TextInput.AlignVCenter
            validator: IntValidator { bottom: 1; top: 99999 }
            maximumLength: 5
            inputMethodHints: Qt.ImhDigitsOnly
            selectByMouse: true
            color: fieldFace.out("textColor", Theme.text)
            selectionColor: Theme.textSelection
            selectedTextColor: Theme.text
            font.pixelSize: Theme.fontCaption
            Accessible.name: field.name
            onTextEdited: box.follow(field.orientation)
            Keys.onPressed: (event) => box.handleKey(event, field.orientation)
        }
    }

    // A small flat key with an icon (the lock, the list's arrow).
    component KeyButton: MouseArea {
        id: key

        property string iconName
        property string name
        property bool checked: false

        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        width: box.rowHeight
        height: box.rowHeight
        hoverEnabled: true
        cursorShape: Qt.ArrowCursor
        Accessible.role: Accessible.Button
        Accessible.name: name

        Hook { // the style's key (Theme.keySurface)
            id: keyFace
            anchors.fill: parent
            part: Theme.keySurface
            hovered: key.containsMouse && key.enabled
            pressed: key.pressed
            checked: key.checked
            flat: true
            role: "toolbar"
            dpr: box.dpr
        }
        Glyph {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: keyFace.out("contentOffsetY", 0)
            iconName: key.iconName
            size: 16
            color: key.enabled ? keyFace.out("foreground", Theme.text) : Theme.faint
        }
    }

    // A row of the list: a size or a shape to take, or the one that adds.
    component ListRow: MouseArea {
        id: item

        property int row: -1
        property string label
        property string note
        property string iconName
        property bool removable: false

        width: parent ? parent.width : 0
        height: box.listRow
        hoverEnabled: true
        cursorShape: Qt.ArrowCursor
        Accessible.role: Accessible.MenuItem
        Accessible.name: label
        onClicked: box.choose(item.row)

        Hook { // the style's row highlight (Theme.rowHighlight)
            anchors.fill: parent
            part: Theme.rowHighlight
            selected: box.current === item.row && item.enabled
            hovered: item.containsMouse && item.enabled
            role: "menu"
            dpr: box.dpr
        }
        Glyph {
            id: mark
            visible: item.iconName.length > 0
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            iconName: item.iconName
            size: 14
            color: item.enabled ? Theme.subtext : Theme.faint
        }
        Text {
            x: mark.visible ? 28 : 10
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - x - (noteText.visible ? noteText.width + 18 : item.removable ? box.listRow : 8)
            elide: Text.ElideRight
            text: item.label
            color: item.enabled ? Theme.text : Theme.faint
            font.pixelSize: Theme.fontCaption
        }
        Text {
            id: noteText
            visible: item.note.length > 0 && !remove.visible
            x: parent.width - width - 10
            anchors.verticalCenter: parent.verticalCenter
            text: item.note
            color: Theme.faint
            font.pixelSize: Theme.fontCaption
        }
        MouseArea { // take it out of the list
            id: remove
            objectName: item.objectName + "Remove"
            visible: item.removable && (item.containsMouse || containsMouse)
            x: parent.width - width - 3
            y: 3
            width: box.listRow - 6
            height: box.listRow - 6
            hoverEnabled: true
            cursorShape: Qt.ArrowCursor
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("删除这个预设")
            onClicked: {
                box.current = -1
                box.session.removeSizePreset(item.row)
            }

            Hook { // the style's key (Theme.keySurface)
                id: removeFace
                anchors.fill: parent
                part: Theme.keySurface
                hovered: remove.containsMouse
                pressed: remove.pressed
                flat: true
                role: "toolbar"
                dpr: box.dpr
            }
            Glyph {
                anchors.centerIn: parent
                iconName: "x"
                size: 12
                color: removeFace.out("foreground", Theme.subtext)
            }
        }
    }
}
