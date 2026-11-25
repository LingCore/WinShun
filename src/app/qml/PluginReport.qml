import QtQuick
import WinShun

// What a running plugin says of its own long work (winshun/status, GitHub
// #458): a line with the tone's dot, a bar under it when it says how far
// along, and its buttons at the right (暂停, 继续) where there is room for
// them (PluginCard; a PluginTile shows the line and the bar).
Item {
    id: report

    property string text
    property string tone // ok | busy | idle | warn | error
    property real progress: -1 // 0…1; below 0: no bar
    property var actions: [] // [{id, title}]
    property bool buttons: true
    property int maximumLineCount: 3
    property real dpr: Screen.devicePixelRatio

    readonly property bool hasBar: progress >= 0
    readonly property color toneColor: tone === "ok" ? Theme.success
                                     : tone === "busy" ? Theme.accent
                                     : tone === "warn" ? Theme.caution
                                     : tone === "error" ? Theme.danger
                                     : Theme.faint

    signal actionRequested(string action)

    visible: text.length > 0
    implicitHeight: Theme.snapUp(line.height + (hasBar ? bar.height + 6 : 0), dpr)
    Accessible.role: hasBar ? Accessible.ProgressBar : Accessible.StaticText
    Accessible.name: text

    Item {
        id: line
        width: parent.width
        height: Theme.snapUp(Math.max(keys.visible ? keys.height : 16, label.implicitHeight), report.dpr)

        Rectangle {
            id: dot
            y: Math.round(label.y + label.fontInfo.pixelSize * 0.62 - height / 2)
            width: 8
            height: 8
            radius: Theme.pillRadius(width)
            color: report.toneColor

            SequentialAnimation on opacity { // at work
                running: report.tone === "busy" && report.visible && SystemTheme.animations && (report.Window.window?.visible ?? false)
                loops: Animation.Infinite
                alwaysRunToEnd: true
                NumberAnimation { to: 0.3; duration: 600; easing.type: Easing.InOutSine }
                NumberAnimation { to: 1; duration: 600; easing.type: Easing.InOutSine }
            }
        }
        Text {
            id: label
            anchors.left: dot.right
            anchors.leftMargin: 8
            anchors.right: keys.visible ? keys.left : parent.right
            anchors.rightMargin: keys.visible ? 10 : 0
            y: lineCount > 1 ? 0 : Theme.snap((line.height - implicitHeight) / 2, report.dpr)
            text: report.text
            textFormat: Text.PlainText
            color: report.tone === "error" ? Theme.danger : report.tone === "warn" ? Theme.caution : Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            maximumLineCount: report.maximumLineCount
            elide: Text.ElideRight
            lineHeight: 1.15
        }
        Row {
            id: keys
            visible: report.buttons && report.actions.length > 0
            anchors.right: parent.right
            spacing: 6

            Repeater {
                model: report.buttons ? report.actions : []

                delegate: FlatButton {
                    required property var modelData

                    objectName: "pluginReportAction_" + modelData.id
                    dpr: report.dpr
                    implicitHeight: 26
                    fontSize: Theme.fontCaption
                    text: modelData.title
                    onClicked: report.actionRequested(modelData.id)
                }
            }
        }
    }

    Rectangle {
        id: bar
        objectName: "pluginReportBar"
        visible: report.hasBar
        y: line.height + 6
        width: parent.width
        height: 4
        radius: Theme.progressRadius
        color: Theme.track

        Rectangle {
            width: Theme.snap(parent.width * Math.max(0, Math.min(1, report.progress)), report.dpr)
            height: parent.height
            radius: Theme.progressRadius
            color: report.tone === "idle" ? Theme.faint : Theme.accent

            Behavior on width {
                enabled: SystemTheme.animations
                NumberAnimation { duration: 150 }
            }
        }
    }
}
