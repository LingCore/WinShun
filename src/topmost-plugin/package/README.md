# 窗口置顶 · Always on top

插件库里的 C++ 常驻插件（设置 → 插件 → 插件库 里安装）。装上并打开后，可以让任意一个窗口一直显示在其他窗口上面：边看视频边记笔记、对着文档填表、把计算器留在手边。

- **快捷键**：默认 `Win+Ctrl+T`，置顶当前窗口；再按一次取消。窗口顶上会出现“已置顶”或“已取消置顶”，一秒后消失。快捷键在插件卡片上改，留空就不用。
- **置顶的窗口有描边**：由这个插件置顶的窗口，边缘有一圈强调色的细线，一眼看得出哪个窗口置顶着。线画在窗口边缘以内，跟着窗口移动、缩放，圆角和窗口一致，最大化时贴着屏幕边；最小化、切到别的虚拟桌面、全屏时不显示，取消置顶或关掉窗口就没了。不挡鼠标，不抢焦点。不想要可以在插件卡片上关掉“给置顶的窗口描边”。
- **在搜索框里**：输入 `置顶`（或 `top`），列出打开的窗口，已经置顶的排在前面；后面接着打字可以按标题或程序名筛。在一个窗口上按 `Enter` 置顶它并切过去，已置顶的按 `Enter` 取消。置顶的窗口不止一个时，最后有一行“全部取消置顶”。
- **用的是 Windows 自己的置顶**：和任务管理器的“置于顶层”是同一个开关，所以别的程序自己置顶的窗口也会列出来，也能在这里取消。几个窗口都置顶时，它们之间照常谁在前谁盖住谁。
- **做不到的时候会说明**：以管理员身份运行的窗口，Windows 不允许普通权限的程序改它（UIPI），窗口上会写明；程序没有响应、窗口已经关了也一样。独占全屏的游戏上面什么窗口都显示不了，置顶也不例外。
- **停用时还原**：停用或卸载插件、退出 Win顺 时，由这个插件置顶的窗口会取消置顶，不会留下取消不了的窗口。
- **ui**：要看有哪些窗口、改它们的置顶状态、在窗口上显示提示。不读窗口里的内容，窗口标题只用来列出和筛选，不写文件，不上传。

---

A resident C++ plugin from the plugin library (Settings → Plugins → Plugin library). Once on, any window can stay above the others: notes beside a video, a form beside a document, the calculator at hand. The shortcut, `Win+Ctrl+T` by default, keeps the window in front on top, and takes it off again on the next press; the window says “On top” or “No longer on top” for a second. A window this plugin keeps on top has a thin line in the accent colour round it, so it shows which windows are on top: just inside the window's edge, following it as it moves and is resized, round where the window is round, on the screen's edge when it is maximized; not while it is minimized, on another desktop or in full screen, and gone once the window comes off the top or closes. It takes no click and no focus; “A line round the windows kept on top” on the plugin's card turns it off. Change the shortcut on the plugin's card, or leave it empty for none. In the search box, `top` lists the open windows, those on top first; type on to filter by title or program. `Enter` on a window keeps it on top and brings it to the front; on one that is on top already, takes it off. With more than one on top, a last row takes them all off. This is Windows' own switch (the one behind Task Manager's “Always on top”), so windows that other programs put on top are listed and can be taken off here too, and several windows on top cover each other as usual. What cannot be done is said on the window: a window run as administrator (Windows will not let a program with normal rights change it, UIPI), a program that does not respond, a window that has closed. Nothing shows above a game in exclusive full screen. Windows this plugin put on top come off it when the plugin is turned off or removed, or WinShun quits. It needs `ui` to see which windows there are, change them and show its note; it reads nothing inside the windows, and titles are only listed and filtered, never written down or sent anywhere.
