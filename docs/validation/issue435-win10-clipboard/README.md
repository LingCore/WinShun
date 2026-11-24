# #435 Win10 上“局域网传输”看不到刚复制的文件

日期：2026-11-24　机器：Hyper-V 虚拟机 Windows 10 22H2（19045）与本机 Windows 11（26200），本地构建 `build/issues-1124b`。

## 原因

插件在沙盒里运行：每个插件进一个作业对象（job），按清单里的权限加上界面限制（`JOB_OBJECT_UILIMIT_*`）。局域网传输有 `clipboard:read`，作业不禁读剪贴板，只加了“不能改系统参数、显示设置、注销、切桌面、写剪贴板”这类限制。

在 Windows 10 上，**作业只要带任何一项界面限制**，作业里的进程就拿不到别的程序放进剪贴板的数据：`IsClipboardFormatAvailable` 说有，`GetClipboardData` 却返回空、错误 6（`ERROR_INVALID_HANDLE`），资源管理器复制的文件、普通文字都一样。Windows 11 只有加了 `UILIMIT_HANDLES` 才这样，所以本机上一直正常。

[jobclip.cpp](jobclip.cpp) 单独验证：父进程把剪贴板设成按需渲染的 CF_HDROP（带不进历史的标记），子进程放进带各项限制的作业里读。

| 作业的界面限制 | Win10 19045 | Win11 26200 |
|---|---|---|
| 无 | 读到 | 读到 |
| 局域网传输实际的一组（0xdc） | 错误 6 | 读到 |
| 任意单项（写剪贴板、桌面、系统参数、显示设置、注销） | 错误 6 | 读到 |
| `HANDLES`（0x01） | 错误 6 | 错误 6 |
| 数据直接放进剪贴板（不按需渲染），0xdc | 错误 6 | — |

虚拟机里在插件里临时加日志，复现时记下：`hdrop available 1` → `hdrop null 6`。

## 改法

- `sandbox::uiRestrictions(limits, build)`：Windows 11 24H2（26100）之前，允许读剪贴板的插件不加界面限制；其余（只能有一个进程、内存上限、随 Win顺 结束、缓解策略、令牌）照旧。19045 实测不行、26200 实测可以，中间的版本没测，按“剪贴板能用”的一侧处理。
- 局域网传输 1.0.9：剪贴板里列着文件或文字却读不出来时，在插件日志里记一行 `clipboard: files listed but not readable (error N)`，以后同类问题看日志就知道。
- 顺带发现：浏览器书签插件的“浏览历史”（`bh`）在 2.1.0 也加了 `ls`，和局域网传输的 `ls` 撞了，两个都装时 `ls` 谁也打不开。书签 2.2.1 去掉 `ls`（`bh` 不变）；新增测试保证官方插件的关键词互不相同。

## 验证

| 项目 | 结果 |
|---|---|
| Win10 虚拟机，修复前（同一构建的旧 runner + 插件 1.0.8）：资源管理器里复制文件，`ls ` | [before.jpg](before.jpg)：只有“接收已关闭”“正在找附近的设备”，没有要发送的文件 |
| Win10 虚拟机，修复后（新 runner + 插件 1.0.9），同样步骤 | [after.jpg](after.jpg)：第一行“要发送：hashme.txt · 共 1 个文件 · 3 B · 剪贴板里复制的”；插件日志无警告 |
| `tst_plugin uiRestrictionsLeaveTheClipboardReadable` | 通过（各版本号、读/写/ui 组合的限制） |
| `tst_plugin clipboardReaderReadsAnotherProgramsFiles` | 通过：经真实 host 和 runner 起的 `clipboard:read` 插件读到测试窗口按需渲染的 CF_HDROP，渲染 1 次；作业限制与 `uiRestrictions` 一致 |
| `tst_plugin officialKeywordsDistinct`、`manifestListenAndNotifications`、`jobLimitsUnelevatedToo` | 通过 |

测试写进剪贴板的内容都带 `ExcludeClipboardContentFromMonitorProcessing` / `CanIncludeInClipboardHistory=0`，结束后放回原来的文字。
