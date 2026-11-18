# 鼠标手势 · Mouse gestures

插件库里的 C++ 常驻插件（设置 → 插件 → 插件库 里安装）。装上并打开后，在任何窗口里按住右键画一下，松开就执行这个手势的动作；画的时候屏幕上有一条轨迹，旁边写着松开会做什么。

- **默认的手势**：← 后退，→ 前进，↑ 回到顶部，↓ 到底部，↓→ 关闭窗口，↓← 最小化，↑↓ 打开 Win顺 搜索框（再画一次收起）。
- **每个都能改**：在插件卡片上，每个手势填一个动作：后退、前进、回到顶部、到底部、刷新、关闭窗口、关闭标签页、最小化、最大化、搜索框；或者按键，如 `Ctrl+W`、`Alt+Left`、`Ctrl+Shift+T`、`F5`、`Win+D`；或者要打开的程序、快捷方式、文件夹、网址（像双击一样打开，要带参数就先做一个快捷方式）。留空就不用这个手势。
- **更多手势**：“更多手势”一栏写 `方向 = 动作`，用分号隔开；方向写箭头或字母 L R U D（左右上下），最多 6 段，例如 `↑← = Ctrl+Shift+T; RL = notepad.exe`。
- **右键照常用**：只是点一下右键，菜单照常弹出（Win顺 先接住这一下，松开后原样补上）。要用右键拖动（比如右键拖文件），按住右键先停一下再拖。按着 Ctrl、Shift、Alt 或 Win 时右键不画手势；画到一半按一下左键就取消。
- **不用手势的地方**：“这些程序里不用手势”里列的程序（程序名用分号隔开）；全屏的游戏、幻灯片、视频（设置里可以打开）；触摸和笔。
- **管理员身份运行的窗口里用不了**：Windows 不让普通权限的程序拦截、补发这些窗口里的鼠标操作（UIPI），右键在那里和平常一样。
- 后退和前进相当于鼠标侧面的后退、前进键；回到顶部、到底部、刷新、关闭标签页和按键，先把画手势的那个窗口放到前面，再按键。关闭窗口、最小化、最大化不会作用到桌面和任务栏。
- 输入 `手势`（或 `gestures`）列出设好的手势，第一行 Enter 暂停或恢复；暂停会记住，重启后还是暂停。
- **ui**：要装鼠标钩子、画轨迹、按键。**processes**、**winshun:open**：手势填了程序或网址时，请 Win顺 用你的普通权限打开它。

---

A resident C++ plugin from the plugin library (Settings → Plugins → Plugin library). Once on, hold the right button anywhere and draw; let go and the gesture's action runs. A trail follows the pointer while you draw, with what letting go will do beside it. Defaults: ← back, → forward, ↑ to the top, ↓ to the bottom, ↓→ close the window, ↓← minimize, ↑↓ WinShun's search box (draw it again to close it). Each gesture takes an action on the plugin's card: back, forward, top, bottom, refresh, close, closetab, minimize, maximize, search; or keys such as `Ctrl+W`, `Alt+Left`, `Ctrl+Shift+T`, `F5`, `Win+D`; or a program, shortcut, folder or web address, opened as a double click would (make a shortcut for arguments). Empty: that gesture is off. "More gestures" takes `directions = action` pairs separated by semicolons, directions as arrows or the letters L R U D, up to 6, e.g. `↑← = Ctrl+Shift+T; RL = notepad.exe`. A plain right click still brings up the menu: the press is held back and sent again on release. For a drag with the right button (dragging files), hold still a moment first. No gesture with Ctrl, Shift, Alt or Win held; a left click while drawing cancels it. Left alone: the programs listed under "No gestures in these programs", full-screen games, slide shows and video (unless turned on), touch and pen, and windows run as administrator (Windows will not let a program with normal rights take or resend their mouse input, UIPI). Back and forward are the mouse's own back and forward buttons; top, bottom, refresh, close the tab and keys first bring the window drawn on to the front; close, minimize and maximize never act on the desktop or the taskbar. `gestures` lists them; Enter on the first row pauses or resumes, kept across restarts. It needs `ui` for the mouse hook, the trail and the keys, and `processes` and `winshun:open` to have WinShun open a program or address with your normal rights.
