# 开发工具 · Developer tools

插件库里的 C++ 常驻插件（`WinShunDevTools.exe`，源码在 `src/devtools-plugin`），不随 Win顺 安装。设置 → 插件 → 插件库 里装上**开发工具**并打开即可。第一次用到时才启动，平时不占后台。

先有一个 **JSON 工具**：

**在搜索框里**
- 输入 `json` 加空格，看剪贴板里的 JSON：第一行「查看 JSON」写着它是什么（对象几个键、数组几项、多大），Enter 打开窗口；下面三行 Enter 分别复制**格式化**、**压缩**、**转义**（放进字符串里用）后的 JSON。空格键预览格式化后的全文。
- `json` 后面直接粘贴 JSON，就用粘贴的这段。
- 不输入 `json`，直接把一段 JSON 粘贴进搜索框，结果里也会出现「查看 JSON」。
- 有错的 JSON 写明错在第几行第几列、缺了什么，Enter 打开窗口去改。

**窗口**
- 左边是原文，可以粘贴、修改，停手片刻就重新读；出错的那一行标红，「在原文中定位」直接跳过去。右边是树：← → 折叠和展开，Enter 切换，Ctrl+C 复制值，Ctrl+Shift+C 复制路径（如 `data.items[3].id`），右键还能复制键名、展开或折叠整棵子树。
- Ctrl+F 搜索键名和值，Enter / F3 下一个，Shift+Enter 上一个；匹配的地方自动展开。
- 工具栏：粘贴、格式化、压缩、全部展开、全部折叠、隐藏原文；「复制」菜单复制格式化、压缩或转义后的全部内容。
- `.json` 文件拖到窗口里打开；文件的右键菜单里有「用 JSON 工具查看」（`.json`、`.jsonl`、`.ndjson`、`.jsonc`、`.json5`、`.geojson`、`.har`、`.log`），最大 32 MB。

**认得出的写法**
- 日志行里夹着的 JSON（`2026-11-18 10:00:01 INFO {"user": …}`），前后多余的文字跳过并说明。
- 一行一段的 JSON（NDJSON、JSON Lines）和每行带一段 JSON 的日志，合成一个数组显示。
- 转义过的 JSON（`{\"a\":1}`）和整段被引号包起来的 JSON 字符串。
- 字段值本身是一段 JSON 字符串的（常见于日志里的 `payload`），标上「JSON」直接展开看里面。
- 宽松写法：注释、单引号、不带引号的键名、末尾多余的逗号、Python 的 `True` / `None`、`NaN`，照样读出来，底栏写明用了哪些。
- 超长整数和小数原样保留，不会被改成别的数。

**设置**：格式化时的缩进（2 个空格、4 个空格或 Tab）。

**权限**：开自己的窗口（`ui`）、读写剪贴板（看剪贴板里的 JSON，复制结果）、读你选择用它查看的文件。不联网，不改文件，不启动别的程序。AI 助手和 `ws` 命令行调用时不读剪贴板。

---

A resident C++ plugin in the plugin library (Settings → Plugins → Plugin library), not in the setup; it starts the first time it is used. First, a **JSON tool**. In the search box, `json` and a space shows the JSON on the clipboard (or what you paste after it): the first row says what it is and opens the window, the next three copy it formatted, minified or escaped for use inside a string; Space previews it formatted. JSON pasted straight into the search box offers the window too, and broken JSON says the line, the column and what is missing. The window has the text on the left (paste, edit, the error line marked, a button to jump to it) and a tree on the right: ← → close and open, Ctrl+C copies the value, Ctrl+Shift+C the path (`data.items[3].id`), right click for the name and whole subtrees; Ctrl+F searches names and values (Enter / F3 next, Shift+Enter previous). Drop a file on the window, or right-click a `.json`, `.jsonl`, `.ndjson`, `.jsonc`, `.json5`, `.geojson`, `.har` or `.log` file for View in JSON tool (up to 32 MB). It finds JSON inside log lines, one-per-line JSON (NDJSON) shown as one array, escaped or quoted JSON, and JSON held in a string field (marked JSON and opened in place); comments, single quotes, bare names, trailing commas, Python's `True` / `None` and `NaN` are read too, and the footer says so. Long integers and decimals are kept exactly. Setting: the indent when formatting. Permissions: `ui`, the clipboard (read and write), and the files you open with it; no network, no writing files, no other programs. The AI and the `ws` command line never get the clipboard.
