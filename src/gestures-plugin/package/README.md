# 鼠标手势 · Mouse gestures

插件库里的 C++ 常驻插件（设置 → 插件 → 插件库 里安装），需要 Win顺 0.41.0 或更新版本。装上并打开后，在任何窗口里按住右键画一下，松开就执行这个手势的动作；画的时候屏幕上有一条轨迹，旁边写着松开会做什么。除了右键，也可以用中键和侧键画。点插件卡片上的“设置”打开手势设置窗口。

## 能做什么

- **画手势**：上下左右组合（最多 6 段），还能**画圈**（顺时针、逆时针）。默认：← 后退，→ 前进，↑ 回到顶部，↓ 到底部，↓→ 关闭窗口，↓← 最小化，↑↓ 打开 Win顺 搜索框。
- **换按键、组合手势**（#436）：设置窗口“常规”页的“触发手势的鼠标按键”里，右键、中键、侧键（后退）、侧键（前进）可以同时启用几个，至少留一个。左键只能当组合手势的第二个键，平常的点击和拖动不受影响。组合手势是按住手势键再点另一个键（比如按住右键点左键），也可以先画一段再点（比如画 ↓ 再点左键）。录手势时可以选它只认当时按的那个键，还是任意启用的键都算。
- **选中文字后画**：先选中一段文字再画，手势就用这段字。默认：↓ 复制，↑ 网页搜索，→ 翻译（结果显示在气泡里，点一下复制；翻译服务的密钥不对时气泡里直接说明，#444）。没选中文字时，还是平常的手势。选中的文字由 Win顺 用 Windows 的 UI 自动化来读，不经过剪贴板，剪贴板里原来的东西不会被换掉。浏览器、VS Code、微信等用 Chromium 做的程序和 Firefox 里不读（一读它们就打开整套无障碍功能，变慢，VS Code 还会进入屏幕阅读器模式），在那里还是平常的手势。
- **选中文字后按快捷键**：在设置窗口“选中文字时”一页下面加一个快捷键（比如 Ctrl+Alt+T），选中文字后按它就翻译，动作可以改成复制、网页搜索等。这个在浏览器、VS Code、微信里也能用：Win顺 读不到时，替你按一下 Ctrl+C 读出选中的字，读完马上把剪贴板原样放回去，Win顺 自己的剪贴板历史里不会多一条（Windows 的剪贴板历史 Win+V 会记下这段字）。快捷键被别的程序占用时，设置里会标红。在浏览器里用要 Win顺 0.42.0 或更新的版本。
- **屏幕分区**：同一个手势在屏幕四个角上画可以做不同的事，比如只在右下角画圈才关机。角上的范围是屏幕宽、高各五分之一。
- **触发角**：把鼠标推到屏幕角落停一下就执行；**摩擦边**：在屏幕边上来回蹭几下就执行。只算屏幕的外边缘，两块屏幕相接的地方不算。默认都没设置。
- **按住手势键滚滚轮**：默认切换上一个、下一个标签页；滚过滚轮，松开按键不弹菜单。不同的手势键可以设不同的动作。
- **每个程序单独设置**：某个手势只在某个程序里换成别的动作，或设成“什么都不做”在那里关掉它。
- **动作**：后退、前进、刷新、回到顶部、到底部；新建、关闭、重新打开、切换标签页；关闭、最小化、最大化、置顶窗口，显示桌面、任务视图、切到上一个窗口；复制、剪切、粘贴、撤销、重做、全选，网页搜索、用 Win顺 搜、翻译；打开 Win顺 搜索框；静音、音量、播放暂停、上一首下一首；锁屏、关闭屏幕、睡眠、关机、重启、注销。也可以按任意组合键、打开程序文件夹或网址、输入一段文字。
- **关机、重启、注销、睡眠先倒数**：屏幕中间出现一张卡片倒数 5 秒，按 Esc 或点“取消”就不做；有没保存的程序时，Windows 照常会问。

## 用起来

- **按键照常用**：只是点一下手势键，菜单或它原来的功能照常（先接住这一下，松开后原样补上）。要用这个键拖动（比如右键拖文件），按住先停一下再拖。
- **反悔**：画到一半按 Esc 或点一下左键，松开就什么都不做（左键已设成组合手势时，点它就是那个组合）。
- 按着 Ctrl、Shift、Alt 或 Win 时不画手势。
- **不用手势的地方**：设置里列的程序；全屏的游戏、幻灯片、视频（设置里可以打开）；触摸和笔。
- **管理员身份运行的窗口里用不了**：Windows 不让普通权限的程序拦截、补发这些窗口里的鼠标操作，鼠标按键在那里和平常一样。
- 输入 `手势`（或 `gestures`）列出设好的手势，第一行 Enter 暂停或恢复；暂停会记住，重启后还是暂停。
- 1.0 里改过的手势，升级后自动搬到新的设置里。

## 最近的版本

