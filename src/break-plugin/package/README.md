# 休息提醒 · Break reminder

插件库里的 C++ 常驻插件（`WinShunBreak.exe`，源码在 `src/break-plugin`），不随 Win顺 安装。设置 → 插件 → 插件库 里装上**休息提醒**并打开，它就跟着 Win顺 一起在后台计时。

**怎么提醒**
- 默认每 20 分钟小休 20 秒（看看远处、眨眨眼），每第二次换成大休 5 分钟（起来走走）。卡片上都能改。
- 提前 30 秒，屏幕上方出现一个小条：「30 秒后小休 20 秒」，可以「推迟 5 分钟」或「跳过」。每次休息只能推迟一次。
- 时间到了，如果你正在打字，等你停手（最多 15 秒；旁边放着视频也一样等），再把所有显示器调暗，中间显示倒计时。休息画面不抢焦点；它开着时键盘不会打进后面的程序，音量和播放键照常能用。按 Esc，或者点「跳过」，可以提前结束。
- **必须休完**（卡片上选「只有大休」或「大休和小休」）：没有「跳过」和「推迟」，长按 Esc 3 秒才能提前离开，留着这个出口是防开会、投屏时卡住。

**不打扰**
- 离开电脑 5 分钟以上（没碰键盘鼠标），就算已经休息过，回来后重新计时。锁屏、睡眠也一样。放视频、开视频会议时虽然不碰键盘，也不算离开。
- 玩全屏游戏、放幻灯片、全屏看视频，或者开着 Windows 的「勿扰」（专注助手）时，到点的休息先等着，结束后再提醒。全屏开着的终端、编辑器、远程桌面也算全屏；常这样工作的话，在卡片上关掉这一项。

**关键字** `休息`（或 `break`）：第一行是离下次休息还有多久，Enter 马上休息。下面可以现在小休、现在大休、暂停直到手动恢复，或暂停 1 小时或 3 小时（重启 Win顺 后也记得）、重新计时。

**权限**：只要「开自己的窗口」（`ui`），用来显示小条和休息画面，以及在休息画面开着时拦住键盘。不联网，不读文件，不启动别的程序。

---

A resident C++ plugin in the plugin library (Settings → Plugins → Plugin library), not in the setup. By default a 20-second short break every 20 minutes (look far away, blink), every second one a 5-minute long break (get up). Thirty seconds before, a small bar at the top of the screen offers Postpone (once per break) and Skip. When the break is due it waits for a pause in typing (at most 15 seconds), then dims every monitor with a countdown in the middle; it never takes the focus, keeps keys from the program underneath while it is up (volume and media keys still work), and Esc or Skip ends it early. Strict mode (long breaks only, or every break) removes Skip and Postpone; holding Esc for 3 seconds still gets you out. Five minutes away from the keyboard and mouse (or locked, or asleep) counts as a break; a video or a call that keeps the display on does not count as away. During full-screen programs, slide shows and Do not disturb a due break waits until they end (a switch on the card; a terminal, an editor or a remote desktop in full screen counts as full screen too). The keyword `break` (or `休息`) shows the time to the next break and offers a break now, a pause until you resume, or for 1 or 3 hours (kept across restarts) and a new count. Permissions: `ui` only, for its windows and the keyboard hook while the break screen is up; no network, no files, no other programs.
