pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The 拾穗计划 page's content; GleaningBackdrop lies under the whole window.
// One screen: the works at the top, the feedback note and the footer down in
// the wheat field, the gap between them growing with the window. It scrolls
// only when the works no longer fit.
Flickable {
    id: page

    required property SettingsEditor editor

    readonly property var works: Gleaning.works.slice().sort((a, b) => Number(b.openSource) - Number(a.openSource))
    readonly property int openSourceCount: Gleaning.works.filter(w => w.openSource).length
    readonly property real iconSize: 36 // the works' icon tiles
    readonly property real iconPadding: 8 // between a tile's edge and its logo

    contentWidth: width
    contentHeight: Math.max(height, top.implicitHeight + bottom.implicitHeight + 40 + 20)
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    // Each time the page opens, the avatar pops up to say hello.
    onVisibleChanged: if (visible) AuthorClock.play()

    component Tag: Rectangle {
        property alias text: tagLabel.text
        property real fontSize: 12

        implicitWidth: tagLabel.implicitWidth + 16
        implicitHeight: tagLabel.implicitHeight + 6
        baselineOffset: tagLabel.y + tagLabel.baselineOffset
        radius: height / 2
        color: WarmPalette.chip

        Text {
            id: tagLabel
            anchors.centerIn: parent
            color: WarmPalette.ink
            font.pixelSize: parent.fontSize
            font.weight: Font.DemiBold
        }
    }

    component IconTile: Rectangle {
        width: page.iconSize
        height: page.iconSize
        radius: page.iconSize * 0.27
        color: WarmPalette.iconFill
        border.width: 1
        border.color: WarmPalette.iconStroke
    }

    // One work, without a frame, straight on the field. With an address its
    // name is a link: underlined under the mouse, opened in the browser.
    component WorkItem: Row {
        id: work

        required property var modelData
        readonly property bool current: modelData.id === Gleaning.thisApp
        readonly property bool hasLink: modelData.url.length > 0

        spacing: 14

        IconTile {
            Image {
                anchors.fill: parent
                anchors.margins: page.iconPadding
                source: work.modelData.icon
                // Image pixels: for a PNG, sourceSize is not scaled by the
                // device pixel ratio (only image providers, SVG and PDF are).
                sourceSize.width: Math.ceil(width * Screen.devicePixelRatio)
                sourceSize.height: Math.ceil(height * Screen.devicePixelRatio)
                fillMode: Image.PreserveAspectFit
                smooth: true
            }
        }

        Column {
            width: work.width - page.iconSize - work.spacing
            spacing: 4

            Item {
                width: title.implicitWidth + (work.hasLink ? arrow.implicitWidth + 6 : 0)
                height: title.implicitHeight

                Text {
                    id: title
                    text: work.modelData.name
                    color: WarmPalette.ink
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    font.underline: link.containsMouse
                }
                Glyph {
                    id: arrow
                    visible: work.hasLink
                    anchors.left: title.right
                    anchors.leftMargin: link.containsMouse ? 7 : 6
                    anchors.verticalCenter: title.verticalCenter
                    anchors.verticalCenterOffset: link.containsMouse ? -1 : 0
                    glyph: "" // OpenInNewWindow
                    size: 12
                    color: link.containsMouse ? WarmPalette.ink : WarmPalette.inkSoft
                }
                MouseArea {
                    id: link
                    anchors.fill: parent
                    enabled: work.hasLink
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.editor.openUrl(work.modelData.url)
                }
            }
            Text {
                width: parent.width
                text: work.modelData.summary
                color: WarmPalette.inkSoft
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
            Row {
                topPadding: 3
                spacing: 6

                Tag { text: work.modelData.openSource ? qsTr("开源") : qsTr("付费") }
                Tag { visible: work.current; text: qsTr("正在使用") }
            }
        }
    }

    // Until there is another work: a dashed tile among the works.
    component ComingSoonItem: Row {
        spacing: 14

        IconTile {
            color: "transparent"
            border.width: 0

            Canvas { // dashed border
                anchors.fill: parent
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.strokeStyle = WarmPalette.iconStroke
                    ctx.lineWidth = 1.2
                    ctx.setLineDash([3, 3])
                    ctx.roundedRect(0.6, 0.6, width - 1.2, height - 1.2, page.iconSize * 0.27, page.iconSize * 0.27)
                    ctx.stroke()
                }
            }
            Glyph {
                anchors.centerIn: parent
                glyph: "" // FavoriteStar
                size: 16
                color: WarmPalette.ink
            }
        }
        Column {
            spacing: 4

            Text {
                text: qsTr("更多作品正在路上")
                color: WarmPalette.ink
                opacity: 0.85
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            Text {
                text: qsTr("新作品做好就放在这里")
                color: WarmPalette.inkSoft
                font.pixelSize: Theme.fontCaption
            }
        }
    }

    Column {
        id: top
        x: 32
        y: 22
        width: page.width - 64

        // Hero: the title and how many works there are.
        Text {
            visible: Gleaning.title.toUpperCase() !== text // in English the title says it already
            text: "GLEANING"
            color: WarmPalette.inkSoft
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: 4.5
        }
        Item {
            width: parent.width
            height: heroTitle.implicitHeight
            Text {
                id: heroTitle
                text: Gleaning.title
                color: WarmPalette.ink
                font.pixelSize: 44
                font.weight: Font.Bold
            }
            Row {
                anchors.left: heroTitle.right
                anchors.leftMargin: 16
                anchors.baseline: heroTitle.baseline
                baselineOffset: worksTag.baselineOffset
                spacing: 6

                Tag { id: worksTag; fontSize: 13; text: qsTr("%n 个作品", "", Gleaning.works.length) }
                Tag { fontSize: 13; visible: page.openSourceCount > 0; text: qsTr("%n 个开源", "", page.openSourceCount) }
                Tag {
                    fontSize: 13
                    visible: Gleaning.works.length > page.openSourceCount
                    text: qsTr("%n 个付费", "", Gleaning.works.length - page.openSourceCount)
                }
            }
        }
        Text {
            topPadding: 2
            text: qsTr("每一个作品，都是认真生活留下的痕迹")
            color: WarmPalette.inkSoft
            font.pixelSize: Theme.fontBody
        }

        // The author: avatar, name and motto.
        Item {
            width: parent.width
            height: 22 + Math.max(avatar.size, authorInfo.implicitHeight)
            AuthorAvatar {
                id: avatar
                y: 22 + Math.max(0, (authorInfo.implicitHeight - size) / 2) // the motto is large: keep both centred
                size: 70
            }
            Column {
                id: authorInfo
                anchors.left: avatar.right
                anchors.leftMargin: 16
                anchors.verticalCenter: avatar.verticalCenter
                spacing: 3

                Row {
                    spacing: 8
                    Text {
                        id: authorName
                        text: Gleaning.authorName
                        color: WarmPalette.ink
                        font.pixelSize: 20
                        font.weight: Font.Bold
                    }
                    Tag {
                        anchors.verticalCenter: authorName.verticalCenter
                        text: qsTr("作者")
                    }
                }
                Text {
                    text: Gleaning.authorMotto
                    color: WarmPalette.inkSoft
                    font.pointSize: 24
                    font.weight: Font.Bold
                }
            }
            Rectangle { // homepage
                visible: Gleaning.authorUrl.length > 0
                anchors.right: parent.right
                anchors.verticalCenter: avatar.verticalCenter
                width: homepage.implicitWidth + 28
                height: homepage.implicitHeight + 14
                radius: height / 2
                color: WarmPalette.chip

                Row {
                    id: homepage
                    anchors.centerIn: parent
                    spacing: 6
                    Text {
                        text: qsTr("作者主页")
                        color: WarmPalette.ink
                        font.pixelSize: Theme.fontCaption
                        font.weight: Font.DemiBold
                    }
                    Glyph {
                        anchors.verticalCenter: parent.verticalCenter
                        glyph: "" // OpenInNewWindow
                        size: 12
                        color: WarmPalette.ink
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.editor.openUrl(Gleaning.authorUrl)
                }
            }
        }

        // The works, as many columns as fit.
        Grid {
            id: grid

            readonly property int minimumColumnWidth: 230
            readonly property real cellWidth: (width - columnSpacing * (columns - 1)) / columns

            topPadding: 26
            width: parent.width
            columns: Math.max(1, Math.floor((width + columnSpacing) / (minimumColumnWidth + columnSpacing)))
            columnSpacing: 28
            rowSpacing: 20

            Repeater {
                model: page.works
                delegate: WorkItem { width: grid.cellWidth }
            }
            ComingSoonItem {
                visible: Gleaning.works.every(w => w.id === Gleaning.thisApp)
                width: grid.cellWidth
            }
        }
    }

    Column {
        id: bottom
        x: 32
        y: page.contentHeight - implicitHeight - 18
        width: page.width - 64
        spacing: 18

        // Write to me: a small tilted envelope, a heading and a capsule button.
        Column {
            width: parent.width
            spacing: 6

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 9

                IconBadge { // a small tilted envelope
                    anchors.verticalCenter: parent.verticalCenter
                    width: 42 // room for the tilted square
                    height: 42
                    badgeSize: 36
                    angle: -9
                    glyph: "" // Mail
                    iconFont: Theme.iconFont
                    topColor: WarmPalette.accentLight
                    bottomColor: WarmPalette.accent
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("您的建议非常重要")
                    color: WarmPalette.ink
                    font.pixelSize: 22
                    font.weight: Font.Bold
                }
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("有任何建议，或想对开发者说的话，欢迎写信给我")
                color: WarmPalette.ink
                opacity: 0.85
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                topPadding: 8
                spacing: 14

                Rectangle { // new mail in the default mail program
                    id: writeButton

                    readonly property bool usable: Gleaning.feedbackEmail.length > 0

                    width: writeLabel.implicitWidth + 36
                    height: writeLabel.implicitHeight + 18
                    radius: height / 2
                    gradient: Gradient {
                        GradientStop { position: 0; color: writeArea.containsMouse ? Qt.lighter(WarmPalette.accentLight, 1.06) : WarmPalette.accentLight }
                        GradientStop { position: 1; color: writeArea.containsMouse ? Qt.lighter(WarmPalette.accent, 1.06) : WarmPalette.accent }
                    }

                    Row {
                        id: writeLabel
                        anchors.centerIn: parent
                        spacing: 7
                        Glyph {
                            anchors.verticalCenter: parent.verticalCenter
                            glyph: "" // Send
                            size: 13
                            color: "white"
                        }
                        Text {
                            text: writeButton.usable ? Gleaning.feedbackEmail : qsTr("还没有")
                            color: "white"
                            font.pixelSize: Theme.fontCaption
                            font.weight: Font.DemiBold
                        }
                    }
                    MouseArea {
                        id: writeArea
                        anchors.fill: parent
                        // Not `enabled: false` with a faded look: the wheat
                        // would show through. Without an address it does nothing.
                        enabled: writeButton.usable
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        // The subject names the program and version, so the author can sort the mail.
                        onClicked: page.editor.openUrl("mailto:" + Gleaning.feedbackEmail + "?subject="
                            + encodeURIComponent(qsTr("%1 %2 的建议").arg(qsTr("Win顺")).arg(Qt.application.version)))
                    }
                }
                Text {
                    anchors.verticalCenter: writeButton.verticalCenter
                    text: copyArea.copied ? qsTr("已复制") : qsTr("复制")
                    color: WarmPalette.ink
                    opacity: copyArea.containsMouse ? 1 : 0.85
                    font.pixelSize: 13
                    font.weight: Font.DemiBold

                    MouseArea {
                        id: copyArea

                        property bool copied: false

                        anchors.fill: parent
                        anchors.margins: -6
                        enabled: writeButton.usable
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            page.editor.copyText(Gleaning.feedbackEmail)
                            copied = true
                            copiedTimer.restart()
                        }
                        Timer {
                            id: copiedTimer
                            interval: 1500
                            onTriggered: copyArea.copied = false
                        }
                    }
                }
            }
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("“拾穗计划”的名字取自米勒的名画《拾穗者》")
            color: WarmPalette.ink
            opacity: 0.8
            font.pixelSize: 13
        }
    }
}
