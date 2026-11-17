# 内存诊断工具验证（2026-11-17）

工具：`tools/memreport`（`WinShunMemReport.exe`）。测试机：Windows 11 25H2（26200），NVMe 固态盘，32 GB 内存，Win顺 0.33.0 安装版正在运行。

| 测试 | 参数 | 结果 |
| --- | --- | --- |
| 普通权限 | `--minutes 3 --trim-at 1 --interval-ms 1000 --no-elevate` | 3:00 结束，生成 447 KB 压缩包。主程序、截图插件、插件宿主的内存图都采集到了；后台服务（SYSTEM）和 conhost 报 `map-failed`（错误 5），符合预期。归还测试：20 MB → 1 MB，60 秒后 5 MB |
| 管理员 | 同上，用 `Start-Process -Verb RunAs` 启动 | 7 个进程的内存图全部采集到，SYSTEM 身份的后台服务也包括在内；`layout_check` 进程表与 `GetProcessMemoryInfo` 吻合（提交 11212 / 11436 KB，工作集 21728 / 22020 KB，两次调用之间的差值）。归还测试：9 MB → 1 MB |
| 提前结束 | `--minutes 5`，25 秒后发送 Ctrl+Break | 0.4 秒内结束，压缩包包含 `00-start` 和 `99-end`，`stop_reason` = `stopped by Ctrl+C` |

逐项核对过的内容：

- 主程序内存图里驻留的私有页合计 19556 KB，进程表的专用工作集是 19576 KB，两者一致。
- 数据文件夹的文件列表与磁盘上一致；`clipboard` 目录只统计了数量和大小，没有列出文件名。
- 设置文件中，`[Proxy]`、`[Mcp]` 等分区的文字值都已替换成 `<removed, N characters>`，数字和开关值保留。路径里的用户文件夹都写成了 `%USERPROFILE%`。
- 事件日志、服务显示名中的中文是正确的 UTF-8。用 Python 往终端打印时显示成乱码，是终端代码页的问题，文件本身没有问题。
- 用 CMake 构建（`/W4`）没有警告，exe 的文件属性显示“Win顺 内存诊断 · WinShun memory report”。

没有测试：

- 直接关闭窗口这条路径。Windows 只给 5 秒时间，工具会在这段时间里写完压缩包；这条路径没有自动化测试。
- 在机械硬盘的电脑上实际运行。
- 在 Windows 10 上运行。用到的 API 都是 Windows 10 已有的，`GetThreadDescription` 是动态加载的。
