# #452 全屏游戏上打开剪贴板，面板一闪就收

验证日期 2026-11-25，Hyper-V 虚拟机 Win10-WinShun（Windows 10 22H2，1024×768，无服务，快捷键 `Ctrl+Alt+V`）。

## 方法

- 游戏用替身程序 `fsgame.cpp`（本目录）代替：铺满显示器的窗口，先向 DXGI 申请独占全屏；有别的程序的置顶窗口显示在它上面时退出全屏并自己最小化，全过程写日志。
- 虚拟机没有真显卡，`SetFullscreenState` 返回 `0x887A0022`（当前不可用），所以实际走的是“铺满屏幕 → 被盖住后自己最小化”这条等效路径；真正的独占全屏模式切换没有测到。
- 步骤：启动 Win顺 → 启动替身并点一下让它到前台 → 按 `Ctrl+Alt+V` → 2.5 s 后截图 → 按 `Enter` 或 `Esc` → 读替身日志与 `events.jsonl`。键鼠由 Hyper-V 合成设备发出。

## 结果

| 情况 | 版本 | 面板 | 诊断事件 | 游戏 |
|---|---|---|---|---|
| 打开 | 修复前（来宾已装版本） | 47 ms 后收起（`before.png`） | `clipboard-hidden` `why=routed-front-moved`、`target_minimized=true` | 最小化，停在桌面 |
| 打开 | 修复后 | 留在屏幕上并拿到焦点（`after.png`） | `clipboard-kept` `front=true`，无 `clipboard-hidden` | 最小化 |
| 打开后按 `Enter` | 修复后 | 收起 | `clipboard-hidden` `why=asked` | 还原到前台并收到 `Ctrl+V`（`WM_KEYDOWN 86`、`WM_CHAR 22`），之后不再最小化 |
| 打开后按 `Esc` | 修复后 | 收起 | `clipboard-hidden` `why=asked` | 还原到前台 |

单元测试：`tst_core` 的 `panelOverLeftFullScreen` 通过（判定规则：全屏、已最小化、无按键、3 s 内）。

## 没有测到的

- 真实游戏与真正的独占全屏（需要真显卡）。反馈者的诊断包与这里的事件顺序一致：按键转发开始后约 430 ms 即结束，期间零按键。
- 打开后 3 s 内按 `Alt+Tab` / `Win+D` 切走的情况只由单元测试覆盖。

## Windows 10 上仍然“一闪即没”（0.48.0 反馈）

验证日期 2026-11-26，本机 Windows 11（26200，真显卡，2560×1600）。

### 诊断包说明了什么

反馈者同一版本在两台机器上的 `events.jsonl`：

| | Windows 10 22H2 | Windows 11 26H2 |
|---|---|---|
| 面板显示后游戏是否退出全屏 | 没有：2 s 内无 `screens` 变化，`target_minimized=false` | 约 0.4 s 后退出并最小化，`clipboard-kept` `front=true` |
| 面板 | 已显示但被游戏画面盖住，按键仍被转发 | 留在屏幕上，可粘贴 |

即：不抢焦点的置顶窗口在 Windows 10 上不会让独占全屏的游戏让出屏幕。

### 改动

前台是独占全屏的程序时，面板不走按键转发，直接拿焦点打开（`panelTakesFocus`），记 `clipboard-over-exclusive`；粘贴、`Esc` 或再按一次快捷键把游戏带回前台，游戏没有最小化时也一样。

### 方法

`run-game-check.ps1`（本目录）：编译替身 `fsgame.cpp` 并启动（DXGI 全屏，`SHQueryUserNotificationState` 报告 D3D 全屏），再运行 `WinShun.exe --probe-runtime <目录> --full-screen-game-check <替身窗口句柄>`。探针用独立配置，真实按下剪贴板快捷键（`SendInput`），先 `Esc` 一轮、再 `Enter` 一轮，结果写入 `full-screen-game.json`；系统剪贴板事后原样放回。

### 结果（两次运行一致）

| 检查 | `Esc` 轮 | `Enter` 轮 |
|---|---|---|
| 按键前替身在前台且系统报告独占全屏 | 是 | 是 |
| 面板拿到焦点 | 是，约 50 ms | 是，约 50 ms |
| 2.5 s 后面板仍在前台、未转发按键 | 是 | 是 |
| 按键后面板收起、替身回到前台 | 是 | 是 |
| 替身日志 | `WM_ACTIVATE 0` → `WM_ACTIVATE 1` | 回到前台后收到 `WM_KEYDOWN 17`、`WM_KEYDOWN 86`、`WM_CHAR 22`（`Ctrl+V`） |

单元测试：`tst_core` 的 `panelFocusOverExclusive`、`panelOverLeftFullScreen` 通过。

### 没有测到的

- 带分辨率切换的真正独占全屏：本机上替身失去焦点后窗口仍留在原地、系统仍报告独占全屏（`game_left_exclusive=false`），没有发生模式切换；真实游戏失去焦点后退出全屏、面板随后是否稳定留在前台，要等反馈者用新版本确认。
- Windows 10 实机：虚拟机没有真显卡，系统不报告独占全屏，走不到这条路径。
- 整份 `tst_core` 在这个工作树里另有两条与本次无关的用例失败（`relocatedPersonalFoldersKeepPriority` 单独运行也失败，`directoryLinksToIndexedDisk` 单独运行通过；主构建目录下两条都通过），未在本次处理。
