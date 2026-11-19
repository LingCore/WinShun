pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The mouse gestures' settings: opened by 设置 on the plugin's card in Win顺's
// 插件 page (the plugin starts this window as a process of its own). Drawn as
// Win顺's settings window: the title bar, the page's name and tabs, and under
// them the tab's cards, which scroll; every change is kept as it is made.
Window {
    id: window

    required property GestureEditor editor
    required property WindowFrame frame
    property int page: 0 // the tab: 手势, 选中文字时, 角落和屏幕边, 程序, 常规
    property string program // 程序: the one chosen

    // One Screen attached object for the whole window (GitHub #111; docs/pitfalls.md).
    readonly property real dpr: Screen.devicePixelRatio
    readonly property var tabs: [qsTr("手势"), qsTr("选中文字时"), qsTr("角落和屏幕边"), qsTr("程序"), qsTr("常规")]

    // For the preview: opens a picker on the first button there is, at the stage asked for.
    function previewPicker(kind: string) {
        const find = (item, names) => {
            if (names.indexOf(item.objectName) >= 0)
                return item
            for (const child of item.children) {
                const found = find(child, names)
                if (found)
                    return found
            }
            return null
        }
        if (kind === "program") {
            const button = find(flick.contentItem, ["addProgramButton", "addExcludedButton"])
            if (button)
                programPicker.ask(button, [], () => {})
            return
        }
        const button = find(flick.contentItem, ["ruleActionButton", "spotButton"])
        if (!button)
            return
        actionPicker.ask(button, { kind: "builtin", id: "back" }, () => {}, () => {})
        if (kind.indexOf("menu:") === 0)
            actionPicker.category = parseInt(kind.substring(5))
        else if (kind !== "menu")
            actionPicker.stageFor(kind)
    }

    // For the preview: what the picker is told once a gesture was drawn.
    function previewChoose(kind: string, value: string) { actionPicker.choose(kind, value) }
    function previewClose() { actionPicker.close() }
    function previewStage(stage: string) { actionPicker.stageFor(stage) }

    width: 760
    height: 600
    minimumWidth: 600
    minimumHeight: 460
    color: Theme.backdrop && SystemTheme.materials && window.active ? "transparent" : Theme.page
    title: qsTr("鼠标手势") // shown as "鼠标手势 - Win顺"
    flags: Qt.Window | Qt.FramelessWindowHint // the title bar is ours (see WindowFrame)

    Item {
        id: focusSink
        focus: true
        Keys.onEscapePressed: window.close()
    }

    TitleBar {
        id: titleBar
        z: 1
        width: parent.width
        frame: window.frame
        title: window.title + " - " + Qt.application.displayName
    }

    // The page's name and its tabs, staying put while the cards scroll (as in
    // Win顺's settings window, SettingsWindow.qml's pageHeader).
    Item {
        id: pageHeader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        height: Theme.snap(pageHeaderColumn.implicitHeight + 16, window.dpr)

        Column {
            id: pageHeaderColumn
            x: 24
            y: 8
            width: parent.width - 48
            spacing: 6

            Text {
                id: pageTitle
                text: qsTr("鼠标手势")
                color: Theme.text
                font.pixelSize: Theme.fontDisplay
                font.weight: Theme.fontStrong
            }
            PageTabs {
                id: pageTabs
                objectName: "gesturesTabs"
                labels: window.tabs
                current: window.page
                Accessible.name: qsTr("鼠标手势设置分类")
                onActivated: (index) => {
                    focusSink.forceActiveFocus()
                    window.page = index
                    flick.contentY = 0
                }
            }
        }
        Text { // as Win顺's settings window says it, at the foot of its list
            anchors.right: parent.right
            anchors.rightMargin: 24
            y: pageHeaderColumn.y + pageTitle.y + Math.round((pageTitle.height - height) / 2)
            visible: window.width >= 700
            text: qsTr("修改会自动保存，立刻生效")
            color: Theme.faint
            font.pixelSize: Theme.fontCaption
        }

        Hook { // once the cards scroll under it (the style's separator)
            x: 24
            width: parent.width - 48
            anchors.bottom: parent.bottom
            height: Theme.edge(window.dpr)
            part: Theme.separator
            orientation: Qt.Horizontal
            dpr: window.dpr
            opacity: flick.contentY > 2 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }
    }

    Flickable {
        id: flick
        objectName: "gesturesFlick"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: pageHeader.bottom
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: pageLoader.height + 36 + 12
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        ListWheelScroll {
            view: flick
            animate: SystemTheme.animations
        }

        MouseArea { // clicking empty space finishes editing a field
            width: flick.width
            height: Math.max(flick.height, flick.contentHeight)
            onClicked: focusSink.forceActiveFocus()
        }

        Loader {
            id: pageLoader
            x: 24
            y: 12
            width: flick.width - 48
            height: item ? item.implicitHeight : 0
            sourceComponent: window.page === 0 ? gesturesPage : window.page === 1 ? selectionPage : window.page === 2 ? cornersPage
                             : window.page === 3 ? programsPage : generalPage
            // On whole device pixels wherever it has scrolled to (as Win顺's settings).
            transform: ScrollPixelAlignment {
                view: flick
                viewY: flick.y
                itemY: pageLoader.y
                pixelBias: 0.02
            }
        }
    }

    Component {
        id: gesturesPage
        GestureList {
            editor: window.editor
            picker: actionPicker
            mode: 0
            dpr: window.dpr
            flick: flick
        }
    }
    Component {
        id: selectionPage
        GestureList {
            editor: window.editor
            picker: actionPicker
            mode: 1
            dpr: window.dpr
            flick: flick
        }
    }
    Component {
        id: cornersPage
        CornersPage {
            editor: window.editor
            picker: actionPicker
            dpr: window.dpr
            flick: flick
        }
    }
    Component {
        id: programsPage
        ProgramsPage {
            editor: window.editor
            picker: actionPicker
            programPicker: programPicker
            dpr: window.dpr
            flick: flick
            program: window.program
            onProgramChanged: window.program = program
        }
    }
    Component {
        id: generalPage
        GeneralPage {
            editor: window.editor
            programPicker: programPicker
            dpr: window.dpr
        }
    }

    // The panels the pages open: one of each for the window.
    ActionPicker {
        id: actionPicker
        editor: window.editor
    }
    ProgramPicker {
        id: programPicker
        editor: window.editor
    }

    WindowEdge {
        id: windowEdge
        fill: Theme.page
    }
}
