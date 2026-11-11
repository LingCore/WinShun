# 勾了“以管理员身份运行”的 Win顺：拖放圆盘实测（2026-11-11）

GitHub #111 里一位用户的日志显示拖放圆盘的助手一直起不来（错误 740）。修复见提交 a5b257f，原因和做法见 `docs/pitfalls.md`。

## 怎么测的

`harness/pie-runasadmin.ps1`：暂存一份 Win顺（0.23.0 的部署目录，换上被测的程序），在 HKCU 里给它的 `WinShun.exe` 加 `RUNASADMIN`，像双击那样启动（于是以管理员身份运行），自己的配置目录，收藏里只有一个文件夹。然后用真实输入：在单独的资源管理器窗口里按住一个文件，按 Esc 下面的键，指针上移到圆盘第一格，松开。

通过的标准：助手进程在运行且不是管理员权限，文件被移进收藏的文件夹。

## 结果

| 程序 | 助手 | 拖放 | 记录 |
| --- | --- | --- | --- |
| 0.23.0（修复前） | 没有启动，日志 `not started: CreateProcessWithTokenW, error 740`，和用户日志一样 | 没有圆盘，文件没动 | `pie-before.txt` |
| 修复后（a5b257f） | 启动，非管理员权限，父进程是管理员权限的 Win顺 | 圆盘出现，文件移进收藏文件夹 | `pie-fixed.txt`、`pie-fixed-over-chip.jpg` |

另有 `tst_shelllaunch::startsOurOwnProgramSetToRunAsAdministrator`，普通权限和管理员权限各跑一次，都通过。

## 没测的

- Windows 10（那位用户的系统）。
- 同一份带标记的 Win顺 里“以普通权限重启”。
- 本机 UAC 不弹确认；会弹确认的机器上，带标记的 Win顺 每次启动本来就要确认一次，这一点和修复无关。
