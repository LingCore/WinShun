pragma ComponentBehavior: Bound
import QtQuick
import WinShun

SettingRow {
    id: row

    required property OcrPackage ocrPackage
    objectName: "enhancedOcrRow"
    title: qsTr("增强文字识别")
    keywords: qsTr("识字;OCR;图片文字;模型;离线;增强;下载;recognition;offline").split(";")
    description: ocrPackage.status.length > 0 ? ocrPackage.status
        : qsTr("提升截图和中英文图片的识别效果。下载 %1，安装后可离线使用；未安装时，剪贴板使用 Windows 支持的识别语言。").arg(ocrPackage.downloadSize)
    bodyShown: !ocrPackage.available

    FlatButton {
        objectName: "downloadEnhancedOcr"
        visible: !row.ocrPackage.available
        text: row.ocrPackage.busy ? qsTr("取消") : qsTr("下载并安装")
        onClicked: row.ocrPackage.busy ? row.ocrPackage.cancel() : row.ocrPackage.download()
    }

    body: [
        Rectangle {
            visible: row.ocrPackage.busy
            width: parent.width
            height: 4
            radius: Theme.progressRadius
            color: Theme.track
            Rectangle {
                width: parent.width * row.ocrPackage.progress
                height: parent.height
                radius: parent.radius
                color: Theme.accent
            }
        },
        FlatButton {
            objectName: "installLocalEnhancedOcr"
            text: qsTr("从本地安装增强识别包…")
            enabled: !row.ocrPackage.busy
            onClicked: row.ocrPackage.installLocal()
        }
    ]
}
