# 内存诊断工具（WinShunMemReport.exe）

用户反馈“Win顺 内存占用下不去”而我们这边复现不了时，把这个工具发给用户，让用户运行一次，然后把桌面上生成的压缩包发回来。工具只运行一次，就能收集齐判断原因所需的证据：到底是不是 Win顺 占用了内存，是哪个进程、哪项计数，内存为什么没有归还，占用的是什么内存。

它是一个单独的 exe，静态链接 C 运行库和 miniz，不依赖 Qt，和任何版本的 Win顺 都能一起用，不需要安装。

## 构建与发给用户

```powershell
. .\scripts\devshell.ps1
cmake --build build\<配置> --target WinShunMemReport
```

`WinShunMemReport.exe` 大约 470 KB。GitHub 评论不能直接附带 exe，需要先打包成 zip 再上传。

## 用户怎么用

1. 双击 `WinShunMemReport.exe`，在弹出的 UAC 窗口里点“是”。需要管理员权限才能看到 SYSTEM 身份运行的后台服务，以及完整的内存分布；用户点“否”时，工具以普通权限继续运行，只是少一部分信息。
2. 照常使用电脑，不要关闭窗口（可以最小化）。中途离开电脑也没关系，工具会记录用户的空闲时间。
3. 默认运行 30 分钟。结束后压缩包保存在桌面，资源管理器会自动选中它，文件名为 `WinShun-memory-report-<日期>-<时间>.zip`。
4. 想提前结束就按 Ctrl+C，已经记录的数据照样打包。工具每 5 分钟会重写一次桌面上的压缩包，所以即使窗口被直接关掉，或者电脑重启，最近一次写入的压缩包也还在。

工具会在第 20 分钟让 Win顺 主程序归还一次内存（`EmptyWorkingSet`，和 IdleTrim 闲置时做的事一样）。归还之后，第一次呼出 Win顺 会稍慢一点。

## 为什么运行 30 分钟

- 内容索引器每 15 分钟检查一轮变更，30 分钟能覆盖两轮。
- IdleTrim 用户离开 20 秒后归还内存，之后每 30 秒复查一次，忙的时候每 15 秒复查一次。30 分钟里会有几十次判断。
- 前 20 分钟只观察 Win顺 自然的表现；第 20 分钟做归还测试，再看 10 分钟，判断内存回涨得多快。
- Win顺 自己的诊断日志（`events.jsonl`，最多保留 7 天）会一起打包，所以更早的归还记录也看得到，不需要让工具运行更久。

## 压缩包内容

| 文件 | 内容 |
| --- | --- |
| `report.txt` | 摘要：每个进程专用工作集的最少/中位/最多/最后值，提交量，硬错误页，读盘量，CPU；用户离开期间主程序超过 100 MB 的次数；归还测试，以及测试后 1、5、10 分钟的值；Win顺 自己的 idle-trim 统计（归还次数、等待原因、唤醒后暂缓次数） |
| `samples.jsonl` | 每 2 秒一行：Win顺 各进程（名称以 WinShun 开头的进程及其子进程）的专用工作集、工作集、提交、峰值、线程、句柄、GDI/USER 对象、CPU、读写字节、硬错误页；用户空闲毫秒数、前台窗口（是 Win顺 窗口时记下类名和标题）；系统可用内存、提交、缓存、内核池；PDH 计数器：备用列表、修改列表、空闲列表、分页读写、各物理磁盘的空闲率、队列和吞吐 |
| `top.jsonl` | 每 30 秒：全机专用工作集最大的 25 个进程、提交最大的 15 个进程；这 30 秒里读写盘最多、硬错误页最多的进程；内存压缩；分页文件使用量 |
| `memmaps/<时刻>/<进程>-<pid>.json` | 内存区域图：私有、映射、映像三类的保留量、提交量和驻留量（驻留量分私有和共享）；write-combine（显卡）、guard、no-cache；私有分配按大小分布（1–2 MB 一档主要是文件索引块）；最大的 80 个私有分配；映射的文件和映像，以及各自的驻留量；所有线程的启动位置（模块+偏移）、名称和 CPU 时间。采集时刻：`00-start`、`10-min`、`20-before-trim`、`21-after-trim`、`99-end`；进程的专用工作集或提交比上次采集时涨了 256 MB 时，会额外采集一次 `growth-N`（最多 4 次）；Win顺 在工具启动后才运行时，会采集 `first-seen` |
| `tool-events.jsonl` | 进程出现和退出（路径、命令行、版本、完整性级别），Win顺 可见窗口的变化，归还测试的结果，采样中断（`gap`，电脑睡眠或工具被卡住），采集失败 |
| `system.json` | Windows 版本、CPU、内存、分页文件配置和使用量、Memory Management 和 Prefetch 注册表项；服务状态：SysMain、WSearch、Everything、Win顺 服务；物理磁盘：机械盘还是固态盘、总线类型、型号、容量；各分区所在的磁盘；内核池占用最多的 25 个标记；进程表结构自检（`layout_check`） |
| `system/eventlog.jsonl` | 14 天内的睡眠、唤醒、开关机和意外断电记录；30 天内的内存不足诊断（Resource-Exhaustion-Detector，会列出当时占用最多的进程）；Win顺 的崩溃和无响应记录 |
| `winshun.json`、`winshun-end.json` | 开始和结束时的 Win顺 进程信息，以及数据文件夹（深 3 层）、索引文件夹、`%ProgramData%\WinShun`、程序文件夹的文件列表和大小（`clipboard` 目录只统计数量和总大小，不列文件名） |
| `app/` | Win顺 的诊断日志（`events*.jsonl`）、`WinShun.log`、`plugins.log`、截图插件日志、`scope.json`，以及脱敏后的设置文件 `WinShun.ini.redacted`；数据文件夹里有用户自己从 Win顺 导出的诊断包时，作为 `WinShun-diagnostics.zip` 一起打包，里面有文件索引的存储数据 |
| `service/` | 后台服务日志 |
| `manifest.json` | 每个收集到的文件的来源、大小、修改时间、是否被截断 |

