pragma ComponentBehavior: Bound
import QtQuick
import WinShun

ChoiceBox {
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
    Accessible.name: qsTr("界面字体")
    onActivated: index => chosen(index > 0 ? families[index - 1] : "")
    onAccepted: {
        const index = find(editText, Qt.MatchFixedString)
        if (index >= 0)
            chosen(index > 0 ? families[index - 1] : "")
        editText = currentText
    }
    onActiveFocusChanged: if (!activeFocus) editText = currentText
    Connections {
        target: picker.popup
        function onOpened() {
            if (picker.frame)
                picker.frame.addPopup(picker.popup.background)
        }
    }
}
