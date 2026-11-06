import QtQuick
import WinShun

// 运行状况: what Win顺 uses now and over the last 10 minutes, hour or day,
// what it was doing at each peak, each process, the latest happenings.
// The monitor reads every second while this page is on screen (watching)
// and every 10 seconds always (HealthMonitor).
SettingsPage {
    id: healthPage

    required property SettingsEditor editor
    required property int pageIndex
    property bool shown: false // on screen: the window up, this page current, not searching

    readonly property HealthMonitor monitor: editor.monitor
    readonly property var h: monitor ? monitor.now : ({})
    readonly property bool known: h.total !== undefined
    readonly property bool busy: known && h.busy
    readonly property var ranges: [10 * 60 * 1000, 60 * 60 * 1000, 24 * 60 * 60 * 1000]
    property int range: 0
    property double hoverTime: 0
    readonly property var partColors: Theme.dark ? ["#5AA9F5", "#A98BF0", "#45C2B6", "#F0A85A"]
                                                 : ["#2F7FD8", "#8C6AD6", "#22958B", "#D9822B"]
    readonly property color serviceColor: Theme.dark ? "#A0A0A0" : "#767676"
    readonly property string hoverEvents: monitor && hoverTime > 0 ? monitor.eventsAt(hoverTime) : ""

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
    function clock(ms: real): string {
        return Qt.formatDateTime(new Date(ms), healthPage.range === 0 ? "HH:mm:ss" : "HH:mm")
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
        note: qsTr("和任务管理器里 Win顺 那一行的“内存”是同一个数；后台服务在任务管理器里单独一行，不算在里面。")

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

        Item { // the parts, as one bar, and what each is
            visible: healthPage.known
            width: parent.width
            height: visible ? partsColumn.implicitHeight + 18 : 0

            Column {
                id: partsColumn
                x: 18
                width: parent.width - 36
                spacing: 10

                Row { // the bar
                    id: bar
                    readonly property real total: Math.max(1, healthPage.h.total ?? 1)
                    width: parent.width
                    height: 8
                    spacing: 0

                    Repeater {
                        model: healthPage.known ? healthPage.h.parts : []
                        delegate: Rectangle {
                            required property int index
                            required property var modelData
                            width: Math.round(bar.width * modelData.mb / bar.total * healthPage.dpr) / healthPage.dpr
                            height: bar.height
                            color: healthPage.partColors[index]
                            topLeftRadius: index === 0 ? 4 : 0
                            bottomLeftRadius: index === 0 ? 4 : 0
                            topRightRadius: index === 3 ? 4 : 0
                            bottomRightRadius: index === 3 ? 4 : 0
                        }
                    }
                }

                Flow {
                    width: parent.width
                    spacing: 14

                    Repeater {
                        model: healthPage.known ? healthPage.h.parts : []
                        delegate: Row {
                            required property int index
                            required property var modelData
                            spacing: 6

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 8
                                height: 8
                                radius: 2
                                color: healthPage.partColors[parent.index]
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: parent.modelData.name + "  " + healthPage.mb(parent.modelData.mb)
                                color: Theme.text
                                font.pixelSize: Theme.fontCaption
                            }
                            Text {
                                visible: text.length > 0
                                anchors.verticalCenter: parent.verticalCenter
                                text: parent.modelData.note
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- 走势 ----------------------------------------------------------------
    SettingsSection {
        id: trends

        page: healthPage.pageIndex
        width: parent.width
        title: qsTr("走势")
        glyph: "" // History: clock-counter-clockwise

        Item {
            width: parent.width
            height: trendColumn.implicitHeight + 32

            Column {
                id: trendColumn
                x: 18
                y: 14
                width: parent.width - 36
                spacing: 14

                Item { // the range, and what was going on under the pointer
                    width: parent.width
                    height: rangeTabs.height

                    Text {
                        anchors.left: parent.left
                        anchors.right: rangeTabs.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: healthPage.hoverTime > 0
                              ? healthPage.clock(healthPage.hoverTime) + (healthPage.hoverEvents.length > 0 ? " · " + healthPage.hoverEvents : "")
                              : qsTr("指向图表，看那一刻在做什么")
                        color: healthPage.hoverTime > 0 ? Theme.text : Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                    }
                    ScopeTabs {
                        id: rangeTabs
                        anchors.right: parent.right
                        labels: [qsTr("10 分钟"), qsTr("1 小时"), qsTr("24 小时")]
                        current: healthPage.range
                        onActivated: (scope) => healthPage.range = scope
                    }
                }

                Repeater {
                    model: [
                        { kind: 0, title: qsTr("内存"), height: 120 },
                        { kind: 1, title: qsTr("CPU"), height: 72 },
                        { kind: 2, title: qsTr("读取"), height: 72 }
                    ]

                    delegate: Column {
                        id: block

                        required property var modelData
                        readonly property bool memory: modelData.kind === 0
                        readonly property var names: memory
                            ? [qsTr("文件索引"), qsTr("界面和其他"), qsTr("插件"), qsTr("读文档"), qsTr("后台服务")]
                            : [qsTr("Win顺"), qsTr("后台服务")]
                        readonly property var colors: memory ? healthPage.partColors.concat([healthPage.serviceColor])
                                                             : [healthPage.partColors[modelData.kind === 1 ? 0 : 2], healthPage.serviceColor]
                        readonly property var values: chart.values

                        function said(value: real): string {
                            return modelData.kind === 0 ? healthPage.mb(value) : modelData.kind === 1 ? healthPage.percent(value) : healthPage.rate(value)
                        }

                        width: trendColumn.width
                        spacing: 6

                        Row { // its name, and the total at the pointer (or now)
                            spacing: 8

                            Text {
                                id: blockTitle
                                text: block.modelData.title
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                font.weight: Theme.fontStrong
                            }
                            Text {
                                anchors.baseline: blockTitle.baseline
                                visible: block.memory && block.values.length > 0
                                text: block.values.length > 0
                                      ? qsTr("共 %1").arg(healthPage.mb(block.values.slice(0, 4).reduce((a, b) => a + b, 0)))
                                      : ""
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                        Flow { // each series and its value
                            width: parent.width + 12 // the last one's gap (rightPadding) may hang outside
                            spacing: 4

                            Repeater {
                                model: block.names.length
                                delegate: Row {
                                    id: key
                                    required property int index
                                    readonly property bool service: index === block.names.length - 1
                                    spacing: 5
                                    rightPadding: 12

                                    Rectangle {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: key.service ? 10 : 8
                                        height: key.service ? Theme.edge(healthPage.dpr) * 2 : 8
                                        radius: key.service ? 0 : 2
                                        color: block.colors[key.index]
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: block.names[key.index] + " " + (key.index < block.values.length ? block.said(block.values[key.index]) : "—")
                                        color: Theme.subtext
                                        font.pixelSize: Theme.fontCaption
                                    }
                                }
                            }
                        }
                        Item {
                            width: parent.width
                            height: block.modelData.height

                            HealthChart {
                                id: chart
                                anchors.fill: parent
                                monitor: healthPage.monitor
                                kind: block.modelData.kind
                                rangeMs: healthPage.ranges[healthPage.range]
                                hoverTime: healthPage.hoverTime
                                colors: block.colors
                                gridColor: Theme.divider
                                eventColor: Theme.dark ? Qt.rgba(1, 1, 1, 0.055) : Qt.rgba(0, 0, 0, 0.045)
                                hoverColor: Theme.subtext
                            }
                            Text { // the axis's top
                                x: 4
                                y: 3
                                text: block.said(chart.axisMax)
                                color: Theme.faint
                                font.pixelSize: Theme.fontCaption - 1
                            }
                            Text {
                                visible: chart.empty
                                anchors.centerIn: parent
                                text: qsTr("还没有记录，稍等几秒")
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                onPositionChanged: (mouse) => healthPage.hoverTime = chart.timeAt(mouse.x)
                                onExited: healthPage.hoverTime = 0
                            }
                        }
                    }
                }

                Item { // the time axis, under the last chart
                    width: parent.width
                    height: startLabel.implicitHeight

                    Text {
                        id: startLabel
                        text: [qsTr("10 分钟前"), qsTr("1 小时前"), qsTr("24 小时前")][healthPage.range]
                        color: Theme.faint
                        font.pixelSize: Theme.fontCaption - 1
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: [qsTr("5 分钟前"), qsTr("30 分钟前"), qsTr("12 小时前")][healthPage.range]
                        color: Theme.faint
                        font.pixelSize: Theme.fontCaption - 1
                    }
                    Text {
                        anchors.right: parent.right
                        text: qsTr("现在")
                        color: Theme.faint
                        font.pixelSize: Theme.fontCaption - 1
                    }
                }
            }
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
        note: healthPage.known
              ? qsTr("这些记录只存在内存里，最多 24 小时，约 %1 KB；重启 Win顺 后重新开始。").arg(healthPage.h.historyKb)
              : ""

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
