pragma Singleton
import QtQuick

// 拾穗计划: the author's other works, open source and paid. Every program of
// the author's carries this page; the name comes from Millet's The Gleaners.
// Ported from MacShun (Gleaning.swift). The content lives here and nothing is
// fetched: the program does not go online.
// To use the page in another program, change only `thisApp`, `works`,
// `authorUrl`, `feedbackUrl` and `thanks`.
QtObject {
    readonly property string title: qsTr("拾穗计划")

    readonly property string authorName: "LingCore" // the avatar is resources/gleaning/avatar.png
    readonly property string authorMotto: qsTr("为人民服务")
    // The author's homepage; when set, the author row gets a 作者主页 button.
    readonly property string authorUrl: ""
    // The official feedback form for this program.
    readonly property url feedbackUrl: "https://lcstp.com/projects/winshun/feedback"

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
