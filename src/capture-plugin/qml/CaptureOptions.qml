pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The screen capture's options (CaptureSettings: [Capture] in capture.ini),
// the page of the plugin's settings window: one group at a time. Up to 0.16
// they were the folded part of the 截图 card on Win顺's 插件 page.
Item {
    id: card

    objectName: "captureOptions"
    required property CaptureSettings editor
    property int currentGroup: 0
    readonly property bool narrow: body.width < 520
    readonly property var groups: [
        { title: qsTr("快捷键") },
        { title: qsTr("截图") },
        { title: qsTr("鼠标") },
        { title: qsTr("贴图") },
        { title: qsTr("打码") },
        { title: qsTr("工具条") }
    ]

    function moveGroup(delta) {
        currentGroup = (currentGroup + delta + groups.length) % groups.length
        groupButtons.itemAt(currentGroup).forceActiveFocus(Qt.TabFocusReason)
    }
    // The hotkey box records: the plugin lets go of its hotkeys meanwhile (CaptureApp).
    readonly property bool recordingHotkey: hotkeyRecorder.recording || pinsHotkeyRow.recording || restoreHotkeyRow.recording || lastPinHotkeyRow.recording
    Binding {
        target: card.editor
        property: "recordingHotkey"
        value: card.recordingHotkey
    }

    // The mouse buttons' choices (Settings::captureMouse*), the right button's with 后退 besides.
    readonly property var mouseChoices: [
        { value: "none", text: qsTr("无") },
        { value: "copy", text: qsTr("复制并关闭") },
        { value: "copypin", text: qsTr("复制并贴图") },
        { value: "save", text: qsTr("保存") },
        { value: "pin", text: qsTr("贴图") },
        { value: "ocr", text: qsTr("识别文字") },
        { value: "ocrcopy", text: qsTr("识别文字并复制") },
        { value: "ocrsearch", text: qsTr("识别文字并搜索") },
        { value: "ocrweb", text: qsTr("识别文字并网页搜索") },
        { value: "cancel", text: qsTr("取消截图") }
    ]
    readonly property var rightChoices: [mouseChoices[0], { value: "back", text: qsTr("后退（重新框选）") }].concat(mouseChoices.slice(1))
    readonly property var mouseValues: [editor.captureMouseDoubleLeft, editor.captureMouseRight, editor.captureMouseMiddle,
        editor.captureMouseBack, editor.captureMouseForward]

    // The five mouse-action drop-downs share one width, the widest option's
    // (rightChoices holds them all): text + gap + chevron + FlatButton's padding.
    readonly property int pickWidth: Math.ceil(Math.max(...rightChoices.map(c => pickMetrics.advanceWidth(c.text)))) + 6 + 10 + 24

    FontMetrics { id: pickMetrics; font.pixelSize: Theme.fontBody }

    // The toolbar's buttons left off (Settings::captureToolbarHidden) and the ids it knows, in its order.
    readonly property var hiddenButtons: editor.captureToolbarHidden.split(",")
    readonly property var toolbarButtons: [
        { id: "undo", text: qsTr("撤销") },
        { id: "redo", text: qsTr("重做") },
        { id: "long", text: qsTr("长截图") },
        { id: "record", text: qsTr("录屏") },
        { id: "delay", text: qsTr("延时") },
        { id: "qr", text: qsTr("二维码") },
        { id: "ocr", text: qsTr("识别文字") },
        { id: "pin", text: qsTr("贴图") },
        { id: "save", text: qsTr("保存") },
        { id: "cancel", text: qsTr("取消") },
        { id: "copy", text: qsTr("复制") }
    ]
    function setButtonShown(id, shown) {
        const left = hiddenButtons.filter(h => h.length > 0 && h !== id)
        if (!shown)
            left.push(id)
        editor.captureToolbarHidden = left.join(",")
    }

    // A shortcut the user records (no default: PixPin and Snipaste have the usual ones), with what went wrong registering it.
    component HotkeyRow: Item {
        id: row

        property string title
        property string words
        property string hotkey
        property string error
        readonly property bool recording: recorder.recording
        signal recorded(string hotkey)

        width: parent.width
        height: card.narrow ? rowTexts.implicitHeight + 8 + rowControls.implicitHeight
                        : Math.max(rowTexts.implicitHeight, rowControls.implicitHeight)

        Column {
            id: rowTexts
            anchors.left: parent.left
            anchors.right: card.narrow ? parent.right : rowControls.left
            anchors.rightMargin: card.narrow ? 0 : 12
            spacing: 4

            Text {
                width: parent.width
                text: row.title
                color: Theme.text
                font.pixelSize: Theme.fontBody
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                text: row.words
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            Text {
                visible: row.error.length > 0
                width: parent.width
                text: row.error
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }
        Row {
            id: rowControls
            anchors.right: parent.right
            y: card.narrow ? rowTexts.implicitHeight + 8 : (parent.height - height) / 2
            spacing: 8

            FlatButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: row.hotkey.length > 0 && !recorder.recording
                flat: true
                text: qsTr("清除")
                onClicked: row.recorded("")
            }
            HotkeyRecorder {
                id: recorder
                anchors.verticalCenter: parent.verticalCenter
                hotkey: row.hotkey
                onRecorded: (hotkey) => row.recorded(hotkey)
            }
        }
    }

    implicitHeight: body.implicitHeight

    Column {
        id: body
        width: parent.width
        spacing: 12

        Column { // one category at a time; keep the editors alive while hidden
            width: parent.width
            spacing: 12

            Flow {
                width: parent.width
                spacing: 6
                Accessible.role: Accessible.PageTabList
                Accessible.name: qsTr("截图设置分类")

                Repeater {
                    id: groupButtons
                    model: card.groups

                    delegate: FlatButton {
                        required property var modelData
                        required property int index
                        objectName: "captureGroup" + index
                        text: modelData.title
                        highlighted: card.currentGroup === index
                        flat: true
                        activeFocusOnTab: highlighted || activeFocus
                        Accessible.role: Accessible.PageTab
                        Accessible.name: text
                        Accessible.selected: highlighted
                        Accessible.onPressAction: clicked()
                        onClicked: {
                            forceActiveFocus(Qt.MouseFocusReason)
                            card.currentGroup = index
                        }
                        Keys.onLeftPressed: card.moveGroup(-1)
                        Keys.onRightPressed: card.moveGroup(1)
                    }
                }
            }

            Column {
                objectName: "capturePanel0"
                visible: card.currentGroup === 0
                width: parent.width
                spacing: 16

                Item {
                    width: parent.width
                    height: Math.max(fullScreenTexts.implicitHeight, fullScreenToggle.implicitHeight)

                    Column {
                        id: fullScreenTexts
                        anchors.left: parent.left
                        anchors.right: fullScreenToggle.left
                        anchors.rightMargin: 12
                        spacing: 4

                        Text {
                            width: parent.width
                            text: qsTr("全屏时不响应截图快捷键")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.Wrap
                        }
                        Text {
                            width: parent.width
                            text: qsTr("其他程序全屏时，截图快捷键和 PrtScn 不弹出截图；仍能从菜单手动开始")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                    ToggleSwitch {
                        id: fullScreenToggle
                        objectName: "capturePauseFullScreen"
                        anchors.right: parent.right
                        checked: card.editor.capturePauseInFullScreen
                        onToggled: (on) => card.editor.capturePauseInFullScreen = on
                    }
                }

                Item {
                    width: parent.width
                    height: card.narrow ? hotkeyTexts.implicitHeight + 8 + hotkeyControls.implicitHeight
                        : Math.max(hotkeyTexts.implicitHeight, hotkeyControls.implicitHeight)

                    Column {
                        id: hotkeyTexts
                        anchors.left: parent.left
                        anchors.right: card.narrow ? parent.right : hotkeyControls.left
                        anchors.rightMargin: card.narrow ? 0 : 12
                        spacing: 4

                        Text {
                            width: parent.width
                            text: qsTr("快捷键")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.Wrap
                        }
                        Text {
                            width: parent.width
                            text: qsTr("按它开始截图，截图时再按一次取消")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                        Text {
                            visible: card.editor.captureHotkeyError.length > 0
                            width: parent.width
                            text: card.editor.captureHotkeyError
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                    Row {
                        id: hotkeyControls
                        anchors.right: parent.right
                        y: card.narrow ? hotkeyTexts.implicitHeight + 8 : (parent.height - height) / 2
                        spacing: 8

                        FlatButton {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: card.editor.captureHotkey.length > 0 && !hotkeyRecorder.recording
                            flat: true
                            text: qsTr("清除")
                            onClicked: card.editor.captureHotkey = ""
                        }
                        HotkeyRecorder {
                            id: hotkeyRecorder
                            anchors.verticalCenter: parent.verticalCenter
                            hotkey: card.editor.captureHotkey
                            onRecorded: (hotkey) => card.editor.captureHotkey = hotkey
                        }
                    }
                }

                Item {
                    width: parent.width
                    height: Math.max(printTexts.implicitHeight, printToggle.implicitHeight)

                    Column {
                        id: printTexts
                        anchors.left: parent.left
                        anchors.right: printToggle.left
                        anchors.rightMargin: 12
                        spacing: 4

                        Text {
                            width: parent.width
                            text: qsTr("用 PrtScn 键截图")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.Wrap
                        }
                        Text {
                            width: parent.width
                            text: qsTr("让 PrtScn 不再打开 Windows 的截图工具，改用这里的截图。Win顺 退出时还给截图工具，下次启动再接管；关掉这项也恢复原来的设置")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                        Text {
                            visible: card.editor.captureNote.length > 0
                            width: parent.width
                            text: card.editor.captureNote
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                    ToggleSwitch {
                        id: printToggle
                        anchors.right: parent.right
                        checked: card.editor.capturePrintScreen
                        onToggled: (on) => card.editor.capturePrintScreen = on
                    }
                }
            }

            Column {
                objectName: "capturePanel1"
                visible: card.currentGroup === 1
                width: parent.width
                spacing: 16

                Column {
                    width: parent.width
                    spacing: 8
                    Text {
                        width: parent.width
                        text: qsTr("截图保存文件夹")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        width: parent.width
                        text: card.editor.captureSaveFolder.length > 0 ? card.editor.captureSaveFolder : qsTr("系统的“屏幕截图”文件夹")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.WrapAnywhere
                    }
                    Row {
                        spacing: 8
                        FlatButton {
                            text: qsTr("选择文件夹…")
                            onClicked: {
                                const chosen = card.editor.pickFolder(qsTr("选择截图保存文件夹"))
                                if (chosen.length > 0) card.editor.captureSaveFolder = chosen
                            }
                        }
                        FlatButton {
                            text: qsTr("使用系统默认")
                            enabled: card.editor.captureSaveFolder.length > 0
                            onClicked: card.editor.captureSaveFolder = ""
                        }
                    }
                }


                Item {
                    width: parent.width
                    height: Math.max(copyTexts.implicitHeight, copyToggle.implicitHeight)

                    Column {
                        id: copyTexts
                        anchors.left: parent.left
                        anchors.right: copyToggle.left
                        anchors.rightMargin: 12
                        spacing: 4

                        Text {
                            width: parent.width
                            text: qsTr("选好就复制")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.Wrap
                        }
                        Text {
                            width: parent.width
                            text: qsTr("松开鼠标或点选窗口后立即复制，跳过标注和调整。关闭时可继续编辑，再按 Enter 复制或 Ctrl+S 保存。鼠标动作在“鼠标”分类设置。")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                    ToggleSwitch {
                        id: copyToggle
                        anchors.right: parent.right
                        checked: card.editor.captureCopyOnSelect
                        onToggled: (on) => card.editor.captureCopyOnSelect = on
                    }
                }

                Repeater { // what goes into the picture and how controls are found (stage 7)
                    model: [
                        { key: "captureMagnifier", title: qsTr("框选时显示放大镜"),
                          words: qsTr("在指针旁放大显示像素和颜色值。关闭后，按 C 复制颜色、按 Shift 换颜色格式也一并停用。") },
                        { key: "capturePinOnCopy", title: qsTr("复制截图时同时贴图"),
                          words: qsTr("复制时同步贴到屏幕，也适用于“选好就复制”。只想偶尔使用，可在“鼠标”分类设置“复制并贴图”。") },
                        { key: "captureCursor", title: qsTr("截图带上鼠标指针"),
                          words: qsTr("在图片中保留截图那一刻的鼠标指针，复制、保存、贴图均生效。") },
                        { key: "captureHdrFile", title: qsTr("HDR 屏幕上同时保存 HDR 图片"),
                          words: qsTr("在开着 HDR 的屏幕上保存截图时，PNG 旁边再存一份同名的 .jxr，亮的地方保持原来的亮度，可用 Windows 自带的“照片”查看。复制和贴图不变。") },
                        { key: "captureControlsUia", title: qsTr("识别 WinUI 等程序里的控件"),
                          words: qsTr("自动框选设置、计算器等程序中的按钮和侧栏。Chrome、Edge、VS Code 这类 Chromium 程序不问（一问它们就打开屏幕阅读器模式），只框子窗口或整个窗口，可手动框选；管理员权限程序中的控件可能不可用。") }
                    ]
                    delegate: Item {
                        id: pictureOption

                        required property var modelData

                        width: parent.width
                        height: Math.max(pictureTexts.implicitHeight, pictureToggle.implicitHeight)

                        Column {
                            id: pictureTexts
                            anchors.left: parent.left
                            anchors.right: pictureToggle.left
                            anchors.rightMargin: 12
                            spacing: 4

                            Text {
                                width: parent.width
                                text: pictureOption.modelData.title
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                wrapMode: Text.Wrap
                            }
                            Text {
                                width: parent.width
                                text: pictureOption.modelData.words
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                            }
                        }
                        ToggleSwitch {
                            id: pictureToggle
                            anchors.right: parent.right
                            checked: card.editor[pictureOption.modelData.key]
                            onToggled: (on) => card.editor[pictureOption.modelData.key] = on
                        }
                    }
                }

                Item { // the delayed capture's seconds
                    width: parent.width
                    height: card.narrow ? delayTexts.implicitHeight + 8 + delayChoices.implicitHeight
                        : Math.max(delayTexts.implicitHeight, delayChoices.implicitHeight)

                    Column {
                        id: delayTexts
                        anchors.left: parent.left
                        anchors.right: card.narrow ? parent.right : delayChoices.left
                        anchors.rightMargin: card.narrow ? 0 : 12
                        spacing: 4

                        Text {
                            width: parent.width
                            text: qsTr("延时截图")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.Wrap
                        }
                        Text {
                            width: parent.width
                            text: qsTr("从托盘菜单“截图 › 延时截图”开始，或截图时按 D，适合截菜单和悬停提示。再次按截图快捷键或点击倒计时可取消。")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                    Row {
                        id: delayChoices
                        anchors.right: parent.right
                        y: card.narrow ? delayTexts.implicitHeight + 8 : (parent.height - height) / 2
                        spacing: 8

                        Repeater {
                            model: [3, 5, 10]

                            delegate: FlatButton {
                                required property int modelData
                            objectName: "captureDelay" + modelData

                                highlighted: card.editor.captureDelay === modelData
                                text: qsTr("%1 秒").arg(modelData)
                                onClicked: card.editor.captureDelay = modelData
                            }
                        }
                    }
                }

                Item { // 之前的画面: how many captures , and . bring back
                    width: parent.width
                    height: card.narrow ? historyTexts.implicitHeight + 8 + historyChoices.implicitHeight
                        : Math.max(historyTexts.implicitHeight, historyChoices.implicitHeight)

                    Column {
                        id: historyTexts
                        anchors.left: parent.left
                        anchors.right: card.narrow ? parent.right : historyChoices.left
                        anchors.rightMargin: card.narrow ? 0 : 12
                        spacing: 4

                        Text {
                            width: parent.width
                            text: qsTr("截图时翻看之前的画面")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.Wrap
                        }
                        Text {
                            width: parent.width
                            text: qsTr("截图时按 , 翻到上一次截图时的画面，按 . 往回翻：整个屏幕、选区和标注都恢复，可以重新框选或接着改。会在本机保存最近几次截图时的整屏画面；选“关闭”不再保存，并删除已保存的（不影响用关键词找截过的图）。")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                    Row {
                        id: historyChoices
                        anchors.right: parent.right
                        y: card.narrow ? historyTexts.implicitHeight + 8 : (parent.height - height) / 2
                        spacing: 8

                        Repeater {
                            model: [0, 10, 30, 100]

                            delegate: FlatButton {
                                required property int modelData

                                highlighted: card.editor.captureHistoryCount === modelData
                                text: modelData === 0 ? qsTr("关闭", "capture history") : qsTr("%1 次").arg(modelData)
                                onClicked: card.editor.captureHistoryCount = modelData
                            }
                        }
                    }
                }
            }

            Column {
                objectName: "capturePanel2"
                visible: card.currentGroup === 2
                width: parent.width
                spacing: 16

                Column { // what the mouse buttons do after a selection (issue #46)
                    width: parent.width
                    spacing: 4

                    Text {
                        width: parent.width
                        text: qsTr("框选后的鼠标操作")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }
                    Text {
                        width: parent.width
                        text: qsTr("选好区域后使用以下动作。标注时右键撤回当前笔画，其他鼠标动作暂停；Esc 可取消选区、重新框选。")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                    Text {
                        width: parent.width
                        text: qsTr("识别出的文字可直接复制、送到 Win顺 搜索，或用网页搜索列表中的第一个网站查询。")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                    Repeater {
                        model: [
                            { title: qsTr("双击左键"), prop: "captureMouseDoubleLeft", withBack: false },
                            { title: qsTr("右键"), prop: "captureMouseRight", withBack: true },
                            { title: qsTr("中键"), prop: "captureMouseMiddle", withBack: false },
                            { title: qsTr("侧键（后退）"), prop: "captureMouseBack", withBack: false },
                            { title: qsTr("侧键（前进）"), prop: "captureMouseForward", withBack: false }
                        ]

                        delegate: Item {
                            id: mouseRow

                            required property var modelData
                            required property int index
                            readonly property var choices: modelData.withBack ? card.rightChoices : card.mouseChoices
                            readonly property string value: card.mouseValues[index]
                            readonly property int current: choices.findIndex(c => c.value === value)

                            width: parent.width
                            height: card.narrow ? mouseLabel.implicitHeight + 8 + pick.height : 40

                            Text {
                                id: mouseLabel
                                anchors.left: parent.left
                                width: card.narrow ? parent.width : parent.width - pick.width - 12
                                y: card.narrow ? 0 : (parent.height - height) / 2
                                text: mouseRow.modelData.title
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                wrapMode: Text.Wrap
                            }
                            FlatButton {
                                id: pick
                                anchors.right: parent.right
                                implicitWidth: Math.min(card.pickWidth, parent.width)
                                y: card.narrow ? mouseLabel.implicitHeight + 8 : (parent.height - height) / 2
                                text: mouseRow.current >= 0 ? mouseRow.choices[mouseRow.current].text : ""
                                trailingGlyph: String.fromCharCode(0xE70D) // ChevronDown
                                onClicked: pickMenu.open = !pickMenu.open
                                onVisibleChanged: if (!visible) pickMenu.open = false

                                DropMenu {
                                    id: pickMenu
                                    anchor: pick
                                    entries: mouseRow.choices.map((c, i) => ({
                                        text: c.text,
                                        glyph: i === mouseRow.current ? String.fromCharCode(0xE73E) : "" // CheckMark
                                    }))
                                    onActivated: (i) => card.editor[mouseRow.modelData.prop] = mouseRow.choices[i].value
                                }
                            }
                        }
                    }
                }
            }

            Column {
                objectName: "capturePanel3"
                visible: card.currentGroup === 3
                width: parent.width
                spacing: 16

                Item { // what a double click on a pin does
                    width: parent.width
                    height: card.narrow ? pinClickTexts.implicitHeight + 8 + pinClickChoices.implicitHeight
                        : Math.max(pinClickTexts.implicitHeight, pinClickChoices.implicitHeight)

                    Column {
                        id: pinClickTexts
                        anchors.left: parent.left
                        anchors.right: card.narrow ? parent.right : pinClickChoices.left
                        anchors.rightMargin: card.narrow ? 0 : 12
                        spacing: 4

                        Text {
                            width: parent.width
                            text: qsTr("双击贴图")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.Wrap
                        }
                        Text {
                            width: parent.width
                            text: qsTr("复位：回到原来的大小；关闭：贴图不要了；收起：缩成小方块，点一下再展开。按 Esc 总是关闭")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                    Row {
                        id: pinClickChoices
                        anchors.right: parent.right
                        y: card.narrow ? pinClickTexts.implicitHeight + 8 : (parent.height - height) / 2
                        spacing: 8

                        Repeater {
                            model: [
                                { value: "reset", label: qsTr("复位") },
                                { value: "close", label: qsTr("关闭") },
                                { value: "collapse", label: qsTr("收起") },
                                { value: "none", label: qsTr("不做") }
                            ]

                            delegate: FlatButton {
                                required property var modelData

                                highlighted: card.editor.capturePinDoubleClick === modelData.value
                                text: modelData.label
                                onClicked: card.editor.capturePinDoubleClick = modelData.value
                            }
                        }
                    }
                }

                HotkeyRow {
                    id: lastPinHotkeyRow
                    objectName: "capturePinImageHotkey"
                    title: qsTr("贴图的快捷键")
                    words: qsTr("优先贴剪贴板里的图片；没有新图片时，把最近截图贴回原位置并保留标注。不设时仍可从托盘菜单操作")
                    hotkey: card.editor.captureLastPinHotkey
                    error: card.editor.captureLastPinHotkeyError
                    onRecorded: (hotkey) => card.editor.captureLastPinHotkey = hotkey
                }

                HotkeyRow {
                    id: pinsHotkeyRow
                    title: qsTr("隐藏 / 显示全部贴图的快捷键")
                    words: qsTr("按一下藏起所有贴图，再按一下放回来；新贴一张图时也会放回来。不设就只能从托盘菜单或贴图的右键菜单操作")
                    hotkey: card.editor.capturePinsHotkey
                    error: card.editor.capturePinsHotkeyError
                    onRecorded: (hotkey) => card.editor.capturePinsHotkey = hotkey
                }

                HotkeyRow {
                    id: restoreHotkeyRow
                    title: qsTr("找回穿透贴图的快捷键")
                    words: qsTr("设成“鼠标穿透”的贴图点不到时，按它让所有贴图都能点；平时按住 Ctrl 把鼠标放上去也行")
                    hotkey: card.editor.capturePinsRestoreHotkey
                    error: card.editor.capturePinsRestoreHotkeyError
                    onRecorded: (hotkey) => card.editor.capturePinsRestoreHotkey = hotkey
                }
            }

            Column {
                objectName: "capturePanel4"
                visible: card.currentGroup === 4
                width: parent.width
                spacing: 16

                Column { // smart redaction (4.1)
                    width: parent.width
                    spacing: 8

                    Text {
                        width: parent.width
                        text: qsTr("智能打码")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }
                    Text {
                        width: parent.width
                        text: qsTr("在打码工具里点“智能打码”（或按 Shift+B），自动找出图里的手机号、身份证号、银行卡号、邮箱、密钥和密码、本机用户名，一次全部盖住，Ctrl+Z 一次撤销。识别在本机完成，图和文字都不会发出去；自动找的可能有漏，发出去前再看一眼")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                    Repeater {
                        model: [
                            { key: "captureSmartPrescan", title: qsTr("框选后自动识别"),
                              words: qsTr("选好区域停一会儿，就在后台找要打码的内容，找到了在工具条边上提示“发现 N 处”。关掉后，点“智能打码”时才识别") },
                            { key: "captureSmartIpMac", title: qsTr("也打码 IP 和 MAC 地址"), words: "" },
                            { key: "captureSmartAllText", title: qsTr("把所有文字都打码"), words: qsTr("点“智能打码”就盖住图里每一行字") }
                        ]
                        delegate: Item {
                            id: smartOption

                            required property var modelData

                            width: parent.width
                            height: Math.max(smartTexts.implicitHeight, smartToggle.implicitHeight)

                            Column {
                                id: smartTexts
                                anchors.left: parent.left
                                anchors.right: smartToggle.left
                                anchors.rightMargin: 12
                                spacing: 4

                                Text {
                                    width: parent.width
                                    text: smartOption.modelData.title
                                    color: Theme.text
                                    font.pixelSize: Theme.fontBody
                                    wrapMode: Text.Wrap
                                }
                                Text {
                                    visible: smartOption.modelData.words.length > 0
                                    width: parent.width
                                    text: smartOption.modelData.words
                                    color: Theme.subtext
                                    font.pixelSize: Theme.fontCaption
                                    wrapMode: Text.Wrap
                                }
                            }
                            ToggleSwitch {
                                id: smartToggle
                                anchors.right: parent.right
                                checked: card.editor[smartOption.modelData.key]
                                onToggled: (on) => card.editor[smartOption.modelData.key] = on
                            }
                        }
                    }
                    Item { // the user's own words
                        width: parent.width
                        height: card.narrow ? wordsTexts.implicitHeight + 8 + wordsBox.implicitHeight
                        : Math.max(wordsTexts.implicitHeight, wordsBox.implicitHeight)

                        Column {
                            id: wordsTexts
                            anchors.left: parent.left
                            anchors.right: card.narrow ? parent.right : wordsBox.left
                            anchors.rightMargin: card.narrow ? 0 : 12
                            spacing: 4

                            Text {
                                width: parent.width
                                text: qsTr("我自己的词")
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                wrapMode: Text.Wrap
                            }
                            Text {
                                width: parent.width
                                text: qsTr("图里出现这些词就打码，用逗号隔开，比如项目代号、客户名")
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                            }
                        }
                        InputBox {
                            id: wordsBox
                            objectName: "captureSmartWords"
                            anchors.right: parent.right
                            y: card.narrow ? wordsTexts.implicitHeight + 8 : (parent.height - height) / 2
                            width: 220
                            text: card.editor.captureSmartWords
                            maximumLength: 400
                            function commit() {
                                card.editor.captureSmartWords = text
                                text = Qt.binding(() => card.editor.captureSmartWords)
                            }
                            onAccepted: commit()
                            onEditingFinished: commit()
                        }
                    }
                }
            }

            Column {
                objectName: "capturePanel5"
                visible: card.currentGroup === 5
                width: parent.width
                spacing: 16

                Column { // which action buttons the toolbar shows (#57)
                    width: parent.width
                    spacing: 8

                    Text {
                        width: parent.width
                        text: qsTr("工具条上的按钮")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }
                    Text {
                        width: parent.width
                        text: qsTr("亮起的按钮会显示，点一下切换。隐藏后快捷键仍可使用；标注工具始终显示。识别文字没有快捷键，隐藏后可在“鼠标”分类设置动作。")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                    Flow {
                        width: parent.width
                        spacing: 8

                        Repeater {
                            model: card.toolbarButtons

                            delegate: FlatButton {
                                id: toolbarChoice

                                required property var modelData
                                readonly property bool shown: card.hiddenButtons.indexOf(modelData.id) < 0

                                highlighted: shown
                                text: modelData.text
                                onClicked: card.setButtonShown(modelData.id, !shown)
                            }
                        }
                    }
                }
            }

        }
    }
}
