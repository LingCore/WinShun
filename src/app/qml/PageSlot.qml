import QtQuick
import WinShun

// Where one page of the settings window goes (SettingsWindow's page column):
// the page is made the first time it is needed (load), or in the background
// a little at a time (prefetch), not with the window. Not a Loader: a Loader
// is a focus scope, which would keep the focus the window takes away
// (SettingsWindow.clearFocus) and scroll the page back to its top.
Item {
    id: slot

    required property int index
    required property Component content
    property SettingsPage item: null
    property var incubator: null // while being made in the background
    property real loadMs: -1 // how long making it took, when made at once

    // The page, made now if it is not yet (finished now if it is being made).
    function load(): SettingsPage {
        if (slot.item)
            return slot.item
        const started = Date.now()
        if (slot.incubator) {
            slot.incubator.forceCompletion()
            if (!slot.item && slot.incubator && slot.incubator.status === Component.Ready)
                slot.item = slot.incubator.object as SettingsPage
            slot.incubator = null
        }
        if (!slot.item) {
            const made = slot.content.createObject(slot) as SettingsPage
            if (!made)
                console.warn("settings page", slot.index, "could not be made:", slot.content.errorString())
            slot.item = made
        }
        slot.loadMs = Date.now() - started
        return slot.item
    }

    // Starts making the page in the background; nothing when it is made or being made.
    // `done`, if given: called once the background making has ended (made or failed).
    function prefetch(done: var): void {
        if (slot.item || slot.incubator)
            return
        const incubator = slot.content.incubateObject(slot, {}, Qt.Asynchronous)
        if (!incubator) {
            console.warn("settings page", slot.index, "could not be made:", slot.content.errorString())
            return
        }
        if (incubator.status === Component.Ready) {
            slot.item = incubator.object as SettingsPage
            if (done)
                done()
            return
        }
        slot.incubator = incubator
        incubator.onStatusChanged = (status) => {
            if (slot.incubator !== incubator)
                return
            if (status === Component.Ready)
                slot.item = incubator.object as SettingsPage
            else if (status === Component.Error)
                console.warn("settings page", slot.index, "could not be made:", slot.content.errorString())
            if (status !== Component.Loading) {
                slot.incubator = null
                if (done)
                    done()
            }
        }
    }

    function layoutNow(): void {
        if (slot.item)
            slot.item.layoutNow()
    }

    width: parent ? parent.width : 0
    implicitHeight: slot.item ? slot.item.implicitHeight : 0
    visible: slot.item !== null
}
