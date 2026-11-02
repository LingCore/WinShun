# GitHub #175 验证（2026-11-03）

## 自动测试

- `tst_numberhints`：新增 `takenDigit()`（只拿设定的修饰键 + 数字；Shift 可带；Win、AltGr、单按数字不拿）和 `shownRows()`（行高不一 20/60、上下边距 4：滚动后从第一个露出一半以上的行编号；高行露不到一半不编号；最多 9 个；`BottomToTop` 倒着排；空列表）。通过。
- `tst_core`：`rawInputAside()` 加了 `away()` 的检查。通过。

## 探针（另一个进程 `RegisterHotKey(Ctrl+5)` 数 `WM_HOTKEY`，探针窗口在前，`SendInput` 3 次）

| 探针自己 | 钩子拿到 | 对方 WM_HOTKEY |
|---|---|---|
| 只有低级键盘钩子 | 3 | 0 |
| 钩子 + 键盘 Raw Input（INPUTSINK／无标志／EXINPUTSINK） | 0 | 3 |
| 只有 `RIDEV_NOHOTKEYS`（自己在前） | 窗口收到 3 | 0 |
| 只有 `RIDEV_NOHOTKEYS`（别的程序在前） | — | 3 |

## 实机（独立副本 E:\wsi12-stage\fx175，临时配置，`ui.lock`，测试程序提权并占着 Ctrl+5、Alt+9）

配置：结果列表数字键 Ctrl，剪贴板数字键 Alt，剪贴板快捷键 Ctrl+Alt+F11，双击 Shift 开关启动器，`[Content] Index=false`。

| 情况 | 结果 |
|---|---|
| A 启动器在前、9 行，按 Ctrl+5 | 打开第 5 行（wsq175n5），对方 0 |
| B 测试程序窗口在前，按 Ctrl+5 | 对方 +1 |
| C 钩子装着时在启动器里双击 Shift（按下、抬起分开发） | 启动器关了 |
| D 剪贴板开在测试程序上面（不抢焦点），按 Alt+9 | 对方 +0 |
| D2 剪贴板关了，按 Alt+9 | 对方 +1 |
| E 从启动器打开剪贴板（激活），按 Alt+9 | 对方 +0 |

日志里窗口切换时有 “Number keys: keyboard Raw Input set aside / put back”。Windows 剪贴板历史每轮前后都是 25 条。跑完后 HKCU 的“在此搜索”和计划任务 `\WinShun` 仍指向 Program Files。

注：最早一轮的 C 不过，是测试发法不对：一次 `SendInput` 把 Shift 的按下和抬起一起发，这正是检测器有意不算的“程序整个发出的单按”（`kSentWholeMs`）。改成分开发以后就过了。

## 钉钉（3b，只观察）

这台机器上的钉钉主进程是高完整性（管理员，0x3000）运行的：它的清单是 asInvoker，多半是开机项里的补丁程序把它提权启动的。不提权的 Win顺 往里发不了 Ctrl+V（UIPI），所以双击只能复制，并提示“那个程序以管理员身份运行”。没有改代码，只改了说明。
