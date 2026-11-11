pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The first-run setup: two steps on the right, each two choices; on the left
// (OnboardingRail) the name, the keys chosen and the steps.
//
// It is as tall as the taller of its two steps with everything on them, in
// the language and font shown, so that nothing is scrolled: a choice never
// adds a line (what it brings is there from the start, greyed until then).
// Only a screen with less room than that (`roomHeight`) gets a shorter window,
// whose step scrolls.
Window {
    id: window
    // The window's, read once (GitHub #111): the cards, the footer and every control take it.
    readonly property real dpr: Screen.devicePixelRatio

    required property Onboarding setup
    required property WindowFrame frame
    readonly property int page: setup.page

    // What the screen leaves for it (set once below; the probe narrows it).
    property real roomWidth: 100000
    property real roomHeight: 100000
    readonly property real wantedHeight: Math.ceil(flick.y + Math.max(pageOne.implicitHeight, pageTwo.implicitHeight)
                                                   + 12 + statusRoom.height + 8 + footer.height)

    width: Math.min(840, roomWidth)
    height: Math.min(wantedHeight, roomHeight)
    title: qsTr("欢迎使用 Win顺")
    flags: Qt.Window | Qt.FramelessWindowHint
    color: Theme.backdrop && SystemTheme.materials && (active || SystemTheme.uiStyle === "liquid") ? "transparent" : Theme.page

    Component.onCompleted: {
        const area = frame.availableArea(contentItem)
        if (area.width > 0 && area.height > 0) {
            roomWidth = area.width - 32
            roomHeight = area.height - 32
        }
    }

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
        if (SystemTheme.animations && visible)
            pageIn.restart()
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
        lineHeight: 1.15
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }

    // One choice of a step: what it is, and beside it the switch or below it the controls.
    component SetupCard: Item {
        id: card
        required property string heading
        required property string glyph
        property string description
        property bool switchShown: false
        property string switchName
        property bool checked: false
        default property alias body: extras.data
        signal toggled(bool on)

        implicitHeight: Theme.snapUp(contents.implicitHeight + 30, window.dpr)

        Hook { // the style's card (Theme.cardPart)
            anchors.fill: parent
            part: Theme.cardPart
            corner: Theme.largeCardRadius
            dpr: window.dpr
        }

        Column {
            id: contents
            x: 18
            y: 15
            width: parent.width - 36
            spacing: 8

            Item {
                width: parent.width
                implicitHeight: Math.max(title.implicitHeight, card.switchShown ? toggle.implicitHeight - 8 : 0)
                height: implicitHeight

                Glyph {
                    y: Theme.snap((title.height - height) / 2, window.dpr)
                    glyph: card.glyph
                    size: 20
                    color: card.checked || !card.switchShown ? Theme.accent : Theme.subtext
                }
                Text {
                    id: title
                    x: 32
                    width: parent.width - x - (card.switchShown ? toggle.width + 20 : 0)
                    text: card.heading
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Theme.fontStrong
                    wrapMode: Text.Wrap
                }
                ToggleSwitch {
                    id: toggle
                    dpr: window.dpr
                    objectName: card.switchName
                    anchors.right: parent.right
                    y: Theme.snap((title.height - height) / 2, window.dpr) // its track level with the heading
                    visible: card.switchShown
                    checked: card.checked
                    onToggled: (on) => card.toggled(on)
                    Accessible.name: card.heading
                }
            }
            Explanation { x: 32; width: parent.width - 32; text: card.description; visible: text.length > 0 }
            Column { id: extras; x: 32; width: parent.width - 32; topPadding: 4; spacing: 10; visible: children.length > 0 }
        }
    }

    // One of a few, side by side (ScopeTabs), with the keyboard: Left and Right choose.
    component Choice: Item {
        id: choice
        property alias labels: tabs.labels
        property int current: 0
        signal chosen(int index)

        implicitWidth: tabs.implicitWidth
        implicitHeight: tabs.implicitHeight
        activeFocusOnTab: true
        Accessible.role: Accessible.PageTabList
        Accessible.name: current >= 0 && current < labels.length ? labels[current] : ""
        Keys.onLeftPressed: if (current > 0) chosen(current - 1)
        Keys.onRightPressed: if (current < labels.length - 1) chosen(current + 1)

        ScopeTabs {
            id: tabs
            dpr: window.dpr
            current: choice.current
            onActivated: (index) => choice.chosen(index)
        }
        Rectangle { // keyboard focus
            visible: choice.activeFocus
            anchors.fill: parent
            anchors.margins: -2
            radius: Theme.largeCardRadius
            color: "transparent"
            border.width: 2
            border.color: Theme.text
        }
    }

    OnboardingRail {
        id: rail
        width: 264
        height: parent.height
        visible: window.width >= 720 // a screen too narrow for both keeps the choices
        setup: window.setup
        dpr: window.dpr
    }

    Item {
        id: pane
        anchors.left: rail.visible ? rail.right : parent.left
        anchors.right: parent.right
        height: parent.height

        Column {
            id: heading
            x: 32
            y: 40
            width: parent.width - 64
            spacing: 6
            visible: !window.setup.committed

            Text {
                width: parent.width - 72 // clear of the caption buttons
                text: window.page === 0 ? qsTr("先定下怎么打开") : qsTr("再挑两样常用的")
                color: Theme.text
                font.pixelSize: 24
                font.weight: Theme.fontStrong
                wrapMode: Text.Wrap
            }
            Explanation {
                width: parent.width
                text: window.page === 0 ? qsTr("文件名搜索装好就能用，索引会自动保存和更新。")
                                        : qsTr("都可以先不开，需要时再到设置里打开。")
            }
        }

        Flickable {
            id: flick
            objectName: "onboardingScroll"
            y: heading.y + heading.height + 18
            width: parent.width
            height: Math.max(0, statusRoom.y - 12 - y)
            visible: !window.setup.committed
            clip: contentHeight > height + 0.5
            interactive: contentHeight > height + 0.5
            contentWidth: width
            contentHeight: (window.page === 0 ? pageOne : pageTwo).implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick

            // On a screen too short for the whole step, the control the
            // keyboard goes to is brought into view.
            Connections {
                target: window
                function onActiveFocusItemChanged() {
                    const item = window.activeFocusItem
                    let owner = item
                    while (owner && owner !== cards)
                        owner = owner.parent
                    if (!item || owner !== cards || !flick.interactive)
                        return
                    const point = item.mapToItem(cards, 0, 0)
                    if (point.y < flick.contentY)
                        flick.contentY = Math.max(0, point.y - 8)
                    else if (point.y + item.height > flick.contentY + flick.height)
                        flick.contentY = Math.min(Math.max(0, flick.contentHeight - flick.height), point.y + item.height - flick.height + 8)
                }
            }

            Item {
                id: cards
                x: 32
                width: flick.width - 64
                height: flick.contentHeight

                // The step shown comes in from a little to the right (the one left is gone at once).
                property real slide: 0
                ParallelAnimation {
                    id: pageIn
                    NumberAnimation { target: cards; property: "slide"; from: 16; to: 0; duration: 180; easing.type: Easing.OutCubic }
                    NumberAnimation { target: cards; property: "opacity"; from: 0; to: 1; duration: 140 }
                }

                Column {
                    id: pageOne
                    x: cards.slide
                    width: parent.width
                    spacing: 12
                    visible: window.page === 0

                    SetupCard {
                        objectName: "onboardingActivation"
                        width: parent.width
                        glyph: "\uE765"
                        heading: qsTr("打开 Win顺")
                        description: qsTr("连续轻按两次同一侧的按键，或者用一组快捷键。")

                        Choice {
                            objectName: "onboardingActivationChoice"
                            labels: [qsTr("双击 Ctrl"), qsTr("双击 Alt"), qsTr("双击 Shift"), qsTr("快捷键")]
                            current: window.setup.activation
                            onChosen: (index) => { window.clearRecording(); window.setup.activation = index }
                        }
                        Flow {
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
                            FlatButton {
                                dpr: window.dpr
                                objectName: "onboardingTestLaunch"
                                glyph: "\uE8B0"
                                text: qsTr("试试按键")
                                onClicked: { window.clearRecording(); window.setup.beginTest(false) }
                                Accessible.role: Accessible.Button
                                Accessible.name: text
                            }
                        }
                    }

                    SetupCard {
                        objectName: "onboardingSearch"
                        width: parent.width
                        glyph: "\uE721"
                        heading: qsTr("文件内容搜索")
                        switchName: "onboardingContentToggle"
                        switchShown: true
                        checked: window.setup.contentIndex
                        onToggled: (on) => window.setup.contentIndex = on
                        description: qsTr("除了文件名，也能搜到文档里的文字。首次会在后台建立索引，占用一些 CPU 和磁盘；索引只保存在本机。")
                    }
                }

                Column {
                    id: pageTwo
                    x: cards.slide
                    width: parent.width
                    spacing: 12
                    visible: window.page === 1

                    SetupCard {
                        objectName: "onboardingClipboard"
                        width: parent.width
                        glyph: "\uE77F"
                        heading: qsTr("剪贴板历史")
                        switchName: "onboardingClipboardToggle"
                        switchShown: true
                        checked: window.setup.clipboard
                        onToggled: (on) => window.setup.clipboard = on
                        description: qsTr("在本机保存复制过的文字、图片和文件，方便再次粘贴。默认最多保留 300 条、30 天。")

                        Item {
                            width: parent.width
                            implicitHeight: Math.max(winVText.implicitHeight, winVToggle.implicitHeight)
                            height: implicitHeight
                            enabled: window.setup.clipboard

                            Text {
                                id: winVText
                                width: parent.width - winVToggle.width - 16
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("用 Win+V 打开，替代 Windows 剪贴板历史")
                                color: parent.enabled ? Theme.text : Theme.faint
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                            }
                            ToggleSwitch {
                                id: winVToggle
                                dpr: window.dpr
                                objectName: "onboardingWinVToggle"
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                word: false
                                checked: window.setup.clipboardWinV
                                onToggled: (on) => window.setup.clipboardWinV = on
                                Accessible.name: qsTr("用 Win+V 打开剪贴板历史")
                            }
                        }
                    }

                    SetupCard {
                        objectName: "onboardingCapture"
                        width: parent.width
                        heading: qsTr("截图")
                        glyph: "\uE722"
                        switchName: "onboardingCaptureToggle"
                        switchShown: true
                        checked: window.setup.capture
                        onToggled: (on) => window.setup.capture = on
                        description: qsTr("框选截图、标注和贴图。首次开启会下载组件；打开即允许它读写剪贴板、保存图片。")

                        Flow {
                            width: parent.width
                            spacing: 8
                            enabled: window.setup.capture
                            opacity: enabled ? 1 : 0.45

                            HotkeyRecorder {
                                id: captureRecorder
                                objectName: "onboardingCaptureRecorder"
                                hotkey: window.setup.captureHotkey
                                onRecorded: (hotkey) => window.setup.captureHotkey = hotkey
                                onRecordingChanged: window.setup.recording = recording || launchRecorder.recording
                            }
                            FlatButton {
                                dpr: window.dpr
                                glyph: "\uE8B0"
                                text: qsTr("试试按键")
                                onClicked: { window.clearRecording(); window.setup.beginTest(true) }
                                Accessible.role: Accessible.Button
                                Accessible.name: qsTr("试用截图快捷键")
                            }
                        }
                    }
                }
            }
        }

        ListScrollBar {
            dpr: window.dpr
            anchors.top: flick.top
            anchors.bottom: flick.bottom
            anchors.right: parent.right
            anchors.rightMargin: 5
            visible: flick.visible && flick.interactive
            flickable: flick
        }

        // After the choices are saved: the screenshot component on its way
        // (download, start), or what kept it. Its permissions are not asked
        // about again: turning it on above was the yes.
        Column {
            id: finishing
            visible: window.setup.committed
            x: 48
            width: parent.width - 96
            y: Theme.snap((footer.y - height) / 2 + 8, window.dpr)
            spacing: 14

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 56
                height: 56
                radius: 28
                color: Theme.badge

                Glyph {
                    anchors.centerIn: parent
                    glyph: window.setup.captureReady ? "\uE73E" : window.setup.canRetry ? "\uE7BA" : "\uE722"
                    size: 26
                    color: window.setup.canRetry ? Theme.danger : Theme.accent
                }
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: window.setup.captureReady ? qsTr("都准备好了")
                    : window.setup.canRetry ? qsTr("截图还没装好")
                                            : qsTr("马上就好")
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Theme.fontStrong
                wrapMode: Text.Wrap
            }
            Explanation {
                objectName: "onboardingProgressText"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: window.setup.message
                visible: text.length > 0
                color: window.setup.canRetry ? Theme.danger : Theme.subtext
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(260, parent.width)
                height: 4
                radius: Theme.progressRadius
                color: Theme.track
                // Its room stays while the component is on its way: the words above do not jump.
                opacity: window.setup.busy && window.setup.progress >= 0 ? 1 : 0

                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, window.setup.progress))
                    height: parent.height
                    radius: parent.radius
                    color: Theme.accentFill
                }
            }
        }

        // What is wrong with a choice, or how a try of the keys goes: two lines' room, kept.
        Item {
            id: statusRoom
            x: 32
            width: parent.width - 64
            height: Theme.snapUp(2 * statusMetrics.height * 1.15, window.dpr)
            anchors.bottom: footer.top
            anchors.bottomMargin: 8
            visible: !window.setup.committed

            FontMetrics { id: statusMetrics; font.pixelSize: Theme.fontCaption }
            readonly property string words: window.setup.message || window.setup.testMessage
            readonly property bool wrong: window.setup.message.length > 0

            Glyph {
                id: statusMark
                visible: statusRoom.words.length > 0 && (statusRoom.wrong || window.setup.testPassed)
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.snap(status.height - statusMetrics.height * 1.15 + 2, window.dpr)
                glyph: statusRoom.wrong ? "\uE7BA" : "\uE73E"
                size: 14
                color: status.color
            }
            Explanation {
                id: status
                objectName: "onboardingStatus"
                x: statusMark.visible ? 22 : 0
                width: parent.width - x
                anchors.bottom: parent.bottom
                maximumLineCount: 2
                elide: Text.ElideRight
                text: statusRoom.words
                visible: text.length > 0
                color: statusRoom.wrong ? Theme.danger : window.setup.testPassed ? Theme.success
                     : window.setup.testing ? Theme.accent : Theme.subtext
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }

        Item {
            id: footer
            x: 32
            width: parent.width - 64
            height: 36 + 24
            anchors.bottom: parent.bottom

            Explanation {
                objectName: "onboardingSummary"
                anchors.left: parent.left
                anchors.right: buttons.left
                anchors.rightMargin: 12
                y: Theme.snap((36 - height) / 2, window.dpr)
                maximumLineCount: 2
                elide: Text.ElideRight
                text: window.setup.captureReady ? qsTr("设置已保存，之后可随时调整。")
                    : window.setup.committed ? qsTr("搜索已准备好，截图也可稍后安装。") : qsTr("之后都能在设置里改。")
            }
            Row {
                id: buttons
                anchors.right: parent.right
                spacing: 8

                FlatButton {
                    dpr: window.dpr
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
                    dpr: window.dpr
                    objectName: "onboardingPrevious"
                    role: "footer"
                    visible: window.page === 1 && !window.setup.committed
                    text: qsTr("上一步")
                    onClicked: { window.clearRecording(); window.setup.previous() }
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                }
                FlatButton {
                    dpr: window.dpr
                    objectName: "onboardingRetry"
                    role: "footer"
                    visible: window.setup.canRetry
                    text: qsTr("重试")
                    onClicked: window.setup.retryCapture()
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                }
                FlatButton {
                    dpr: window.dpr
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
                    dpr: window.dpr
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

    // Over both sides: the whole top drags the window; of the caption only its buttons show.
    TitleBar {
        width: parent.width
        frame: window.frame
        title: window.title
        labelled: false
        maximizable: false
        closeIntercepted: true
        onCloseRequested: window.continueWith(true)
    }

    WindowEdge { id: windowEdge; fill: Theme.page }
}
