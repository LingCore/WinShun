# #266 #270 #274 验证记录（2026-11-10）

构建：`build/issues-249-253-256-20261108`（Ninja Release，增量），Windows 11 26200。只跑了改动涉及的测试。

## #274 便携版升级被退回旧版

- **原因的证据**：报告人诊断包 `events.jsonl` 里 4 次升级都是同一个样子：0.22.0 以管理员身份启动（`rights.elevated: true`），约 10 秒后 0.21.0 以普通权限启动。只有“管理员直读”在手动启动时提权（`startupaccess::needsElevation`，0.22.0 的 7609c2f）会让普通权限启动的新版本变成管理员身份。
- **实测修复**（`scratchpad/i274/update-test.ps1`）：旧版本 = `dist/release022/WinShun`（0.22.0 便携版，`data` 里 `Index/Access=direct`），运行 **0.22.0 自带的** `WinShunUpdate.exe --portable`，暂存区里换成新构建的 WinShun.exe。测试脚本自己也持有 `Local\WinShun.Updating`，让新版本一直以为升级没完，最后一步不弹 UAC。结果：

  ```
  helper exit code: 0
  swapped in 41 entries, backed up 41
  started ...\upd\WinShun\WinShun.exe (pid 35736)
  done
  running from the test folder: 1 pid=35736
  exe in place is the new one: True
  stage left: False; backup left: False
  app-start version=0.22.0 elevated=False
  autostart task unchanged: True（右键菜单注册表已恢复）
  ```

- **完整流程实测**（同日补测，`update-test-full.ps1`）：测试脚本不再替它占着互斥量，最后一步真的提权（这台机器 `ConsentPromptBehaviorAdmin=0`，提权不弹框）。新版本先以普通权限启动，助手结束后 1 秒内记下 `elevation-after-update`，再以管理员身份启动，没有退回旧版：

  ```
  helper exit code: 0
  swapped in 41 entries, backed up 41 / started ... (pid 13396) / done
  elevated start seen 3 s after the helper ended
  exe in place is the new one: True
  stage left: False; backup left: False
  03:22:53 app-start version=0.22.0 elevated=False
  03:22:54 elevation-after-update {"helper_still_running":false,"restart":true}
  03:22:57 app-start version=0.22.0 elevated=True
  autostart task unchanged: True（右键菜单注册表已恢复）
  ```

  普通权限那一份让位退出时，正在读的索引被中途停下，诊断日志里留一条 `cannot read the MFT of C:: stopped`，界面上什么也不显示。
- **没测**：默认 UAC 设置下的确认框（会多一次点击，流程相同）；从 0.20/0.21 的助手升级（助手从 0.16 起就持有同一个互斥量，代码相同）；Windows 10。

## #270 卸载后 Win+V 失灵

- `installer/WinShun.iss` 用 Inno Setup 6.7 编译通过（`/DSourceDir=dist/release022/WinShun`，产物已删）。`--restart-explorer` 与 `--uninstall-cleanup` 的改动随 WinShun 编译通过。
- 本机当时开着一个文件夹窗口（`CabinetWClass visible=True`），确认了判断要看的窗口类和可见性。
- **实测**（同日补测，`scripts/` 里的 `i270-gen.py`、`harness.ps1`、`guard.ps1`、`Diff.cs`）：不装正式安装包（同一个 AppId 会换掉、再卸掉 Program Files 里那一份，还会动自启动、PATH）。`i270-gen.py` 从 `installer/WinShun.iss` **原样抽出** `GiveBackKey`、`RestartExplorerFor`、卸载时那段调用和两条 `RestartExplorer` 提示文字，编成一个独立 AppId、要管理员权限的小安装包，`{app}` 是放了新 WinShun.exe 的 0.22.0 便携版副本。测试脚本以管理员身份运行（和为所有用户安装时的卸载程序一样）：真卸载、真重启资源管理器、用 `SendInput` 真按 Win+V。

  ```
  P0 没接管时 Win+V 弹出面板                          PASS
  P1 DisabledHotkeys=V 并重启资源管理器后 Win+V 没反应  PASS
  A  静默卸载，没开文件夹窗口：两个注册表值删掉、资源管理器重启、
     新的资源管理器是普通权限、Win+V 弹出面板              PASS ×4
  B  普通卸载，开着文件夹窗口：先问
     “Win+V 已还给 Windows，但要重启资源管理器才能用。…”，点“是”后重启、
     普通权限、Win+V 弹出面板                              PASS ×5
  C  静默卸载，开着文件夹窗口：键还了，资源管理器不动，
     Win+V 仍没反应（就是 #270 的样子）；手动 --restart-explorer 后弹出  PASS ×5
  ```

  测试结束时 `DisabledHotkeys`、`HKCU\Software\WinShun\ExplorerHotkeys` 都和开始时一样（都不存在），资源管理器是普通权限，测试过程没复制任何内容。
- **测试本身的坑**：前台是管理员身份的窗口时，Windows 的 Win+V 面板不弹出（第一轮测试窗口由提权脚本自己开，P0 就判成“没弹出”，整轮作废重跑）；面板不抢前台，`EnumWindows` 也看不出多了窗口，只能比较按键前后的截图（面板一大块变了：338–342 个 32 px 格子，噪声 0–4 个）；按 Esc 给前台窗口关不掉它，前台窗口关掉或资源管理器重启（`TextInputHost` 跟着重启）时它才消失。提权进程运行 `explorer.exe <文件夹>` 开出的文件夹窗口在单独的 explorer 进程里，重启桌面那个进程时它不关。
- **没测**：Scoop 卸载（`--uninstall-cleanup` 只是换了调用方，判断窗口用的是同一组类名）；Windows 10。

## #266 关了“包括 U 盘和移动硬盘”仍占用 U 盘

- 报告人的截图：火绒“U 盘保护”列出 `WinShun.exe` 占用 `G:`（卷本身），解除失败；关开关、重启 Win顺 都无效，符合“这个 U 盘对 Windows 说自己是固定磁盘”。
- `tst_core externalDisksFollowTheRemovableSwitch selectedDrivesRespectAnEmptySelection`：通过。本机 NVMe 上的 C: D: E: F: 都判为自带（`C: own D: own E: own F: own`）；`WINSHUN_EXTERNAL` 标成外接后，开关关时不列出、开时列出；系统盘不用测试开关时永远不算外接。
- **没测**：真实的 USB 移动硬盘 / 固态 U 盘（手头没有）、火绒的弹出。
