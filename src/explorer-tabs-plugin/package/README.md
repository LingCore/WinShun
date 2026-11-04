# 资源管理器标签页 · Explorer tabs

插件库里的 C++ 常驻插件（设置 → 插件 → 插件库 里安装）。装上并打开后，新开的文件夹窗口不再单独弹出：桌面上双击文件夹、别的程序"打开所在文件夹"、Win顺 里按 Enter 打开文件夹，都会变成已经开着的资源管理器窗口里的一个新标签页，那个窗口来到前面。

- **按住 Ctrl+Shift 打开**：就是新窗口，这一次不合并。
- **文件夹已经开着**：要开的新窗口改成切到那个标签页，不再多开一个（设置里可以关）。有的打开方式资源管理器自己不开新窗口，只把开着它的窗口放到前面，这时标签页不会切过去。
- **并到哪个窗口**：默认是最近用过的那个；也可以选最早打开的。
- **从资源管理器窗口里要的新窗口照常是窗口**：窗口里按 Ctrl+N、右键“在新窗口中打开”。看的是打开那一刻前面是哪个窗口：前面是资源管理器窗口就保留新窗口；前面是桌面、Win顺 或别的程序就并成标签页。设置里“这些程序打开的仍是新窗口”可以再加程序（比如 `WindowsTerminal.exe;cmd.exe`）。
- **不合并的情况**：只开着这一个资源管理器窗口时；控制面板这类标签页放不下的地方；比 Win顺 权限高的（管理员）资源管理器窗口。Win顺 以管理员身份运行时，在插件卡片上点一次"确认"，以管理员身份运行的文件管理器窗口也能并成标签页。
- 输入 `标签页`（或 `tabs`）看它是否开着，Enter 暂停或恢复；暂停会记住，重启后还是暂停。
- **ui**：要找到资源管理器的窗口、打开标签页，所以需要"开自己的窗口、找别的窗口"这一项。
- **elevated**：Win顺 以管理员身份运行、你点过"确认"时，它也以管理员身份运行；Win顺 不是管理员时照常用普通权限，不会弹 UAC。

---

A resident C++ plugin from the plugin library (Settings → Plugins → Plugin library). Once on, a folder that would open in a new File Explorer window becomes a new tab of the Explorer window already open, which comes to the front -- from a double-click on the desktop, "open folder" in another program, or Enter on a folder in Win顺. Hold Ctrl+Shift while opening to get a window. A new window asked for from an Explorer window stays a window: Ctrl+N, "Open in new window" -- what counts is the window in front when the folder opens: an Explorer window keeps it a window; the desktop, Win顺 or another program gets a tab. The setting "Programs that still get a window" adds programs of your own (e.g. `WindowsTerminal.exe;cmd.exe`). A folder already open in a tab: the new window becomes a switch to that tab (a setting); when Explorer itself opens no window and only brings that window forward, the tab is not switched. The tab goes to the window used last (or the one opened first). Left alone: the only Explorer window, Control Panel and the like, Explorer windows running above Win顺's rights. When Win顺 runs as administrator, one click on Confirm (确认) on the plugin's card lets it merge the windows of a file manager running as administrator too. `tabs` shows whether it is on; Enter pauses or resumes it, and a pause is kept across restarts. It needs `ui` to find Explorer's windows, and `elevated` to follow Win顺 when it runs as administrator (never a UAC prompt when Win顺 does not).
