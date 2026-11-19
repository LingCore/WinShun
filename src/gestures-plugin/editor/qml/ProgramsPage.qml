pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Gestures for one program: the programs that have rules of their own on
// the left (and a button to add one), the chosen program's gestures and
// wheel on the right, laid out as the other pages are. The item its left
// section reads (see SettingsSection.qml).
Item {
    id: page

    required property GestureEditor editor
    required property ActionPicker picker
    required property ProgramPicker programPicker
    property real dpr: Screen.devicePixelRatio
    property Flickable flick: null
    property string program // the one chosen (the first when this is not one of them)

    readonly property int currentPage: -1
    readonly property int currentTab: 0
    readonly property var programs: editor.programs
    readonly property string current: {
        for (const entry of programs) {
            if (entry.name === program)
                return program
        }
        return programs.length > 0 ? programs[0].name : ""
    }

    implicitHeight: Math.max(left.height, right.implicitHeight)
    width: parent ? parent.width : 0

    SettingsSection {
        id: left
        x: 0
        width: 216
        title: qsTr("程序")
        glyph: "ph:app-window"

        Repeater {
            model: page.programs

            delegate: Item {
                id: entry

                required property var modelData
                readonly property bool selected: modelData.name === page.current
                readonly property real edge: Theme.edge(page.dpr)

                width: parent.width
                height: 48

                Hook {
                    x: entry.edge
                    width: parent.width - 2 * entry.edge
                    height: entry.edge
                    part: entry.y > 0 ? Theme.separator : null
                    extra: ({ card: true })
                    orientation: Qt.Horizontal
                    dpr: page.dpr
                }
                Rectangle { // the one chosen, as the settings' own list marks its category
                    anchors.fill: parent
                    anchors.margins: 4
                    radius: Theme.controlRadius
                    color: entry.selected ? Theme.navSelected : pick.containsMouse ? Theme.navHover : "transparent"
                }
                Rectangle {
                    visible: entry.selected
                    x: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 3
                    height: 16
                    radius: Theme.lineRadius
                    color: Theme.accent
                }
                Glyph {
                    x: 16
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: "ph:app-window"
                    size: 18
                    color: entry.selected ? Theme.text : Theme.subtext
                }
                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: 44
                    anchors.right: parent.right
                    anchors.rightMargin: 36
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    Text {
                        width: parent.width
                        text: entry.modelData.name
                        elide: Text.ElideRight
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: entry.selected ? Theme.fontStrong : Font.Normal
                    }
                    Text {
                        width: parent.width
                        text: entry.modelData.count > 0 ? qsTr("%1 个手势").arg(entry.modelData.count) : qsTr("还没有手势")
                        elide: Text.ElideRight
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption - 2
                    }
                }
                MouseArea {
                    id: pick
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.program = entry.modelData.name
                }
                Item { // drop the program's gestures: shown while the pointer is on the row
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24
                    height: 24
                    visible: pick.containsMouse || dropArea.containsMouse

                    Hook {
                        id: dropPart
                        anchors.fill: parent
                        part: Theme.keySurface
                        hovered: dropArea.containsMouse
                        pressed: dropArea.pressed
                        flat: true
                        corner: Theme.smallControlRadius
                        role: "toolbar"
                        dpr: page.dpr
                    }
                    Glyph {
                        anchors.centerIn: parent
                        anchors.verticalCenterOffset: dropPart.out("contentOffsetY", 0)
                        glyph: "ph:x"
                        size: 12
                        color: dropArea.containsMouse ? Theme.danger : Theme.subtext
                    }
                    MouseArea {
                        id: dropArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.editor.removeProgram(entry.modelData.name)
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("不再给这个程序单独设手势")
                }
            }
        }
        Item { // no program yet
            width: parent.width
            height: 64
            visible: page.programs.length === 0

            Text {
                anchors.centerIn: parent
                width: parent.width - 32
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("还没有程序")
                color: Theme.subtext
                font.pixelSize: Theme.fontBody
            }
        }
        Item { // add one
            width: parent.width
            height: 56

            Hook {
                x: Theme.edge(page.dpr)
                width: parent.width - 2 * Theme.edge(page.dpr)
                height: Theme.edge(page.dpr)
                part: Theme.separator
                extra: ({ card: true })
                orientation: Qt.Horizontal
                dpr: page.dpr
            }
            FlatButton {
                id: addButton
                objectName: "addProgramButton"
                anchors.centerIn: parent
                width: parent.width - 24
                height: 32
                dpr: page.dpr
                glyph: "ph:plus"
                text: qsTr("添加程序")
                onClicked: page.programPicker.ask(addButton, page.programs.map(entry => entry.name), (name) => {
                    page.program = page.editor.addProgram(name)
                })
            }
        }
    }

    Item {
        id: right
        x: left.width + 16
        width: parent.width - x
        // The column its section reads (see SettingsSection.qml).
        readonly property int currentPage: -1
        readonly property int currentTab: 0
        implicitHeight: page.current.length > 0 ? list.implicitHeight : empty.height

        Loader {
            id: list
            width: parent.width
            active: page.current.length > 0
            visible: active

            sourceComponent: GestureList {
                editor: page.editor
                picker: page.picker
                mode: 2
                app: page.current
                dpr: page.dpr
                flick: page.flick
            }
        }
        SettingsSection {
            id: empty
            width: parent.width
            visible: page.current.length === 0
            title: qsTr("这个程序里的手势")
            glyph: "ph:hand-pointing"
            note: qsTr("这些手势只在这个程序里生效，并且优先于其它页上的同一个手势。先在左边添加一个程序")
        }
    }
}
