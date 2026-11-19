pragma Singleton
import QtQuick

// 拾穗计划: the author's other works, open source and paid. Every program of
// the author's carries this page; the name comes from Millet's The Gleaners.
// Ported from MacShun (Gleaning.swift). The content lives here; only each
// work's number of users comes from lcstp.com (Updater.userCounts, keyed by
// `id`, the work's name on that site), shown when the page opens.
// To use the page in another program, change only `thisApp`, `works`,
// `authorUrl`, `feedbackUrl`, `donateUrl`, `qqGroup` and `thanks`.
QtObject {
    readonly property string title: qsTr("拾穗计划")

    readonly property string authorName: "LingCore" // the avatar is resources/gleaning/avatar.png
    readonly property string authorMotto: qsTr("为人民服务")
    // The author's homepage; when set, the author row gets a 作者主页 button.
    readonly property string authorUrl: ""
    // The official feedback form for this program.
    readonly property url feedbackUrl: "https://lcstp.com/projects/winshun/feedback"
    // Where the 捐赠 button in the settings' title bar leads: the author's
    // 爱发电 page, as lcstp.com's own 向我捐赠 button; "" shows no button.
    readonly property string donateUrl: "https://afdian.com/a/lingcore"
    // The users' QQ group, for talking together (feedback still goes to the
    // website); null shows no 加入 QQ 群 button.
    //   url: the group's join link, also drawn as the QR code (tools/make_qq_qr.py)
    readonly property var qqGroup: ({
        name: qsTr("麦田"),
        number: "576446871",
        url: "https://qm.qq.com/q/btbyWvs5ag",
        qr: "qrc:/qt/qml/WinShun/gleaning/qq-group.svg"
    })

    // Sites that list this program, thanked at the foot of the page; [] shows nothing.
    //   logo:  the site's own logo, drawn on a light plate
    //   words: its slogan, one short line
    //   url:   the site's page for this program
    readonly property var thanks: [
        {
            name: qsTr("果核剥壳"),
            logo: "qrc:/qt/qml/WinShun/gleaning/ghxi.png",
            words: qsTr("互联网的净土"),
            url: "https://www.ghxi.com/winshun.html"
        }
    ]

    // The work this program is: it gets the 正在使用 tag.
    readonly property string thisApp: "winshun"

    // New works go here; open source ones are listed before paid ones.
    //   summary: one short line (about ten characters), the rest is cut off
    //   url:     homepage, repository or shop page; "" when there is none yet
    //   icon:    the work's own coloured logo without a plate, drawn on a tile
    readonly property var works: [
        {
            id: "winshun",
            name: qsTr("Win顺"),
            summary: qsTr("让 Windows 用起来更顺手"),
            openSource: true,
            url: "https://github.com/LingCore/WinShun",
            icon: "qrc:/qt/qml/WinShun/gleaning/winshun.png"
        },
        {
            id: "macshun",
            name: qsTr("Mac顺"),
            summary: qsTr("把 Windows 的顺手带到 Mac"),
            openSource: true,
            url: "https://github.com/LingCore/MacShun",
            icon: "qrc:/qt/qml/WinShun/gleaning/macshun.png"
        }
    ]
}
