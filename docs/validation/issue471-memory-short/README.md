# #471 内存紧张时呼出和搜索卡顿：内存紧张时不主动归还

2026-11-26，本机（Windows 11 26200，32 GB，NVMe，150% 缩放），`build\release`。

## 反馈日志读出来的

反馈者 CPU 60%、内存 86%。几条关键日志：

- `a name search took 4816 ms (… 4415 finding names …)`、`4334 ms (… 3745 finding names …)`：时间几乎全花在扫文件名索引本身，正常是几十毫秒。
- 两次慢搜索都紧跟在闲置之后（14:12:27 收起，约 20 秒后归还，14:12:59 再搜）。
- `new rows 6945 ms after the list asked for them`：引擎算完到列表换上新结果又隔了 2.6 秒，界面线程也在等页面读回。

原因：闲置 20 秒后 Win顺 把内存交还 Windows（`IdleTrim`），页面进备用列表。内存充足时它们留在内存里，下次用到是软缺页，几乎不花时间；内存占用 86% 时，备用列表几分钟内就被别的程序占走，下次呼出和第一次搜索只能从页面文件一页一页读回来，磁盘又正忙，于是几秒。

## 改动

- `IdleTrim`（主程序和编入它的截图、图片文字搜索、开发工具、鼠标手势插件）与 `src/plugins-common/IdleTrim.h`（压缩包、按意思找）：系统内存占用 ≥ 80%（`MEMORYSTATUSEX::dwMemoryLoad`，即任务管理器的“内存”百分比）时不主动归还，`freed()` 也一样；内存宽裕后照常。真缺内存时由 Windows 自己按页龄从各进程收回。诊断日志记 `idle-trim` `held: short of memory` 与 `memory_load`。
- 按快捷键呼出搜索框时，后台线程在读回热页之后，把文件名索引里每次搜索都要从头读到尾的部分（`EntryScan` 与名字块，`FileIndex::searchBlocks`）用 `PrefetchVirtualMemory` 大块预读回来（`FileIndex::prefetchSearch`，不超过可用内存的 1/4）：先前已经还掉、后来被别的程序占走的情况下，不再 4 KB 一页地缺页。
- 诊断：慢搜索日志末尾加 `Windows' memory N% in use, M page faults in all of WinShun meanwhile`；`trigger` 记录加 `memory_load`；设置 › 运行状况在内存紧张时多一行“系统内存已用 N%，闲着时先不归还，宽裕了再还”。
- 探针 `--idle-open-check` 新增：`--idle-open-query` 在每次 use / idle / warm 打开后输入并计时（`search_ms`、`search_engine_ms`、`search_faults`、`search_hard_faults`），`hard_faults` 只数从磁盘读回的缺页，`--idle-open-memory-load` 让 `IdleTrim` 按给定占用判断。

## 复测

```powershell
# 另开一个提权进程：收到 purge.request 时把修改页写出并清空备用列表（模拟“还回去的页被别的程序占走”）
build\release\WinShun.exe --probe-runtime build\issue471\<run> --idle-open-check --renderer d3d11 --idle-open-cycles 3 `
    --idle-open-index "C:\Windows;C:\Program Files" --idle-open-query setup --idle-open-pressure <dir> [--idle-open-memory-load 86]
```

索引 271,088 项。每轮“闲置后”= 归还（或按规则不归还）→ 清空备用列表 → 按快捷键打开 → 输入 setup。三轮的值：

| 情况 | 闲置后打开：硬缺页 | 闲置后第一次搜索 | 其中硬缺页 | 紧接着再搜（参照） |
|---|---|---|---|---|
| 改前（归还，无预读） | 293–689 | 23–60 ms，2,475–4,504 次缺页 | 226–722 | 13–31 ms |
| 改后，内存宽裕（归还 + 预读索引） | 325–633 | 20–94 ms，2,509–4,633 次缺页 | 97–628 | 14–31 ms |
| 改后，内存紧张 86%（不归还） | 0–4 | 17–25 ms，102–450 次缺页 | **0** | 13–18 ms |

- 内存紧张时：闲置后第一次搜索不再从磁盘读任何页，和紧接着再搜一样快。反馈者那台忙碌的电脑上每次硬缺页的代价远高于本机 NVMe（4,415 ms / 约 4,000 页），省下的就是那几秒。
- 内存宽裕时仍照常归还（任务管理器降到几 MB）；预读让第一次搜索的硬缺页少了约 100–130 次（索引部分），其余来自窗口和结果列表等别的内存，这种情况下 Windows 一般不会占走备用页，平时是软缺页。
- 本机的 NVMe 与空闲磁盘上，改前的绝对时间不大；反馈者的数量级（秒）来自慢盘和磁盘争用，本机没有复现到秒级。

## 测试

- `tst_idletrim` 8/8 通过，新增 `heldWhileWindowsIsShortOfMemory`（86% 时空闲与 `freed()` 都不归还、日志与状态说明原因；降到 79% 后照常归还），其余用例固定按 50% 占用跑，免得测试机内存紧张时误判。
- `tst_core` 的 `indexKeepsWhatASearchReadsApart`、`mappedIndexStorageSurvivesCompactionAndSnapshot` 通过（`searchBlocks` 恰为扫描块加名字块，`prefetchSearch` 返回其总字节）。
- 编译通过同样用到这段代码的插件：截图、开发工具、鼠标手势、图片文字搜索、压缩包、按意思找。
- `--preview-settings --scale 1.5 --health-memory-load 86`（浅色、深色）检查运行状况页新增一行的排版。

![运行状况页，内存紧张时多出的一行（浅色 150%）](health-memory-short.png)
