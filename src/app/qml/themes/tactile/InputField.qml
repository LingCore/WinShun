pragma ComponentBehavior: Bound
import QtQuick

// A form input: a smaller groove (radius 6) with a light cut line under its
// bottom edge so it reads on plates too. Focused: a 1 px neutral rim (no
// blue); error: the rim in the danger colour; disabled: flat, no shade.
Item {
    id: input

    property bool focused: false
    property bool hovered: false
    property bool error: false
    property real dpr: 1
    property real corner: -1
    readonly property real radius: corner >= 0 ? corner : look ? look.keyRadius : 0

    // for the host
    readonly property color textColor: !look ? "transparent" : enabled ? look.ink : look.ink3
    readonly property real disabledOpacity: 1 // disabled ink and flat shading already distinguish this state
    readonly property color placeholderColor: look ? look.ink3 : "transparent"
    readonly property color caretColor: look ? look.caret : "transparent"

    property QtObject look // injected by Look.qml
    readonly property real hair: look ? look.hair(dpr) : 1

    // Built once `look` is set: a part's own bindings can run before Look's
    // `look: root` (Hook makes it with createObject).
    Loader {
        anchors.fill: parent
        active: input.look !== null
        sourceComponent: Item {
            // the light lip under the cut
            Rectangle {
                y: input.hair
                width: input.width
                height: input.height
                radius: input.radius
                color: (input.look?.seamLight ?? Qt.rgba(0, 0, 0, 0))
            }
            Groove {
                anchors.fill: parent
                radius: input.radius
                dpr: input.dpr
                look: input.look
                tone: input.enabled ? (input.focused || input.hovered ? Qt.tint((input.look?.slot ?? Qt.rgba(0, 0, 0, 0)), (input.look?.dark ?? false) ? Qt.rgba(1, 1, 1, 0.02) : Qt.rgba(1, 1, 1, 0.35)) : (input.look?.slot ?? Qt.rgba(0, 0, 0, 0)))
                                    : (input.look?.panelBottom ?? Qt.rgba(0, 0, 0, 0))
                shade: input.enabled ? (input.look?.slotShade ?? Qt.rgba(0, 0, 0, 0)) : "transparent"
                fade: input.enabled ? (input.look?.slotFade ?? Qt.rgba(0, 0, 0, 0)) : "transparent"
            }
            Rectangle {
                visible: input.enabled && (input.focused || input.error)
                anchors.fill: parent
                radius: input.radius
                color: "transparent"
                border.width: input.hair
                border.color: input.error ? (input.look?.dangerInk ?? Qt.rgba(0, 0, 0, 0)) : Qt.rgba((input.look?.ink3 ?? Qt.rgba(0, 0, 0, 0)).r, (input.look?.ink3 ?? Qt.rgba(0, 0, 0, 0)).g, (input.look?.ink3 ?? Qt.rgba(0, 0, 0, 0)).b, 0.6)
            }
        }
    }
}
