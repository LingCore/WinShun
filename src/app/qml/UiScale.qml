import QtQuick

// The type and heights of one 界面大小 step (Settings uiSize): the launcher
// and the dialog bar read the step Theme follows (Theme.metrics); the
// clipboard has a step of its own (Clipboard.uiSize, ClipboardPage.metrics) and
// adds 密集, a single line per row. Whole pixels in every step, even where
// they can be (whole device pixels at 150 %); 标准 is the scale in Theme.qml.
// Widths, icons (32: one of Windows' own icon sizes at every scaling) and
// 1-pixel lines stay. C++ has the heights it places windows by in UiSize.h.
QtObject {
    property string size: "standard" // dense | compact | standard | roomy

    readonly property bool dense: size === "dense"
    readonly property int level: {
        const at = ["dense", "compact", "standard", "roomy"].indexOf(size)
        return at < 0 ? 2 : at
    }
    readonly property int uiSearch: [18, 20, 24, 28][level]
    readonly property int uiTitle: [14, 16, 18, 20][level]
    readonly property int uiBody: [14, 14, 16, 18][level] // compact: Windows 11's own body text
    readonly property int uiCaption: [12, 12, 14, 16][level]
    readonly property int uiTab: [12, 14, 15, 16][level] // the clipboard's category chips
    readonly property int uiSmall: [12, 12, 12, 14][level] // the Ctrl+G key on the dialog bar
    readonly property int headerHeight: [44, 56, 64, 72][level] // the search field's row
    readonly property int rowHeight: [32, 52, 60, 68][level] // a result or clipboard entry
    readonly property int footerHeight: [32, 36, 42, 48][level]
    readonly property int tabHeight: [24, 28, 32, 36][level] // the launcher's scope tabs
    readonly property int chipHeight: [20, 24, 28, 32][level] // type chips, footer switches, the clipboard's categories (+2)
    readonly property int stripHeight: [32, 36, 40, 44][level] // the type strip
    readonly property int tabsHeight: [32, 40, 44, 48][level] // the clipboard's row of categories
    readonly property int keycapHeight: [18, 20, 22, 24][level]
    readonly property int badgeHeight: [20, 22, 24, 26][level] // "应用", a group's name
    readonly property int labelRowHeight: [20, 24, 28, 32][level] // headings between the rows
    readonly property int hintRowHeight: [28, 32, 36, 40][level]
    readonly property int fileRowHeight: [30, 36, 40, 44][level] // the clipboard preview's list of files
    readonly property int panelRowHeight: [30, 36, 42, 48][level] // the search panel's rows, one line each
    readonly property int searchIconSize: [18, 20, 22, 24][level]
}
