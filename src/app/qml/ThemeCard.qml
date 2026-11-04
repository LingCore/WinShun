pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// One choice of the theme picker: a small picture of the launcher in that
// theme and its name under it. "system" shows light and dark side by side.
// With `look` (a 界面风格's Look, Theme.lookFor), a choice of the style
// picker: the launcher as that style draws it (its colours, radii and parts,
// whatever style is in effect), in the theme in effect.
Item {
    id: card

    property string mode: "light" // light | dark | system
    property string label
    property bool selected: false
    property QtObject look: null
    // Read once for the picture and its parts (GitHub #111; docs/pitfalls.md).
    readonly property real dpr: Screen.devicePixelRatio

    signal activated()

    implicitWidth: 176
    implicitHeight: frame.height + caption.height
    activeFocusOnTab: true

    Keys.onSpacePressed: card.activated()
    Keys.onReturnPressed: card.activated()
    Keys.onEnterPressed: card.activated()

    // The launcher in miniature for a theme (light, dark): search box, three
    // results (the first one selected), a status line. These colours are
    // fixed on purpose: the picture shows the theme, not the one in effect
    // (a style's Look has only the one in effect), so the picker can show
    // light and dark side by side whichever is current.
    component Preview: Rectangle {
        id: preview

        property bool dark: false
        property real line: 1 // one device pixel, for the search box's edge
        readonly property color accent: dark ? Qt.lighter(SystemTheme.accent, 1.55) : SystemTheme.accent
        readonly property color ink: dark ? "#5A5A5A" : "#CFCFCF"
        readonly property color faintInk: dark ? "#3A3A3A" : "#E3E3E3"

        color: dark ? "#202020" : "#F9F9F9"

        Rectangle { // search box
            x: parent.width * 0.08
            y: parent.height * 0.1
            width: parent.width * 0.84
            height: parent.height * 0.17
            radius: Theme.pillRadius(height)
            color: preview.dark ? "#2B2B2B" : "#FFFFFF"
            border.width: preview.line
            border.color: preview.dark ? "#3A3A3A" : "#E2E2E2"

            Rectangle { // caret
                x: parent.height * 0.6
                anchors.verticalCenter: parent.verticalCenter
                width: 1.5
                height: parent.height * 0.5
                color: preview.accent
            }
            Rectangle { // typed text
                x: parent.height * 0.6 + 5
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width * 0.3
                height: 3
                radius: Theme.smallRadius
                color: preview.ink
            }
        }

        Column {
            x: parent.width * 0.08
            y: parent.height * 0.34
            width: parent.width * 0.84
            spacing: parent.height * 0.035

            Repeater {
                model: 3
                delegate: Rectangle {
                    id: result
                    required property int index
                    width: parent.width
                    height: preview.height * 0.15
                    radius: Theme.smallControlRadius
                    color: index !== 0 ? "transparent" : preview.dark ? "#2F2F2F" : "#EAEAEA"

                    Rectangle { // selection mark
                        visible: result.index === 0
                        x: 1
                        anchors.verticalCenter: parent.verticalCenter
                        width: 2
                        height: parent.height * 0.5
                        radius: Theme.lineRadius
                        color: preview.accent
                    }
                    Rectangle { // icon
                        x: parent.height * 0.35
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.height * 0.55
                        height: width
                        radius: Theme.smallRadius
                        color: result.index === 0 ? preview.accent : preview.ink
                        opacity: result.index === 0 ? 0.85 : 1
                    }
                    Rectangle { // name
                        x: parent.height * 1.2
                        y: parent.height * 0.28
                        width: parent.width * (0.5 - result.index * 0.08)
                        height: 3
                        radius: Theme.smallRadius
                        color: preview.ink
                    }
                    Rectangle { // folder
                        x: parent.height * 1.2
                        y: parent.height * 0.6
                        width: parent.width * (0.32 + result.index * 0.05)
                        height: 2
                        radius: Theme.lineRadius
                        color: preview.faintInk
                    }
                }
            }
        }

        Rectangle { // status line
            x: parent.width * 0.08
            y: parent.height * 0.88
            width: parent.width * 0.26
            height: 2
            radius: Theme.lineRadius
            color: preview.faintInk
        }
    }

    // The launcher drawn as a 界面风格 draws it: at its real size (640 wide),
    // with the style's own parts (the search field, the selected row, the
    // file icons, the key caps, the scope keys, the results tray), and then
    // scaled down by the card, so that a style's shapes, edges, shadows and
    // lamps are the ones the launcher has. Every value comes from `look`; a
    // thing it leaves out is fluent's (Theme.base).
    component LauncherScene: Item {
        id: scene

        property QtObject look
        // Device pixels per pixel of the scene: the card's, over its scale.
        property real dpr: 1
        readonly property real corner: look.windowRadius
        readonly property real headerHeight: 56
        readonly property real footerHeight: 36
        readonly property real rowHeight: 60
        readonly property real bodyTop: headerHeight + 1
        readonly property real rowInset: value("rowInset")
        readonly property real fieldInset: value("fieldInset")

        // A token of the style's, or fluent's.
        function value(name: string): var {
            const own = look[name]
            return own === undefined ? Theme.base[name] : own
        }
        // A part of the style's: `typeIcon` and `tray` may be null in it,
        // and stay so (fluent's own are null too).
        function part(name: string): var {
            const own = look[name]
            return own === undefined ? Theme.base[name] : own
        }

        Rectangle { // the window
            anchors.fill: parent
            radius: scene.corner
            color: scene.look.background
        }

        Hook { // under the rows (a style may sink them into a tray)
            y: scene.bodyTop
            width: parent.width
            height: parent.height - scene.footerHeight - y
            part: scene.part("tray")
            dpr: scene.dpr
        }

        Rectangle { // the chrome: the search row
            width: parent.width
            height: scene.headerHeight
            color: scene.look.layer
            topLeftRadius: scene.corner
            topRightRadius: scene.corner
        }
        Rectangle { // where the chrome meets the rows
            y: scene.headerHeight
            width: parent.width
            height: Theme.base.edge(scene.dpr)
            color: scene.look.layerEdge
        }

        Hook { // the search field, focused
            id: field
            y: scene.fieldInset
            x: scene.fieldInset
            width: parent.width - 2 * scene.fieldInset
            height: scene.headerHeight - 2 * scene.fieldInset
            part: scene.part("searchField")
            flat: true
            focused: true
            corner: scene.value("fieldRadius")
            dpr: scene.dpr
        }
        Image {
            x: scene.fieldInset + 18
            y: scene.fieldInset + (field.height - height) / 2
            width: 24
            height: 24
            source: "qrc:/branding/logo.svg"
            sourceSize.width: width
            sourceSize.height: height
        }
        Rectangle { // the caret
            x: scene.fieldInset + 54
            y: scene.fieldInset + (field.height - height) / 2
            width: 2
            height: 24
            color: scene.look.accentLine
        }
        Rectangle { // what is typed
            x: scene.fieldInset + 62
            y: scene.fieldInset + (field.height - height) / 2
            width: 150
            height: 10
            radius: Theme.base.pill(height)
            color: scene.look.subtext
            opacity: 0.55
        }
        Item { // the scopes, as keys in the field (全部 文件 内容)
            id: scopes
            readonly property real keyHeight: Math.min(32, field.height - 12)
            x: scene.width - scene.fieldInset - width - Math.max(10, scene.fieldInset + 6)
            y: scene.fieldInset + (field.height - keyHeight) / 2
            width: 150
            height: keyHeight

            Hook {
                id: scopePart
                anchors.fill: parent
                part: scene.part("segmented")
                count: 3
                currentIndex: 0
                segments: [{ x: 0, width: 50 }, { x: 50, width: 50 }, { x: 100, width: 50 }]
                role: "scope"
                dpr: scene.dpr
            }
            Repeater {
                model: 3
                delegate: Rectangle {
                    id: scopeLabel
                    required property int index
                    x: scopePart.call("segmentX", [index], index * 50) + (50 - width) / 2
                         + (index === 0 ? scopePart.out("leadingInset", 0) / 2 : 0)
                    y: (parent.height - height) / 2 + scopePart.call("labelOffsetY", [index], 0)
                    width: 22
                    height: 8
                    radius: Theme.base.pill(height)
                    color: scopePart.call("labelColor", [index], index === 0 ? scene.look.text : scene.look.subtext)
                }
            }
        }

        Repeater { // three results, the first one selected
            model: 3

            delegate: Item {
                id: result

                required property int index
                readonly property bool selected: index === 0

                y: scene.bodyTop + 4 + index * scene.rowHeight
                width: scene.width
                height: scene.rowHeight

                Hook {
                    x: scene.rowInset
                    width: parent.width - 2 * scene.rowInset
                    height: parent.height
                    part: result.selected ? scene.part("rowHighlight") : null
                    selected: true
                    role: "list"
                    dpr: scene.dpr
                }
                Item { // the file's icon: the style's, or a plain tile where it has none (fluent shows the system's)
                    x: scene.value("iconInset")
                    y: (parent.height - height) / 2
                    width: 32
                    height: 32

                    Hook {
                        anchors.fill: parent
                        part: scene.part("typeIcon")
                        kind: result.index === 0 ? "folder" : result.index === 1 ? "doc" : "image"
                        size: 32
                        hueIndex: result.index === 0 ? -2 : result.index - 1
                        dpr: scene.dpr
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 3
                        visible: scene.part("typeIcon") === null
                        radius: Theme.base.smallControlRadius * 2
                        color: Theme.base.hueFor(result.index === 0 ? -2 : result.index - 1)
                    }
                }
                Rectangle { // name
                    x: scene.value("textStart")
                    y: 14
                    width: 190 - result.index * 40
                    height: 12
                    radius: Theme.base.pill(height)
                    color: scene.look.text
                    opacity: 0.8
                }
                Rectangle { // folder
                    x: scene.value("textStart")
                    y: 36
                    width: 280 - result.index * 40
                    height: 8
                    radius: Theme.base.pill(height)
                    color: scene.look.faint
                    opacity: 0.8
                }
            }
        }

        Rectangle { // the footer
            y: parent.height - scene.footerHeight
            width: parent.width
            height: scene.footerHeight
            color: scene.look.layer
            bottomLeftRadius: scene.corner
            bottomRightRadius: scene.corner
        }
        Rectangle {
            y: parent.height - scene.footerHeight - height
            width: parent.width
            height: Theme.base.edge(scene.dpr)
            color: scene.look.layerEdge
        }
        Repeater { // two key hints
            model: 2

            delegate: Item {
                id: hint

                required property int index

                x: 18 + index * 110
                y: scene.height - scene.footerHeight + (scene.footerHeight - height) / 2
                width: index === 0 ? 50 : 86
                height: 22

                Hook {
                    id: cap
                    width: 44
                    height: parent.height
                    part: scene.part("keyHint")
                    dpr: scene.dpr
                }
                Rectangle {
                    x: (cap.width - width) / 2
                    y: (parent.height - height) / 2 + cap.out("contentOffsetY", 0)
                    width: hint.index === 0 ? 24 : 30
                    height: 8
                    radius: Theme.base.pill(height)
                    color: cap.out("foreground", scene.look.subtext)
                    opacity: 0.7
                }
                Rectangle {
                    x: cap.width + 8
                    y: (parent.height - height) / 2
                    width: hint.index === 0 ? 30 : 36
                    height: 8
                    radius: Theme.base.pill(height)
                    color: scene.look.faint
                    opacity: 0.7
                }
            }
        }
    }

    // One device pixel border, as the style's edges are drawn.
    readonly property real edgeWidth: Theme.edge(dpr)

    Rectangle {
        id: frame
        width: card.width
        height: Math.round(card.width * 0.64)
        radius: Theme.largeCardRadius
        color: "transparent"
        border.width: card.edgeWidth // neutral 1 px edges only; the selection shows in the radio button below
        border.color: card.selected || area.containsMouse ? Theme.subtext : Theme.cardBorder

        Behavior on border.color { ColorAnimation { duration: 120 } }

        Item {
            id: pictures
            anchors.fill: parent
            anchors.margins: 4
            clip: true
            scale: area.pressed ? 0.98 : 1

            Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }

            LauncherScene { // a style's choice: the launcher as that style draws it
                readonly property real fit: pictures.width / 640

                visible: card.look !== null
                width: 640
                height: pictures.height / fit
                scale: fit
                transformOrigin: Item.TopLeft
                look: card.look ?? Theme.base
                dpr: card.dpr * fit
            }
            Preview { // a theme's choice: light, dark, or both
                visible: card.look === null
                width: pictures.width
                height: pictures.height
                radius: Theme.insetCardRadius
                dark: card.mode === "dark"
                line: card.edgeWidth
            }
            Item { // "system": the right half dark
                visible: card.mode === "system" && !card.look
                x: pictures.width / 2
                width: pictures.width / 2
                height: pictures.height
                clip: true

                Preview {
                    x: -pictures.width / 2
                    width: pictures.width
                    height: pictures.height
                    radius: Theme.insetCardRadius
                    dark: true
                    line: card.edgeWidth
                }
            }
        }

        Rectangle { // tick in the corner of the chosen one
            visible: card.selected
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 9
            width: 20
            height: 20
            radius: Theme.pillRadius(width)
            color: Theme.accentFill

            Glyph {
                anchors.centerIn: parent
                glyph: "" // CheckMark
                size: 11
                color: Theme.accentText
            }
        }

        Rectangle { // keyboard focus
            visible: card.activeFocus
            anchors.fill: parent
            anchors.margins: -4
            radius: Theme.cardFocusRadius
            color: "transparent"
            border.width: 2
            border.color: Theme.text
        }
    }

    Row {
        id: caption
        y: frame.height
        height: 36
        spacing: 8

        Rectangle { // radio button
            anchors.verticalCenter: parent.verticalCenter
            width: 18
            height: 18
            radius: Theme.pillRadius(width)
            color: card.selected ? Theme.accentFill : "transparent"
            border.width: card.selected ? 0 : card.edgeWidth
            border.color: Theme.subtext

            Rectangle {
                visible: card.selected
                anchors.centerIn: parent
                width: area.containsMouse ? 9 : 8
                height: width
                radius: Theme.pillRadius(width)
                color: Theme.accentText
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: card.width - 26
            text: card.label
            color: Theme.text
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: card.activated()
    }
}
