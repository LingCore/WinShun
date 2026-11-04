pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The screen capture's options (CaptureSettings: [Capture] in capture.ini),
// the page of the plugin's settings window: one tab (group) at a time, its
// options in cards as on Win顺's settings window (SettingsSection holding
// SettingRows). Up to 0.16 they were the folded part of the 截图 card on
// Win顺's 插件 page.
//
// The column the sections read (see SettingsSection.qml): one page, whose
// tabs are the groups; no search (SettingsSearch.qml here is a stand-in).
Column {
    id: card

    objectName: "captureOptions"
    required property CaptureSettings editor
    property int currentGroup: 0
    property real dpr: Screen.devicePixelRatio
    readonly property int currentPage: 0
    readonly property int currentTab: currentGroup
    readonly property var groups: [
        { title: qsTr("快捷键") },
        { title: qsTr("按键") },
        { title: qsTr("截图") },
        { title: qsTr("鼠标") },
        { title: qsTr("贴图") },
        { title: qsTr("打码") },
        { title: qsTr("工具条") }
    ]

    spacing: 20

    // The hotkey box records: the plugin lets go of its hotkeys meanwhile (CaptureApp).
    readonly property bool recordingHotkey: !!group0.page?.recordingHotkey || !!group4.page?.recordingHotkey || keyRecording !== null
    // The key box (KeyRow) recording now: the plugin's hotkeys are let go of
    // then too, so that one of them pressed there is refused, not acted on.
    property Item keyRecording: null
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
        { id: "copy", text: qsTr("复制") },
        { id: "help", text: qsTr("全部按键") }
    ]
    // The same buttons in the toolbar's runs, a row of switches each.
    readonly property var toolbarRows: [
        { title: qsTr("编辑"), ids: ["undo", "redo"] },
        { title: qsTr("截图方式"), ids: ["long", "record", "delay"] },
        { title: qsTr("识别"), ids: ["qr", "ocr"] },
        { title: qsTr("结束截图"), ids: ["pin", "save", "cancel", "copy"] },
        { title: qsTr("帮助"), ids: ["help"] }
    ]
    function setButtonShown(id, shown) {
        const left = hiddenButtons.filter(h => h.length > 0 && h !== id)
        if (!shown)
            left.push(id)
        editor.captureToolbarHidden = left.join(",")
    }

    // An option that is on or off.
    component ToggleRow: SettingRow {
        id: toggleRow

        property string key

        ToggleSwitch {
            dpr: card.dpr
            checked: card.editor[toggleRow.key]
            onToggled: (on) => card.editor[toggleRow.key] = on
        }
    }

    // A shortcut the user records (no default: PixPin and Snipaste have the usual ones), with what went wrong registering it.
    component HotkeyRow: SettingRow {
        id: row

        property string hotkey
        property string error
        readonly property bool recording: recorder.recording
        signal recorded(string hotkey)

        FlatButton {
            dpr: card.dpr
            visible: row.hotkey.length > 0 && !recorder.recording
            flat: true
            text: qsTr("清除")
            onClicked: row.recorded("")
        }
        HotkeyRecorder {
            id: recorder
            hotkey: row.hotkey
            onRecorded: (hotkey) => row.recorded(hotkey)
        }

        bodyShown: row.error.length > 0
        body: Text {
            width: parent.width
            text: row.error
            color: Theme.danger
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
    }

    // A key inside the capture or a pin (#187; CaptureSettings.captureKeys):
    // any key, Esc and Space too; one refused says why underneath and the box
    // goes on listening. 恢复默认 shows while it is not the default, 清除 while
    // the pointer is on the row (or the keyboard in the box).
    component KeyRow: SettingRow {
        id: keyRow

        required property string keyId
        property bool capture: false // the capture's: Esc stops recording there (it is never one of them)
        readonly property var info: card.editor.captureKeys[keyId]
        property string error

        // A click takes no focus: the box goes on recording until it is done here.
        FlatButton {
            dpr: card.dpr
            visible: keyRow.info.text.length > 0 && (keyRow.hovered || keyRecorder.recording || activeFocus)
            flat: true
            text: qsTr("清除")
            onClicked: {
                keyRecorder.stop()
                keyRow.error = ""
                card.editor.clearCaptureKey(keyRow.keyId)
            }
        }
        FlatButton {
            dpr: card.dpr
            visible: keyRow.info.changed
            flat: true
            text: qsTr("恢复默认")
            onClicked: {
                keyRecorder.stop()
                keyRow.error = ""
                card.editor.resetCaptureKey(keyRow.keyId)
            }
        }
        HotkeyRecorder {
            id: keyRecorder
            objectName: "captureKey" + keyRow.keyId
            keyMode: true
            // Ctrl+Shift+F12 and the like fit (about 170): the boxes line up; only three modifiers grow one.
            keyMinimumWidth: 180
            hotkey: keyRow.info.text
            prompt: qsTr("请按下按键…")
            placeholder: qsTr("未设置")
            onKeyPressed: (key, modifiers, scanCode) => {
                if (keyRow.capture && key === Qt.Key_Escape && modifiers === Qt.NoModifier) {
                    stop()
                    return
                }
                keyRow.error = card.editor.setCaptureKey(keyRow.keyId, key, modifiers, scanCode)
                if (keyRow.error.length === 0)
                    stop()
            }
            onRecordingChanged: {
                if (recording) {
                    keyRow.error = ""
                    card.keyRecording = keyRow
                } else if (card.keyRecording === keyRow) {
                    card.keyRecording = null
                }
            }
        }

        bodyShown: keyRow.error.length > 0 || keyRow.info.conflict.length > 0
        body: Text {
            width: parent.width
            text: keyRow.error.length > 0 ? keyRow.error : keyRow.info.conflict
            color: Theme.danger
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
    }

    // A tab's cards are made the first time it is shown, the other tabs' once
    // the window is up, one per turn (fill), not all with the window: all
    // seven at once were the largest part of the first opening (80 of its
    // 190 ms of the GUI thread before the first frame, measured 2026-11-04),
    // many times longer on a busy computer, the window already showing.
    component GroupSlot: Item {
        id: slot

        required property int group
        required property Component content
        property GroupPage page: null

        function make() {
            if (!slot.page)
                slot.page = slot.content.createObject(slot) as GroupPage
        }

        width: parent ? parent.width : 0
        implicitHeight: slot.page ? slot.page.implicitHeight : 0
        visible: card.currentGroup === slot.group
    }

    // A tab's cards: the column its sections read (SettingsSection.qml).
    component GroupPage: Column {
        readonly property int currentPage: card.currentPage
        readonly property int currentTab: card.currentTab
        readonly property real dpr: card.dpr
        property bool recordingHotkey: false // one of its hotkey boxes records

        width: parent ? parent.width : 0
        spacing: 20
    }

    readonly property list<GroupSlot> slots: [group0, group1, group2, group3, group4, group5, group6]
    property bool allMade: false
    Component.onCompleted: slots[currentGroup].make()
    onCurrentGroupChanged: slots[currentGroup].make()

    Timer { // fill: the tabs not made yet, once the window shows
        interval: 100
        repeat: true
        running: !card.allMade && card.Window.window !== null && card.Window.window.visible
        onTriggered: {
            for (let i = 0; i < card.slots.length; ++i) {
                if (!card.slots[i].page) {
                    card.slots[i].make()
                    return
                }
            }
            card.allMade = true
        }
    }

    // ---- 快捷键 ----

    GroupSlot {
        id: group0
        group: 0
        content: Component {
            GroupPage {
                recordingHotkey: hotkeyRecorder.recording

                SettingsSection {
                    page: 0
                    tab: 0
                    width: parent.width
                    title: qsTr("开始截图")
                    glyph: "" // Keyboard

                    SettingRow {
                        title: qsTr("快捷键")
                        description: qsTr("按它开始截图，截图时再按一次取消")

                        FlatButton {
                            dpr: card.dpr
                            visible: card.editor.captureHotkey.length > 0 && !hotkeyRecorder.recording
                            flat: true
                            text: qsTr("清除")
                            onClicked: card.editor.captureHotkey = ""
                        }
                        HotkeyRecorder {
                            id: hotkeyRecorder
                            hotkey: card.editor.captureHotkey
                            onRecorded: (hotkey) => card.editor.captureHotkey = hotkey
                        }

                        bodyShown: card.editor.captureHotkeyError.length > 0
                        body: Text {
                            width: parent.width
                            text: card.editor.captureHotkeyError
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }

                    SettingRow {
                        title: qsTr("用 PrtScn 键截图")
                        description: qsTr("让 PrtScn 不再打开 Windows 的截图工具，改用这里的截图。Win顺 退出时还给截图工具，下次启动再接管；关掉这项也恢复原来的设置")

                        ToggleSwitch {
                            dpr: card.dpr
                            checked: card.editor.capturePrintScreen
                            onToggled: (on) => card.editor.capturePrintScreen = on
                        }

                        bodyShown: card.editor.captureNote.length > 0
                        body: Text {
                            width: parent.width
                            text: card.editor.captureNote
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }

                    SettingRow {
                        title: qsTr("全屏时不响应截图快捷键")
                        description: qsTr("其他程序全屏时，截图快捷键和 PrtScn 不弹出截图；仍能从菜单手动开始")

                        ToggleSwitch {
                            dpr: card.dpr
                            objectName: "capturePauseFullScreen"
                            checked: card.editor.capturePauseInFullScreen
                            onToggled: (on) => card.editor.capturePauseInFullScreen = on
                        }
                    }

                    SettingRow {
                        title: qsTr("截图时显示快捷键提示")
                        description: qsTr("停下来时在屏幕左下角列出此刻能用的按键，一按鼠标就消失，挡路时自动让开。关闭后截图时按 ? 仍可临时显示")

                        ToggleSwitch {
                            dpr: card.dpr
                            objectName: "captureKeyHintsSwitch"
                            checked: card.editor.captureKeyHints
                            onToggled: (on) => card.editor.captureKeyHints = on
                        }
                    }
                }

            }
        }
    }

    // ---- 按键 ----

    GroupSlot {
        id: group1
        group: 1
        content: Component {
            GroupPage {

                SettingsSection { // the keys inside the capture and the editing window (#187)
                    page: 0
                    tab: 1
                    width: parent.width
                    title: qsTr("截图时")
                    glyph: "" // Keyboard
                    note: qsTr("截图和编辑图片时按。清除后仍可点工具条上的按钮")

                    KeyRow {
                        keyId: "Pin"
                        capture: true
                        title: qsTranslate("ws::capture::CaptureKeys", "贴到屏幕")
                    }
                    KeyRow {
                        keyId: "Save"
                        capture: true
                        title: qsTranslate("ws::capture::CaptureKeys", "保存")
                    }
                }

                SettingsSection { // the keys of a pin window
                    page: 0
                    tab: 1
                    width: parent.width
                    title: qsTr("贴图窗口")
                    glyph: "" // Pin
                    note: qsTr("点一下贴图再按。清除后仍可从贴图的右键菜单操作")

                    Repeater {
                        model: [
                            { id: "PinClose", name: QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "关闭贴图") },
                            { id: "PinCopy", name: QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "复制") },
                            { id: "PinSave", name: QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "保存") },
                            { id: "PinCollapse", name: QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "收起/展开") },
                            { id: "PinTextPick", name: QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "选取文字") },
                            { id: "PinRotateRight", name: QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "向右转") },
                            { id: "PinRotateLeft", name: QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "向左转") },
                            { id: "PinFlipHorizontal", name: QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "左右翻转") },
                            { id: "PinFlipVertical", name: QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "上下翻转") }
                        ]

                        delegate: KeyRow {
                            required property var modelData
                            keyId: modelData.id
                            title: qsTranslate("ws::capture::CaptureKeys", modelData.name)
                        }
                    }
                }

            }
        }
    }

    // ---- 截图 ----

    GroupSlot {
        id: group2
        group: 2
        content: Component {
            GroupPage {

                SettingsSection {
                    page: 0
                    tab: 2
                    width: parent.width
                    title: qsTr("框选")
                    glyph: "" // TouchPointer

                    ToggleRow {
                        key: "captureMagnifier"
                        title: qsTr("框选时显示放大镜")
                        description: qsTr("在指针旁放大显示像素和颜色值。关闭后，按 C 复制颜色、按 Shift 换颜色格式也一并停用。")
                    }
                    ToggleRow {
                        key: "captureControlsUia"
                        title: qsTr("识别 WinUI 等程序里的控件")
                        description: qsTr("自动框选设置、计算器等程序中的按钮和侧栏。Chrome、Edge、VS Code 这类 Chromium 程序不问（一问它们就打开屏幕阅读器模式），只框子窗口或整个窗口，可手动框选；管理员权限程序中的控件可能不可用。")
                    }
                }

                SettingsSection {
                    page: 0
                    tab: 2
                    width: parent.width
                    title: qsTr("复制与保存")
                    glyph: "" // Copy

                    ToggleRow {
                        key: "captureCopyOnSelect"
                        title: qsTr("选好就复制")
                        description: qsTr("松开鼠标或点选窗口后立即复制，跳过标注和调整。关闭时可继续编辑，再按 Enter 复制，或按保存键保存。鼠标动作在“鼠标”分类设置。")
                    }
                    ToggleRow {
                        key: "capturePinOnCopy"
                        title: qsTr("复制截图时同时贴图")
                        description: qsTr("复制时同步贴到屏幕，也适用于“选好就复制”。只想偶尔使用，可在“鼠标”分类设置“复制并贴图”。")
                    }
                    ToggleRow {
                        key: "captureCursor"
                        title: qsTr("截图带上鼠标指针")
                        description: qsTr("在图片中保留截图那一刻的鼠标指针，复制、保存、贴图均生效。")
                    }
                    SettingRow {
                        title: qsTr("截图保存文件夹")
                        description: card.editor.captureSaveFolder.length > 0 ? card.editor.captureSaveFolder : qsTr("系统的“屏幕截图”文件夹")

                        FlatButton {
                            dpr: card.dpr
                            text: qsTr("使用系统默认")
                            enabled: card.editor.captureSaveFolder.length > 0
                            onClicked: card.editor.captureSaveFolder = ""
                        }
                        FlatButton {
                            dpr: card.dpr
                            text: qsTr("选择文件夹…")
                            onClicked: {
                                const chosen = card.editor.pickFolder(qsTr("选择截图保存文件夹"))
                                if (chosen.length > 0) card.editor.captureSaveFolder = chosen
                            }
                        }
                    }
                    ToggleRow {
                        key: "captureHdrFile"
                        title: qsTr("HDR 屏幕上同时保存 HDR 图片")
                        description: qsTr("在开着 HDR 的屏幕上保存截图时，PNG 旁边再存一份同名的 .jxr，亮的地方保持原来的亮度，可用 Windows 自带的“照片”查看。复制和贴图不变。")
                    }
                }

                SettingsSection {
                    page: 0
                    tab: 2
                    width: parent.width
                    title: qsTr("延时与回看")
                    glyph: "" // History

                    SettingRow { // the delayed capture's seconds
                        title: qsTr("延时截图")
                        description: qsTr("从托盘菜单“截图 › 延时截图”开始，或截图时按 D，适合截菜单和悬停提示。再次按截图快捷键或点击倒计时可取消。")

                        ScopeTabs {
                            objectName: "captureDelayTabs"
                            readonly property var seconds: [3, 5, 10]
                            labels: seconds.map(s => qsTr("%1 秒").arg(s))
                            current: seconds.indexOf(card.editor.captureDelay)
                            onActivated: (index) => card.editor.captureDelay = seconds[index]
                        }
                    }
                    SettingRow { // 之前的画面: how many captures , and . bring back
                        title: qsTr("截图时翻看之前的画面")
                        description: qsTr("截图时按 , 翻到上一次截图时的画面，按 . 往回翻：整个屏幕、选区和标注都恢复，可以重新框选或接着改。会在本机保存最近几次截图时的整屏画面；选“关闭”不再保存，并删除已保存的（不影响用关键词找截过的图）。")

                        ScopeTabs {
                            objectName: "captureHistoryTabs"
                            readonly property var counts: [0, 10, 30, 100]
                            labels: counts.map(n => n === 0 ? qsTr("关闭", "capture history") : qsTr("%1 次").arg(n))
                            current: counts.indexOf(card.editor.captureHistoryCount)
                            onActivated: (index) => card.editor.captureHistoryCount = counts[index]
                        }
                    }
                }

            }
        }
    }

    // ---- 鼠标 ----

    GroupSlot {
        id: group3
        group: 3
        content: Component {
            GroupPage {

                SettingsSection { // what the mouse buttons do after a selection (issue #46)
                    page: 0
                    tab: 3
                    width: parent.width
                    title: qsTr("框选后的鼠标操作")
                    glyph: "" // Mouse
                    note: qsTr("选好区域后使用以下动作。标注时右键撤回当前笔画，其他鼠标动作暂停；Esc 可取消选区、重新框选。")
                          + "\n" + qsTr("识别出的文字可直接复制、送到 Win顺 搜索，或用网页搜索列表中的第一个网站查询。")

                    Repeater {
                        model: [
                            { title: qsTr("双击左键"), prop: "captureMouseDoubleLeft", withBack: false },
                            { title: qsTr("右键"), prop: "captureMouseRight", withBack: true },
                            { title: qsTr("中键"), prop: "captureMouseMiddle", withBack: false },
                            { title: qsTr("侧键（后退）"), prop: "captureMouseBack", withBack: false },
                            { title: qsTr("侧键（前进）"), prop: "captureMouseForward", withBack: false }
                        ]

                        delegate: SettingRow {
                            id: mouseRow

                            required property var modelData
                            required property int index
                            readonly property var choices: modelData.withBack ? card.rightChoices : card.mouseChoices
                            readonly property string value: card.mouseValues[index]
                            readonly property int current: choices.findIndex(c => c.value === value)

                            title: modelData.title

                            FlatButton {
                                dpr: card.dpr
                                id: pick
                                implicitWidth: card.pickWidth
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
        }
    }

    // ---- 贴图 ----

    GroupSlot {
        id: group4
        group: 4
        content: Component {
            GroupPage {
                recordingHotkey: lastPinHotkeyRow.recording || pinsHotkeyRow.recording || restoreHotkeyRow.recording || presentHotkeyRow.recording

                SettingsSection {
                    page: 0
                    tab: 4
                    width: parent.width
                    title: qsTr("贴图操作")
                    glyph: "" // Pin

                    SettingRow { // what a double click on a pin does
                        title: qsTr("双击贴图")
                        description: qsTr("复位：回到原来的大小；关闭：贴图不要了；收起：缩成小方块，点一下再展开。贴图里的按键在“按键”分类设置")

                        ScopeTabs {
                            objectName: "capturePinClickTabs"
                            readonly property var values: ["reset", "close", "collapse", "none"]
                            labels: [qsTr("复位"), qsTr("关闭"), qsTr("收起"), qsTr("不做")]
                            current: values.indexOf(card.editor.capturePinDoubleClick)
                            onActivated: (index) => card.editor.capturePinDoubleClick = values[index]
                        }
                    }
                }

                SettingsSection {
                    page: 0
                    tab: 4
                    width: parent.width
                    title: qsTr("快捷键")
                    glyph: "" // Keyboard

                    HotkeyRow {
                        id: lastPinHotkeyRow
                        objectName: "capturePinImageHotkey"
                        title: qsTr("贴图的快捷键")
                        description: qsTr("优先贴剪贴板里的图片；没有新图片时，把最近截图贴回原位置并保留标注。不设时仍可从托盘菜单操作")
                        hotkey: card.editor.captureLastPinHotkey
                        error: card.editor.captureLastPinHotkeyError
                        onRecorded: (hotkey) => card.editor.captureLastPinHotkey = hotkey
                    }
                    HotkeyRow {
                        id: pinsHotkeyRow
                        title: qsTr("隐藏 / 显示全部贴图的快捷键")
                        description: qsTr("按一下藏起所有贴图，再按一下放回来；新贴一张图时也会放回来。不设就只能从托盘菜单或贴图的右键菜单操作")
                        hotkey: card.editor.capturePinsHotkey
                        error: card.editor.capturePinsHotkeyError
                        onRecorded: (hotkey) => card.editor.capturePinsHotkey = hotkey
                    }
                    HotkeyRow {
                        id: restoreHotkeyRow
                        title: qsTr("找回穿透贴图的快捷键")
                        description: qsTr("设成“鼠标穿透”的贴图点不到时，按它让所有贴图都能点；平时按住 Ctrl 把鼠标放上去也行")
                        hotkey: card.editor.capturePinsRestoreHotkey
                        error: card.editor.capturePinsRestoreHotkeyError
                        onRecorded: (hotkey) => card.editor.capturePinsRestoreHotkey = hotkey
                    }
                    HotkeyRow { // the presenter (#189)
                        id: presentHotkeyRow
                        objectName: "capturePresentHotkey"
                        title: qsTr("演示 / 白板的快捷键")
                        description: qsTr("在屏幕上直接画、用激光笔指，W/K 切白板黑板，Esc 让鼠标去操作下面的程序；画的时候再按一次结束。录屏或共享时请选显示器采集或共享整个屏幕")
                        hotkey: card.editor.capturePresentHotkey
                        error: card.editor.capturePresentHotkeyError
                        onRecorded: (hotkey) => card.editor.capturePresentHotkey = hotkey
                    }
                }

            }
        }
    }

    // ---- 打码 ----

    GroupSlot {
        id: group5
        group: 5
        content: Component {
            GroupPage {

                SettingsSection { // smart redaction (4.1)
                    page: 0
                    tab: 5
                    width: parent.width
                    title: qsTr("智能打码")
                    glyph: "" // Admin (shield)
                    note: qsTr("在打码工具里点“智能打码”（或按 Shift+B），自动找出图里的手机号、身份证号、银行卡号、邮箱、密钥和密码、本机用户名，一次全部盖住，Ctrl+Z 一次撤销。识别在本机完成，图和文字都不会发出去；自动找的可能有漏，发出去前再看一眼")

                    ToggleRow {
                        key: "captureSmartPrescan"
                        title: qsTr("框选后自动识别")
                        description: qsTr("选好区域停一会儿，就在后台找要打码的内容，找到了在工具条边上提示“发现 N 处”。关掉后，点“智能打码”时才识别")
                    }
                    ToggleRow {
                        key: "captureSmartIpMac"
                        title: qsTr("也打码 IP 和 MAC 地址")
                    }
                    ToggleRow {
                        key: "captureSmartAllText"
                        title: qsTr("把所有文字都打码")
                        description: qsTr("点“智能打码”就盖住图里每一行字")
                    }
                    SettingRow { // the user's own words
                        title: qsTr("我自己的词")
                        description: qsTr("图里出现这些词就打码，用逗号隔开，比如项目代号、客户名")

                        InputBox {
                            dpr: card.dpr
                            id: wordsBox
                            objectName: "captureSmartWords"
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
        }
    }

    // ---- 工具条 ----

    GroupSlot {
        id: group6
        group: 6
        content: Component {
            GroupPage {

                SettingsSection { // which action buttons the toolbar shows (#57)
                    page: 0
                    tab: 6
                    width: parent.width
                    title: qsTr("工具条上的按钮")
                    glyph: "" // ViewAll
                    note: qsTr("勾选的按钮会显示，点一下切换。隐藏后快捷键仍可使用；标注工具始终显示。识别文字没有快捷键，隐藏后可在“鼠标”分类设置动作。")

                    Repeater {
                        model: card.toolbarRows

                        delegate: SettingRow {
                            id: toolbarRow

                            required property var modelData
                            required property int index

                            title: modelData.title

                            ScopeTabs {
                                objectName: "captureToolbarTabs" + toolbarRow.index
                                multiSelect: true
                                labels: toolbarRow.modelData.ids.map(id => card.toolbarButtons.find(b => b.id === id).text)
                                checkedIndices: toolbarRow.modelData.ids.map((id, i) => card.hiddenButtons.indexOf(id) < 0 ? i : -1).filter(i => i >= 0)
                                onToggled: (index, on) => card.setButtonShown(toolbarRow.modelData.ids[index], on)
                            }
                        }
                    }
                }
            }
        }
    }
}
