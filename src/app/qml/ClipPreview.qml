pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The current clipboard entry in full, beside the list: the text, the
// picture, the colour or the files, and below them where it came from, when,
// and how big it is. Scrolls when the text is long.
Item {
    id: preview

    property UiScale metrics: Theme.metrics
    property var info: ({}) // Clipboard.preview(row)
    readonly property string entryId: info.id ?? ""
    property bool completed: false
    Component.onCompleted: completed = true
    onEntryIdChanged: {
        picture.animationPaused = !SystemTheme.animations
        if (completed) {
            wheelScroll.cancel()
            flick.cancelFlick()
            flick.contentY = flick.originY - flick.topMargin
        }
    }
    property real parentTop // where the parent's top is in the window: to draw on whole device pixels
    property real resizeMargin: 0
    signal copyRequested(string text, bool remember) // one of a colour's notations, a picture's words

    // A picture's words: those selected with the mouse, or all of them, copied
    // as a copy of anyone's (into the history too).
    readonly property string selectedWords: kind === 4 ? words.selectedText : ""
    function copyWords() {
        copyRequested(selectedWords.length > 0 ? selectedWords : words.text, true)
        words.deselect()
    }

    readonly property int kind: info.kind ?? -1
    readonly property bool hasColors: (info.colors ?? []).length > 0
    // Paths show as files; a colour shows as its notations.
    readonly property bool hasText: info.text !== undefined && kind !== 3 && kind !== 2 && !hasColors
    readonly property int padding: 16

    Flickable {
        id: flick
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: meta.top
        anchors.leftMargin: preview.padding
        anchors.rightMargin: preview.padding - 6
        anchors.topMargin: 12
        anchors.bottomMargin: 8
        clip: true
        contentWidth: width
        contentHeight: content.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ListWheelScroll {
            id: wheelScroll
            view: flick
            animate: SystemTheme.animations
        }

        Column {
            id: content
            width: flick.width - 8
            spacing: 10
            // On whole device pixels wherever it has scrolled to, a fiftieth
            // of one lower: the lines of a long text sit at all sorts of
            // fractions, and those on a half pixel then always round the same
            // way (see SettingsWindow).
            transform: ScrollPixelAlignment {
                view: flick
                viewY: preview.parentTop + preview.y + flick.y
                itemY: content.y
                pixelBias: 0.02
            }

            ColorSwatch { // a colour value; a translucent one opaque on the left, over a checkerboard on the right
                visible: (preview.info.swatch ?? "").length > 0
                width: parent.width
                height: 72
                color: visible ? preview.info.swatch : "transparent"
                radius: Theme.largeCardRadius
                borderColor: Theme.chipBorder
                cellSize: 8
                checkerColor: Theme.checker
                checkerAltColor: Theme.checkerAlt
            }

            Column { // the colour's other notations: a click copies one
                visible: preview.hasColors
                width: parent.width
                spacing: 2

                Repeater {
                    model: preview.info.colors ?? []

                    delegate: Rectangle {
                        id: notation

                        required property var modelData

                        width: parent.width
                        height: 32
                        radius: Theme.largeControlRadius
                        color: notationArea.containsMouse ? Theme.hover : "transparent"

                        Text {
                            id: notationLabel
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 48 // "ARGB" and a gap
                            text: notation.modelData.label
                            color: Theme.faint
                            font.pixelSize: preview.metrics.uiCaption
                        }
                        Text {
                            anchors.left: notationLabel.right
                            anchors.right: copyGlyph.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: notation.modelData.value
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.text
                            font.pixelSize: preview.metrics.uiBody
                        }
                        Glyph {
                            id: copyGlyph
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            visible: notationArea.containsMouse
                            glyph: "" // Copy
                            size: 14
                            color: Theme.subtext
                        }
                        MouseArea {
                            id: notationArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: preview.copyRequested(notation.modelData.value, notation.modelData.remember)
                        }
                    }
                }
            }

            Text {
                visible: preview.hasText
                width: parent.width
                text: preview.info.text ?? ""
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                color: preview.kind === 1 ? Theme.accent : Theme.text
                font.pixelSize: preview.metrics.uiBody
                lineHeight: 1.15
            }

            Text { // the rest of a very long text: pasted, not shown
                visible: preview.kind <= 2 && (preview.info.more ?? 0) > 0
                text: qsTr("…还有 %Ln 字", "", preview.info.more ?? 0)
                color: Theme.faint
                font.pixelSize: preview.metrics.uiCaption
            }

            Item { // only the selected preview decodes animation frames
                id: picture

                readonly property real fit: Math.min(1, parent.width / Math.max(1, preview.info.width ?? 1),
                                                     (flick.height - 4) / Math.max(1, preview.info.height ?? 1))
                readonly property bool animated: preview.info.animated ?? false
                readonly property string imageSource: preview.info.image ?? ""
                readonly property Image loadedImage: viewer.status === Loader.Ready ? viewer.item as Image : null
                readonly property bool ready: loadedImage !== null && loadedImage.status === Image.Ready
                readonly property bool failed: loadedImage !== null && loadedImage.status === Image.Error
                readonly property bool inViewport: y + height > flick.contentY && y < flick.contentY + flick.height
                property bool animationPaused: !SystemTheme.animations

                visible: preview.kind === 4
                width: Math.max(1, Math.round((preview.info.width ?? 1) * fit))
                height: Math.max(1, Math.round((preview.info.height ?? 1) * fit))

                Rectangle {
                    anchors.fill: parent
                    color: Theme.track
                }

                Loader {
                    id: viewer
                    anchors.fill: parent
                    active: picture.visible && preview.visible && (preview.Window.window?.visible ?? false)
                            && picture.imageSource.length > 0
                    sourceComponent: picture.animated ? animation : still
                }

                Component {
                    id: still
                    Image {
                        source: picture.imageSource
                        sourceSize.width: Math.max(1, Math.round(picture.width * Screen.devicePixelRatio))
                        sourceSize.height: Math.max(1, Math.round(picture.height * Screen.devicePixelRatio))
                        fillMode: Image.PreserveAspectFit
                        autoTransform: true
                        asynchronous: true // kept decoded: hovering back to a picture shows it at once
                    }
                }

                Component {
                    id: animation
                    AnimatedImage {
                        objectName: "clipboardAnimation"
                        source: picture.imageSource
                        sourceSize.width: Math.max(1, Math.round(picture.width * Screen.devicePixelRatio))
                        sourceSize.height: Math.max(1, Math.round(picture.height * Screen.devicePixelRatio))
                        fillMode: Image.PreserveAspectFit
                        playing: true
                        paused: picture.animationPaused || !picture.inViewport
                        asynchronous: true
                        cache: false // keep current frames, not the whole animation
                    }
                }

                Text {
                    anchors.centerIn: parent
                    width: parent.width
                    visible: !picture.ready
                    text: picture.failed ? qsTr("图片加载失败，请重新复制")
                          : picture.imageSource.length === 0 ? qsTr("正在保存图片…") : qsTr("正在加载图片…")
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: picture.failed ? Theme.danger : Theme.faint
                    font.pixelSize: preview.metrics.uiCaption
                }

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.divider
                }
            }

            FlatButton {
                objectName: "clipboardAnimationToggle"
                visible: preview.kind === 4 && picture.animated
                enabled: picture.ready
                text: picture.animationPaused ? qsTr("播放动图") : qsTr("暂停动图")
                glyph: picture.animationPaused ? "\uE768" : "\uE769"
                height: 32
                flat: true
                keyboardActivation: false // the clipboard panel routes keys to its list
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.onPressAction: clicked()
                onClicked: picture.animationPaused = !picture.animationPaused
            }

            Column { // a picture's words, read by Windows: what the search finds it by
                visible: preview.kind === 4 && (preview.info.words ?? "").length > 0
                width: parent.width
                spacing: 4

                Item {
                    width: parent.width
                    height: copyWordsButton.height

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("图中的文字")
                        color: Theme.faint
                        font.pixelSize: preview.metrics.uiCaption
                    }
                    Rectangle { // copies the words selected, or all of them
                        id: copyWordsButton
                        anchors.right: parent.right
                        width: copyWordsRow.implicitWidth + 16
                        height: 26
                        radius: Theme.largeControlRadius
                        color: copyWordsArea.containsMouse ? Theme.hover : "transparent"

                        Row {
                            id: copyWordsRow
                            anchors.centerIn: parent
                            spacing: 6

                            Glyph {
                                anchors.verticalCenter: parent.verticalCenter
                                glyph: "" // Copy
                                size: 13
                                color: Theme.accent
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: preview.selectedWords.length > 0 ? qsTr("复制选中") : qsTr("复制文字")
                                color: Theme.accent
                                font.pixelSize: preview.metrics.uiCaption
                            }
                        }
                        MouseArea {
                            id: copyWordsArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: preview.copyWords()
                        }
                    }
                }
                // Selectable with the mouse, without the focus: the keys
                // stay in the search field, whose Ctrl+C copies what is
                // selected here (ClipboardPage), as the button does.
                TextEdit {
                    id: words
                    width: parent.width
                    text: preview.info.words ?? ""
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap
                    readOnly: true
                    selectByMouse: true
                    activeFocusOnPress: false
                    color: Theme.subtext
                    selectionColor: Theme.textSelection
                    selectedTextColor: Theme.text
                    font.pixelSize: preview.metrics.uiBody
                }
            }

            Image { // the first file's thumbnail, if Explorer has one; never enlarged
                id: thumbnail
                readonly property bool shown: (preview.kind === 3 || preview.kind === 2) && status === Image.Ready
                                              && implicitWidth > 0
                // Its pixels, in logical pixels: shown no larger than that.
                readonly property real naturalWidth: implicitWidth / Screen.devicePixelRatio
                readonly property real naturalHeight: implicitHeight / Screen.devicePixelRatio
                readonly property real fit: Math.min(1, parent.width / Math.max(1, naturalWidth),
                                                     260 / Math.max(1, naturalHeight))
                visible: shown
                width: shown ? Math.round(naturalWidth * fit) : 0
                height: shown ? Math.round(naturalHeight * fit) : 0
                source: preview.kind === 3 || preview.kind === 2 ? (preview.info.thumbnail ?? "") : ""
                // An image provider's size is in logical pixels (Qt asks for it × the
                // device pixel ratio); its pixels come back as the implicit size.
                sourceSize.width: flick.width
                sourceSize.height: flick.width
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.divider
                }
            }

            Column { // files
                visible: preview.kind === 3 || preview.kind === 2
                width: parent.width
                spacing: 2

                Repeater {
                    model: preview.kind === 3 || preview.kind === 2 ? (preview.info.files ?? []) : []

                    delegate: Item {
                        id: file

                        required property var modelData

                        width: parent.width
                        height: preview.metrics.fileRowHeight

                        Image {
                            id: fileIcon
                            x: 1 / 64
                            anchors.verticalCenter: parent.verticalCenter
                            width: 20
                            height: 20
                            source: file.modelData.icon
                            sourceSize.width: 20
                            sourceSize.height: 20
                            asynchronous: true
                            opacity: file.modelData.missing ? 0.45 : 1
                        }
                        Column {
                            anchors.left: fileIcon.right
                            anchors.leftMargin: 10
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter

                            Text {
                                width: parent.width
                                text: file.modelData.name
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: file.modelData.missing ? Theme.faint : Theme.text
                                font.pixelSize: preview.metrics.uiBody
                                font.strikeout: file.modelData.missing
                            }
                            Text {
                                width: parent.width
                                text: file.modelData.folder
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: Theme.faint
                                font.pixelSize: preview.metrics.uiCaption
                            }
                        }
                    }
                }

                Text {
                    visible: preview.kind === 3 && (preview.info.more ?? 0) > 0
                    text: qsTr("还有 %Ln 个", "", preview.info.more ?? 0)
                    color: Theme.faint
                    font.pixelSize: preview.metrics.uiCaption
                }
            }
        }
    }

    Text { // nothing to show
        anchors.centerIn: parent
        visible: preview.kind < 0
        text: qsTr("没有可预览的内容")
        color: Theme.faint
        font.pixelSize: preview.metrics.uiBody
    }

    ListScrollBar {
        objectName: "clipboardPreviewScrollBar"
        flickable: flick
        anchors.right: parent.right
        anchors.rightMargin: preview.resizeMargin
        anchors.top: flick.top
        anchors.bottom: flick.bottom
    }

    // Where it came from, when, how big.
    Column {
        id: meta
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: preview.padding
        anchors.rightMargin: preview.padding
        anchors.bottomMargin: 10
        spacing: 6
        visible: preview.kind >= 0

        Rectangle {
            width: parent.width
            height: Theme.edge(Screen.devicePixelRatio)
            color: Theme.divider
        }

        Row {
            width: parent.width
            spacing: 8
            visible: (preview.info.source ?? "").length > 0

            Image {
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                source: preview.info.sourceIcon ?? ""
                sourceSize.width: 16
                sourceSize.height: 16
                asynchronous: true
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 24
                text: preview.info.source ?? ""
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.subtext
                font.pixelSize: preview.metrics.uiCaption
            }
        }

        Text {
            width: parent.width
            text: [preview.info.time ?? "", preview.info.size ?? ""].filter(s => s.length > 0).join(" · ")
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.faint
            font.pixelSize: preview.metrics.uiCaption
        }

        Text {
            width: parent.width
            visible: text.length > 0
            text: [preview.info.formatted ? qsTr("带格式，Shift+Enter 粘贴为纯文本") : "",
                   (preview.info.group ?? "").length > 0 ? qsTr("在“%1”里，不会过期").arg(preview.info.group) : ""]
                  .filter(s => s.length > 0).join(" · ")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.faint
            font.pixelSize: preview.metrics.uiCaption
        }
    }
}
