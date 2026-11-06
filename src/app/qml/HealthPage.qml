import QtQuick
import WinShun

// 运行状况: what Win顺 uses now, its memory told apart by what holds it,
// what it is doing, each process, the latest happenings. The monitor reads
// every second while this page is on screen (watching; HealthMonitor).
SettingsPage {
    id: healthPage

    required property SettingsEditor editor
    required property int pageIndex
    property bool shown: false // on screen: the window up, this page current, not searching

    readonly property HealthMonitor monitor: editor.monitor
    readonly property var h: monitor ? monitor.now : ({})
    readonly property bool known: h.total !== undefined
    readonly property bool busy: known && h.busy
    // Each part's colour, by its key (HealthMonitor::publish).
    readonly property var partKeys: ["fileIndex", "windows", "pictures", "spare", "program", "plugins", "documents"]
    readonly property var partColors: Theme.dark ? ["#5AA9F5", "#A98BF0", "#E57FB8", "#8FA3B8", "#6E7F94", "#45C2B6", "#F0A85A"]
                                                 : ["#2F7FD8", "#8C6AD6", "#C8508F", "#93A4B5", "#5F6F82", "#22958B", "#D9822B"]
    function partColor(key: string): color {
        return partColors[Math.max(0, partKeys.indexOf(key))]
    }
    readonly property color serviceColor: Theme.dark ? "#A0A0A0" : "#767676"
    // The parts worth a line: the file index and the windows always, the rest while there is some.
    readonly property var shownParts: known ? h.parts.filter(p => p.key === "fileIndex" || p.key === "windows" || p.mb >= 0.5) : []

    function mb(value: real): string {
        return value >= 10 ? qsTr("%1 MB").arg(Math.round(value))
             : value >= 1 ? qsTr("%1 MB").arg(value.toFixed(1))
             : value > 0 ? qsTr("不到 1 MB") : qsTr("0 MB")
    }
    function rate(value: real): string {
        return value >= 10 ? qsTr("%1 MB/秒").arg(Math.round(value)) : qsTr("%1 MB/秒").arg(value.toFixed(1))
    }
    function percent(value: real): string {
        return value >= 10 ? Math.round(value) + "%" : value.toFixed(1) + "%"
    }

    Binding {
        when: healthPage.monitor !== null
        healthPage.monitor.watching: healthPage.shown
    }

    // ---- 现在 ----------------------------------------------------------------
    SettingsSection {
        page: healthPage.pageIndex
        width: parent.width
        title: qsTr("现在")
        glyph: "" // Diagnostic: pulse
        note: qsTr("总数和任务管理器里 Win顺 那一行的“内存”是同一个数；后台服务在那里单独一行，不算在内。")

        SettingRow {
            title: healthPage.known ? qsTr("Win顺 现在用了 %1").arg(healthPage.mb(healthPage.h.total)) : qsTr("正在读取内存占用…")
            keywords: qsTr("内存;占用;任务管理器;归还;释放;卡;运行状况;CPU;磁盘;memory;ram;task manager").split(";")
            description: !healthPage.known ? "" : qsTr("CPU %1 · 读取 %2").arg(healthPage.percent(healthPage.h.cpu)).arg(healthPage.rate(healthPage.h.read))
                         + "\n" + healthPage.h.lastTrim
                         + (healthPage.busy ? "\n" + qsTr("后台还在忙，归还后它会再用回一些") : "")

            FlatButton {
                text: qsTr("现在就归还")
                glyph: "" // Refresh: arrow-clockwise
                contentSpacing: 8
                onClicked: healthPage.editor.giveMemoryBack()
            }
        }

        Item { // the parts: one bar, then a line each
            visible: healthPage.known
            width: parent.width
            height: visible ? partsColumn.implicitHeight + 16 : 0

            Column {
                id: partsColumn
                x: 18
                width: parent.width - 36
                spacing: 0

                Row { // the bar
                    id: bar
                    readonly property real total: Math.max(1, healthPage.h.total ?? 1)
                    width: parent.width
                    height: 8
                    spacing: 0

                    Repeater {
                        model: healthPage.shownParts
                        delegate: Rectangle {
                            required property int index
                            required property var modelData
                            readonly property bool first: index === 0
                            readonly property bool last: index === healthPage.shownParts.length - 1
                            width: Math.round(bar.width * modelData.mb / bar.total * healthPage.dpr) / healthPage.dpr
                            height: bar.height
                            color: healthPage.partColor(modelData.key)
                            topLeftRadius: first ? 4 : 0
                            bottomLeftRadius: first ? 4 : 0
                            topRightRadius: last ? 4 : 0
                            bottomRightRadius: last ? 4 : 0
                        }
                    }
                }

                Item { width: 1; height: 8 }

                Repeater {
                    model: healthPage.shownParts
                    delegate: PartLine {
                        required property var modelData
                        swatch: healthPage.partColor(modelData.key)
                        name: modelData.name
                        note: modelData.note
                        amount: healthPage.mb(modelData.mb)
                    }
                }

                PartLine { // the service: its own row in Task Manager, not in the total
                    visible: healthPage.known && healthPage.h.hasService
                    swatch: healthPage.serviceColor
                    line: true
                    name: qsTr("后台服务")
                    note: qsTr("读磁盘、建索引；任务管理器里单独一行，不算在总数里")
                    amount: healthPage.mb(healthPage.h.serviceMb ?? 0)
                }
            }
        }
    }

    // One part: its colour, name and what it is, its size at the right.
    component PartLine: Item {
        property color swatch
        property bool line: false // drawn as a line: not part of the bar
        property string name
        property string note
        property string amount

        width: parent ? parent.width : 0
        height: Math.max(28, nameText.y + nameText.height + 4)

        Rectangle {
            id: mark
            x: 0
            y: Math.round(nameText.y + (nameText.height - height) / 2)
            width: 8
            height: parent.line ? Theme.edge(healthPage.dpr) * 2 : 8
            radius: parent.line ? 0 : 2
            color: parent.swatch
        }
        Text {
            id: nameText
            x: 16
            y: 4
            text: parent.name
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
        Text {
            id: noteText
            anchors.left: nameText.right
            anchors.leftMargin: 10
            anchors.right: amountText.left
            anchors.rightMargin: 12
            anchors.baseline: nameText.baseline
            text: parent.note
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }
        Text {
            id: amountText
            anchors.right: parent.right
            anchors.baseline: nameText.baseline
            text: parent.amount
            color: Theme.text
            font.pixelSize: Theme.fontBody
            font.features: { "tnum": 1 }
        }
    }

    // ---- 现在在做什么 ----------------------------------------------------------
    SettingsSection {
        page: healthPage.pageIndex
        width: parent.width
        title: qsTr("现在在做什么")
        glyph: "" // Recent: clock

        SettingRow {
            visible: !healthPage.busy
            title: healthPage.known ? healthPage.h.idleText : qsTr("正在读取…")
            keywords: qsTr("后台;进度;剩余时间;建立索引;何时结束;progress;busy").split(";")
        }

        Repeater {
            model: healthPage.busy ? healthPage.h.tasks : []

            delegate: Item {
                id: task

                required property int index
                required property var modelData

                width: parent ? parent.width : 0
                height: taskColumn.implicitHeight + 28

                Rectangle {
                    visible: task.index > 0
                    x: Theme.edge(healthPage.dpr)
                    width: parent.width - 2 * x
                    height: Theme.edge(healthPage.dpr)
                    color: Theme.cardBorder
                }
                Column {
                    id: taskColumn
                    x: 18
                    y: 14
                    width: parent.width - 36
                    spacing: 8

                    Item {
                        width: parent.width
                        height: taskName.implicitHeight

                        Text {
                            id: taskName
                            text: task.modelData.name + (task.modelData.count.length > 0 ? "  " + task.modelData.count : "")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.baseline: taskName.baseline
                            text: task.modelData.remaining
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                    Rectangle { // how far, when known
                        visible: task.modelData.fraction >= 0
                        width: parent.width
                        height: 4
                        radius: Theme.progressRadius
                        color: Theme.track

                        Rectangle {
                            width: Math.max(height, parent.width * Math.min(1, task.modelData.fraction))
                            height: parent.height
                            radius: parent.radius
                            color: Theme.accentFill
                        }
                    }
                }
            }
        }
    }

    // ---- 各个进程 --------------------------------------------------------------
    SettingsSection {
        page: healthPage.pageIndex
        width: parent.width
        title: qsTr("各个进程")
        glyph: "" // TVMonitor: monitor

        Repeater {
            model: healthPage.known ? healthPage.h.processes : []

            delegate: Item {
                id: process

                required property int index
                required property var modelData
                readonly property string note: modelData.kind === "service" ? qsTr("读磁盘、建索引；任务管理器里单独一行")
                                              : modelData.kind === "document" ? qsTr("读完 30 秒后退出")
                                              : modelData.kind === "main" ? qsTr("搜索框、剪贴板、设置都在这里")
                                              : ""

                width: parent ? parent.width : 0
                height: 44

                Rectangle {
                    visible: process.index > 0
                    x: Theme.edge(healthPage.dpr)
                    width: parent.width - 2 * x
                    height: Theme.edge(healthPage.dpr)
                    color: Theme.cardBorder
                }
                Row {
                    x: 18
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10

                    Text {
                        text: process.modelData.name
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        visible: process.note.length > 0
                        anchors.baseline: parent.children[0].baseline
                        text: process.note
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                    }
                }
                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 0

                    Text {
                        width: 64
                        horizontalAlignment: Text.AlignRight
                        text: healthPage.percent(process.modelData.cpu)
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                    }
                    Text {
                        width: 84
                        horizontalAlignment: Text.AlignRight
                        text: healthPage.mb(process.modelData.mb)
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                }
            }
        }

        SettingRow {
            visible: healthPage.known && healthPage.h.processes.length === 0
            title: qsTr("正在读取…")
        }
    }

    // ---- 最近的事 --------------------------------------------------------------
    SettingsSection {
        page: healthPage.pageIndex
        width: parent.width
        title: qsTr("最近的事")
        glyph: "" // BulletedList
        note: qsTr("只记在内存里，重启 Win顺 后重新开始。")

        Repeater {
            model: healthPage.monitor && healthPage.shown ? healthPage.monitor.recent : []

            delegate: Item {
                id: happening

                required property int index
                required property var modelData

                width: parent ? parent.width : 0
                height: happeningColumn.implicitHeight + 22

                Rectangle {
                    visible: happening.index > 0
                    x: Theme.edge(healthPage.dpr)
                    width: parent.width - 2 * x
                    height: Theme.edge(healthPage.dpr)
                    color: Theme.cardBorder
                }
                Text {
                    id: happeningTime
                    x: 18
                    y: 11
                    width: 44
                    text: happening.modelData.time
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    font.features: { "tnum": 1 }
                }
                Column {
                    id: happeningColumn
                    anchors.left: happeningTime.right
                    anchors.leftMargin: 8
                    anchors.right: parent.right
                    anchors.rightMargin: 18
                    y: 11
                    spacing: 2

                    Text {
                        width: parent.width
                        text: happening.modelData.title + (happening.modelData.running ? qsTr("（进行中）") : "")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: happening.modelData.detail
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        SettingRow {
            visible: !healthPage.monitor || healthPage.monitor.recent.length === 0
            title: qsTr("还没有什么事")
        }
    }
}
