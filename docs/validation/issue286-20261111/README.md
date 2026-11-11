# #286 对话框旁的搜索框：只搜对话框当前的文件夹（2026-11-11）

GitHub #286：“打开文件的窗口能否实现仅在当前目录搜索，就像在资源管理器里直接打字搜索一样。”

改法：搜索框里按 `Ctrl+Shift+D`（和主搜索框切换文件夹范围是同一个键），或“…”菜单里的“只搜“某文件夹”里面”，之后只找对话框正显示的文件夹和它下面的内容；框前出现“文件夹名 ›”，点它、再按一次键或菜单里的“搜索整个电脑”恢复。选择记在 `Launcher/DialogBarFolderOnly`。设计见 `docs/architecture.md` 的同名条目。

这里只有文字日志；截图看过后没有入库。Qt 6.12.0 / MSVC Release。

## 现场 `live/run.log`

`harness/run-live.ps1`（普通权限，Windows PowerShell 5.1）：暂存一份 0.24.0 换上这次构建的程序，独立配置（搜索框开、放在下方、`DialogBarFolderOnly=true`、只索引一个小文件夹）；另一个普通权限的 PowerShell 开一个真的“打开”对话框，停在测试文件夹 `here286`。索引里有三个名字带 `wsq286` 的文件：`here286` 里一个、`here286\deeper` 里一个、旁边的 `elsewhere` 里一个。点搜索框、打 `wsq286`：

- 开着时（截图 1）：框前是蓝色的“here286 ›”，结果两条：`here286` 里的和 `deeper` 里的；`elsewhere` 里的不出现。
- 点一下“here286 ›”（截图 2）：名字消失，结果三条；配置文件里变成 `DialogBarFolderOnly=false`。
- 改之前的同一搜索（第一次跑，那时配置是关）：三条，和原来一样。

## 没有测的

- `Ctrl+Shift+D` 这个键本身：这台机器上它是别的程序的全局快捷键（`RegisterHotKey` 报 1409），按键到不了搜索框。键处理只有一行，调用的是和点名字、菜单项同一个 `toggleFolderOnly`。
- “…”菜单里的那一项没有点过（同一个函数）。
- 对话框换文件夹后名字跟着换：每次列结果前重读位置，没有单独实测。
- 管理员程序的对话框、WPS、Blender、旧式对话框：读不到当前位置的照常搜全部。

## 这次测试留下的教训

- 这台机器装着 Listary，它在对话框底边放自己的工具条，盖住搜索框中间一段：点搜索框前用 `WindowFromPoint` 找一处没被盖住的地方。
- 脚本一开始沿用 #277 的做法：有 Win顺 在运行就先让它退出、测完再启动。那时正好有别的会话在用构建目录里的一份，脚本“重新启动”时没有 Qt 的路径，弹出了“找不到 Qt6Quick.dll”的系统错误框（系统进程的窗口，脚本点不掉）。现在这两个脚本（#286、#287）改为：有别的 Win顺 在运行就不跑。

## 复现

```
powershell -File docs/validation/issue286-20261111/harness/run-live.ps1 -Build build\<构建目录> -Stage dist\<一份已部署的>\WinShun -Root dist\issue286 -Run build\issue286\live
```
