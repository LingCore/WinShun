# #277 粘贴到以管理员身份运行的程序（2026-11-11）

GitHub #277：剪贴板里选一条，目标是以管理员身份运行的程序时粘贴不上，只复制。

改法：到按粘贴键那一步，目标窗口比 Win顺 权限高就请索引服务代按。服务启动 UIAccess 助手 `WinShunService.exe --paste-keys <hwnd>`，它只按一次 Ctrl+V（mintty、PuTTY 是 Shift+Insert），只对正在最前面的管理员窗口，只认 Win顺 自己的界面。设置 › 剪贴板 › “粘贴到以管理员身份运行的程序”，默认开启（用户 2026-11-11 定）。设计、限制和剩下的风险见 `docs/architecture.md`“往管理员程序里粘贴”和 `docs/pitfalls.md` 的同名条目。

这里只有文字日志。Qt 6.12.0 / MSVC Release，构建目录 `build/press-feedback`。只跑了这次改动涉及的测试，没有跑全量 ctest。

## 单元测试

- `tst_servicepaste`（新）：6 通过，1 跳过（跳过的是下面的现场部分，不带环境变量时不跑）。按窗口类名选键、助手发出的那组键（4 个，都带 Win顺 自己的标记）、结果名、没有服务时 0.5 秒内回“没有服务”。
- `unit-totals.txt`：`tst_numberhints` 12 通过（新设置项默认开、能存能读）、`tst_settings` 55 通过、`tst_aisettings` 158 通过。

## 现场：服务通道 `live/`

`harness/run-live.ps1`（普通权限）启动 `harness/live-paste.ps1`（提权）：两个管理员窗口（一个在前、一个不在前，各自记下收到的键）和一个隔离的 SYSTEM 控制台服务（`--installed-from tst_servicepaste.exe`，把测试程序当成 Win顺 自己的界面）；随后以普通权限运行 `tst_servicepaste` 的 `administratorWindowLive`。全程不写剪贴板。

- `tst_servicepaste.txt`：7 通过。普通权限的窗口被拒（`ERROR_NOT_SUPPORTED`）；不在前面的管理员窗口被拒（`ERROR_NOT_READY`）；在前面的两次都按成，从发请求到服务回话 19 ms 和 23 ms（含启动助手进程）。
- `window.log`：在前面的管理员窗口收到两组 Ctrl 按下、V 按下、V 抬起、Ctrl 抬起；不在前面的那个一个键也没收到。
- `tst_service.txt`（需要管理员，由提权部分先跑）：25 通过，含新增的 `pasteForOwnUiOnly`（不是自家界面：握手不给能力位，请求回 `ERROR_ACCESS_DENIED`）和 `pasteRejectsBadRequest`（没有窗口、窗口为 0、多带一个值：`ERROR_INVALID_PARAMETER`）。
- `tst_paster.txt`（要拿前台，同样由提权部分先跑）：6 通过。界面自己的粘贴改用了共用的按键构造（`core/PasteInput.h`），行为不变。

## 现场：整条路 `e2e/`

`harness/run-e2e.ps1`（普通权限）：把已部署的一份 Win顺 复制到 `dist/issue277/WinShun`，换上这次构建的程序；独立配置（剪贴板快捷键 Ctrl+Alt+F11、只索引一个小文件夹、不注册资源管理器右键项）；复制一行测试文字让它记录；`harness/e2e-paste.ps1`（提权）开一个带文本框的管理员窗口，在它前面按快捷键、再按回车。按键由提权的一端发，因为管理员窗口在前时普通进程发的键会被系统丢掉。

- `run.log`：`PASS: pasted into the administrator window`，文本框里是那一行测试文字。测完剪贴板文字放回，资源管理器右键项没变，Windows 自己的剪贴板历史条数前后都是 1。当时用户的 Win顺 没有运行，所以没有退出和重启它。
- `harness.log`：按快捷键前，前台是管理员窗口；按后是 Win顺 的剪贴板面板（管理员窗口在前时面板自己拿焦点）；回车后 134 ms 文本框出现文字，前台回到管理员窗口。
- `journal.txt`：诊断记录 `clipboard-key-routing-skipped`（`higher-integrity`）和 `clipboard-paste`（`method: service`，`outcome: pasted`）。
- `winshun-log-lines.txt`：暂存版当时的几行日志。

第一次跑端到端没通过，原因在测试脚本：提权的 PowerShell 以隐藏方式启动，它显示的第一个窗体实际不可见（`IsWindowVisible` 为假），Win顺 判定“没有可粘贴的窗口”。脚本改为把窗口再显示一次后通过。已记入 `docs/pitfalls.md`。

## 没有测的

- 真键盘、真实的管理员程序（记事本、终端、注册表编辑器等）：这次的目标是测试脚本自己开的窗口，按键是注入的。
- mintty / PuTTY 的 Shift+Insert 只有单元测试，没有以管理员身份运行它们实测。
- 设置里的开关只测了存取和默认值，没有在界面上点过；关闭后的提示走的是原来的代码。
- 没有服务、旧版服务、服务属于另一份 Win顺 时的三句提示：`noServiceIsSaidAtOnce` 覆盖了判断，提示本身没有现场看过。
- 便携版：要先在设置里把索引服务更新到这一版才有这项能力（协议小版本 7）。

## 复现

```
./docs/validation/issue277-20261111/harness/run-live.ps1 -Build build\press-feedback -Run build\issue277\run
./docs/validation/issue277-20261111/harness/run-e2e.ps1 -Build build\press-feedback -Stage dist\<一份已部署的>\WinShun -Root dist\issue277 -Run build\issue277\e2e
```

两个脚本都会弹一次 UAC；结束时自己删掉计划任务和测试服务。`-Root`、`-Run` 用完可以删。
