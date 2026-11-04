pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import WinShun

Window {
    id: window

    required property Onboarding setup
    required property WindowFrame frame
    readonly property int page: setup.page

    width: 720
    height: 560
    minimumWidth: 660
    minimumHeight: 480
    title: qsTr("欢迎使用 Win顺")
    flags: Qt.Window | Qt.FramelessWindowHint
    color: Theme.backdrop && SystemTheme.materials && active ? "transparent" : Theme.page

    function clearRecording() {
        launchRecorder.stop()
        captureRecorder.stop()
        setup.recording = false
    }

    function continueWith(later) {
        clearRecording()
        if (setup.committed)
            setup.finish()
        else
            setup.apply(later)
    }

    onPageChanged: {
        clearRecording()
        setup.stopTest()
        flick.cancelFlick()
        flick.contentY = 0
        Qt.callLater(() => {
            if (window.visible && !window.setup.committed)
                (window.page === 0 ? nextButton : startButton).forceActiveFocus(Qt.TabFocusReason)
        })
    }

    // Native close leaves an unfinished draft for the next launch. The title
    // bar's close action uses the same safe defaults as “set up later”.
    onClosing: {
        clearRecording()
        setup.stopTest()
        if (setup.committed)
            setup.finish()
        else
            Qt.quit()
    }

    component Explanation: Text {
        color: Theme.subtext
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }

    // The style's line between two parts (Theme.separator), as thick as the style wants it.
    component Divider: Hook {
        id: seam
        height: seam.out("thickness", Theme.edge(seam.dpr))
        part: Theme.separator
    }

    component SetupCard: Item {
        id: card
        required property string heading
        required property string glyph
        property string description
        property bool switchShown: false
        property string switchName
        property bool checked: false
        default property alias body: extras.data
        readonly property real dpr: Screen.devicePixelRatio
        signal toggled(bool on)

        implicitHeight: contents.implicitHeight + 32

        Hook { // the style's card (Theme.cardPart)
            anchors.fill: parent
            part: Theme.cardPart
            corner: Theme.largeCardRadius
            dpr: card.dpr
        }
        Rectangle { // switched on: the card's edge in the accent
            visible: card.checked
            anchors.fill: parent
            radius: Theme.largeCardRadius
            color: "transparent"
            border.width: Theme.edge(card.dpr)
            border.color: Theme.accent
            opacity: 0.45
        }

        Column {
            id: contents
            x: 20
            y: 16
            width: parent.width - 40
            spacing: 10

            RowLayout {
                width: parent.width
                spacing: 12
                Rectangle {
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 36
                    radius: Theme.largeControlRadius
                    color: card.checked || !card.switchShown ? Theme.badge : Theme.keycap
                    Glyph {
                        anchors.centerIn: parent
                        glyph: card.glyph
                        size: 20
                        color: card.checked || !card.switchShown ? Theme.accent : Theme.subtext
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: card.heading
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                    font.weight: Theme.fontStrong
                    wrapMode: Text.Wrap
                }
                ToggleSwitch {
                    objectName: card.switchName
                    Layout.minimumWidth: implicitWidth
                    Layout.preferredWidth: implicitWidth
                    visible: card.switchShown && card.visible
                    checked: card.checked
                    onToggled: (on) => card.toggled(on)
                    Accessible.name: card.heading
                }
            }
            Explanation { x: 48; width: parent.width - 48; text: card.description; visible: text.length > 0 }
            Column { id: extras; x: 48; width: parent.width - 48; spacing: 10 }
        }
    }

    TitleBar {
        id: titleBar
        width: parent.width
        frame: window.frame
        title: qsTr("Win顺 · 首次设置")
        closeIntercepted: true
        onCloseRequested: window.continueWith(true)
    }

    Column {
        id: welcome
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 32
        anchors.topMargin: 12
        spacing: 10
        RowLayout {
            width: parent.width
            spacing: 16
            Text {
                Layout.fillWidth: true
                text: window.page === 0 ? qsTr("搜索与打开方式") : qsTr("剪贴板与截图")
                color: Theme.text
                font.pixelSize: 24
                font.weight: Theme.fontStrong
                wrapMode: Text.Wrap
            }
            Column {
                spacing: 8
                Accessible.name: qsTr("第 %1 页，共 2 页").arg(window.page + 1)
                Text {
                    anchors.right: parent.right
                    text: (window.page + 1) + " / 2"
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                }
                Row {
                    spacing: 4
                    Repeater {
                        model: 2
                        Rectangle {
                            required property int index
                            width: 24
                            height: 3
                            radius: Theme.progressRadius
                            color: index <= window.page ? Theme.accentFill : Theme.track
                        }
                    }
                }
            }
        }
        Explanation {
            width: parent.width
            text: window.page === 0 ? qsTr("文件名索引自动保存并更新，无需每次重新建立。")
                : qsTr("选择需要的功能，确认后生效。")
        }
    }

    Flickable {
        id: flick
        objectName: "onboardingScroll"
        anchors.top: welcome.bottom
        anchors.topMargin: 20
        anchors.bottom: footer.top
        anchors.left: parent.left
        anchors.right: parent.right
        clip: true
        contentWidth: width
        contentHeight: cards.implicitHeight + 20
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick

        // Keyboard traversal keeps its focused control visible at small window
        // sizes and at larger Windows scales, including the screenshot row.
        Connections {
            target: window
            function onActiveFocusItemChanged() {
                const item = window.activeFocusItem
                let owner = item
                while (owner && owner !== cards)
                    owner = owner.parent
                if (!item || owner !== cards)
                    return
                const point = item.mapToItem(cards, 0, 0)
                if (point.y < flick.contentY)
                    flick.contentY = Math.max(0, point.y - 8)
                else if (point.y + item.height > flick.contentY + flick.height)
                    flick.contentY = Math.min(Math.max(0, flick.contentHeight - flick.height), point.y + item.height - flick.height + 8)
            }
        }

        Column {
            id: cards
            x: 32
            width: flick.width - 64
            spacing: 12
            enabled: !window.setup.committed

            SetupCard {
                objectName: "onboardingSearch"
                visible: window.page === 0
                width: parent.width
                glyph: "\uE721"
                heading: qsTr("文件内容搜索")
                switchName: "onboardingContentToggle"
                switchShown: true
                checked: window.setup.contentIndex
                onToggled: (on) => window.setup.contentIndex = on
                description: qsTr("除了文件名，也能搜索文档里的文字。")
                Explanation {
                    width: parent.width
                    text: window.setup.contentIndex
                        ? qsTr("首次在后台建立索引，会占用 CPU 和磁盘；索引仅保存在本机，之后更新变化的文件。")
                        : qsTr("暂不开启也能使用文件名搜索，或在“内容”页手动搜索。")
                }
            }

            SetupCard {
                objectName: "onboardingActivation"
                visible: window.page === 0
                width: parent.width
                heading: qsTr("打开 Win顺")
                glyph: "\uE765"
                description: qsTr("连续轻按两次同一侧的按键，即可呼出搜索框。")
                Flow {
                    width: parent.width
                    spacing: 8
                    Repeater {
                        model: [qsTr("双击 Ctrl"), qsTr("双击 Alt"), qsTr("双击 Shift"), qsTr("自定义快捷键")]
                        FlatButton {
                            required property string modelData
                            required property int index
                            text: modelData
                            highlighted: window.setup.activation === index
                            onClicked: { window.clearRecording(); window.setup.activation = index }
                            Accessible.role: Accessible.RadioButton
                            Accessible.name: text
                            Accessible.checked: highlighted
                        }
                    }
                    FlatButton {
                        objectName: "onboardingTestLaunch"
                        text: qsTr("试试按键")
                        flat: true
                        onClicked: { window.clearRecording(); window.setup.beginTest(false) }
                        Accessible.role: Accessible.Button
                        Accessible.name: text
                    }
                }
                Flow {
                    visible: window.setup.activation === 3
                    width: parent.width
                    spacing: 8
                    HotkeyRecorder {
                        id: launchRecorder
                        objectName: "onboardingLaunchRecorder"
                        visible: window.setup.activation === 3
                        hotkey: window.setup.launchHotkey
                        onRecorded: (hotkey) => window.setup.launchHotkey = hotkey
                        onRecordingChanged: window.setup.recording = recording || captureRecorder.recording
                    }
                }
            }

            SetupCard {
                objectName: "onboardingClipboard"
                visible: window.page === 1
                width: parent.width
                glyph: "\uE77F"
                heading: qsTr("剪贴板历史")
                switchName: "onboardingClipboardToggle"
                switchShown: true
                checked: window.setup.clipboard
                onToggled: (on) => window.setup.clipboard = on
                description: qsTr("在本机保存复制过的文字、图片和文件，方便再次粘贴。")
                Explanation {
                    width: parent.width
                    text: qsTr("默认最多保留 300 条、30 天，可随时清理。")
                }
                RowLayout {
                    width: parent.width
                    visible: window.setup.clipboard
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("用 Win+V 打开，替代 Windows 剪贴板历史")
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                    ToggleSwitch {
                        objectName: "onboardingWinVToggle"
                        checked: window.setup.clipboardWinV
                        onToggled: (on) => window.setup.clipboardWinV = on
                        Accessible.name: qsTr("用 Win+V 打开剪贴板历史")
                    }
                }
            }

            SetupCard {
                objectName: "onboardingCapture"
                visible: window.page === 1
                width: parent.width
                heading: qsTr("截图")
                glyph: "\uE722"
                switchName: "onboardingCaptureToggle"
                switchShown: true
                checked: window.setup.capture
                onToggled: (on) => window.setup.capture = on
                description: qsTr("框选截图、标注和贴图。首次开启需下载组件，并确认权限。")
                Flow {
                    visible: window.setup.capture
                    width: parent.width
                    spacing: 8
                    HotkeyRecorder {
                        id: captureRecorder
                        objectName: "onboardingCaptureRecorder"
                        hotkey: window.setup.captureHotkey
                        onRecorded: (hotkey) => window.setup.captureHotkey = hotkey
                        onRecordingChanged: window.setup.recording = recording || launchRecorder.recording
                    }
                    FlatButton {
                        text: qsTr("试试按键")
                        flat: true
                        onClicked: { window.clearRecording(); window.setup.beginTest(true) }
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("试用截图快捷键")
                    }
                }
                Explanation {
                    width: parent.width
                    visible: window.setup.capture
                    text: qsTr("录制快捷键后即可使用；此处试用仅检测按键。")
                }
            }
        }
    }

    ListScrollBar {
        anchors.top: flick.top
        anchors.bottom: flick.bottom
        anchors.right: parent.right
        anchors.rightMargin: 5
        flickable: flick
    }

    Rectangle {
        id: footer
        readonly property real dpr: Screen.devicePixelRatio
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: footerContents.implicitHeight + 28
        color: Theme.background
        bottomLeftRadius: windowEdge.cornerRadius // the window's corners as drawn
        bottomRightRadius: windowEdge.cornerRadius
        Divider { width: parent.width; dpr: footer.dpr }

        Column {
            id: footerContents
            x: 32
            y: 12
            width: parent.width - 64
            spacing: 10

            Explanation {
                id: status
                objectName: "onboardingStatus"
                width: parent.width
                text: window.setup.message || window.setup.testMessage
                visible: text.length > 0
                color: window.setup.canRetry || (!window.setup.committed && window.setup.message.length > 0)
                    ? Theme.danger : window.setup.testing ? Theme.accent : Theme.subtext
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            Rectangle {
                width: parent.width
                height: 3
                radius: Theme.progressRadius
                color: Theme.track
                visible: window.setup.busy && window.setup.progress >= 0
                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, window.setup.progress))
                    height: parent.height
                    radius: parent.radius
                    color: Theme.accentFill
                }
            }
            RowLayout {
                width: parent.width
                spacing: 10
                Explanation {
                    objectName: "onboardingSummary"
                    Layout.fillWidth: true
                    text: window.setup.captureReady ? qsTr("设置已保存，之后可随时调整。")
                        : window.setup.committed ? qsTr("搜索已准备好，截图也可稍后安装。") : qsTr("默认：文件名搜索 · 双击 Ctrl")
                }
                FlatButton {
                    objectName: "onboardingPrevious"
                    role: "footer"
                    visible: window.page === 1 && !window.setup.committed
                    text: qsTr("上一步")
                    flat: true
                    onClicked: { window.clearRecording(); window.setup.previous() }
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                }
                FlatButton {
                    objectName: "onboardingRetry"
                    role: "footer"
                    visible: window.setup.canRetry
                    text: qsTr("重试")
                    onClicked: window.setup.retryCapture()
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                }
                FlatButton {
                    objectName: "onboardingLater"
                    role: "footer"
                    flat: true
                    visible: !window.setup.captureReady
                    text: window.setup.committed ? qsTr("稍后安装") : qsTr("稍后设置")
                    onClicked: window.continueWith(true)
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                }
                FlatButton {
                    id: nextButton
                    objectName: "onboardingNext"
                    role: "footer"
                    text: qsTr("下一步")
                    highlighted: true
                    visible: window.page === 0 && !window.setup.committed
                    onClicked: { window.clearRecording(); window.setup.next() }
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                }
                FlatButton {
                    id: startButton
                    objectName: "onboardingStart"
                    role: "footer"
                    text: qsTr("开始使用")
                    highlighted: true
                    visible: window.page === 1 && (!window.setup.committed || window.setup.captureReady)
                    onClicked: window.continueWith(false)
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                }
            }
        }
    }

    Loader {
        anchors.fill: parent
        active: window.setup.plugins !== null
        sourceComponent: PluginDialog {
            objectName: "onboardingPermissions"
            fitToWindow: true
            plugins: window.setup.plugins
            onOpenChanged: if (!open) window.setup.permissionClosed()
        }
    }
    WindowEdge { id: windowEdge; fill: Theme.page }
}
