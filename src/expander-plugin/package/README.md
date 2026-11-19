# 缩写展开 · Text expander

插件库里的 C++ 常驻插件（设置 → 插件 → 插件库 里安装）。装上并打开后，在任何程序的输入框里打出一个缩写，再按空格、回车、Tab 或标点，缩写就换成设好的文字。

- **写缩写**：插件卡片上的“缩写”框里每行一条，`缩写 = 换成的文字`，例如 `cmd = command`、`;addr = 北京市海淀区…`、`@@ = me@example.com`。按第一个 `=`（全角 `＝` 也行）分开，两边的空格不算；`#` 开头的行是注释。
- **多行内容**：接着往下写，下一行不带 `=` 就算上一条的下一行（行首的空格保留，中间的空行也保留）。也可以写成 `sig =`，内容全写在下面几行。换行按 Shift+Enter 打出，聊天软件里不会把半截消息发出去。
- **什么时候换**：打完缩写后按的空格、回车、Tab 或标点照常打出，跟在内容后面。缩写以字母或数字开头时，前面要是空格、标点或刚开始打字，`xcmd` 里的 `cmd` 不算；以符号开头的（`;addr`）在哪都算。几条都对得上时用最长的。打开“打完缩写立刻换”就不等空格，适合 `;sig` 这种不会是别的词一部分的缩写。
- **大小写**：缩写写成小写时，打首字母大写的 `Btw` 换成 `By the way`，打全大写的 `BTW` 换成 `BY THE WAY`；其他混写不算。写成别的样子的缩写只认原样。
- **撤回**：刚换完马上按退格，换回原来打的字（多行内容和以回车、Tab 结束的不撤回）。
- **不换的地方**：输入法在中文（日文、韩文）状态时，字母是输入法的，不换；切到英文状态就换。菜单打开时、Win顺 自己的窗口里（改缩写时不会被换掉）、“这些程序里不展开”列的程序里不换。管理员身份运行的窗口里用不了：Windows 不让普通权限的程序往那里打字（UIPI）。点一下鼠标、切换窗口、按方向键或快捷键后，之前打的字不再算进缩写。
- 缩写里不能有空格，最长 32 个字符；中文、全角字符是输入法打的，看不到，不能当缩写。用不上的行在 `缩写` 关键字里列出来，写明原因。
- 输入 `缩写`（或 `expander`）列出所有缩写，Enter 复制某条的内容；第一行 Enter 暂停或恢复，暂停会记住，重启后还是暂停。
- **ui**：要装键盘、鼠标钩子，替你打字。打的字只在本机这个插件里用来比对缩写，只留最后几十个，不写文件，不上传。

---

A resident C++ plugin from the plugin library (Settings → Plugins → Plugin library). Once on, type an abbreviation in any program's text box, then a space, Enter, Tab or punctuation, and the abbreviation becomes the text you set. Write them in the "Abbreviations" box on the plugin's card, one a line: `abbreviation = text`, such as `cmd = command` or `@@ = me@example.com`; split at the first `=`, spaces around it left out; lines starting with `#` are notes. For text of several lines, go on below: a line without `=` is the next line of the one above (its leading spaces and blank lines between are kept), or write `sig =` with all of it below; new lines are typed as Shift+Enter, so a chat box does not send half a message. The key that ended the abbreviation is typed after the text as usual. An abbreviation that starts with a letter or digit counts after a space, punctuation or nothing (not `cmd` in `xcmd`); one starting with a sign (`;addr`) counts anywhere; the longest one wins. "Expand as soon as it is typed" skips the space, for abbreviations such as `;sig`. Written in small letters, an abbreviation typed as `Btw` becomes `By the way`, as `BTW` `BY THE WAY`; other mixtures do not count, and one written otherwise counts only as written. Backspace right after takes it back (not text of several lines, nor after Enter or Tab). Left alone: an input method writing Chinese, Japanese or Korean (the letters are its own; in its English mode they count), an open menu, WinShun's own windows (so writing abbreviations does not expand them), the programs under "Not in these programs", and windows run as administrator (Windows will not let a program with normal rights type there, UIPI). A click, another window, an arrow key or a shortcut starts over. An abbreviation has no spaces and 32 characters at most, and cannot be Chinese or full-width (an input method types those out of sight); lines not used are listed under the keyword with the reason. `expander` lists the abbreviations, Enter copies one's text; Enter on the first row pauses or resumes, kept across restarts. It needs `ui` for the keyboard and mouse hooks and to type for you; what you type is only compared with the abbreviations in this plugin, the last few dozen characters kept in memory, never written down or sent anywhere.
