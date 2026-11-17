import QtQuick
import QtQuick.Controls.Basic as Controls
import WinShun

// A setting's slider, as Windows 11's: a thin track filled with the accent up
// to a round thumb, the value beside it. Dragged, it previews (previewed) and
// is committed once let go; a key moves it a step (←→↑↓), a tenth of the range
// (PageUp/PageDown) or to an end (Home/End), committed when the key comes up.
// The wheel stays the page's (Control.wheelEnabled is off): rows scroll on
// under the pointer. Its values are a plugin's (plugin::Setting::fit): from +
// n × stepSize, and `to`. `editable` (a plugin's number, GitHub #359): the
// value in a box to type it into, its unit after it.
Item {
    id: root

    property real value: 50
    property real from: 0
    property real to: 100
    property real stepSize: 1
    property int decimals: 0 // digits after the point, as the value is shown
    property string unit: "%"
    property bool editable: false
    property color rail: Theme.track // the track not filled in; a host on a card passes a stronger one
    property string label
    property real dpr: 0 // the window's, passed by a host that has it (GitHub #111)
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio
    readonly property bool hasUnit: editable && unit.length > 0
    // PageUp/PageDown: a tenth of the range, in whole steps.
    readonly property int pageSteps: stepSize > 0 ? Math.max(1, Math.round((to - from) / stepSize / 10)) : 1
    // The box and the unit after it; a host with several sliders gives them
    // all the same widths, so that their tracks line up.
    property real boxWidth: Math.max(52, Math.ceil(widest.advanceWidth) + 24)
    property real unitWidth: hasUnit ? Math.ceil(unitText.implicitWidth) : 0
    readonly property real unitSpace: editable && unitWidth > 0 ? 6 + unitWidth : 0

    signal previewed(real value)
    signal committed(real value)

    // The nearest of from + n × stepSize and `to`, within them.
    function snap(v) {
        let x = Math.min(Math.max(Number(v), root.from), root.to)
        if (isNaN(x))
            x = root.from
        if (root.stepSize > 0) {
            let snapped = root.from + Math.round((x - root.from) / root.stepSize) * root.stepSize
            if (snapped > root.to || root.to - x < Math.abs(x - snapped))
                snapped = root.to // past it, or nearer: `to` is a value too
            x = Math.max(snapped, root.from)
        }
        return Number(x.toFixed(root.decimals))
    }
    function shown(v) { return root.snap(v).toFixed(root.decimals) }
    // `steps` steps on from v (back when negative), `to` one of them.
    function stepped(v, steps) {
        if (!(root.stepSize > 0))
            return root.snap(v)
        const n = (v - root.from) / root.stepSize
        const k = steps > 0 ? Math.floor(n + 1e-9) + steps : Math.ceil(n - 1e-9) + steps
        return root.snap(Math.min(Math.max(root.from + k * root.stepSize, root.from), root.to))
    }
    function commit() {
        root.committed(root.snap(slider.value))
        slider.value = Qt.binding(() => root.value) // what the setting holds now
    }
    function typed(text) {
        const n = Number(String(text).replace(/[,\s]/g, ""))
        if (String(text).trim().length > 0 && isFinite(n) && root.snap(n) !== root.snap(root.value)) {
            slider.value = root.snap(n)
            root.previewed(slider.value)
            root.commit()
        }
        box.text = Qt.binding(() => root.shown(slider.value)) // also a value fitted to the one it had
    }

    implicitWidth: editable ? 176 + 10 + boxWidth + unitSpace : 240
    implicitHeight: 36
    opacity: enabled ? 1 : 0.4

    Controls.Slider {
        id: slider
        objectName: "settingSlider"
        anchors.left: parent.left
        anchors.right: root.editable ? box.left : readout.left
        anchors.rightMargin: root.editable ? 10 : 12
        height: parent.height
        from: root.from
        to: root.to
        value: root.value
        Accessible.name: root.label
        Keys.onPressed: (event) => {
            if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.ShiftModifier | Qt.MetaModifier))
                return
            let target
            switch (event.key) {
            case Qt.Key_Left:
            case Qt.Key_Down: target = root.stepped(slider.value, -1); break
            case Qt.Key_Right:
            case Qt.Key_Up: target = root.stepped(slider.value, 1); break
            case Qt.Key_PageDown: target = root.stepped(slider.value, -root.pageSteps); break
            case Qt.Key_PageUp: target = root.stepped(slider.value, root.pageSteps); break
            case Qt.Key_Home: target = root.from; break
            case Qt.Key_End: target = root.snap(root.to); break
            default: return
            }
            event.accepted = true
            slider.pressed = true // as Qt's own arrow keys: committed when the key comes up
            if (target !== slider.value) {
                slider.value = target
                root.previewed(target)
            }
        }
        // Held, a key repeats, each repeat after a release: only the last one ends it.
        Keys.onReleased: (event) => { if (event.isAutoRepeat) event.accepted = true }
        onActiveFocusChanged: if (!slider.activeFocus && slider.pressed) slider.pressed = false // left while a key was down
        onMoved: {
            const v = root.snap(slider.value)
            if (v !== slider.value)
                slider.value = v // on a step while dragged; its binding comes back with the commit
            root.previewed(v)
            if (!slider.pressed)
                root.commit()
        }
        onPressedChanged: if (!slider.pressed) root.commit()
        background: Rectangle {
            x: slider.leftPadding + slider.handle.width / 2
            y: (slider.height - height) / 2
            width: slider.availableWidth - slider.handle.width
            height: 4
            radius: Theme.pillRadius(height)
            color: root.rail
            Rectangle {
                width: slider.visualPosition * parent.width
                height: parent.height
                radius: parent.radius
                color: Theme.accentLine
            }
        }
        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: (slider.height - height) / 2
            width: 20
            height: 20
            radius: Theme.pillRadius(height)
            color: Theme.control
            border.width: slider.activeFocus ? 2 : Theme.edge(root.pixelRatio)
            border.color: slider.activeFocus ? Theme.accentLine : Theme.controlBorder
            Rectangle {
                anchors.centerIn: parent
                width: slider.pressed ? 8 : slider.hovered ? 12 : 10
                height: width
                radius: Theme.pillRadius(height)
                color: Theme.accentLine
            }
        }
    }
    Text {
        id: readout
        visible: !root.editable
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: 36
        horizontalAlignment: Text.AlignRight
        text: root.shown(slider.value) + root.unit
        font.pixelSize: Theme.fontBody
        color: Theme.subtext
    }
    InputBox { // the value to type (editable): Enter or leaving it commits it, fitted
        id: box
        objectName: "settingSliderValue"
        visible: root.editable
        anchors.right: parent.right
        anchors.rightMargin: root.unitSpace
        anchors.verticalCenter: parent.verticalCenter
        width: root.boxWidth
        height: Math.min(32, root.height)
        dpr: root.dpr
        tabFocus: true
        horizontalAlignment: TextInput.AlignHCenter
        text: root.shown(slider.value)
        validator: DoubleValidator { notation: DoubleValidator.StandardNotation; decimals: root.decimals }
        Accessible.name: root.label
        onEditingFinished: root.typed(text)
        // The validator lets Enter, and leaving, say so only for a number:
        // left empty (or "-"), the box shows the value again, nothing saved.
        onInputFocusedChanged: if (!inputFocused) root.typed(text)
    }
    Text {
        id: unitText
        visible: root.hasUnit
        anchors.left: box.right
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        text: root.unit
        textFormat: Text.PlainText
        font.pixelSize: Theme.fontBody
        color: Theme.subtext
    }
    TextMetrics { // the box fits the longest of the values it shows
        id: widest
        font.pixelSize: Theme.fontBody
        text: root.shown(root.from).length > root.shown(root.to).length ? root.shown(root.from) : root.shown(root.to)
    }
}
