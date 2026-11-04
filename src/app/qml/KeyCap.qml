import QtQuick
import WinShun

// A row's shortcut while its modifier is held: Ctrl+1 (or Alt+1) acts on the
// row that shows "Ctrl+1". On the rows of the bar by file dialogs (the
// launcher's and the clipboard's rows have theirs on the icon, IconNumber).
// `taken`: another program holds the chord as a global hotkey, so pressing it
// goes there: faint and struck through.
Item {
    id: cap

    property string keys
    property bool taken: false
    property real dpr: Screen.devicePixelRatio // the window's, passed by a host that has it (GitHub #111)

    width: Math.max(height, Math.ceil(label.implicitWidth) + 12)
    height: Theme.keycapHeight + 4

    Hook { // the style's key cap (Theme.keyHint)
        id: capPart
        anchors.fill: parent
        part: Theme.keyHint
        dpr: cap.dpr
    }

    CenteredNumber {
        id: label
        text: cap.keys
        color: cap.taken ? Theme.faint : capPart.out("foreground", Theme.text)
        font.pixelSize: Theme.uiBody
        font.weight: Theme.fontStrong
        font.strikeout: cap.taken
    }
}
