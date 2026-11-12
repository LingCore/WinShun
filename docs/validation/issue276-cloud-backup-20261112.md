# 云备份插件（GitHub #276）验证记录

日期：2026-11-12。构建：`build/cloud-backup`（Release，MSVC，Qt 6.12.0），源码在工作树 `.worktrees/cloud-backup`。

## 做了什么

- 插件库新插件 `winshun.cloudbackup` 1.0.0（`src/cloudbackup-plugin`）：把设置备份传到 WebDAV 文件夹或 GitHub 私有仓库，两处可同时用；`cloud` 立即备份、列出云端备份、恢复。
- 主程序：新权限 `winshun:backup`，工具 `backup_export`、`backup_restore`（`Backup::exportTool` / `restoreTool`）。
- C++ SDK：`tool()` 可以从 `serve()` 以外的线程调用。

## 自动测试（只跑了相关的）

| 测试 | 结果 | 覆盖 |
|---|---|---|
| `tst_cloudbackup` | 10 通过 | 备份文件名、base64 与网址编码、仓库写法、PROPFIND 解析；WebDAV 与 GitHub 两种目标对本地服务器的列出、上传、下载、删除和各种报错；插件进程全流程（两处同时备份、没变不重传、同一秒两份不重名、只留 N 份、云端被删后补传、恢复先确认、重启后记得传过什么、公开仓库不上传、等宿主回答时收到 shutdown 能按时退出） |
| `tst_backupservice` | 5 通过 | `backup_export` / `backup_restore` 的实现：导出与“导出备份”一致且不含本机备份位置、哈希相同不写文件、只认插件数据文件夹里的文件（外面的、`..`、相对路径都拒绝）、不是备份的文件报错且不重启、恢复前先在本机留底、一次只做一个 |
| `tst_plugin` | 222 通过，1 跳过（原有） | 新增：`winshun:backup` 的名字与解析、随包清单能通过校验 |
| `tst_backup`、`tst_translate`、`tst_bookmarks`、`tst_semantic`、`tst_localsend`、`tst_break`、`tst_tools`、`tst_aisettings` | 全部通过 | SDK 与备份代码的改动没有影响它们 |

## 实际运行

- **真实 WebDAV 服务器**（wsgidav 4.4.0，本机，Basic 认证）：地址含中文和空格的文件夹、密码含空格和中文；末级文件夹自动建出；连传三份后服务器上剩两份（保留数设为 2）；列表行带时间、电脑名和大小；下载回来的文件与上传的逐字节相同；密码填错时状态行写“WebDAV 的用户名或密码不对”。全部通过。
- **真实 GitHub 接口**（只读）：公开仓库 `LingCore/WinShun` 被拒绝并说明原因；不存在的仓库、错误的令牌各自报对了原因。
- **真实 GitHub 私有仓库**（临时建的测试仓库，贴的是仓库网址）：空仓库里第一次上传成功；连传三份后仓库里剩两份（保留数设为 2，最旧的一份被删掉）；仓库里最新的文件与上传的逐字节相同；设置没变时不再上传；列表两行带时间（同一分钟的显示到秒）、电脑名和大小；下载回来交给恢复的文件与最新备份逐字节相同。全部通过。
- **经真实运行器启动**（`wsbench --plugin`）：清单校验通过，进程在作业里以普通权限运行，冷启动约 1 秒，之后每次查询约 0.2 毫秒。

## 没做的

- **没有在真实的 Win顺 界面里走一遍**（设置页的权限确认行、搜索框里按 Enter、恢复后重启）。同一时间只能运行一个 Win顺，做这个要先退出正在用的那个；这部分由上面的自动测试分层覆盖，发布前按发布流程实测。
- 没有对坚果云、Nextcloud、群晖等具体服务逐个实测。