- **2.1.3**：中键、侧键也能画手势，几个键同时启用，还有组合手势（#436）；翻译密钥不对时直接说明（#444）；用 Win顺 0.45.0 的共用代码重新编译。
- **2.1.2**：Windows 10 上手势卡片和气泡画出 1 像素的边；用 Win顺 0.44.0 的共用代码重新编译。
- **2.1.1**：选中文字快捷键带 `Alt`、`Win`、`Shift` 时，松开后不再触发浏览器的“…”按钮、菜单栏、开始菜单或切换输入法；设置窗口在 Windows 11 上恢复边框、阴影和圆角（#419）。
- **2.1.0**：选中文字后按快捷键；浏览器里按住右键滚滚轮连续切标签页。
- **2.0.0**：选中文字后画、画圈、屏幕分区、触发角、摩擦边、按程序设置、更多动作和新的设置窗口。

## 权限

- **ui**：装鼠标钩子、画轨迹和提示、按键。
- **processes**、**winshun:open**：动作是程序、文件或网址时，请 Win顺 用你的普通权限打开；网页搜索也由 Win顺 打开。
- **clipboard:write**：点翻译气泡时把译文放到剪贴板。
- **system:power**：关机、重启、睡眠由 Win顺 来做（插件自己没有这个权限）。
- **selection:read**：画手势或按快捷键时请 Win顺 读一下前台窗口里选中的文字（插件的沙盒碰不到别的程序的窗口）。

---

A resident C++ plugin from the plugin library (Settings → Plugins → Plugin library), for WinShun 0.41.0 or later. Hold the right button anywhere and draw; let go and the gesture's action runs, with a trail and what it will do shown while you draw. The middle and side buttons can draw too. The 设置 button on the plugin's card opens the gestures window.

- **Gestures**: up to 6 directions, and **circles** (clockwise, counter-clockwise). Defaults: ← back, → forward, ↑ top, ↓ bottom, ↓→ close the window, ↓← minimize, ↑↓ WinShun's search box.
- **Other buttons and chords** (#436): under 常规 › 触发手势的鼠标按键, any of the right, middle, back and forward buttons can start gestures, at least one of them. The left button only ever completes a chord, so ordinary clicks and drags are untouched. A chord is a gesture button held and another button clicked (hold right, click left), or a stroke followed by a click (draw ↓, then click left). When recording a gesture, choose whether it belongs to the button pressed or to any enabled one.
- **With words selected**: select some text first, then draw, and the gesture acts on it. Defaults: ↓ copy, ↑ web search, → translate (in a bubble; click it to copy; a refused translation key is named there, #444). WinShun reads the selection through UI Automation, never the clipboard, so what is on it stays. Browsers, VS Code, WeChat and other Chromium programs, and Firefox, are not read (asked, they turn their whole accessibility on and get slower; VS Code goes into its screen reader mode): the plain gestures hold there.
- **Shortcuts for selected text**: add a shortcut (e.g. Ctrl+Alt+T) at the bottom of the 选中文字时 page; select some text and press it to translate, or set it to copy, search the web and so on. It works in browsers, VS Code and WeChat too: where WinShun cannot read the selection, it presses Ctrl+C for you and puts the clipboard back at once, without a new entry in its own clipboard history (Windows' own, Win+V, keeps the words). A shortcut another program holds is marked in red. Browsers need WinShun 0.42.0 or later.
- **Screen corners for gestures**: the same gesture can do something else when drawn in a corner of the screen (a fifth of its width and height), e.g. a circle in the bottom right shuts down.
- **Hot corners and rubbed edges**: push the pointer into a corner and rest a moment, or rub it back and forth along an edge. Only the outer edges of the desktop count. Off by default.
- **Gesture button + wheel**: previous / next tab by default; no menu on release after the wheel was used. Each gesture button can have its own.
- **Per program**: a gesture can do something else in one program, or nothing there.
- **Actions**: page, tab, window, text, WinShun, media and system actions (lock, screen off, sleep, shut down, restart, sign out), any keys, open a program, file, folder or address, or type some text.
- **Shut down, restart, sign out and sleep count down first**: a card counts down 5 seconds; Esc or Cancel stops it. Programs with unsaved work can still hold Windows up.

A plain click of a gesture button still does what it did (the menu for the right button); to drag with it, hold still a moment first. Esc or a left click while drawing cancels (unless the left button completes a chord there). Not with Ctrl, Shift, Alt or Win held, not in the programs listed in the settings, full-screen games and video (unless turned on), touch and pen, or windows run as administrator. `gestures` lists them; Enter on the first row pauses or resumes. Gestures changed in 1.0 are carried over.

Recent versions: **2.1.3** middle and side buttons, several at once, and chords (#436); a refused translation key is named (#444); rebuilt with WinShun 0.45.0's shared code. **2.1.2** a 1 px edge on the cards and bubbles on Windows 10; rebuilt with WinShun 0.44.0. **2.1.1** shortcuts with `Alt`, `Win` or `Shift` no longer leave the browser's “…” button, a menu bar, Start or an input-method switch behind; the settings window has its border, shadow and corners back on Windows 11 (#419). **2.1.0** shortcuts for selected text; right button + wheel keeps switching tabs in browsers. **2.0.0** selected-text gestures, circles, screen corners, hot corners, rubbed edges, per-program rules, more actions and the new settings window.

Permissions: `ui` (hook, trail, keys), `processes` and `winshun:open` (WinShun opens programs, files, addresses and web searches with your normal rights), `clipboard:write` (copying a translation), `system:power` (WinShun shuts down, restarts or sleeps; the plugin has no right to), `selection:read` (WinShun reads the words selected in the window in front when you draw or press a shortcut; the plugin's sandbox keeps it from other programs' windows).
