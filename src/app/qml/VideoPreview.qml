pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// A video in the preview (FilePreview.Video): played by Windows (VideoView),
// the shell's thumbnail until its first frame is up. Under it a bar: play or
// pause, where it is (a click or a drag goes there), its length, the sound
// (the speaker mutes; a click, a drag or the wheel on the track sets it). A
// click on the picture plays or pauses. One Windows cannot play (no decoder
// for it here, or a damaged file): says so, without the bar.
Item {
    id: root

    required property FilePreview preview
    required property real dpr
    readonly property bool shown: preview.kind === FilePreview.Video

    function onPixel(v) { return Math.round(v * dpr) / dpr }
    function clock(seconds) {
        const s = Math.max(0, Math.floor(seconds))
        const two = (n) => (n < 10 ? "0" : "") + n
        return s >= 3600 ? Math.floor(s / 3600) + ":" + two(Math.floor(s / 60) % 60) + ":" + two(s % 60)
                         : Math.floor(s / 60) + ":" + two(s % 60)
    }

    visible: shown

    Item {
        id: screen
        anchors.fill: parent
        anchors.bottomMargin: bar.visible ? bar.height : 0

        Image {
            id: poster
            anchors.fill: parent
            anchors.margins: 8
            visible: !video.hasFrame && video.status !== VideoView.Failed && status === Image.Ready
            source: root.shown ? root.preview.imageUrl : ""
            fillMode: Image.PreserveAspectFit
            cache: false
            smooth: true
            mipmap: true
        }
        VideoView {
            id: video
            objectName: "previewVideo"
            anchors.fill: parent
            anchors.margins: 8
            source: root.shown ? root.preview.path : ""
        }
        Text {
            anchors.centerIn: parent
            width: parent.width - 32
            visible: video.status === VideoView.Failed
            text: qsTr("播放不了这个视频：这台电脑没有它的解码器，或者文件已损坏")
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: Theme.faint
            font.pixelSize: Theme.uiBody
        }
        TapHandler {
            enabled: video.status !== VideoView.Failed
            onTapped: video.togglePlay()
        }
    }

    Item {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 4
        anchors.rightMargin: 12
        height: 40
        visible: video.status !== VideoView.Failed

        FlatButton {
            id: playButton
            objectName: "previewVideoPlay"
            dpr: root.dpr
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 32
            height: 32
            flat: true
            keyboardActivation: false
            glyph: video.playing ? "ph-fill:pause" : video.ended ? "ph:arrow-counter-clockwise" : "ph-fill:play"
            Accessible.name: video.playing ? qsTr("暂停") : qsTr("播放")
            onClicked: video.togglePlay()
        }
        Text {
            id: now
            anchors.left: playButton.right
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: root.clock(seek.dragging ? seek.ratio * video.duration : video.position)
            color: Theme.text
            font.pixelSize: Theme.uiCaption
            font.features: { "tnum": 1 }
        }
        Track {
            id: seek
            objectName: "previewVideoSeek"
            dpr: root.dpr
            anchors.left: now.right
            anchors.leftMargin: 10
            anchors.right: length.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            value: video.duration > 0 ? video.position / video.duration : 0
            Accessible.name: qsTr("播放进度")
            onMoved: (ratio) => video.seek(ratio * video.duration)
        }
        Text {
            id: length
            anchors.right: speaker.left
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: video.duration > 0 ? root.clock(video.duration) : "--:--"
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
            font.features: { "tnum": 1 }
        }
        FlatButton {
            id: speaker
            objectName: "previewVideoMute"
            dpr: root.dpr
            anchors.right: loudness.left
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            width: 32
            height: 32
            flat: true
            keyboardActivation: false
            glyph: video.muted || video.volume === 0 ? "ph:speaker-x" : video.volume < 0.5 ? "ph:speaker-low" : "ph:speaker-high"
            Accessible.name: video.muted ? qsTr("取消静音") : qsTr("静音")
            onClicked: video.muted = !video.muted
        }
        Track {
            id: loudness
            objectName: "previewVideoVolume"
            dpr: root.dpr
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 72
            value: video.muted ? 0 : video.volume
            Accessible.name: qsTr("音量")
            onMoved: (ratio) => {
                video.volume = ratio
                video.muted = ratio === 0
            }
            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: (event) => {
                    const v = Math.max(0, Math.min(1, (video.muted ? 0 : video.volume) + event.angleDelta.y / 120 * 0.05))
                    video.volume = v
                    video.muted = v === 0
                }
            }
        }
    }

    // A thin track filled with the accent up to a round thumb (shown under
    // the pointer and while dragged), as the settings' sliders.
    component Track: Item {
        id: track
        required property real dpr
        property real value: 0 // 0 to 1
        readonly property bool dragging: area.pressed
        readonly property real ratio: dragging ? area.ratio : value
        signal moved(real ratio)

        implicitHeight: 24
        Accessible.role: Accessible.Slider

        function onPixel(v) { return Math.round(v * dpr) / dpr }

        Rectangle {
            id: rail
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: track.onPixel(4)
            radius: height / 2
            color: Theme.track
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: track.onPixel(Math.max(rail.height, track.width * Math.max(0, Math.min(1, track.ratio))))
            height: rail.height
            radius: rail.radius
            color: Theme.accentFill
        }
        Rectangle {
            visible: area.containsMouse || area.pressed
            x: track.onPixel(track.width * Math.max(0, Math.min(1, track.ratio)) - width / 2)
            anchors.verticalCenter: parent.verticalCenter
            width: 12
            height: 12
            radius: 6
            color: Theme.accentFill
            border.width: 2
            border.color: Theme.card
        }
        MouseArea {
            id: area
            property real ratio: 0
            function follow(x) {
                ratio = Math.max(0, Math.min(1, x / track.width))
                track.moved(ratio)
            }
            anchors.fill: parent
            anchors.topMargin: -6
            anchors.bottomMargin: -6
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onPressed: (mouse) => follow(mouse.x)
            onPositionChanged: (mouse) => { if (pressed) follow(mouse.x) }
        }
    }
}
