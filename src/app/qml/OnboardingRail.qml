pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The left side of the first-run setup (OnboardingWindow): the program's
// name, the keys that open it as chosen so far, drawn as keys, and the two
// steps. It only tells: every choice is made on the right.
Item {
    id: rail

    required property Onboarding setup
    required property real dpr

    readonly property bool chord: setup.activation === 3
    readonly property string tapKey: setup.activation === 1 ? "Alt" : setup.activation === 2 ? "Shift" : "Ctrl"
    readonly property list<string> keys: !chord ? [tapKey, tapKey]
                                       : setup.launchHotkey.length > 0 ? setup.launchHotkey.split("+") : []
    // The keys tried on the first page are these ones (the second page tries the screenshot's).
    readonly property bool trying: setup.testing && setup.page === 0
    readonly property bool passed: setup.testPassed && setup.page === 0 && !setup.committed

    Rectangle { // a shade over the window's own colour, in every style and with Mica behind
        anchors.fill: parent
        color: Qt.alpha(Theme.text, Theme.dark ? 0.045 : 0.03)
    }
    Hook { // the style's line between two parts (Theme.separator)
        id: seam
        anchors.right: parent.right
        width: seam.out("thickness", Theme.edge(rail.dpr))
        height: parent.height
        part: Theme.separator
        orientation: Qt.Vertical
        dpr: rail.dpr
    }

    Row {
        x: 24
        y: 22
        spacing: 10
        Image {
            anchors.verticalCenter: parent.verticalCenter
            width: 28
            height: 28
            source: "qrc:/branding/logo.svg"
            sourceSize.width: width
            sourceSize.height: height
            smooth: false
            Accessible.ignored: true
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Win顺")
            color: Theme.text
            font.pixelSize: Theme.fontTitle
            font.weight: Theme.fontStrong
        }
    }

    Column {
        id: hero
        // Fewer keys, larger ones: a chord of four or five still fits the rail.
        readonly property int capHeight: rail.keys.length <= 2 ? 44 : rail.keys.length === 3 ? 36 : 28
        readonly property int capFont: rail.keys.length <= 2 ? Theme.fontTitle : rail.keys.length === 3 ? Theme.fontBody : 12

        x: 24
        width: parent.width - 48
        y: Theme.snap((parent.height - height) / 2 - 12, rail.dpr)
        spacing: 14
        Accessible.role: Accessible.StaticText
        Accessible.name: how.text

        Row {
            id: caps
            x: Theme.snap((parent.width - width) / 2, rail.dpr)
            height: 44 // the tallest keys': the words below stay where they are
            spacing: rail.chord ? (rail.keys.length > 3 ? 4 : 6) : 10

            Repeater {
                model: rail.keys.length > 0 ? rail.keys : ["?"]

                Row {
                    id: part
                    required property int index
                    required property string modelData

                    anchors.verticalCenter: parent.verticalCenter
                    spacing: caps.spacing

                    Text {
                        visible: rail.chord && part.index > 0 && rail.keys.length <= 3
                        anchors.verticalCenter: parent.verticalCenter
                        text: "+"
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                    }
                    Item {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(height, Theme.snapUp(capText.implicitWidth + (hero.capHeight > 28 ? 24 : 14), rail.dpr))
                        height: hero.capHeight
                        opacity: rail.keys.length > 0 ? 1 : 0.5

                        Hook { // the style's key cap (Theme.keyHint)
                            id: capPart
                            anchors.fill: parent
                            part: Theme.keyHint
                            dpr: rail.dpr
                        }
                        Text {
                            id: capText
                            x: Theme.snap((parent.width - width) / 2, rail.dpr)
                            y: Theme.snap((parent.height - height) / 2 + capPart.out("contentOffsetY", 0), rail.dpr)
                            text: part.modelData
                            color: capPart.out("foreground", Theme.text)
                            font.pixelSize: hero.capFont
                            font.weight: Theme.fontStrong
                        }
                    }
                }
            }
        }

        Row {
            x: Theme.snap((parent.width - width) / 2, rail.dpr)
            spacing: 6

            Glyph {
                visible: rail.passed
                anchors.verticalCenter: parent.verticalCenter
                glyph: "\uE73E"
                size: 14
                color: Theme.success
            }
            Text {
                id: how
                width: Math.min(implicitWidth, hero.width - (rail.passed ? 20 : 0))
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: rail.passed ? qsTr("识别成功")
                    : rail.trying ? qsTr("现在按按看…")
                    : !rail.chord ? qsTr("连按两下 %1，呼出搜索框").arg(rail.tapKey)
                    : rail.keys.length > 0 ? qsTr("按下这组键，呼出搜索框")
                                           : qsTr("还没有录制快捷键")
                color: rail.passed ? Theme.success : rail.trying ? Theme.accent : Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
        }
    }

    Column {
        x: 24
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: parent.width - 48
        spacing: 10
        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("第 %1 步，共 2 步").arg(rail.setup.page + 1)

        Repeater {
            model: [qsTr("打开与搜索"), qsTr("剪贴板与截图")]

            Row {
                id: step
                required property int index
                required property string modelData
                readonly property bool done: rail.setup.committed || index < rail.setup.page
                readonly property bool current: !rail.setup.committed && index === rail.setup.page

                spacing: 10

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 22
                    height: 22
                    radius: 11
                    color: step.done || step.current ? Theme.accentFill : Theme.track

                    Glyph {
                        visible: step.done
                        anchors.centerIn: parent
                        glyph: "\uE73E"
                        size: 12
                        color: Theme.accentText
                    }
                    CenteredNumber {
                        visible: !step.done
                        text: step.index + 1
                        color: step.current ? Theme.accentText : Theme.subtext
                        font.pixelSize: 12
                        font.weight: Theme.fontStrong
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: step.modelData
                    color: step.current ? Theme.text : Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    font.weight: step.current ? Theme.fontStrong : Font.Normal
                }
            }
        }
    }
}
