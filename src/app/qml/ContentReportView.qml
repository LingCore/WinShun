pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import WinShun

ScrollView {
    id: root
    required property ContentPane pane
    readonly property bool selectedOnly: pane.reportOptions.scope === 1
    readonly property bool allFiles: pane.reportOptions.scope === 2
    readonly property bool includeFiles: pane.reportOptions.files
    readonly property bool includeMatches: pane.reportOptions.matches
    readonly property bool context: pane.reportOptions.context
    property int format: 0
    contentWidth: availableWidth
    font.pixelSize: Theme.uiBody
    palette.window: Theme.background
    palette.windowText: Theme.text
    palette.base: Theme.control
    palette.text: Theme.text
    palette.button: Theme.control
    palette.buttonText: Theme.text
    palette.mid: Theme.controlBorder
    palette.highlight: Theme.accentFill
    palette.highlightedText: Theme.accentText

    component ReportChoice: ComboBox {
        id: choice
        implicitHeight: 34
        background: Hook { // the style's field box (Theme.inputField)
            part: Theme.inputField
            hovered: choice.hovered
            focused: choice.activeFocus
            dpr: Screen.devicePixelRatio
        }
    }
    component ReportCheck: CheckBox {
        implicitHeight: 32
        spacing: 8
    }
    ColumnLayout {
        width: root.availableWidth
        spacing: 6
        Text {
            Layout.fillWidth: true; Layout.margins: 12
            text: qsTr("按当前搜索生成报告快照，完成后可复制、打印或保存。完整搜索会继续读取其余候选文件。")
            wrapMode: Text.Wrap; color: Theme.subtext; font.pixelSize: Theme.uiBody
        }
        ReportChoice {
            Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
            model: [qsTr("当前结果"), qsTr("所选文件（未多选时为当前文件）"), qsTr("完整搜索")]
            enabled: !root.pane.reporting
            currentIndex: root.pane.reportOptions.scope
            onActivated: index => root.pane.setReportOptions(index, root.includeFiles, root.includeMatches, root.context)
            Accessible.name: qsTr("报告范围")
        }
        ReportCheck { Layout.leftMargin: 8; text: qsTr("文件列表"); checked: root.includeFiles; onToggled: root.pane.setReportOptions(root.pane.reportOptions.scope, checked, root.includeMatches, root.context); enabled: !root.pane.reporting }
        ReportCheck { Layout.leftMargin: 8; text: qsTr("每处匹配"); checked: root.includeMatches; onToggled: root.pane.setReportOptions(root.pane.reportOptions.scope, root.includeFiles, checked, root.context); enabled: !root.pane.reporting }
        ReportCheck { Layout.leftMargin: 8; text: qsTr("包含上下文"); checked: root.context; onToggled: root.pane.setReportOptions(root.pane.reportOptions.scope, root.includeFiles, root.includeMatches, checked); enabled: !root.pane.reporting && root.includeMatches }
        FlatButton {
            objectName: "prepareContentReport"
            Layout.leftMargin: 12
            text: root.pane.reporting ? qsTr("取消") : qsTr("生成报告")
            enabled: root.pane.reporting || root.includeFiles || root.includeMatches
            onClicked: root.pane.reporting ? root.pane.cancelReport()
                : root.pane.prepareReport(root.selectedOnly, root.allFiles, root.includeFiles, root.includeMatches, root.context)
            Accessible.role: Accessible.Button; Accessible.name: text
        }
        Text {
            Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
            text: root.pane.reportStatus
            textFormat: Text.PlainText; wrapMode: Text.Wrap
            color: Theme.subtext; font.pixelSize: Theme.uiBody
        }
        TextEdit {
            Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
            visible: root.pane.reportReady
            text: root.pane.reportPreview
            textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap
            readOnly: true; selectByMouse: true
            color: Theme.text; font.pixelSize: Theme.uiBody
            selectionColor: Theme.textSelection; selectedTextColor: Theme.text
        }
        ReportChoice {
            Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
            model: [qsTr("文本 (.txt)"), "CSV (.csv)", "HTML (.html)", "Excel (.xlsx)"]
            onActivated: index => root.format = index
            Accessible.name: qsTr("导出格式")
        }
        Flow {
            Layout.fillWidth: true; Layout.margins: 12
            spacing: 6
            enabled: root.pane.reportReady && !root.pane.reporting
            FlatButton { text: qsTr("复制"); onClicked: root.pane.copyReport(); Accessible.role: Accessible.Button; Accessible.name: text }
            FlatButton { text: qsTr("打印…"); onClicked: root.pane.printReport(); Accessible.role: Accessible.Button; Accessible.name: text }
            FlatButton { text: qsTr("保存…"); onClicked: root.pane.saveReport(root.format); Accessible.role: Accessible.Button; Accessible.name: text }
        }
    }
}
