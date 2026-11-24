pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Compact themed input prompt for creating or renaming a pin group (#451).
Window {
    id: prompt

    required property WindowFrame frame

    property string heading: qsTr("新建贴图分组")
    property string hint: qsTr("输入分组名称，用于按工作场景分组显示贴图")
    property string confirmLabel: qsTr("确定")
    property string errorText: ""

    signal submitted(string name)
    signal cancelled()

    readonly property real dpr: Screen.devicePixelRatio

    width: 360
    height: Theme.snap(body.implicitHeight + titleBar.height + 24, dpr)
    minimumWidth: 320
    maximumWidth: 420
    minimumHeight: height
    maximumHeight: height
    title: heading
    color: Theme.backdrop && SystemTheme.materials && prompt.active ? "transparent" : Theme.page
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint

    function openWith(titleText: string, descriptionText: string, initialText: string, buttonText: string) {
        heading = titleText
        hint = descriptionText
        confirmLabel = buttonText
        errorText = ""
        nameInput.text = initialText
        show()
        raise()
        requestActivate()
        Qt.callLater(() => {
            nameInput.takeFocus()
            nameInput.select(0, nameInput.text.length)
        })
    }

    function submit() {
        prompt.submitted(nameInput.text)
    }

    function cancel() {
        errorText = ""
        prompt.cancelled()
        prompt.hide()
    }

    Shortcut {
        sequence: StandardKey.Cancel
        onActivated: prompt.cancel()
    }

    TitleBar {
        id: titleBar
        z: 1
        width: parent.width
        frame: prompt.frame
        title: prompt.heading
        maximizable: false
        closeIntercepted: true
        onCloseRequested: prompt.cancel()
    }

    Column {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.margins: 16
        anchors.topMargin: 8
        spacing: 10

        Text {
            width: parent.width
            visible: prompt.hint.length > 0
            text: prompt.hint
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
        }

        InputBox {
            id: nameInput
            objectName: "pinGroupNameInput"
            width: parent.width
            dpr: prompt.dpr
            maximumLength: 32
            placeholder: qsTr("例如：工作、参考、设计")
            error: prompt.errorText.length > 0
            onTextChanged: if (prompt.errorText.length > 0) prompt.errorText = ""
            onAccepted: prompt.submit()
        }

        Text {
            width: parent.width
            visible: prompt.errorText.length > 0
            text: prompt.errorText
            color: Theme.danger
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
        }

        Row {
            anchors.right: parent.right
            spacing: 8

            FlatButton {
                dpr: prompt.dpr
                text: qsTr("取消")
                onClicked: prompt.cancel()
            }
            FlatButton {
                objectName: "pinGroupConfirmButton"
                dpr: prompt.dpr
                highlighted: true
                text: prompt.confirmLabel
                enabled: nameInput.text.trim().length > 0
                onClicked: prompt.submit()
            }
        }
    }

    WindowEdge {
        fill: prompt.color
    }
}
