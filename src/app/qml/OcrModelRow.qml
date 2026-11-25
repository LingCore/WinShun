pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The larger reading model (ws::OcrModel), under 增强文字识别 once that is
// installed: downloaded on request, in use while installed, removable.
SettingRow {
    id: row

    required property OcrModel ocrModel
    objectName: "preciseOcrRow"
    sub: true
    title: qsTr("高精度识别模型")
    keywords: qsTr("识字;OCR;日语;日文;假名;繁体;生僻字;高精度;模型;下载;Japanese;traditional;model").split(";")
    description: ocrModel.status.length > 0 ? ocrModel.status
        : ocrModel.installed ? qsTr("已安装并在使用：截图识别、剪贴板图片和图片搜索都用它，剪贴板里的图片会重新识别一遍。")
        : qsTr("能认日文、繁体字和更多生僻字，中英文小字也更准；每张图多花约 0.2 秒。需下载%1，可随时移除。").arg(ocrModel.downloadSize)
    bodyShown: !ocrModel.installed

    FlatButton {
        dpr: row.dpr
        objectName: "preciseOcrButton"
        text: row.ocrModel.installed ? qsTr("移除") : row.ocrModel.busy ? qsTr("取消") : qsTr("下载并安装")
        onClicked: row.ocrModel.installed ? row.ocrModel.remove() : row.ocrModel.busy ? row.ocrModel.cancel() : row.ocrModel.download()
    }

    body: [
        Rectangle {
            visible: row.ocrModel.busy
            width: parent.width
            height: 4
            radius: Theme.progressRadius
            color: Theme.track
            Rectangle {
                width: parent.width * row.ocrModel.progress
                height: parent.height
                radius: parent.radius
                color: Theme.accent
            }
        },
        FlatButton {
            dpr: row.dpr
            objectName: "installLocalPreciseOcr"
            text: qsTr("从本地安装模型文件…")
            enabled: !row.ocrModel.busy
            onClicked: row.ocrModel.installLocal()
        }
    ]
}
