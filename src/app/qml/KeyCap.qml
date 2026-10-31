import QtQuick
import WinShun

// A row's shortcut while its modifier is held: Ctrl+1 (or Alt+1) acts on the
// row that shows "Ctrl+1". On the rows of the bar by file dialogs (the
// launcher's and the clipboard's rows have theirs on the icon, IconNumber).
// `taken`: another program holds the chord as a global hotkey, so pressing it
// goes there: faint and struck through.
Rectangle {
    id: cap

    property string keys
    property bool taken: false

    width: Math.max(height, Math.ceil(label.implicitWidth) + 12)
    height: Theme.keycapHeight + 4
    radius: Theme.largeControlRadius
    color: Theme.track
    border.width: 1
    border.color: Theme.chipBorder

    CenteredNumber {
        id: label
        text: cap.keys
        color: cap.taken ? Theme.faint : Theme.text
        font.pixelSize: Theme.uiBody
        font.weight: Theme.fontStrong
        font.strikeout: cap.taken
    }
}
