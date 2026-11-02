# #172 / #183 / #150：收藏圆盘辅助进程验证（fx172，2026-11-02）

Windows 11 Pro 26200，Qt 6.12.0，Release。分支 `fx172`，提交 `aa4c9c8`。所有实测都用 `E:\wsi12-stage\fx172` 暂存副本和 `E:\wsi12-test\fx172` 下的临时配置、测试文件；没有动安装版、计划任务和用户配置（运行后核对：三个 SearchHere 键、`\WinShun` 任务仍指向 Program Files）。

## 已证实的问题

| 问题 | 证据 |
| --- | --- |
| 临时配置写入偶发失败（“无法创建圆盘的临时配置”，圆盘收藏不更新） | 基线 `--drop-pie-check` 13 次失败 4 次。加日志后看到 `QSaveFile::commit` 返回错误 5（拒绝访问）：另一进程正打开该文件读取时，替换会被拒绝。主进程写 `favorites.json`、辅助进程写 `ready` 两边都出现。 |
| 外观设置从未传到辅助进程 | 主进程写 `<临时目录>\WinShun.ini`，辅助进程按 `Settings::useProfile` 读 `<临时目录>\WinShun\WinShun.ini`。 |
| 只启动一次，失败后整个会话不再尝试 | 代码：`update()` 启动一次；没有桌面外壳、启动被拒、进程退出、5 秒无响应，都只弹一条“请确认资源管理器正在运行”的通知，此后不再重启。 |
| UAC 关闭（或内置 Administrator）时辅助进程必然退出 | 代码：辅助进程拿资源管理器的令牌启动，UAC 关闭时该令牌本身已提权，而辅助进程见到“已提权”就以代码 2 退出。本机无法关 UAC，未实测。 |

## 自动测试

- `tst_droppiehelper`（新，ctest）：真实辅助进程启动、收藏 2→3、外观（`theme=dark`）传到、被结束后自动重启、停止。修复后 24 次连续运行收藏和外观同步全部通过；重启耗时 1.8–9 s（机器负载高时）。
- `tst_shelllaunch`：新增 `startUnelevatedSaysWhyNot`（失败步骤和错误码、引号路径、正常启动退出码）。普通权限全套 13 通过；管理员权限下该用例和 `capturesWhatAProgramWrites` 通过。

## 实测（暂存副本，真实输入）

管理员启动按开机自启的方式（`--profile <dir> --background`，经 `-Verb RunAs`）。手势：提权测试脚本在测试资源管理器窗口里对 `drag-me.txt` 按住左键、拖动几像素、按 `` ` ``（VK_OEM_3），再 Esc、松开左键；每次按键前检查前台窗口，点击前检查 `WindowFromPoint`，结束后 `GetAsyncKeyState(1)` 无按下。

| 场景 | 结果 |
| --- | --- |
| A 管理员，`WINSHUN_TEST_PIE_NO_SHELL=3`（前 3 次找不到桌面外壳） | 7.6 s 后辅助进程就绪（3×2 s 等待 + 启动），普通权限；日志无失败、无通知。结束辅助进程后 1.8 s 新进程就绪，日志 `exit 0xFFFFFFFF (1 in a row)` → `running, after 1 failed tries`。 |
| B 管理员，`WINSHUN_TEST_NO_SECLOGON=1`（模拟 Secondary Logon 被关） | 改用资源管理器为父进程启动：4.1 s 就绪，父进程 explorer.exe，普通权限；真实手势 66 ms 弹出圆盘（423×309）。 |
| C 普通权限（#150 的方式） | 真实手势 46 ms 弹出圆盘；日志 `1 files over it (read from the ID list)`，Esc 后收起。 |
| D 便携版（暂存目录带 `data`，管理员，无 `--profile`） | 4.9 s 就绪，普通权限；真实手势 112 ms 弹出圆盘。 |
| E 管理员，正常路径 | 真实手势 41 ms 弹出圆盘。 |

测试文件始终留在原处，没有移动。

## 未证实

- 两位报告者具体落在哪个分支：没有他们的诊断包。新版诊断包的 `drop_pie_helper` 带 `failures`、`last_failure`（步骤 + 错误码或退出码），日志有同样内容。
- UAC 关闭的路径只按代码推理，未实测；Secondary Logon 回退只用测试开关模拟，未真关服务。
- #150：普通权限路径在本机用真实手势可以弹出，和 10-31 的结果一致；报告者环境里不弹的原因仍未知。
