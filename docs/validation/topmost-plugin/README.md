# 窗口置顶插件（winshun.topmost 1.0.0）验证

2026-11-25，Windows 11 Pro 26200，150% 缩放，工作树 `window-top`。

| 验证 | 结果 |
|---|---|
| `ctest -R "^tst_topmost$\|^tst_pluginsdk$"` | 通过。哪些窗口算用户的窗口；在测试自己建的隐藏窗口上置顶、取消、重复置顶、窗口已销毁；标题与程序名的读取和筛选 |
| `wsbench --plugin build\window-top\library-plugins\winshun.topmost --query "置顶 "` | 通过真实的 `WinShunPluginHost`（中完整性、作业内）列出 13 个打开的窗口；`wsbench` 不替插件注册快捷键，插件按“宿主不支持”记日志（错误码 50） |
| `python drive.py <WinShunTopmost.exe> <输出目录>`（本目录的脚本，模拟宿主按协议驱动插件进程，目标是脚本自己开的一个测试窗口，不发送任何键鼠输入） | 18 项全部通过：`initialize` 后先报 `winshun/hotkeys` 再调 `register_hotkeys`；按标题筛到测试窗口；`Enter` 置顶（`WS_EX_TOPMOST` 置位）并回 `hide`；再查询时排在最前、写着“已置顶”；`hotkey` 通知取消置顶、再次置顶；快捷键被占用（1409）时写日志；筛不到时给一行说明；进程号对不上的旧结果行不改任何窗口；`shutdown` 后测试窗口自动取消置顶，进程自行退出（退出码 0） |

提示条实拍（150%）：置顶 [pin.png](pin.png)，取消置顶 [unpin.png](unpin.png)。

没有验证的：

- 在真实运行的 Win顺 里按 `Win+Ctrl+T`。同一用户下 Win顺 不能并存两个实例，这次没有退出正在使用的那一份；快捷键注册走的是截图插件已在用的宿主 `register_hotkeys`，这里只验证了插件一侧的协议。
- 以管理员身份运行的窗口上的“改不了”提示（本机提权不弹确认，但没有为此起一个提权窗口）。
- Windows 10 和 100%、125%、175% 缩放下的提示条。
