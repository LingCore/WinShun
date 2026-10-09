pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The current clipboard entry in full, beside the list: the text, the
// picture, the colour or the files, and below them where it came from, when,
// and how big it is. Scrolls when the text is long.
Item {
    id: preview

    property var info: ({}) // Clipboard.preview(row)
    signal copyRequested(string text, bool remember) // one of a colour's notations was clicked

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

        Column {
            id: content
            width: flick.width - 8
            spacing: 10

            ColorSwatch { // a colour value; a translucent one opaque on the left, over a checkerboard on the right
                visible: (preview.info.swatch ?? "").length > 0
                width: parent.width
                height: 72
                color: visible ? preview.info.swatch : "transparent"
                radius: 8
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
                        radius: 6
                        color: notationArea.containsMouse ? Theme.hover : "transparent"

                        Text {
                            id: notationLabel
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 48 // "ARGB" and a gap
                            text: notation.modelData.label
                            color: Theme.faint
                            font.pixelSize: Theme.fontCaption
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
                            font.pixelSize: Theme.fontBody
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
                font.pixelSize: Theme.fontBody
                lineHeight: 1.15
            }

            Text { // the rest of a very long text: pasted, not shown
                visible: preview.kind <= 2 && (preview.info.more ?? 0) > 0
                text: qsTr("…还有 %Ln 字", "", preview.info.more ?? 0)
                color: Theme.faint
                font.pixelSize: Theme.fontCaption
            }

            Image { // a picture, as large as fits
                id: picture

                readonly property real fit: Math.min(1, parent.width / Math.max(1, preview.info.width ?? 1),
                                                     (flick.height - 4) / Math.max(1, preview.info.height ?? 1))

                visible: preview.kind === 4
                width: Math.max(1, Math.round((preview.info.width ?? 1) * fit))
                height: Math.max(1, Math.round((preview.info.height ?? 1) * fit))
                source: visible ? preview.info.image : ""
                // A PNG file: sourceSize is in the image's own pixels.
                sourceSize.width: Math.round(width * Screen.devicePixelRatio)
                sourceSize.height: Math.round(height * Screen.devicePixelRatio)
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.divider
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
                        height: 40

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
                                font.pixelSize: Theme.fontBody
                                font.strikeout: file.modelData.missing
                            }
                            Text {
                                width: parent.width
                                text: file.modelData.folder
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: Theme.faint
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                    }
                }

                Text {
                    visible: preview.kind === 3 && (preview.info.more ?? 0) > 0
                    text: qsTr("还有 %Ln 个", "", preview.info.more ?? 0)
                    color: Theme.faint
                    font.pixelSize: Theme.fontCaption
                }
            }
        }
    }

    ListScrollBar {
        flickable: flick
        anchors.right: parent.right
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
            height: 1
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
                font.pixelSize: Theme.fontCaption
            }
        }

        Text {
            width: parent.width
            text: [preview.info.time ?? "", preview.info.size ?? ""].filter(s => s.length > 0).join(" · ")
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.faint
            font.pixelSize: Theme.fontCaption
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
            font.pixelSize: Theme.fontCaption
        }
    }
}
