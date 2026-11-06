pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import WinShun

ComboBox {
    id: picker
    required property list<string> families
    property WindowFrame frame: null
    property string selectedFamily
    signal chosen(string family)

    objectName: "uiFontPicker"
    implicitWidth: 260
    implicitHeight: 36
    model: [qsTr("默认（阿里巴巴普惠体）")].concat(families)
    currentIndex: Math.max(0, families.indexOf(selectedFamily) + 1)
    editable: true
    font.pixelSize: Theme.fontBody
    Accessible.name: qsTr("界面字体")
    palette.text: Theme.text
    palette.buttonText: Theme.text
    palette.base: Theme.menuBackground
    palette.button: Theme.menuBackground
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.accentText
    onActivated: index => chosen(index > 0 ? families[index - 1] : "")
    onAccepted: {
        const index = find(editText, Qt.MatchFixedString)
        if (index >= 0)
            chosen(index > 0 ? families[index - 1] : "")
        editText = currentText
    }
    onActiveFocusChanged: if (!activeFocus) editText = currentText
    background: Hook {
        part: Theme.keySurface
        hovered: picker.hovered
        pressed: picker.down
    }
    indicator: Glyph {
        x: picker.width - width - 12
        y: (picker.height - height) / 2
        glyph: "\uE70D"
        size: 12
        color: Theme.faint
    }
    rightPadding: 34
    delegate: ItemDelegate {
        id: option
        required property int index
        required property string modelData
        width: picker.width
        height: Theme.menuItemHeight
        text: modelData
        font.pixelSize: Theme.menuTextSize
        font.weight: picker.currentIndex === index ? Font.DemiBold : Font.Normal
        highlighted: picker.highlightedIndex === index
        hoverEnabled: true
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
            border.width: option.visualFocus ? 1 : 0
            border.color: Theme.accent
        }
    }
    popup.height: Math.min(280, picker.popup.contentItem.implicitHeight + picker.popup.topPadding + picker.popup.bottomPadding)
    popup.background: Hook { part: Theme.menuFrame }
    Connections {
        target: picker.popup
        function onOpened() {
            if (picker.frame)
                picker.frame.addPopup(picker.popup.contentItem)
        }
    }
}
