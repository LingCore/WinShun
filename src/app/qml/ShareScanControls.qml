import QtQuick
import WinShun

Column {
    id: controls
    property real dpr: 0 // the window's, passed by the host (GitHub #111)
    readonly property real pixelRatio: dpr > 0 ? dpr : Screen.devicePixelRatio // Screen only without one
    objectName: "shareScanControls"
    property var share: ({})
    signal edited(bool scheduled, string from, string until, int depth)
    property bool expanded: false
    readonly property string scanFrom: share.scanFrom ?? "22:00"
    readonly property string scanUntil: share.scanUntil ?? "07:00"
    readonly property int scanDepth: share.scanDepth ?? 0
    readonly property bool scanScheduled: share.scanScheduled === true
    readonly property string scanSummary: (scanScheduled ? qsTr("每天 %1–%2").arg(scanFrom).arg(scanUntil) : qsTr("不限时段"))
                                         + " · " + (scanDepth > 0 ? qsTr("最多 %1 层").arg(scanDepth) : qsTr("不限层数"))
    spacing: 8

    Flow {
        width: parent.width
        spacing: 8
        FlatButton {
            dpr: controls.pixelRatio
            objectName: "scanExpand"
            text: qsTr("扫描设置")
            flat: true
            implicitHeight: 32
            trailingGlyph: controls.expanded ? "\uE70E" : "\uE70D"
            Accessible.expanded: controls.expanded
            onClicked: controls.expanded = !controls.expanded
        }
        Text {
            text: controls.scanSummary
            height: 32
            verticalAlignment: Text.AlignVCenter
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            textFormat: Text.PlainText
        }
    }

    Column {
        visible: controls.expanded
        width: parent.width
        spacing: 8

        Item {
            width: parent.width
            height: Math.max(scheduleLabel.implicitHeight, scheduleSwitch.height)
            Text {
                id: scheduleLabel
                anchors.left: parent.left
                anchors.right: scheduleSwitch.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("仅在指定时段自动扫描")
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            ToggleSwitch {
                dpr: controls.pixelRatio
                id: scheduleSwitch
                objectName: "scanSchedule"
                anchors.right: parent.right
                checked: controls.scanScheduled
                Accessible.name: scheduleLabel.text
                onToggled: (on) => controls.edited(on, controls.scanFrom, controls.scanUntil, controls.scanDepth)
            }
        }
        Flow {
            visible: controls.scanScheduled
            width: parent.width
            spacing: 8
            InputBox {
                dpr: controls.pixelRatio
                id: scanStart
                objectName: "scanFrom"
                tabFocus: true
                width: 82
                text: controls.scanFrom
                maximumLength: 5
                horizontalAlignment: TextInput.AlignHCenter
                Accessible.name: qsTr("扫描开始时间，24 小时制")
                validator: RegularExpressionValidator { regularExpression: /([01]\d|2[0-3]):[0-5]\d/ }
                onEditingFinished: {
                    controls.edited(controls.scanScheduled, text, controls.scanUntil, controls.scanDepth)
                    text = Qt.binding(() => controls.scanFrom)
                }
            }
            Text {
                text: "–"
                height: 36
                verticalAlignment: Text.AlignVCenter
                color: Theme.subtext
                font.pixelSize: Theme.fontBody
            }
            InputBox {
                dpr: controls.pixelRatio
                id: scanEnd
                tabFocus: true
                width: 82
                objectName: "scanUntil"
                text: controls.scanUntil
                maximumLength: 5
                horizontalAlignment: TextInput.AlignHCenter
                Accessible.name: qsTr("扫描结束时间，24 小时制")
                validator: RegularExpressionValidator { regularExpression: /([01]\d|2[0-3]):[0-5]\d/ }
                onEditingFinished: {
                    controls.edited(controls.scanScheduled, controls.scanFrom, text, controls.scanDepth)
                    text = Qt.binding(() => controls.scanUntil)
                }
            }
            Text {
                text: qsTr("本地时间，可跨午夜；相同时间表示全天")
                width: Math.min(implicitWidth, parent.width)
                height: Math.max(36, implicitHeight)
                verticalAlignment: Text.AlignVCenter
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }
        Flow {
            width: parent.width
            spacing: 8
            Text {
                text: qsTr("最多扫描")
                height: 36
                verticalAlignment: Text.AlignVCenter
                color: Theme.text
                font.pixelSize: Theme.fontCaption
            }
            InputBox {
                dpr: controls.pixelRatio
                id: depthField
                tabFocus: true
                width: 72
                objectName: "scanDepth"
                text: String(controls.scanDepth)
                maximumLength: 4
                horizontalAlignment: TextInput.AlignHCenter
                Accessible.name: qsTr("最大扫描层数，0 表示不限")
                validator: IntValidator { bottom: 0; top: 1024 }
                onEditingFinished: {
                    controls.edited(controls.scanScheduled, controls.scanFrom, controls.scanUntil, Number(text))
                    text = Qt.binding(() => String(controls.scanDepth))
                }
            }
            Text {
                text: qsTr("层（0 表示不限）")
                height: 36
                verticalAlignment: Text.AlignVCenter
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
        }
        Text {
            width: parent.width
            readonly property bool invalidTime: controls.scanScheduled
                && (!/^([01]\d|2[0-3]):[0-5]\d$/.test(scanStart.text) || !/^([01]\d|2[0-3]):[0-5]\d$/.test(scanEnd.text))
            readonly property bool invalidDepth: !/^\d{1,4}$/.test(depthField.text) || Number(depthField.text) > 1024
            text: invalidTime ? qsTr("请输入 00:00–23:59 之间的时间")
                  : invalidDepth ? qsTr("请输入 0–1024 之间的层数，0 表示不限")
                  : qsTr("1 层只读取当前文件夹，不进入子文件夹。时段外暂停后台读取，已有结果保留；“立即核对”可临时扫描一次文件名。")
            color: invalidTime || invalidDepth ? Theme.danger : Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
    }
}
