pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// What the AI changed in Win顺's settings, said after the fact (App::
// showSettingsChangedNotice): a small notice in the corner of the work area
// with 撤销. It never takes the focus (the AI client's window stays in front;
// WindowFrame::setNoActivate and Qt::WindowDoesNotAcceptFocus), goes after
// 8 s, and stays while the mouse is on it. A newer batch replaces it.
// `host` is the App: undoSettingsBatch, showSettingsFor, restartExplorer.
Window {
    id: notice

    required property WindowFrame frame
    required property QtObject host

    // {id, client, count, explorerRestart, items: [{title, old_text, new_text,
    // diff_text, is_list, effect}], failed: [{title, reason}]}, as
    // App::SettingsBatch::toVariant.
    property var batch: ({})
    // After 撤销: what came of it, in words; empty until then.
    property string outcome
    property bool outcomeOk: false
    property bool needsRestart: false // File Explorer has to restart for some of it to show

    readonly property int padding: 16
    readonly property int shownMax: 4
    readonly property var items: batch.items ?? []
    readonly property var failures: batch.failed ?? []
    readonly property real dpr: Screen.devicePixelRatio

    // Called by the app with each new batch, before it places the window.
    function present(newBatch) {
        batch = newBatch
        outcome = ""
        outcomeOk = false
        needsRestart = newBatch.explorerRestart === true
        hideTimer.interval = 8000
        hideTimer.restart()
    }

    function dismiss() {
        hideTimer.stop()
        visible = false
    }

    function undo() {
        const result = host.undoSettingsBatch(batch.id)
        outcome = result.text
        outcomeOk = result.ok === true
        needsRestart = result.explorerRestart === true
        hideTimer.interval = needsRestart ? 5000 : 2500
        hideTimer.restart()
    }

    function effectNote(effect) {
        if (effect === "explorer_restart")
            return qsTr("重启资源管理器后生效")
        if (effect === "app_restart")
            return qsTr("重启 Win顺 后生效")
        return ""
    }

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.backdrop && SystemTheme.materials ? "transparent" : Theme.background
    title: qsTr("设置已被 AI 修改") // Qt adds " - Win顺"
    width: 400 + 20 * Theme.step
    height: body.implicitHeight + 2 * padding

    Timer {
        id: hideTimer
        interval: 8000
        running: notice.visible && !hover.hovered
        onTriggered: notice.dismiss()
    }

    Item { // the whole window: the timer waits while the mouse is anywhere on it
        anchors.fill: parent

        HoverHandler {
            id: hover
        }
    }

    Column {
        id: body
        x: notice.padding
        y: notice.padding
        width: parent.width - 2 * notice.padding
        spacing: 10

        Item {
            width: parent.width
            height: Math.max(headline.height, closeButton.height)

            Glyph {
                id: tick
                anchors.top: headline.top
                anchors.topMargin: 1
                width: 18
                height: 18
                // 撤销 that could not (all of it) put back is a caution, not a tick.
                glyph: notice.outcome.length > 0 && !notice.outcomeOk ? "" : "" // Warning, Accept
                size: 14
                color: notice.outcome.length > 0 && !notice.outcomeOk ? Theme.caution : Theme.success
            }
            Text {
                id: headline
                anchors.left: tick.right
                anchors.leftMargin: 8
                anchors.right: closeButton.left
                anchors.rightMargin: 8
                text: notice.outcome.length > 0 ? notice.outcome
                    : qsTr("已按 %1 的请求修改了 %Ln 项设置", "", notice.batch.count ?? 0).arg(notice.batch.client ?? "")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: Theme.uiBody
                font.weight: Theme.fontStrong
            }
            Rectangle {
                id: closeButton
                anchors.right: parent.right
                width: 24
                height: 24
                radius: Theme.smallControlRadius
                color: closeArea.pressed ? Theme.controlPressed : closeArea.containsMouse ? Theme.controlHover : "transparent"

                Glyph {
                    anchors.centerIn: parent
                    glyph: "" // Cancel
                    size: 10
                    color: Theme.subtext
                }
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: notice.dismiss()
                }
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("关闭")
            }
        }

        // The changes, a few lines; the rest is in the settings window.
        Column {
            width: parent.width
            spacing: 6
            visible: notice.outcome.length === 0

            Repeater {
                model: notice.items.slice(0, notice.shownMax)

                delegate: Column {
                    id: line

                    required property var modelData

                    width: body.width
                    spacing: 1

                    Text {
                        width: parent.width
                        text: line.modelData.title + qsTr("：") + (line.modelData.is_list === true ? line.modelData.diff_text
                            : line.modelData.old_text + " → " + line.modelData.new_text)
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        color: Theme.text
                        font.pixelSize: Theme.uiCaption
                    }
                    Text {
                        width: parent.width
                        visible: text.length > 0
                        text: notice.effectNote(line.modelData.effect)
                        color: Theme.subtext
                        font.pixelSize: Theme.uiCaption
                    }
                }
            }
            Text {
                visible: notice.items.length > notice.shownMax
                text: qsTr("还有 %Ln 项", "", notice.items.length - notice.shownMax)
                color: Theme.subtext
                font.pixelSize: Theme.uiCaption
            }
        }

        // What did not take and stays as it was.
        Column {
            width: parent.width
            spacing: 2
            visible: notice.outcome.length === 0 && notice.failures.length > 0

            Repeater {
                model: notice.failures

                delegate: Text {
                    required property var modelData

                    width: body.width
                    text: modelData.title + qsTr("：") + modelData.reason
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    color: Theme.caution
                    font.pixelSize: Theme.uiCaption
                }
            }
        }

        Item {
            width: parent.width
            height: buttons.height

            Row {
                id: buttons
                anchors.right: parent.right
                spacing: 8

                FlatButton {
                    visible: notice.needsRestart
                    text: qsTr("重启资源管理器")
                    onClicked: {
                        notice.host.restartExplorer()
                        notice.dismiss()
                    }
                }
                FlatButton {
                    visible: notice.outcome.length === 0
                    text: qsTr("打开设置")
                    onClicked: {
                        notice.host.showSettingsFor(notice.items[0]?.key ?? "")
                        notice.dismiss()
                    }
                }
                FlatButton {
                    visible: notice.outcome.length === 0
                    text: qsTr("撤销")
                    onClicked: notice.undo()
                }
            }
        }
    }

    WindowEdge {}
}
