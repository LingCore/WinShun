pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import WinShun

// The field only shows the choice; names are typed in the menu's own search row.
ChoiceBox {
    id: picker
    required property list<string> families
    property WindowFrame frame: null
    property string selectedFamily
    property string query
    property int cursor: 0
    readonly property int visibleRows: 5
    readonly property string defaultLabel: qsTr("默认（阿里巴巴普惠体）")
    // "" stands for the default font.
    readonly property list<string> matches: {
        const needle = query.trim().toLowerCase()
        const all = [""].concat(families)
        if (!needle)
            return all
        return all.filter(family => (family || defaultLabel).toLowerCase().includes(needle))
    }
    signal chosen(string family)

    function acceptQuery() {
        if (cursor < 0 || cursor >= matches.length)
            return
        chosen(matches[cursor])
        popup.close()
    }
    function moveCursor(step: int) {
        if (matches.length > 0)
            cursor = Math.max(0, Math.min(matches.length - 1, cursor + step))
    }

    objectName: "uiFontPicker"
    implicitWidth: 260
    implicitHeight: 36
    model: [defaultLabel].concat(families)
    currentIndex: Math.max(0, families.indexOf(selectedFamily) + 1)
    Accessible.name: qsTr("界面字体")
    onActivated: index => chosen(index > 0 ? families[index - 1] : "")
    onQueryChanged: cursor = query.trim() ? 0 : currentIndex

    popup: Popup {
        id: menu
        readonly property real rowsHeight: Math.min(picker.visibleRows, Math.max(1, picker.matches.length)) * Theme.menuItemHeight

        y: picker.height + 4
        width: picker.width
        height: search.height + body.spacing + rowsHeight + topPadding + bottomPadding
        topMargin: 6
        bottomMargin: 6
        padding: Theme.menuPadding
        focus: true
        font: picker.font
        palette: picker.palette

        onAboutToShow: {
            search.clear()
            picker.query = ""
            picker.cursor = picker.currentIndex
        }
        onOpened: {
            if (picker.frame)
                picker.frame.addPopup(menu.background)
            search.takeFocus()
            rows.positionViewAtIndex(picker.cursor, ListView.Center)
        }

        contentItem: Item {
            id: body
            readonly property alias list: rows
            readonly property real spacing: 4

            InputBox {
                id: search
                objectName: "uiFontSearch"
                width: parent.width
                height: 32
                dpr: picker.dpr
                placeholder: qsTr("查找字体")
                Accessible.name: placeholder
                maximumLength: 128
                onTextChanged: picker.query = text
                onAccepted: picker.acceptQuery()
                Keys.onUpPressed: picker.moveCursor(-1)
                Keys.onDownPressed: picker.moveCursor(1)
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_PageUp)
                        picker.moveCursor(-picker.visibleRows)
                    else if (event.key === Qt.Key_PageDown)
                        picker.moveCursor(picker.visibleRows)
                    else
                        return
                    event.accepted = true
                }
            }
            ListView {
                id: rows
                anchors.top: search.bottom
                anchors.topMargin: body.spacing
                anchors.bottom: parent.bottom
                width: parent.width
                clip: true
                model: picker.matches
                currentIndex: picker.cursor
                highlightMoveDuration: 0
                boundsBehavior: Flickable.StopAtBounds
                ScrollIndicator.vertical: ScrollIndicator { }
                delegate: ItemDelegate {
                    id: option
                    required property int index
                    required property string modelData
                    width: ListView.view.width
                    height: Theme.menuItemHeight
                    text: modelData || picker.defaultLabel
                    highlighted: picker.cursor === index
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    font.pixelSize: Theme.menuTextSize
                    font.weight: modelData === picker.selectedFamily ? Font.DemiBold : Font.Normal
                    onClicked: {
                        picker.cursor = index
                        picker.acceptQuery()
                    }
                    contentItem: Text {
                        text: option.text
                        font: option.font
                        color: Theme.text
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    background: Rectangle {
                        color: option.highlighted || option.hovered ? Theme.menuHover : "transparent"
                        radius: Theme.menuItemRadius
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: picker.matches.length === 0
                    text: qsTr("没有匹配的字体")
                    color: Theme.subtext
                    font.pixelSize: Theme.menuTextSize
                }
            }
        }

        background: Hook {
            part: Theme.menuFrame
            dpr: picker.dpr
        }
    }
}
