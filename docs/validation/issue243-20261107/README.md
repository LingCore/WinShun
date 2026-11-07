# #243 管理员窗口在前时的单键热键（2026-11-07）

GitHub #243：截图快捷键设成 F1 时，以管理员身份运行的程序在前面按了没反应；系统错误框（缺运行库、找不到 DLL）在前面时截不了。

改法：索引服务的 UIAccess 按键助手（`--listen-keys`）在前台是更高完整性级别的窗口时，替 Win顺 自己的界面接住不带修饰键的 F1–F24、PrintScreen，Win顺 再向注册这个键的窗口补发 `WM_HOTKEY`。设计和安全限制见 `docs/pitfalls.md`“单键全局热键在管理员窗口前：由索引服务的按键助手转发”。

这里只放文字日志，没有桌面截图（仓库公开）。按键都是 System 完整性的发送端用 `SendInput` 注入的 F21（0x84），没测真键盘。

## 单元测试

只跑这次改动涉及的两组（Qt 6.12.0，MSVC，`build/issue243/build`）：

- `tst_keylistener.txt`：15 通过，4 跳过（跳过的是要隔离服务的现场测试和需要手动开启的桌面测试）。新增 `workerRelaysPlainKeysOnly`、`plainKeysRegistry`、`plainHotkeyFromTheListener`；`ordinaryServiceRefusesWorker` 还查非自家服务不给 `kCapabilityHotkeyRelay`。
- `tst_pluginhotkeys.txt`：8 通过。

## 便携版一键更新索引服务

转发在服务里，便携版换新版本后服务还是旧的。以前设置里装好服务后只有“卸载”按钮，要先卸载再装。现在便携版或 Scoop 版、服务是这一份装的、版本比自己旧时，设置那一行显示“更新索引服务”，每个版本弹一次通知，点了直接 `--install`（UAC 一次，原地升级）。

- `tst_serviceupdate.txt`：11 通过（版本解析；自己的旧服务提示；安装版、不能装、别的副本的服务、同版本、服务更新、服务没回话、版本读不出时都不提示）。
- `tst_settings.txt`：54 通过（新增设置项 `Launcher/ServiceUpdateHinted`）。
- 没有在这台电脑上真跑 `--install` 升级：这里的服务是安装版装的，从便携版装会把它换掉。通知和按钮也没有现场看过（需要“服务是这份便携版装的”这种状态）。

## 现场转发测试 `live-relay/`

`harness/live-relay.ps1`（提权）起两个隔离的 SYSTEM 控制台服务：A 带 `--installed-from tst_keylistener.exe`（算自家界面），B 不带；一个 High 窗口（`relaysend window`）；System 完整性的发送端（`hkprobe sys-launch relaysend send`）。测试本体是 `tst_keylistener elevatedForegroundPlainHotkey`。

- `test.txt`：`elevatedForegroundPlainHotkey` 通过。
- `timeline.txt`：管理员窗口在前 3 次都转发（`relayed vk=84` 后约 7 ms `WM_HOTKEY`，`front=1` 即 Win顺 拿到了前台）；第 4 次是测试自己的窗口在前，只有自己的 `WM_HOTKEY`，没有转发、没有重复；第 5 次是 csrss 的系统错误框在前，同样转发。
- `window.log`：管理员窗口只有 `window up`，一个键事件也没收到（按下、抬起都被吞了）。
- 服务 B（不是自家界面）：握手不给转发能力位，带按键清单打开监听被拒（`ERROR_ACCESS_DENIED`），在测试里断言。

## 真截图插件 `e2e-before/` 和 `e2e-after/`

`harness/run-e2e.ps1`（不提权）：占 `F:\wswt\ui.lock`，退出用户的 Win顺，用 `--profile dist\issue243\profile` 运行暂存的新版 `WinShun.exe`（截图插件快捷键设为 F21），连隔离的 SYSTEM 服务（`harness/live-e2e.ps1` 起，`--installed-from` 指向暂存的 WinShun.exe）；结束时退出暂存版，重新打开用户的 Win顺，核对开机自启的计划任务没变。

- `e2e-before/`（没有转发的基线：这次服务没认出自家界面）：管理员窗口在前按 F21，前台还是管理员窗口，`capture=0`，没有截图层；管理员窗口收到了 F21 的抬起（`window got up vk=0x84`），按下被系统吃掉。和用户说的“按了没反应”一致。
- `e2e-after/`：
  - `harness.log`：服务启动的助手命令行 `--listen-keys <pid> <hwnd> 132`（132 = F21）。
  - `sender.log`：管理员窗口在前按 F21，前台变成截图插件的“截图 - Win顺”（`capture=1`），Z 序里截图层在管理员窗口上面；按 Esc 后前台回到管理员窗口。csrss 错误框在前按 F21，截图层同样出来并拿到前台，但 Z 序里错误框（`z2`，System 完整性、置顶）仍在截图层（`z3`）上面；Esc 后前台回到错误框。
  - `window.log`：管理员窗口一个键事件也没收到。
  - `winshun-log-lines.txt`：暂存版日志里两次“Plain hotkey vk 0x84 over an administrator's window, from the service's listener”。第一行“Hotkeys with no modifier…”是之前基线那次运行（服务没认出自家界面）留下的。
  - `run.log`：用户的 Win顺 退出后重新打开，开机自启任务没变，锁已释放。

## 结论

- 管理员程序在前时，单键 F1–F24、PrintScreen 快捷键（Win顺 自己的和插件的）在装了新版索引服务时能用，这个键也不会再传给管理员程序。
- 系统错误框在前时快捷键能截图，冻结的画面里有它，但它盖在截图层上面。要盖住它得把截图层放进 UIAccess 层级，这次没做。
- 没有索引服务、或服务是旧版时，行为同以前（日志记一次原因）；以管理员身份运行 Win顺 时本来就没这个问题。

## 复现

`harness/` 里是这次的探针源码：`relaysend.cpp`（High 窗口、发送端、端到端记录前台和 Z 序）、`hkprobe.cpp`（`sys-launch`：从 SYSTEM 计划任务把 System 完整性的进程放到当前桌面）、`hkpmissing.c`（引用不存在的 DLL 函数，用来弹出 csrss 的“系统错误”框）。脚本里的路径是这台电脑上的 `build/issue243/`。
