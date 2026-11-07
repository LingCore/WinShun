import QtQuick
import WinShun
import "../fluent" as Fluent

// A single native Acrylic backdrop; neutral surfaces keep the content legible.
// Text and input stay in the normal scene graph, without texture resampling.
QtObject {
    id: root

    readonly property string name: "liquid"
    readonly property bool dark: SystemTheme.dark
    readonly property bool backdrop: SystemTheme.backdrop && SystemTheme.materials
    readonly property bool backdropCapable: true
    readonly property bool animations: SystemTheme.animations
    readonly property bool dwmEdge: SystemTheme.nativeWindowBorder
    readonly property real windowRadius: SystemTheme.roundedCorners && SystemTheme.nativeWindowBorder ? 8 : 0
    function r(v: real): real { return SystemTheme.roundedCorners ? v : 0 }
    function pill(size: real): real { return r(size / 2) }
    function edge(dpr: real): real { return Math.max(1, Math.round(dpr)) / dpr }
    function hair(dpr: real): real { return 1 / Math.max(1, dpr) }

    readonly property color accent: dark ? Qt.lighter(SystemTheme.accent, 1.65) : SystemTheme.accentFill
    readonly property color accentFill: SystemTheme.accentFill
    readonly property color accentLine: dark ? accent : accentFill
    readonly property color accentText: "#FFFFFF"
    readonly property color accentTint: Qt.rgba(accentLine.r, accentLine.g, accentLine.b, dark ? 0.18 : 0.12)
    readonly property color caret: accentLine
    readonly property color matchColor: accent
    readonly property int matchWeight: Font.Bold

    readonly property color background: dark ? "#202020" : "#F3F3F3"
    readonly property color page: background
    readonly property color text: dark ? "#F5F5F5" : "#1A1A1A"
    readonly property color subtext: dark ? "#C5C5C5" : "#525252"
    readonly property color faint: dark ? "#A3A3A3" : "#686868"
    readonly property color layer: backdrop ? (dark ? Qt.rgba(0.12, 0.12, 0.12, 0.40) : Qt.rgba(1, 1, 1, 0.50))
                                          : dark ? "#272727" : "#FAFAFA"
    readonly property color layerEdge: dark ? Qt.rgba(0, 0, 0, 0.18) : Qt.rgba(0, 0, 0, 0.06)
    readonly property color divider: dark ? Qt.rgba(1, 1, 1, 0.07) : Qt.rgba(0, 0, 0, 0.07)
    readonly property color windowEdge: dark ? "#555555" : "#BEBEBE"
    readonly property color hover: dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.035)
    readonly property color pressed: dark ? Qt.rgba(1, 1, 1, 0.035) : Qt.rgba(0, 0, 0, 0.055)
    readonly property color selection: dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.055)
    readonly property color navHover: hover
    readonly property color navSelected: selection
    readonly property color track: dark ? Qt.rgba(0, 0, 0, 0.16) : Qt.rgba(0, 0, 0, 0.04)
    readonly property color tagFill: dark ? "#3A3A3A" : "#E9E9E9"
    readonly property color chipRest: dark ? "#3B3B3B" : "#E6E6E6"
    readonly property color chipActive: control
    readonly property color chipHover: controlHover
    readonly property color chipBorder: controlBorder
    readonly property color keyEdge: controlBorder

    readonly property color control: dark ? "#383838" : "#FFFFFF"
    readonly property color controlHover: dark ? "#404040" : "#FAFAFA"
    readonly property color controlPressed: dark ? "#323232" : "#F0F0F0"
    readonly property color controlBorder: dark ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(0, 0, 0, 0.14)
    readonly property color inputFocused: dark ? "#252525" : "#FFFFFF"
    readonly property color cardFill: backdrop ? (dark ? Qt.rgba(0.19, 0.19, 0.19, 0.88) : Qt.rgba(1, 1, 1, 0.82))
                                             : dark ? "#303030" : "#FFFFFF"
    readonly property color cardBorder: dark ? Qt.rgba(0, 0, 0, 0.18) : Qt.rgba(0, 0, 0, 0.06)
    // Menus need an opaque reading surface, including when over dense text.
    readonly property color menuBackground: dark ? "#2C2C2C" : "#F9F9F9"
    readonly property color menuHover: dark ? "#3B3B3B" : "#EBEBEB"
    readonly property color menuEdge: controlBorder
    readonly property color menuShadow: Qt.rgba(0, 0, 0, dark ? 0.30 : 0.16)
    readonly property int menuShadowSpread: 8

    readonly property color windowTint: dark ? Qt.rgba(0.12, 0.12, 0.12, 0.42) : Qt.rgba(0.98, 0.98, 0.98, 0.70)
    readonly property color danger: dark ? "#FFABB3" : "#B4232F"
    readonly property color dangerFill: "#B4232F"
    readonly property string iconFont: SystemTheme.iconFont

    readonly property real cardRadius: r(10)
    readonly property real largeCardRadius: r(12)
    readonly property real insetCardRadius: r(8)
    readonly property real cardFocusRadius: r(15)
    readonly property real controlRadius: r(6)
    readonly property real largeControlRadius: r(8)
    readonly property real smallControlRadius: r(4)
    readonly property real lineRadius: r(1)
    readonly property real menuRadius: r(8)
    readonly property real menuItemRadius: r(4)
    readonly property real rowRadius: r(8)
    readonly property real fieldInset: 0
    readonly property real fieldRadius: r(4)
    function fieldKeyInsetAt(dpr: real): real { return edge(dpr) * 4 }
    readonly property real rowInset: 6
    readonly property real pageTabsInset: -12
    readonly property real checkSize: 18
    readonly property int animFast: animations ? 100 : 0
    readonly property int animMove: animations ? 160 : 0

    readonly property Component windowFrame: Component { WindowBody { look: root } }
    readonly property Component searchField: Component { SearchField { look: root } }
    readonly property Component inputField: Component { InputField { look: root } }
    readonly property Component keySurface: Component { KeySurface { look: root } }
    readonly property Component segmented: Component { Segmented { look: root } }
    readonly property Component chipSurface: Component { ChipSurface { look: root } }
    readonly property Component rowHighlight: Component { RowHighlight { look: root } }
    readonly property Component menuFrame: Component { MenuFrame { look: root } }
    readonly property Component card: Component { CardSurface { look: root } }
    readonly property Component toggle: Component { Toggle { look: root } }
    readonly property Component keyHint: Component { Fluent.HintCap { look: root } }
    readonly property Component separator: Component { Fluent.Separator { look: root } }
    readonly property Component scrollBar: Component { Fluent.ScrollThumb { look: root } }
}
