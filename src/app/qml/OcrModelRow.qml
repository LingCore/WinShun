pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The reading model (ws::OcrModel), under 增强文字识别 once that is
// installed: the larger one downloaded on request, in use while installed,
// removable; or a model of the user's own, picked as a file, before both.
SettingRow {
    id: row

    required property OcrModel ocrModel
    objectName: "preciseOcrRow"
    sub: true
    title: qsTr("高精度识别模型")
    keywords: qsTr("识字;OCR;日语;日文;假名;繁体;生僻字;高精度;模型;下载;Japanese;traditional;model").split(";")
    readonly property bool own: ocrModel.ownName.length > 0
    description: ocrModel.status.length > 0 ? ocrModel.status
        : own ? qsTr("正在使用你自己的模型 %1：截图识别、剪贴板图片和图片搜索都用它。移除后回到原来的模型。").arg(ocrModel.ownName)
        : ocrModel.installed ? qsTr("已安装并在使用：截图识别、剪贴板图片和图片搜索都用它，剪贴板里的图片会重新识别一遍。")
        : qsTr("能认日文、繁体字和更多生僻字，中英文小字也更准；每张图多花约 0.2 秒。需下载%1，可随时移除。").arg(ocrModel.downloadSize)

    FlatButton {
        dpr: row.dpr
        objectName: "preciseOcrButton"
        text: row.own ? qsTr("移除自己的模型") : row.ocrModel.installed ? qsTr("移除") : row.ocrModel.busy ? qsTr("取消") : qsTr("下载并安装")
        enabled: !row.ocrModel.busy || (!row.own && !row.ocrModel.installed)
        onClicked: row.own ? row.ocrModel.removeOwn() : row.ocrModel.installed ? row.ocrModel.remove()
                 : row.ocrModel.busy ? row.ocrModel.cancel() : row.ocrModel.download()
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
        Flow {
            width: parent.width
            spacing: 8
            FlatButton {
                dpr: row.dpr
                objectName: "installLocalPreciseOcr"
                visible: !row.ocrModel.installed && !row.own
                text: qsTr("从本地安装模型文件…")
                enabled: !row.ocrModel.busy
                onClicked: row.ocrModel.installLocal()
            }
            FlatButton {
                dpr: row.dpr
                id: ownButton
                objectName: "chooseOwnOcrModel"
                text: row.own ? qsTr("换一个自己的模型…") : qsTr("用自己下载的模型…")
                enabled: !row.ocrModel.busy
                onClicked: row.ocrModel.chooseOwn()
                // What can be picked, said where it is asked for.
                HoverTip {
                    target: ownButton.hovered ? ownButton : null
                    essential: true
                    maxWidth: 360
                    dpr: row.dpr
                    text: qsTr("PP-OCR 的文字识别（rec）模型，ONNX 格式，比如 PP-OCRv6 medium、PP-OCRv5 server 或韩文等其他语言的模型。没带字典的，把字典改成和模型同名的 .txt 放在旁边。")
                }
            }
        }
    ]
}
