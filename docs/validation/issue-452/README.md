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
