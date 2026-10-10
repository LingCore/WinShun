import QtQuick
import WinShun

// A row's number while its modifier is held: Ctrl+1 (or Alt+1) acts on the
// row that shows "1". On the rows of the launcher, of the bar by file
// dialogs and of the clipboard.
Rectangle {
    id: cap

    property int number: 0

    width: 26
    height: 26
    radius: 6
    color: Theme.track
    border.width: 1
    border.color: Theme.chipBorder

    CenteredNumber {
        text: cap.number
        color: Theme.text
        font.pixelSize: Theme.fontBody
        font.weight: Font.DemiBold
    }
}
