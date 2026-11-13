# 资源管理器标签页 · Explorer tabs

插件库里的 C++ 常驻插件（设置 → 插件 → 插件库 里安装）。装上并打开后，新开的文件夹窗口不再单独弹出：桌面上双击文件夹、别的程序"打开所在文件夹"、Win顺 里按 Enter 打开文件夹，都会变成已经开着的资源管理器窗口里的一个新标签页，那个窗口来到前面。

- **按住 Ctrl+Shift 打开**：就是新窗口，这一次不合并。
- **文件夹已经开着**：要开的新窗口改成切到那个标签页，不再多开一个（设置里可以关）。有的打开方式资源管理器自己不开新窗口，只把开着它的窗口放到前面，这时标签页不会切过去。
- **并到哪个窗口**：默认是最近用过的那个；也可以选最早打开的。
- **从资源管理器窗口里要的新窗口照常是窗口**：窗口里按 Ctrl+N、右键“在新窗口中打开”。看的是打开那一刻前面是哪个窗口：前面是资源管理器窗口就保留新窗口；前面是桌面、Win顺 或别的程序就并成标签页。设置里“这些程序打开的仍是新窗口”可以再加程序（比如 `WindowsTerminal.exe;cmd.exe`）。
- **不合并的情况**：只开着这一个资源管理器窗口时；控制面板这类标签页放不下的地方；比 Win顺 权限高的（管理员）资源管理器窗口。Win顺 以管理员身份运行时，在插件卡片上点一次"确认"，以管理员身份运行的文件管理器窗口也能并成标签页。
- 输入 `标签页`（或 `tabs`）看它是否开着，Enter 暂停或恢复；暂停会记住，重启后还是暂停。
- **打开上次的标签页**：开着的文件夹窗口和每个窗口里的标签页会一直记着。设置里打开“开机后打开上次的标签页”，关机（或注销）时还开着的窗口，下次登录后自己回来：标签页、哪个在前面、窗口的位置和大小都照原样。不开这项也行，输入 `标签页` 选第二行“打开上次的标签页”，随时手动打开。
  - 已经开着的文件夹不会重复打开；文件夹不在了（U 盘没插、网络盘连不上）就跳过。
  - 关机前自己把窗口都关了，下次登录不会自动打开；手动那一行仍能把最后关掉的窗口找回来。
  - Windows 自己的“登录时还原上一个文件夹窗口”也开着时，等它先还原，再把缺的标签页补到它还原的窗口里。
  - 标签页按打开的先后排，手动拖过顺序的不保证一样；控制面板这类窗口不记；比 Win顺 权限高的资源管理器窗口不记。
  - Win顺 要随 Windows 启动（设置 › 常规 › 开机时自动启动）；登录十分钟后才启动 Win顺，不会自动打开。
- **系统要求**：Windows 11 22H2（build 22621）或更新。资源管理器标签页是从这一版才有的；Windows 10 和 Windows 11 21H2 的资源管理器没有标签栏，装了也不会生效：插件不拦截任何窗口，新开的文件夹照常是窗口，输入 `标签页` 会看到“需要 Windows 11 22H2 或更新”。
- **ui**：要找到资源管理器的窗口、打开标签页，所以需要"开自己的窗口、找别的窗口"这一项。
- **elevated**：Win顺 以管理员身份运行、你点过"确认"时，它也以管理员身份运行；Win顺 不是管理员时照常用普通权限，不会弹 UAC。

---

A resident C++ plugin from the plugin library (Settings → Plugins → Plugin library). Once on, a folder that would open in a new File Explorer window becomes a new tab of the Explorer window already open, which comes to the front -- from a double-click on the desktop, "open folder" in another program, or Enter on a folder in Win顺. Hold Ctrl+Shift while opening to get a window. A new window asked for from an Explorer window stays a window: Ctrl+N, "Open in new window" -- what counts is the window in front when the folder opens: an Explorer window keeps it a window; the desktop, Win顺 or another program gets a tab. The setting "Programs that still get a window" adds programs of your own (e.g. `WindowsTerminal.exe;cmd.exe`). A folder already open in a tab: the new window becomes a switch to that tab (a setting); when Explorer itself opens no window and only brings that window forward, the tab is not switched. The tab goes to the window used last (or the one opened first). Left alone: the only Explorer window, Control Panel and the like, Explorer windows running above Win顺's rights. When Win顺 runs as administrator, one click on Confirm (确认) on the plugin's card lets it merge the windows of a file manager running as administrator too. `tabs` shows whether it is on; Enter pauses or resumes it, and a pause is kept across restarts. The open folder windows and their tabs are remembered: with the setting "Open the tabs from last time after logon" on, the windows still open at shutdown or sign-out come back after the next logon -- tabs, the tab in front, each window's place and size; without it, the second row of `tabs` ("Open the tabs from last time") opens them on request. Folders open already are not opened twice, folders that are gone are skipped, windows you closed yourself before shutting down do not come back by themselves, and with Windows' own "Restore previous folder windows at logon" on, the missing tabs are added to the windows it restores. Tabs come back in the order they were opened; Control Panel windows and Explorer windows above Win顺's rights are not remembered; Win顺 has to start with Windows. It needs Windows 11 22H2 (build 22621) or newer, the first Explorer with tabs: on Windows 10 and Windows 11 21H2 it does nothing at all (no window is touched, and `tabs` says so). It needs `ui` to find Explorer's windows, and `elevated` to follow Win顺 when it runs as administrator (never a UAC prompt when Win顺 does not).
