# #299：开始菜单打字后搜索框不接收输入（2026-11-12）

报告：0.24.0 便携版，`WinShun.exe` 勾了“以管理员身份运行”，开着“在开始菜单里打字，也用 Win顺 搜索”和任务栏搜索框。桌面状态下点开始菜单打字，搜索框闪一下就没；之后点任务栏上的搜索框，框出来了但字母进不去。

## 复现

本机 Windows 11 25H2（26200），150% 缩放。0.25.0 的部署副本以管理员身份启动（等同于那个兼容性标记），独立配置目录，`repro.ps1` 以管理员身份用 SendInput 做真实的鼠标点击和按键，每一步记下前台窗口并截图。

关键条件是**开始菜单打开之前，活动窗口是任务栏**（点一下任务栏空白处；所有窗口最小化时也是这样）：

| 步骤 | 0.25.0 | 修复后 |
|---|---|---|
| 点开始按钮，打 `qw` | 搜索框出现约 230 毫秒后消失，字母丢失 | 搜索框留着，框里是 `qw`，有结果 |
| 按 Esc，点任务栏搜索框，打 `er` | 搜索框出现，框里还是上一次的字，`er` 没进去 | 框里是 `er`，有结果 |

同一场景连做两轮，结果相同。之前活动窗口是别的程序（VS Code）时，0.25.0 也正常。

0.25.0 的日志：

```text
TypeToSearch: keyboard Raw Input set aside
Could not bring the window to the front over Shell_TrayWnd (explorer.exe)
Launcher: active only in Win顺, another program's window in front: as if it lost the focus
Launcher: lost the front 230 ms after it was asked for, to Shell_TrayWnd (explorer.exe) - no input since - it goes
...
Could not bring the window to the front over Shell_TrayWnd (explorer.exe)      （点任务栏搜索框）
```

修复后：

```text
Could not bring the window to the front over Shell_TrayWnd (explorer.exe)
Launcher: refused the front, its keys routed to it while explorer.exe keeps the focus
```

## 原因和改法

开始菜单为搜索框关掉后，Windows 把前台还给之前的窗口；那是任务栏时，以管理员身份运行的 Win顺 从此在任务栏上拿不到前台。试过的三种办法都被拒绝：补发一次空输入（原有做法）、`LockSetForegroundWindow(LSFW_UNLOCK)`（返回失败，错误 5）、补发一次 Alt。为什么被拒没有查明。

拿不到前台时本来有后备：按键经 Win顺 自己的钩子直接送进搜索框（#203）。但任务栏被排除在外（当初认为“按在搜索框上，前台自己会回来”），于是什么都没有接手。现在任务栏和别的窗口一样处理（`App::startLauncherRouting`）；之后真的到了前台，转交自己结束。

## 没有覆盖的

- 按键转交期间输入法不参与（#203 的既有限制）：这种状态下打的是字母，不能直接拼中文。
- 只测了鼠标点开始按钮；Win 键打开开始菜单的那一组没有单独跑。
- 没有在不带管理员权限的 Win顺 上跑同一场景做对照（报告人的条件是管理员）。
- 没有自动化测试：这条路径要真实的开始菜单和任务栏。截图含本机其他窗口的内容，没有入库。

## 顺带看到的（未处理）

以管理员身份启动时，任务栏搜索框大约一半的启动里建不出来：一分钟后日志写 `Taskbar box: no taskbar: its window not made in the taskbar (error 5)`，期间没有任何提示。11 次启动里 6 次如此，和上一次退出的间隔没有明显关系。
