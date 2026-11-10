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

- **没测**：助手退出后 `App::elevateOnceUpdated` 的提权重启（会弹 UAC，要人点）；从 0.20/0.21 的助手升级（助手从 0.16 起就持有同一个互斥量，代码相同）；Windows 10。

## #270 卸载后 Win+V 失灵

- `installer/WinShun.iss` 用 Inno Setup 6.7 编译通过（`/DSourceDir=dist/release022/WinShun`，产物已删）。`--restart-explorer` 与 `--uninstall-cleanup` 的改动随 WinShun 编译通过。
- 本机当时开着一个文件夹窗口（`CabinetWClass visible=True`），确认了判断要看的窗口类和可见性。
- **没测**：真正的卸载和资源管理器重启（会关掉用户开着的文件夹窗口）；静默卸载；Scoop 卸载。

## #266 关了“包括 U 盘和移动硬盘”仍占用 U 盘

- 报告人的截图：火绒“U 盘保护”列出 `WinShun.exe` 占用 `G:`（卷本身），解除失败；关开关、重启 Win顺 都无效，符合“这个 U 盘对 Windows 说自己是固定磁盘”。
- `tst_core externalDisksFollowTheRemovableSwitch selectedDrivesRespectAnEmptySelection`：通过。本机 NVMe 上的 C: D: E: F: 都判为自带（`C: own D: own E: own F: own`）；`WINSHUN_EXTERNAL` 标成外接后，开关关时不列出、开时列出；系统盘不用测试开关时永远不算外接。
- **没测**：真实的 USB 移动硬盘 / 固态 U 盘（手头没有）、火绒的弹出。
