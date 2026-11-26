pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// 公式识别's model (ws::FormulaModel) under 增强文字识别: downloaded on
// request (the OCR package with it when that is not there yet), from a folder
// of the files fetched by hand, removable. The screenshot window downloads
// it too, on Shift+F's first use.
SettingRow {
    id: row

    required property FormulaModel formulaModel
    objectName: "formulaModelRow"
    sub: true
    title: qsTr("公式识别模型")
    keywords: qsTr("公式;数学;LaTeX;MathML;Word;Typst;识别;模型;下载;formula;math;equation").split(";")
    description: formulaModel.status.length > 0 ? formulaModel.status
        : formulaModel.installed ? qsTr("已安装：截图或贴图后按 Shift + F，识别图中的公式为 LaTeX，可复制为 Word 公式、Markdown 或 Typst。")
        : qsTr("截图或贴图后按 Shift + F 识别图中的公式。需下载%1，可随时移除；识别在本机进行，图片不会上传。").arg(formulaModel.downloadSize)
    bodyShown: formulaModel.busy || !formulaModel.installed

    FlatButton {
        dpr: row.dpr
        objectName: "formulaModelButton"
        text: row.formulaModel.installed ? qsTr("移除") : row.formulaModel.busy ? qsTr("取消") : qsTr("下载并安装")
        onClicked: row.formulaModel.installed ? row.formulaModel.remove()
                 : row.formulaModel.busy ? row.formulaModel.cancel() : row.formulaModel.download()
    }

    body: [
        Rectangle {
            visible: row.formulaModel.busy
            width: parent.width
            height: 4
            radius: Theme.progressRadius
            color: Theme.track
            Rectangle {
                width: parent.width * row.formulaModel.progress
                height: parent.height
                radius: parent.radius
                color: Theme.accent
            }
        },
        FlatButton {
            dpr: row.dpr
            objectName: "installLocalFormulaModel"
            visible: !row.formulaModel.installed
            text: qsTr("从本地安装模型文件…")
            enabled: !row.formulaModel.busy
            onClicked: row.formulaModel.installLocal()
        }
    ]
}