## 分析思路

1. 先看 `report.txt`，确认任务管理器“内存”列显示的数字（专用工作集）到底来自哪个进程。
2. 用户离开时主程序仍然很高：在 `app/diagnostics/events*.jsonl` 里找 `idle-trim`。
   - `waiting`：Win顺 正忙（`loading`、`catching up`、`reading content`、`moving`）。机械硬盘上内容索引读得很慢，`reading content` 可能持续很久。
   - `held`：电脑唤醒后暂缓归还，要等用户用一次 Win顺 才恢复。
   - 都没有：看 `samples.jsonl` 里前台是不是一直停着 Win顺 的窗口，或 `tool-events.jsonl` 里的可见窗口（剪贴板窗口、固定的搜索框、对话框路径条都算“正在用”）。
3. 归还测试后降到几 MB、并且一直保持低位：内存是能还的，只是 Win顺 自己没有还，按第 2 步找原因。很快又涨回去：是后台工作在用这些内存。找同一时段 `samples.jsonl` 里 `read_kb`、`cpu_ms` 的增长，以及日志里的 `content-*` 事件。
4. 提交量一直上涨，或者出现了 `growth-N`：对比前后两次的 memmap，看是哪一档私有分配在涨。
5. Win顺 本身不高，但用户说内存满了：看 `top.jsonl`、`system.json` 里的内核池标记，以及 PDH 的备用列表、修改列表。备用列表算作可用内存，不是真正被占用。

## 隐私

工具不收集文件内容、剪贴板内容、搜索记录和密码。用户文件夹路径一律写作 `%USERPROFILE%`。设置文件里，`[Proxy]`、`[Shares]`、`[Favorites]`、`[Network]`、`[Mcp]`、`[Backup]` 和 `[Plugin.*]` 这几个分区只保留数字和开关值，其他值替换成 `<removed, N characters>`；其他分区里，键名含 pass、token、secret、apikey、cookie、auth、credential、account、user 等字样的项同样处理。工具对其他进程只读取地址空间布局（`VirtualQueryEx`、`QueryWorkingSet`）、模块列表、命令行和线程信息这类元数据，不读取程序数据，也不生成 dump；唯一会改变其他进程状态的操作，是第 20 分钟的那次归还测试。

## 参数（排查用，普通用户不需要）

| 参数 | 作用 |
| --- | --- |
| `--minutes N` | 运行时长，默认 30 分钟 |
| `--trim-at N` | 在第 N 分钟做归还测试，默认 20；运行时长不到 3 分钟时不做 |
| `--no-trim` | 不做归还测试 |
| `--interval-ms N` | 采样间隔，默认 2000 毫秒 |
| `--out 目录` | 压缩包的保存位置，默认桌面 |
| `--no-elevate` | 不请求管理员权限 |
| `--no-wait` | 结束后不等按键、不打开资源管理器，用于自动测试 |
