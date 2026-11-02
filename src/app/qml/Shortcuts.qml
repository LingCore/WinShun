pragma Singleton
import QtQuick

// Every keyboard shortcut of the search box and the clipboard window, in one
// table: the panel behind the footer's info button (ShortcutsButton) shows
// the useful ones, grouped; the settings' 全部快捷键 tab (SettingsWindow)
// all of them. An entry:
//   where    "launcher", "clipboard", or "global" (settings only: what opens them)
//   group    its heading
//   keys     a string, or a function of the context
//   text     what the keys do
//   brief    in the panel (else in the settings only)
//   when     a function of the context; false hides the entry in the panel
//   setting  {page, tab}: the settings can change the keys; where
//
// The context (`c`) is what the entries depend on: launcher.previewHotkey as
// `previewKey`; `numberModifier` ("Alt", "Ctrl", or "" for none) of the
// number keys; `folderTyping` (the folder window's Ctrl+Shift+D applies),
// `intoField` (the clipboard pastes into a field of Win顺's) and `holdKey`
// (the clipboard's hotkey, when it can be held to step). The settings show
// every entry (`all`), and give the keys that open Win顺 (`openKeys`,
// `clipboardHotkey`), and `captureKeys` (the screenshot plugin's).
QtObject {
    readonly property var table: [
        // what opens them (the settings only)
        { where: "global", group: qsTr("打开"), keys: c => c.openKeys, text: qsTr("打开搜索框"), setting: { page: 0, tab: 0 } },
        { where: "global", group: qsTr("打开"), keys: c => c.clipboardHotkey, text: qsTr("打开剪贴板"), setting: { page: 6, tab: 1 } },
        { where: "global", group: qsTr("打开"), keys: c => c.captureKeys, text: qsTr("截图"), setting: { page: 9, tab: 0 } },
        // the search box
        { where: "launcher", group: qsTr("移动"), brief: true, keys: "↑ ↓", text: qsTr("上下移动，PgUp PgDn 翻页") },
        { where: "launcher", group: qsTr("移动"), keys: "Ctrl+J / N · Ctrl+K / P", text: qsTr("向下 / 向上移动") },
        { where: "launcher", group: qsTr("移动"), keys: "Ctrl+U / D", text: qsTr("向上 / 向下翻页") },
        { where: "launcher", group: qsTr("移动"), brief: true, keys: "Shift+↑ ↓", text: qsTr("选中多行，也可 Ctrl+单击、Shift+单击") },
        { where: "launcher", group: qsTr("范围与筛选"), brief: true, keys: "Shift+Tab", text: qsTr("切换到上一个范围") },
        { where: "launcher", group: qsTr("范围与筛选"), brief: true, keys: "Ctrl+Tab", text: qsTr("在“文件”里切换类型，在“内容”里换个文件夹") },
        { where: "launcher", group: qsTr("范围与筛选"), brief: true, keys: "Ctrl+S", text: qsTr("切换排序；不输入时切换最近打开、最近修改") },
        { where: "launcher", group: qsTr("范围与筛选"), brief: true, keys: "Ctrl+R", text: qsTr("正则模式：每个词都按正则表达式匹配名字；再按一次关闭") },
        { where: "launcher", group: qsTr("范围与筛选"), brief: true, keys: "Ctrl+Shift+D", text: qsTr("只搜当前文件夹，或也搜其他位置"),
          when: c => c.folderTyping },
        { where: "launcher", group: qsTr("打开与操作"), brief: true, keys: "→ / Ctrl+O", text: qsTr("打开操作菜单；→ 需光标在末尾且未选中文字") },
        { where: "launcher", group: qsTr("打开与操作"), keys: "← / Esc", text: qsTr("关闭操作菜单，返回结果列表") },
        { where: "launcher", group: qsTr("打开与操作"), brief: true, keys: c => c.previewKey, text: qsTr("预览文件（鼠标中键点一行也行）；再按一次关闭"),
          when: c => c.previewKey.length > 0, setting: { page: 3, tab: 1 } },
        { where: "launcher", group: qsTr("打开与操作"), brief: true, keys: c => c.numberModifier.length > 0 ? c.numberModifier + "+1…9" : "", text: qsTr("打开第几行（图标上标着快捷键）"),
          when: c => c.numberModifier.length > 0, setting: { page: 3, tab: 2 } },
        { where: "launcher", group: qsTr("打开与操作"), keys: "Ctrl+Shift+Enter", text: qsTr("以管理员身份运行") },
        { where: "launcher", group: qsTr("打开与操作"), keys: "Ctrl+C", text: qsTr("复制文件") },
        { where: "launcher", group: qsTr("打开与操作"), keys: "Ctrl+Shift+C", text: qsTr("复制路径") },
        { where: "launcher", group: qsTr("打开与操作"), keys: "Shift+F10", text: qsTr("右键菜单") },
        { where: "launcher", group: qsTr("窗口"), brief: true, keys: "Ctrl+E", text: qsTr("展开为搜索面板，或收回搜索框") },
        { where: "launcher", group: qsTr("窗口"), brief: true, keys: "Ctrl+T", text: qsTr("钉住：点别处、打开文件后都不收起；再按一次取消") },
        { where: "launcher", group: qsTr("窗口"), keys: "Ctrl+,", text: qsTr("打开 Win顺 设置") },
        { where: "launcher", group: qsTr("窗口"), keys: "Esc", text: qsTr("关闭；选中了多行时先取消选择") },
        // the clipboard
        { where: "clipboard", group: qsTr("粘贴"), brief: true, keys: c => c.numberModifier.length > 0 ? c.numberModifier + "+1…9" : "", text: qsTr("粘贴第几条"),
          when: c => c.numberModifier.length > 0, setting: { page: 6, tab: 1 } },
        { where: "clipboard", group: qsTr("粘贴"), brief: true, keys: "Shift+Enter", text: qsTr("粘贴为纯文本"), when: c => !c.intoField },
        { where: "clipboard", group: qsTr("粘贴"), keys: c => c.numberModifier.length > 0 ? c.numberModifier + "+Shift+1…9" : "", text: qsTr("第几条粘贴为纯文本"),
          when: c => c.numberModifier.length > 0 },
        // Only where it works: a hotkey with something to hold (a lone F9 has not), over another program.
        { where: "clipboard", group: qsTr("粘贴"), brief: true, keys: c => c.holdKey, text: qsTr("按住连按往下选，松开粘贴"),
          when: c => c.holdKey.includes("+") && !c.intoField, setting: { page: 6, tab: 1 } },
        { where: "clipboard", group: qsTr("移动与选择"), brief: true, keys: "↑ ↓", text: qsTr("上下移动，PgUp PgDn 翻页") },
        { where: "clipboard", group: qsTr("移动与选择"), brief: true, keys: "Shift+Tab", text: qsTr("上一个分类") },
        { where: "clipboard", group: qsTr("移动与选择"), brief: true, keys: "Shift+↑ ↓", text: qsTr("选中多条，也可 Ctrl+单击、Shift+单击；按选的顺序一起粘贴") },
        { where: "clipboard", group: qsTr("移动与选择"), brief: true, keys: "→  Alt+P", text: qsTr("打开或关闭预览") },
        { where: "clipboard", group: qsTr("管理"), brief: true, keys: "Ctrl+P", text: qsTr("固定或取消固定") },
        { where: "clipboard", group: qsTr("管理"), brief: true, keys: "Delete", text: qsTr("删除，Ctrl+Z 撤销") },
        { where: "clipboard", group: qsTr("管理"), keys: "Ctrl+C", text: qsTr("复制；预览里选中了图中的字时，复制那一段") },
        { where: "clipboard", group: qsTr("管理"), keys: "Shift+F10", text: qsTr("右键菜单") },
        { where: "clipboard", group: qsTr("管理"), keys: "Esc", text: qsTr("关闭；选中了多条时先取消选择") }
    ]

    // The entries of a window that apply in context `c`: the brief ones for
    // the panel, else all of them. [{group, keys, text, setting}, …]
    function entries(where, c, brief) {
        const list = []
        for (const entry of table) {
            if (entry.where !== where || (brief && !entry.brief) || (!c.all && entry.when && !entry.when(c)))
                continue
            list.push({ group: entry.group, keys: typeof entry.keys === "function" ? entry.keys(c) : entry.keys,
                        text: entry.text, setting: entry.setting ?? null })
        }
        return list
    }
}
