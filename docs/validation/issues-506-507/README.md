# #506 任务栏图标缩放、#507 对话框搜索框的搜索范围

## #507 对话框旁的搜索框默认搜整个电脑

0.52.0 把对话框旁的搜索框改成了“当前文件夹优先”而且没法关：和 `Ctrl+Shift+D`（只搜当前文件夹）看上去差不多，在“另存为”里妨碍换目录，结果最多 100 条时其他位置的还可能排不进来。

改后：

- 默认搜整个电脑（和 0.52.0 以前一样，不读对话框的路径）。
- 设置 › 系统整合 › 对话框 › 搜索的范围：整个电脑 / 当前文件夹优先 / 只搜当前文件夹（`DialogBarFolderFirst` 新键，默认关；`DialogBarFolderOnly` 照旧）。
- `Ctrl+Shift+D`、“…”菜单、点文件夹名：在“只搜当前文件夹”和前两档里选定的那档之间切换；菜单写“搜索整个电脑”或“也搜其他位置”，输入框提示同样跟着变。
- AI 设置目录加了 `Launcher/DialogBarFolderFirst`。

验证：

- `ctest -R '^(tst_dialogbarsuggestions|tst_settings)$'`：2 项全过。`searchLooksEverywhereUnlessAsked` 覆盖三档和“此电脑”；`newInstallDefaults` 覆盖新装默认、写回读出，以及 0.52.0 升上来时恢复整个电脑。
- `--preview-results <dir> --search-navigation --renderer d3d11`：126 + 54 + 64 项全过，`0 warning(s)`。对话框一组新增 4 条 #507 检查（菜单文字随所选档变化、`Ctrl+Shift+D` 换回时保留“当前文件夹优先”），见 `dialog-navigation.json`。
- `--preview-settings <dir> --page 5 --scale 1 / 1.75 --renderer d3d11`：三档在 100%、175% 下都排得下，默认选中“整个电脑”（`scope-row-175.jpg`）。
- 没有在真实的“打开 / 另存为”对话框里实测：本机运行着用户的 Win顺，另开一份会给同一个对话框挂两个搜索框。默认档走的是 0.52.0 以前的同一条搜索路径（请求不带文件夹）。

## #506 远程虚拟屏 175% 下任务栏上的图标被放大裁切

未能复现，已在 issue 里向报告者要信息。查到的情况：

- 报告者的截图里，图标正好被多放大了一次 175%（42 px 的格子里画了约 74 px，左上角对齐裁切），别的程序图标正常。系统是 26300 预览版，经 UU 远程的虚拟屏。
- `app.ico` 的各尺寸、用 `SHDefExtractIconW` / `PrivateExtractIconsW` 按 24–74 px 取 `WinShun.exe` 的图标，都正常。
- 本机（26200，150%）用测试窗口试：程序文件能对上的窗口（没有自己的应用 ID），任务栏按钮用的是程序文件的图标，`WM_SETICON` 给的图标不用；有单独应用 ID 的窗口才用窗口图标，给大 1.75 倍、或窗口不感知 DPI（窗口 96、屏幕 144）时，任务栏都会缩放到格子大小，不裁切。
- 诊断信息的 display 部分新增 `system_dpi`（登录时定下的系统缩放）、`taskbar_dpi`、`remote_session`，下次这类缩放问题能直接看出系统缩放和屏幕、任务栏是否对不上。
