# 压缩包搜索插件（winshun.archive 1.0.0）验证

2026-11-25，Windows 11 Pro 26200，工作树 `archive-search`（基于 4ab4898b），构建目录 `build\archive-search`。libarchive 3.8.11 + zlib 1.4 + liblzma 5.8.4，静态链接进 `WinShunArchive.exe`（1.0 MB）。

| 验证 | 结果 |
|---|---|
| `ctest -R "^(tst_archive\|tst_mcp\|tst_plugin\|tst_pluginreadme\|tst_pluginsdk)$"` | 5 个套件全部通过。`tst_archive`（20 项）：索引的搜索规则（词都要有、压缩包自己的名字也算、名字里命中的排前、类型和文件夹筛选、条数上限）、存盘与损坏文件、拷贝共享一份清单；经 `WinShunArchive.exe --list / --take` 读 `tests/fixtures/archives` 里的 zip（UTF-8 标记、GBK、未标记的 UTF-8）、7z、tar、tar.gz、tar.xz、RAR4、RAR5，以及内容加密、文件名加密（7z、rar）、不是压缩包、下载到一半的 zip。`tst_mcp::listFiles`：`list_files` 分页、扩展名不分大小写、限定文件夹、读不了的文件不列、对 AI 客户端不开放 |
| `python tools/verify_archive_plugin.py <WinShunArchive.exe>`（模拟宿主按协议驱动插件进程，压缩包是样本的临时副本） | 56 项全部通过：首次读取时卡片上的进度条和“暂停”，读完后的统计（只读到一部分、文件名加密、不是压缩包各有数）；关键字和内容检索两种查询、面板的文件夹和类型筛选；Enter / Ctrl+Enter / Shift+Enter 三个动作，加密文件、包内文件夹、程序和脚本各自的提示；`details` 给出解压到数据文件夹的文件，包内文件夹给出清单；压缩包删除、被替换后“立即更新”；宿主索引还在建时不遗忘；设置里加类型、限定文件夹；拿不到清单时卡片写明原因而已有结果照常可搜；重启后不重读；进程死在某个包上时下次启动跳过它并计数，它变了再读；暂停与继续；`shutdown` 后进程自行退出 |
| `wsbench --archive readme qglobal setup …`（真实的 `WinShunPluginHost`，严格沙箱、低完整性；`list_files` 由 `plugin::Tools` 在本机索引的副本上回答） | 本机 303 万条索引里有 672 个压缩包、113.9 万个包内文件：首次 24 秒读完（后台最低磁盘优先级），9 个确实不是压缩包（微信的资源文件、测试占位文件）；查询 8–25 ms；`details` 取出包内文件，三个动作的 effects 正确；插件进程专用内存 26 MB，`archives.bin` 24 MB；带着数据再启动，0.2 秒就绪、不重读 |
| `wsbench --launcher-sim --plugins <只含本插件> --data winshun.archive=<上一步的数据> qglobal "zip qglobal"` | “全部”里压缩包的前 3 行排在前 5 个名称结果之后，是三个不同的包内文件，其余排在最后；关键字模式只列压缩包里的 |

去重前后（同一台机器，同样的 672 个压缩包，其中约八成的文件名来自同一批源码包的多份拷贝）：

| | 清单各存各的 | 相同的清单共享、同名同大小同时间的拷贝不重读 |
|---|---|---|
| 首次读完 | 78 秒 | 24 秒 |
| 插件专用内存 | 122 MB | 26 MB |
| `archives.bin` | 123 MB | 24 MB |
| 查 `readme`（200 行） | 151 ms | 20 ms |

没有验证的：

- 在真实运行的 Win顺 界面里装上并使用（结果行的样子、内容详情窗格里的预览）。这次主工作树正在发布 0.46.0，没有另起实例；插件一侧走的是真实宿主、真实沙箱和真实合并逻辑，界面用的是内容检索提供者和 `details` 已有的显示。
- Windows 10，以及压缩包在机械硬盘上时的读取速度。
- 网盘上“仅联机”的压缩包（代码按文件属性跳过并计数，本机没有这样的文件）。
- tar.bz2、tar.zst 未编入（libarchive 构建时没带 bzip2 和 zstd），按扩展名本来也不列。
