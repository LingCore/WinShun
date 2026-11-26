pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import WinShun

// 公式识别 (Shift+F on a capture or a pin, CaptureFormula): the picture, the
// formula drawn from the LaTeX read in it, and that LaTeX to be corrected,
// the drawing following each change. 复制 puts it on the clipboard in the
// format chosen at the right (kept for next time): LaTeX, Markdown, Word's
// equation (MathML) or Typst; 导出 as a picture. Beside the picture, one
// window used again each time. Ctrl+Enter copies and closes, Esc closes.
Window {
    id: window

    required property CaptureFormula formula
    required property WindowFrame frame

    readonly property int padding: 16
    readonly property real dpr: Screen.devicePixelRatio
    readonly property bool reading: formula.model === "ready" && formula.busy
    readonly property bool hasFormula: formula.model === "ready" && !formula.busy && formula.message.length === 0
    readonly property var formats: [
        {value: "latex", title: "LaTeX", caption: qsTr("论文、公式编辑器")},
        {value: "markdown", title: "Markdown", caption: qsTr("笔记软件，$$ 包围")},
        {value: "word", title: qsTr("Word 公式"), caption: qsTr("粘贴到 Word 即为公式")},
        {value: "typst", title: "Typst", caption: qsTr("Typst 文档")}
    ]
    readonly property string formatTitle: formats.find(f => f.value === formula.format)?.title ?? "LaTeX"
    property string notice: ""
    property bool noticeFailed: false

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: Theme.backdrop && SystemTheme.materials ? "transparent" : Theme.background
    title: qsTr("公式识别") // Qt adds " - Win顺"
    width: 680
    height: 560

    function copyAndClose() {
        if (!window.hasFormula || source.text.trim().length === 0)
            return
        window.formula.copy(source.text)
        window.formula.close()
    }

    Timer {
        id: noticeTimer
        interval: 2000
        onTriggered: window.notice = ""
    }

    Connections {
        target: window.formula
        // A new picture, or its reading: the field takes what was read, over what was edited.
        function onChanged() {
            window.notice = ""
            source.text = window.formula.busy ? "" : window.formula.latex
            source.cursorPosition = 0
            if (window.hasFormula)
                source.forceActiveFocus()
        }
        function onNoticed(text: string, failed: bool) {
            window.notice = text
            window.noticeFailed = failed
            noticeTimer.restart()
        }
    }

    Item {
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: window.formula.close()
        Keys.onPressed: (event) => {
            if ((event.modifiers & Qt.ControlModifier) && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                window.copyAndClose()
                event.accepted = true
            }
        }
    }

    // ---- the header: what this is, what became of the last copy, close ---------------------

    Item {
        id: header
        width: parent.width
        height: Theme.headerHeight
        Component.onCompleted: window.frame.addDragArea(header)

        Row {
            x: window.padding
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("公式识别")
                color: Theme.text
                font.pixelSize: Theme.uiTitle
                font.weight: Theme.fontStrong
            }
            Row {
                objectName: "formulaNotice"
                visible: window.notice.length > 0
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                Glyph {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: window.noticeFailed ? "warning" : "check"
                    size: 14
                    color: window.noticeFailed ? Theme.danger : Theme.success
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: window.notice
                    color: window.noticeFailed ? Theme.danger : Theme.success
                    font.pixelSize: Theme.uiCaption
                }
            }
        }
        FlatButton {
            id: closeButton
            dpr: window.dpr
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            flat: true
            width: 32
            height: 32
            glyph: "" // ChromeClose
            Accessible.name: qsTr("关闭")
            onClicked: window.formula.close()
            Component.onCompleted: window.frame.addControl(closeButton)
        }
        Hook { // the style's separator
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.edge(window.dpr)
            part: Theme.separator
            orientation: Qt.Horizontal
            dpr: window.dpr
        }
    }

    // ---- the picture, the formula, its LaTeX | the format to copy in ------------------------

    RowLayout {
        id: content
        anchors.top: header.bottom
        anchors.topMargin: 12
        anchors.left: parent.left
        anchors.leftMargin: window.padding
        anchors.right: parent.right
        anchors.rightMargin: window.padding
        anchors.bottom: footer.top
        spacing: 16

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 6

            SectionLabel { text: qsTr("原始图像") }
            Panel {
                Layout.fillWidth: true
                Layout.preferredHeight: 88
                PictureView {
                    objectName: "formulaPicture"
                    anchors.fill: parent
                    anchors.margins: 8
                    image: window.formula.picture
                }
            }

            SectionLabel {
                Layout.topMargin: 6
                text: qsTr("识别结果")
            }
            Panel {
                Layout.fillWidth: true
                // Taller when what the model setup says takes more lines (a longer language, larger text).
                Layout.preferredHeight: Math.max(120, modelSetup.visible ? modelSetup.implicitHeight + 24 : 0)

                FormulaView {
                    id: drawing
                    objectName: "formulaDrawing"
                    anchors.fill: parent
                    anchors.margins: 12
                    visible: window.hasFormula
                    latex: source.text
                    color: Theme.text
                    fontSize: 24
                }
                Text { // reading, nothing found, or the formula as written cannot be drawn
                    anchors.centerIn: parent
                    width: parent.width - 32
                    visible: window.formula.model === "ready" && (!window.hasFormula || drawing.error.length > 0
                                                                  || source.text.trim().length === 0)
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: window.reading ? qsTr("正在识别…")
                        : window.formula.message.length > 0 ? window.formula.message
                        : source.text.trim().length === 0 ? qsTr("在下面输入 LaTeX，这里显示公式。")
                        : qsTr("公式有误，暂时无法显示，请检查括号是否成对。")
                    color: Theme.subtext
                    font.pixelSize: Theme.uiBody
                }
                ModelSetup {
                    id: modelSetup
                    formula: window.formula
                    anchors.centerIn: parent
                    width: parent.width - 32
                    visible: window.formula.model !== "ready"
                }
            }
            Row { // a reading the model was not sure of, to be checked
                objectName: "formulaUnsure"
                visible: window.hasFormula && window.formula.unsure
                spacing: 4
                Glyph {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "warning"
                    size: 14
                    color: Theme.caution
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("可能不准确，请对照原图检查")
                    color: Theme.subtext
                    font.pixelSize: Theme.uiCaption
                }
            }

            SectionLabel {
                Layout.topMargin: 6
                text: qsTr("LaTeX 源码")
            }
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 64

                Hook { // the style's input field
                    id: sourcePart
                    anchors.fill: parent
                    part: Theme.inputField
                    focused: source.activeFocus
                    hovered: sourceHover.hovered
                    dpr: window.dpr
                }
                HoverHandler { id: sourceHover }

                Flickable {
                    id: flick
                    anchors.fill: parent
                    anchors.margins: 10
                    anchors.rightMargin: 14 // room for the scroll bar
                    clip: true
                    contentWidth: width
                    contentHeight: source.height
                    boundsBehavior: Flickable.StopAtBounds

                    function ensureVisible(r) {
                        if (contentY >= r.y)
                            contentY = r.y
                        else if (contentY + height <= r.y + r.height)
                            contentY = r.y + r.height - height
                    }

                    TextEdit {
                        id: source
                        objectName: "formulaSource"
                        width: flick.width
                        height: Math.max(contentHeight, flick.height) // a click below short text still lands in it
                        enabled: window.hasFormula
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.WrapAnywhere
                        selectByMouse: true
                        persistentSelection: true
                        color: sourcePart.out("textColor", Theme.text)
                        selectionColor: Theme.textSelection
                        selectedTextColor: Theme.text
                        font.family: "Consolas"
                        font.pixelSize: Theme.uiBody
                        Accessible.name: qsTr("LaTeX 源码")
                        onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)
                        Keys.onEscapePressed: (event) => {
                            if (source.inputMethodComposing) // Esc while composing belongs to the input method
                                event.accepted = false
                            else
                                window.formula.close()
                        }
                        Keys.onPressed: (event) => {
                            if (!source.inputMethodComposing && (event.modifiers & Qt.ControlModifier)
                                && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                                window.copyAndClose()
                                event.accepted = true
                            }
                        }
                    }
                }

                ListScrollBar {
                    dpr: window.dpr
                    flickable: flick
                    visible: flick.contentHeight > flick.height
                    anchors.right: parent.right
                    anchors.top: flick.top
                    anchors.bottom: flick.bottom
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: false // a layout fills by default: the left one takes the rest
            Layout.preferredWidth: 184
            Layout.fillHeight: true
            spacing: 6

            SectionLabel { text: qsTr("复制格式") }
            Column {
                Layout.fillWidth: true
                spacing: 2
                Accessible.role: Accessible.Grouping
                Accessible.name: qsTr("复制格式")

                Repeater {
                    model: window.formats
                    delegate: FormatChoice {
                        required property var modelData
                        width: parent.width
                        formula: window.formula
                        value: modelData.value
                        title: modelData.title
                        caption: modelData.caption
                    }
                }
            }
            Item { Layout.fillHeight: true }
        }
    }

    // ---- what to do with it -----------------------------------------------------------------

    Row {
        id: footer
        anchors.right: parent.right
        anchors.rightMargin: window.padding
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        topPadding: 12
        spacing: 8

        FlatButton {
            id: exportButton
            objectName: "formulaExport"
            dpr: window.dpr
            text: qsTr("导出")
            trailingGlyph: String.fromCharCode(0xE70D) // ChevronDown
            enabled: window.hasFormula && drawing.error.length === 0 && source.text.trim().length > 0
            onClicked: exportMenu.open = !exportMenu.open

            DropMenu {
                id: exportMenu
                anchor: exportButton
                menuWidth: 180
                entries: [
                    {text: qsTr("复制为图片"), glyph: "ph:copy"},
                    {text: qsTr("保存为 PNG…"), glyph: "ph:file-png"},
                    {text: qsTr("保存为 SVG…"), glyph: "ph:file-svg"}
                ]
                onActivated: (i) => window.formula.exportPicture(source.text, ["copy", "png", "svg"][i], window)
            }
        }
        FlatButton {
            objectName: "formulaCopy"
            dpr: window.dpr
            highlighted: true
            text: qsTr("复制 %1").arg(window.formatTitle)
            enabled: window.hasFormula && source.text.trim().length > 0
            onClicked: window.formula.copy(source.text)
        }
    }

    WindowEdge {
        fill: Theme.background
    }

    // ---- its parts ----------------------------------------------------------------------------

    component SectionLabel: Text {
        color: Theme.subtext
        font.pixelSize: Theme.uiCaption
    }

    component Panel: Rectangle {
        color: Theme.card
        border.color: Theme.cardBorder
        border.width: Theme.edge(Screen.devicePixelRatio)
        radius: Theme.cardRadius
    }

    // One of the formats: a radio button with what it is for under its name.
    component FormatChoice: Item {
        id: choice
        required property CaptureFormula formula
        required property string value
        required property string title
        required property string caption
        readonly property bool chosen: formula.format === value

        height: 48
        activeFocusOnTab: true
        Accessible.role: Accessible.RadioButton
        Accessible.name: title
        Accessible.description: caption
        Accessible.checked: chosen
        Keys.onSpacePressed: choice.formula.format = choice.value

        Rectangle {
            anchors.fill: parent
            radius: Theme.controlRadius
            color: area.pressed ? Theme.pressed : area.containsMouse || choice.activeFocus ? Theme.hover : "transparent"
        }
        Glyph {
            id: mark
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            iconName: choice.chosen ? "radio-button" : "circle"
            filled: choice.chosen
            size: 18
            color: choice.chosen ? Theme.accent : Theme.subtext
        }
        Column {
            anchors.left: mark.right
            anchors.leftMargin: 8
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 1
            Text {
                width: parent.width
                text: choice.title
                color: Theme.text
                font.pixelSize: Theme.uiBody
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: choice.caption
                color: Theme.subtext
                font.pixelSize: Theme.uiCaption
                elide: Text.ElideRight
            }
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: choice.formula.format = choice.value
        }
    }

    // Before the first reading: the model to fetch (Win顺 fetches it), its progress, or why not.
    component ModelSetup: Column {
        id: setup
        required property CaptureFormula formula
        spacing: 12
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: setup.formula.model === "update" ? qsTr("这个 Win顺 版本还不能下载公式识别模型，请先更新 Win顺。")
                : setup.formula.model === "downloading" ? qsTr("正在下载公式识别模型… %1%").arg(Math.floor(setup.formula.progress * 100))
                : setup.formula.modelStatus.length > 0 ? setup.formula.modelStatus
                : qsTr("公式识别需要先下载识别模型（%1），之后在本机识别，图片不会上传。")
                    .arg(setup.formula.modelSize.length > 0 ? setup.formula.modelSize : qsTr("约 120 MB"))
            color: setup.formula.modelStatus.length > 0 && setup.formula.model === "missing" ? Theme.danger : Theme.subtext
            font.pixelSize: Theme.uiBody
        }
        Rectangle {
            visible: setup.formula.model === "downloading"
            width: parent.width
            height: 4
            radius: Theme.progressRadius
            color: Theme.track
            Rectangle {
                width: parent.width * setup.formula.progress
                height: parent.height
                radius: parent.radius
                color: Theme.accent
            }
        }
        FlatButton {
            objectName: "formulaModelDownload"
            anchors.horizontalCenter: parent.horizontalCenter
            dpr: Screen.devicePixelRatio
            visible: setup.formula.model === "missing" || setup.formula.model === "downloading"
            highlighted: setup.formula.model === "missing"
            text: setup.formula.model === "downloading" ? qsTr("取消")
                : setup.formula.modelStatus.length > 0 ? qsTr("重试") : qsTr("下载模型")
            onClicked: setup.formula.model === "downloading" ? setup.formula.cancelDownload() : setup.formula.downloadModel()
        }
    }
}
