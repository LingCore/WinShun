pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The gestures of one page, as the settings window's pages are made: a card
// with the pad to draw on, a card with the gestures drawn so far (a row
// each), and a card for the wheel. `mode`: 0 the gestures drawn anywhere, 1
// those for words selected, 2 one program's (`app`). It is the column its
// sections read (see SettingsSection.qml): one page, no tabs, no search.
Column {
    id: list

    required property GestureEditor editor
    required property ActionPicker picker
    property int mode: 0
    property string app
    property real dpr: Screen.devicePixelRatio
    property Flickable flick: null // the page's, to scroll the row just drawn into view
    readonly property bool wheel: mode !== 1

    readonly property int currentPage: -1
    readonly property int currentTab: 0

    width: parent ? parent.width : 0
    spacing: 20

    RulesFilter {
        id: filter
        sourceModel: list.editor.rules
        mode: list.mode
        app: list.app
    }

    function scrollTo(item: Item) {
        if (!list.flick)
            return
        // A row just added is not placed until the next frame: the layout now, to read where it is.
        rowsSection.layoutNow()
        list.forceLayout()
        const top = item.mapToItem(list.flick.contentItem, 0, 0).y
        const bottom = top + item.height
        if (top < list.flick.contentY + 8)
            list.flick.contentY = Math.max(0, top - 8)
        else if (bottom > list.flick.contentY + list.flick.height - 8)
            list.flick.contentY = Math.min(bottom + 8 - list.flick.height, Math.max(0, list.flick.contentHeight - list.flick.height))
    }

    Connections {
        target: list.editor

        // A gesture drawn: shown where it is (flashed), or asked for what it does.
        function onRuleFocused(sourceRow: int, created: bool) {
            const at = filter.proxyRow(sourceRow)
            const item = at >= 0 ? rows.itemAt(at) : null
            if (!item)
                return
            Qt.callLater(() => {
                list.scrollTo(item)
                if (created)
                    item.askAction()
                else
                    item.flash()
            })
        }
    }

    SettingsSection {
        width: parent.width
        title: qsTr("画一个新手势")
        glyph: "ph:mouse-right-click"

        GesturePad {
            editor: list.editor
            app: list.app
            selection: list.mode === 1
            dpr: list.dpr
        }
    }

    SettingsSection {
        id: rowsSection
        width: parent.width
        title: list.mode === 1 ? qsTr("选中文字时的手势") : list.mode === 2 ? qsTr("这个程序里的手势") : qsTr("画出的手势")
        glyph: "ph:hand-pointing"
        note: list.mode === 1 ? qsTr("先选中一段文字，再按住右键画：只在有选中文字时生效，没选中时还是上一页的手势。浏览器、VS Code、微信等用 Chromium 做的程序和 Firefox 里读不到选中的字（读了会让它们变慢），在那里还是上一页的手势")
            : list.mode === 2 ? qsTr("这些手势只在这个程序里生效，并且优先于其它页上的同一个手势；设成“什么都不做”可以在这个程序里关掉某个手势")
            : ""

        Repeater {
            id: rows
            model: filter

            delegate: RuleRow {
                editor: list.editor
                picker: list.picker
                app: list.app
                showSelection: list.mode !== 1
                dpr: list.dpr
            }
        }
        Item { // nothing yet
            width: parent.width
            height: 56
            visible: filter.count === 0

            Text {
                anchors.verticalCenter: parent.verticalCenter
                x: 18
                width: parent.width - 36
                text: qsTr("还没有手势：在上面的框里按住右键画一个")
                color: Theme.subtext
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
        }
    }

    SettingsSection {
        width: parent.width
        visible: list.wheel
        title: qsTr("按住右键滚滚轮")
        glyph: "ph:mouse-scroll"

        SlotRow {
            editor: list.editor
            picker: list.picker
            trigger: "wheel:up"
            title: qsTr("向上滚")
            caption: list.mode === 2 ? qsTr("没设置：沿用通用设置") : qsTr("滚轮向上滚一格做一次")
            app: list.app
            dpr: list.dpr
        }
        SlotRow {
            editor: list.editor
            picker: list.picker
            trigger: "wheel:down"
            title: qsTr("向下滚")
            caption: list.mode === 2 ? qsTr("没设置：沿用通用设置") : qsTr("滚轮向下滚一格做一次")
            app: list.app
            dpr: list.dpr
        }
    }
}
