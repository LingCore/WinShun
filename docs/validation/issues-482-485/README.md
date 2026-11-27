# #482 #483 #484 #485（2026-11-27）

| Issue | 改动 | 验证 |
|---|---|---|
| #485 演示：黑板变白板 | `PresenterOverlay.qml` 记“上一块板”的颜色时读了还没更新的 `onBoard`，从桌面直接开黑板取到旧值，留在白色 | `WinShunCapture.exe --preview-presenter`：修复前 `FAIL: the black board from the desktop is black: #ffffffff`，修复后三项颜色检查全部 PASS |
| #483 大图没有排序入口 | 面板大图模式下底栏显示排序芯片，控制并显示面板的排序（`Launcher::panelSortChoice` / `choosePanelSort`） | `WinShun.exe --preview-results <目录> --preview-hotkeys --renderer d3d11`：真实点击第二个芯片、`Ctrl+S`、菜单里的“大的在前”、列头给出的“名称倒序”、切回列表后芯片隐藏，0 条 regression；`483-gallery-order.png` |
| #482 预览里命中的词不显眼 | 命中的词垫底色加粗，第一处所在的行整行衬出 | 同一次运行：代码里 1181 px、JSON 里 1092 px 底色，没搜词时 0 px，行带只在第一处所在行；`482-preview-match.png` |
| #484 便携版在系统盘的文件 | 便携版的更新下载、助手副本、日志改放 `data\Updates`；旧版留在 `%LOCALAPPDATA%\WinShun\Updates` 的文件启动时清掉 | `ctest -R ^tst_updater$` 通过：`updatesFolderByKind`、`strayUpdatesOfAPortableCopy`（含旁边有别的文件、里面有文件夹、`Updates` 是联接三种不删的情况）、`helperStartsNewVersion(from-the-data-folder)`（真实 `WinShunUpdate.exe` 从 `data\Updates` 里运行并完成交换） |

未跑：`tools/verify-installed-upgrade.ps1 -Mode Portable`（真实便携包整套在线更新）。它会退出这台电脑上正在运行的 Win顺，留到发版时征得同意后跑。

索引服务的程序（`%ProgramFiles%\WinShun Service`）和日志（`%ProgramData%\WinShun`）不搬：以 SYSTEM 运行的服务不能放在普通程序能写的文件夹里。
